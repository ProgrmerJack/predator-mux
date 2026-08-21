// SPDX-License-Identifier: MIT
/*
 * nvmuxk - issue NVIDIA RM display-mux controls at RS_PRIV_LEVEL_KERNEL.
 *
 * The mux sequence (INIT_MUX_DATA / RUN_PRE / SWITCH / RUN_POST / GET_STATUS)
 * is KERNEL_PRIVILEGED, so no userspace process can call it at any uid.
 * nvidia.ko exports nvidia_get_rm_ops(); its .op is rm_kernel_rmapi_op(),
 * which dispatches NV04_CONTROL -> Nv04ControlKernel() -> RS_PRIV_LEVEL_KERNEL.
 *
 * All ABI below was read from open-gpu-kernel-modules tag 610.57.04:
 *   kernel-open/common/inc/nv-modeset-interface.h   rm_ops struct (note the two
 *                                                   leading fields)
 *   kernel-open/common/inc/nv-gpu-info.h            nv_gpu_info_t
 *   src/nvidia/arch/nvalloc/unix/include/nv-kernel-rmapi-ops.h
 *   src/common/sdk/nvidia/inc/nvos.h                NVOS54/NVOS64, op codes
 *   src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073dfp.h
 *
 * Usage:
 *   echo status  > /proc/nvmuxk   read-only mux status for every display
 *   echo seq     > /proc/nvmuxk   INIT -> PRE -> SWITCH -> POST, hold, reverse
 *   echo dgpu    > /proc/nvmuxk   full sequence to the dGPU
 *   echo igpu    > /proc/nvmuxk   force sequence back to the iGPU
 *   echo sw-dgpu > /proc/nvmuxk   INIT + SWITCH only (live/session-preserving)
 *   echo sw-igpu > /proc/nvmuxk   INIT + SWITCH only, back to the iGPU
 * Output goes to dmesg.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/string.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("NVIDIA display-mux control at kernel RM privilege");

/* EDID bytes 0x08..0x0B read as native u16, matching nvt_edid.c:
 *   pInfo->manuf_id = p->wIDManufName;  pInfo->product_id = p->wIDProductCode;
 * INIT_MUX_DATA fails without these (nvkms-rm.c MuxInit), and a failed
 * INIT_MUX_DATA wedges GSP's mux state until the next reboot - every later
 * GET_DISP_MUX_STATUS returns NV_ERR_NOT_SUPPORTED, and unloading this module
 * does not clear it. They are therefore not defaulted to any particular panel:
 * a wrong id is worse than a missing one, so 0 refuses the call instead.
 * mux-switch reads them off the panel and passes them on insmod. */
static uint manfid;
static uint productid;
static uint tconid;               /* NVKMS leaves this 0 */
static uint usequery = 1;         /* call QUERY_DISPLAY_IDS_WITH_MUX first */
static uint brightness;           /* NVKMS leaves iGpuBrightness at 0 */
/* ACPI id of the internal panel, as RM's acpiId <-> displayId map wants it.
   NVKMS ships a hardcoded table with one machine in it (acpiId 0x8001a420,
   commented in nvkms-rm.c as "a poor-man's alternative to the WDDM driver's
   CDisplayMgr::NVInitializeACPIToDeviceMaskMap()"). Ours differs, and so will
   every other machine's, so it is derived at load time from the dGPU's own
   ACPI namespace instead - see acpi_panel_id(). 0 means "derive it"; a non-zero
   module parameter overrides the probe.
   RUN_PRE toggles LCD VDD / BL EN / PWM MUX, which are ACPI-side, so RM likely
   needs this mapping to act on the panel's displayId. */
static uint acpiid;
/* The skip bits NVKMS clobbers (see MUX_F_* below). Measured on this machine
   with 'probe-pre', twice, identical both times:
     RUN_PRE  sr=no  -> 0x56 NOT_SUPPORTED   (both directions)
     RUN_PRE  sr=YES -> 0x0  OK              (both directions)
     RUN_POST         -> 0x0  OK             (all four combinations)
   So RUN_PRE is NOT unimplemented: only its PSR-entry sub-step is refused.
   The rest of pre-ops - SOR sequencer, BL GPIO control, LCD VDD / BL EN / PWM
   MUX toggling - is supported. NVKMS can never reach this because nvRmMuxPre()
   assigns params.flags with '=' immediately after setting SR_ENTER_SKIP,
   discarding it. Default srskip=1: it is the only accepted combination. */
static uint srskip = 1;           /* 1 = SR_ENTER_SKIP_YES / SR_EXIT_SKIP_YES */
static uint blskip;               /* 1 = SKIP_BACKLIGHT_ENABLE_YES */
/* Load anyway on a driver whose ABI this module was not read from. Off by
   default: the failure mode is not a clean error but wrong bytes in a
   KERNEL_PRIVILEGED control. */
static uint force;
module_param(manfid, uint, 0444);
module_param(productid, uint, 0444);
module_param(tconid, uint, 0444);
module_param(usequery, uint, 0444);
module_param(brightness, uint, 0644);
module_param(acpiid, uint, 0444);
module_param(srskip, uint, 0644);
module_param(blskip, uint, 0644);
module_param(force, uint, 0444);

MODULE_PARM_DESC(acpiid, "ACPI id of the internal panel (0 = derive from the GPU's ACPI namespace)");
MODULE_PARM_DESC(manfid, "panel EDID manufacturer id (0 = derive from the attached panel)");
MODULE_PARM_DESC(productid, "panel EDID product id (0 = derive from the attached panel)");
MODULE_PARM_DESC(force, "load even if the driver version does not match the ABI this was read from");

#define PFX "nvmuxk: "

/* The driver release every struct below was transcribed from. Checked against
   the running driver at load; see rm_setup(). Update this only together with a
   re-read of the headers listed at the top of this file. */
#define NVMUXK_ABI_VERSION "610.57.04"

typedef u32 NvU32; typedef s32 NvV32; typedef u32 NvHandle;
typedef u8 NvU8;  typedef u8 NvBool; typedef u64 NvP64;
#define NV_TRUE 1
#define NV_FALSE 0

typedef struct nvidia_stack_s *nvidia_modeset_stack_ptr;

typedef struct {
	NvU32 gpu_id;
	struct { NvU32 domain; NvU8 bus, slot, function; } pci_info;
	NvBool needs_numa_setup;
	NvBool is_soc_disp;
	void *os_device_ptr;
} nv_gpu_info_t;

typedef struct {
	void (*suspend)(NvU32 gpu_id);
	void (*resume)(NvU32 gpu_id);
	void (*remove)(NvU32 gpu_id);
	void (*probe)(const nv_gpu_info_t *gpu_info);
} nvidia_modeset_callbacks_t;

typedef struct {
	const char *version_string;                      /* must be preset */
	struct { NvBool allow_write_combining; } system_info;
	int   (*alloc_stack)(nvidia_modeset_stack_ptr *sp);
	void  (*free_stack)(nvidia_modeset_stack_ptr sp);
	NvU32 (*enumerate_gpus)(nv_gpu_info_t *gpu_info);
	int   (*open_gpu)(NvU32 gpu_id, nvidia_modeset_stack_ptr sp, NvBool reset_aware);
	void  (*close_gpu)(NvU32 gpu_id, nvidia_modeset_stack_ptr sp, NvBool reset_aware);
	void  (*op)(nvidia_modeset_stack_ptr sp, void *ops_cmd);
	int   (*set_callbacks)(const nvidia_modeset_callbacks_t *cb);
} nvidia_modeset_rm_ops_t;

extern u32 nvidia_get_rm_ops(nvidia_modeset_rm_ops_t *rm_ops);

/* nvos.h */
#define NV01_FREE     0x00000000
#define NV04_ALLOC    0x00000015
#define NV04_CONTROL  0x00000036

typedef struct { NvHandle hRoot, hObjectParent, hObjectNew; NvV32 hClass;
		 NvP64 pAllocParms; NvP64 pRightsRequested;
		 NvU32 paramsSize; NvU32 flags; NvV32 status; } NVOS64;
typedef struct { NvHandle hClient, hObject; NvV32 cmd; NvU32 flags;
		 NvP64 params; NvU32 paramsSize; NvV32 status; } NVOS54;
typedef struct { NvHandle hRoot, hObjectParent, hObjectOld; NvV32 status; } NVOS00;

typedef struct {
	NvU32 op;
	union { NVOS00 free; u8 _pad0[64];
		NVOS64 alloc; NVOS54 control; u8 _pad[512]; } params;
} rmapi_ops_t;

/* classes */
#define NV01_ROOT            0x00000000
#define NV01_DEVICE_0        0x00000080
#define NV20_SUBDEVICE_0     0x00002080
#define NV04_DISPLAY_COMMON  0x00000073

/* controls */
#define CMD_GET_SUPPORTED   0x730107
#define CMD_INIT_MUX_DATA   0x731158
#define CMD_SWITCH_MUX      0x731160
#define CMD_PRE_MUX_OPS     0x731161
#define CMD_POST_MUX_OPS    0x731162
#define CMD_GET_MUX_STATUS  0x731163
#define CMD_DFP_GET_INFO    0x731140
#define CMD_QUERY_IDS_WITH_MUX 0x73013d
#define CMD_SET_ACPI_ID_MAP    0x730284
#define CMD_GET_CONNECT_STATE  0x730108
#define ACPI_MAP_ENTRIES       16

/* NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_FLAGS_METHOD 1:0
 *   _DEFAULT 0x0 (perform a real detect)  _CACHED 0x1 (return last known)
 * After a mux switch RM still holds the pre-switch connection state; asking
 * with _DEFAULT forces it to go out and detect over the (now re-routed) AUX. */
#define CONNECT_METHOD_DEFAULT 0x0
#define CONNECT_METHOD_CACHED  0x1

/* Flag bitfields, copied from ctrl0073dfp.h. NVIDIA writes them as DRF ranges;
 * the shift is the low bit of each range.
 *   NV0073_CTRL_DFP_DISP_MUX_FLAGS_SWITCH_TYPE                  0:0
 *     _IGPU_TO_DGPU 0x0   _DGPU_TO_IGPU 0x1
 *   NV0073_CTRL_DFP_DISP_MUX_FLAGS_SR_ENTER_SKIP                1:1   (PRE)
 *   NV0073_CTRL_DFP_DISP_MUX_FLAGS_SR_EXIT_SKIP                 1:1   (POST)
 *     _NO 0x0             _YES 0x1
 *   NV0073_CTRL_DFP_DISP_MUX_FLAGS_MUX_SWITCH_IGPU_POWER_TIMING 2:2
 *     _KNOWN 0x0          _UNKNOWN 0x1
 *   NV0073_CTRL_DFP_DISP_MUX_FLAGS_SKIP_BACKLIGHT_ENABLE        3:3
 *     _NO 0x0             _YES 0x1
 * NVKMS never exercises the skip bits: nvkms-rm.c nvRmMuxPre()/nvRmMuxPost()
 * assign params.flags with '=' right after setting SR_*_SKIP, clobbering it.
 * Both constants are 0 so it is harmless there, but it means those paths are
 * untested upstream - hence the module params below. */
#define MUX_F_SWITCH_TYPE_SHIFT     0
#define MUX_F_SR_SKIP_SHIFT         1
#define MUX_F_IGPU_POWER_TIMING_SHIFT 2
#define MUX_F_SKIP_BACKLIGHT_SHIFT  3

/* NV0073_CTRL_DISP_MUX_BACKLIGHT_BRIGHTNESS_MIN / _MAX */
#define MUX_BRIGHTNESS_MIN     0U
#define MUX_BRIGHTNESS_MAX   100U

typedef struct { NvU32 deviceId; NvHandle hClientShare, hTargetClient, hTargetDevice;
		 NvV32 flags; u64 vaSpaceSize, vaStartInternal, vaLimitInternal;
		 NvV32 vaMode; } NV0080_ALLOC;
typedef struct { NvU32 subDeviceId; } NV2080_ALLOC;
typedef struct { NvU32 subDeviceInstance, displayMask, displayMaskDDC; } GetSupported;
typedef struct { NvU32 subDeviceInstance, displayId, muxStatus; } MuxStatus;
typedef struct { NvU32 subDeviceInstance, displayId, flags, UHBRSupportedByDfp; } DfpInfo;
typedef struct { NvU32 subDeviceInstance, muxDisplayMask; } MuxIds;
typedef struct { NvU32 subDeviceInstance, acpiId, displayId, dodIndex; } AcpiMapEnt;
typedef struct { AcpiMapEnt mapTable[ACPI_MAP_ENTRIES]; } AcpiMap;
typedef struct { NvU32 subDeviceInstance, displayId; u16 manfId, productId, tconId; } InitMuxData;
typedef struct { NvU32 subDeviceInstance, displayId, flags, iGpuBrightness,
		 preOpsLatencyMs, psrEntryLatencyMs; } PreMuxOps;
typedef struct { NvU32 subDeviceInstance, displayId, flags, auxSettleDelay,
		 muxSwitchLatencyMs; } SwitchMux;
typedef struct { NvU32 subDeviceInstance, displayId, flags, postOpsLatencyMs,
		 psrExitLatencyMs, psrExitTransitionToInactiveLatencyMs; } PostMuxOps;
typedef struct { NvU32 subDeviceInstance, flags, displayMask, retryTimeMs; } ConnectState;

static nvidia_modeset_rm_ops_t ops;
static nvidia_modeset_stack_ptr sp;
static NvU32 gpu_id;
static NvHandle hClient, hDevice, hSub, hDisp;
static bool rm_up;

static const char *st_name(NvU32 s)
{
	switch (s & 3) { case 0: return "INVALID"; case 1: return "INTEGRATED_GPU";
			 case 2: return "DISCRETE_GPU"; default: return "?"; }
}
static const char *md_name(NvU32 s)
{
	switch ((s >> 2) & 7) { case 0: return "INVALID"; case 1: return "INTEGRATED_ONLY";
		case 2: return "DISCRETE_ONLY"; case 3: return "HYBRID";
		case 4: return "DYNAMIC(switchable)"; default: return "?"; }
}

static int rm_alloc(NvHandle parent, NvHandle *obj, NvV32 cls, void *p, NvU32 psz)
{
	rmapi_ops_t c;
	memset(&c, 0, sizeof c);
	c.op = NV04_ALLOC;
	c.params.alloc.hRoot = hClient;
	c.params.alloc.hObjectParent = parent;
	c.params.alloc.hObjectNew = *obj;
	c.params.alloc.hClass = cls;
	c.params.alloc.pAllocParms = (NvP64)(uintptr_t)p;
	c.params.alloc.paramsSize = psz;
	ops.op(sp, &c);
	if (c.params.alloc.status) {
		pr_err(PFX "alloc class 0x%x -> status 0x%x\n", cls, c.params.alloc.status);
		return -EIO;
	}
	*obj = c.params.alloc.hObjectNew;
	return 0;
}

static NvV32 rm_ctrl(NvHandle obj, NvV32 cmd, void *p, NvU32 psz)
{
	rmapi_ops_t c;
	memset(&c, 0, sizeof c);
	c.op = NV04_CONTROL;
	c.params.control.hClient = hClient;
	c.params.control.hObject = obj;
	c.params.control.cmd = cmd;
	c.params.control.params = (NvP64)(uintptr_t)p;
	c.params.control.paramsSize = psz;
	ops.op(sp, &c);
	return c.params.control.status;
}

static void rm_free(NvHandle obj, NvHandle parent)
{
	rmapi_ops_t c;
	memset(&c, 0, sizeof c);
	c.op = NV01_FREE;
	c.params.free.hRoot = hClient;
	c.params.free.hObjectParent = parent;
	c.params.free.hObjectOld = obj;
	ops.op(sp, &c);
}

/* Derive the internal panel's ACPI id from the dGPU's own ACPI namespace.
 *
 * Two sources, in order of authority:
 *
 *   _DOD on the GPU's ACPI device is the list of display devices the firmware
 *   says this adapter drives, and its entries are exactly the ids RM's map
 *   wants. It is the same thing the WDDM driver walks. On some firmware it is
 *   gated on the BIOS display-mode setting and returns an empty or zero-only
 *   package, which is why there is a second source.
 *
 *   Failing that, each display child of the GPU carries the same value in its
 *   own _ADR, unconditionally.
 *
 * Either way an id only counts if bit 31 is set: ACPI Appendix B reserves that
 * bit for "this id conforms to the display-device layout", and the small
 * indices firmware uses for non-display children would otherwise be accepted.
 */
#define ACPI_DISPLAY_ID_VALID 0x80000000u

static NvU32 acpi_id_from_dod(acpi_handle handle)
{
	struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *pkg;
	NvU32 found = 0;
	int i;

	if (ACPI_FAILURE(acpi_evaluate_object(handle, "_DOD", NULL, &buf)))
		return 0;

	pkg = buf.pointer;
	if (pkg && pkg->type == ACPI_TYPE_PACKAGE) {
		for (i = 0; i < pkg->package.count && !found; i++) {
			union acpi_object *e = &pkg->package.elements[i];
			NvU32 id;

			if (e->type != ACPI_TYPE_INTEGER)
				continue;
			id = (NvU32)e->integer.value;
			if (id & ACPI_DISPLAY_ID_VALID)
				found = id;
		}
	}
	kfree(buf.pointer);
	return found;
}

static NvU32 acpi_id_from_children(acpi_handle handle)
{
	acpi_handle child = NULL;
	NvU32 found = 0;

	while (!found &&
	       ACPI_SUCCESS(acpi_get_next_object(ACPI_TYPE_DEVICE, handle,
						 child, &child))) {
		unsigned long long adr;

		if (ACPI_FAILURE(acpi_evaluate_integer(child, "_ADR", NULL, &adr)))
			continue;
		if ((NvU32)adr & ACPI_DISPLAY_ID_VALID)
			found = (NvU32)adr;
	}
	return found;
}

static NvU32 acpi_panel_id(const nv_gpu_info_t *gpu)
{
	struct pci_dev *pdev;
	acpi_handle handle;
	NvU32 id;

	pdev = pci_get_domain_bus_and_slot(gpu->pci_info.domain,
					   gpu->pci_info.bus,
					   PCI_DEVFN(gpu->pci_info.slot,
						     gpu->pci_info.function));
	if (!pdev) {
		pr_info(PFX "no pci_dev for %04x:%02x:%02x.%x\n",
			gpu->pci_info.domain, gpu->pci_info.bus,
			gpu->pci_info.slot, gpu->pci_info.function);
		return 0;
	}

	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle) {
		pr_info(PFX "GPU has no ACPI companion\n");
		pci_dev_put(pdev);
		return 0;
	}

	id = acpi_id_from_dod(handle);
	if (id)
		pr_info(PFX "acpiId 0x%08x from _DOD\n", id);
	else if ((id = acpi_id_from_children(handle)))
		pr_info(PFX "acpiId 0x%08x from a display child's _ADR\n", id);
	else
		pr_info(PFX "no ACPI display id on this GPU\n");

	pci_dev_put(pdev);
	return id;
}

static int rm_setup(void)
{
	nv_gpu_info_t *gi;
	NV0080_ALLOC da; NV2080_ALLOC sa;
	NvU32 n, pick, i;
	int rc;

	/* nvidia_get_rm_ops() rejects a version mismatch but writes the expected
	   string back into the struct, so a second call with it matches exactly.
	   That makes the handshake itself version-independent - but only the
	   handshake. Everything below hand-copies NVOS54_PARAMETERS and the
	   NV0073_CTRL_* parameter structs out of one release's headers, and a
	   layout change in a later driver would compile clean and send wrong
	   bytes to a KERNEL_PRIVILEGED control. So the version the ABI was read
	   from is checked here rather than assumed. */
	ops.version_string = "";
	rc = nvidia_get_rm_ops(&ops);
	if (rc) {
		pr_info(PFX "driver reports version \"%s\", retrying\n", ops.version_string);
		rc = nvidia_get_rm_ops(&ops);
	}
	if (rc) { pr_err(PFX "nvidia_get_rm_ops failed 0x%x\n", rc); return -EINVAL; }
	pr_info(PFX "rm_ops ok, version \"%s\"\n", ops.version_string);

	if (strcmp(ops.version_string, NVMUXK_ABI_VERSION) != 0) {
		if (!force) {
			pr_err(PFX "driver is %s, this module's ABI was read from %s\n",
			       ops.version_string, NVMUXK_ABI_VERSION);
			pr_err(PFX "refusing: re-check the structs against your driver, then load with force=1\n");
			return -EINVAL;
		}
		pr_warn(PFX "force=1: proceeding on %s with ABI from %s\n",
			ops.version_string, NVMUXK_ABI_VERSION);
	}

	if (ops.alloc_stack(&sp)) { pr_err(PFX "alloc_stack failed\n"); return -ENOMEM; }

	gi = kzalloc(sizeof(*gi) * 32, GFP_KERNEL);
	if (!gi) { ops.free_stack(sp); return -ENOMEM; }
	n = ops.enumerate_gpus(gi);
	if (!n) { pr_err(PFX "no GPUs\n"); kfree(gi); ops.free_stack(sp); return -ENODEV; }

	/* Prefer a GPU whose ACPI namespace actually describes a panel. On a
	   laptop with one dGPU that is gi[0] either way, but taking the first
	   entry unconditionally picks wrongly the moment a second NVIDIA GPU is
	   present - an eGPU, or a second card - and the mux is on neither. */
	pick = 0;
	if (!acpiid) {
		for (i = 0; i < n; i++) {
			NvU32 id = acpi_panel_id(&gi[i]);
			if (id) { pick = i; acpiid = id; break; }
		}
	}
	gpu_id = gi[pick].gpu_id;
	pr_info(PFX "gpu_id 0x%x at %04x:%02x:%02x.%x (%u GPU%s, using #%u)\n",
		gpu_id, gi[pick].pci_info.domain, gi[pick].pci_info.bus,
		gi[pick].pci_info.slot, gi[pick].pci_info.function,
		n, n == 1 ? "" : "s", pick);
	if (!acpiid)
		pr_warn(PFX "no ACPI panel id found; SET_ACPI_ID_MAPPING will be skipped\n");
	else
		pr_info(PFX "acpiId 0x%08x\n", acpiid);
	kfree(gi);

	if (ops.open_gpu(gpu_id, sp, NV_FALSE)) {
		pr_err(PFX "open_gpu failed\n"); ops.free_stack(sp); return -EIO; }

	hClient = 0;
	if (rm_alloc(0, &hClient, NV01_ROOT, NULL, 0)) goto err;
	pr_info(PFX "hClient 0x%08x (kernel privilege)\n", hClient);

	memset(&da, 0, sizeof da); hDevice = 0xAB000080;
	if (rm_alloc(hClient, &hDevice, NV01_DEVICE_0, &da, sizeof da)) goto err;
	memset(&sa, 0, sizeof sa); hSub = 0xAB002080;
	if (rm_alloc(hDevice, &hSub, NV20_SUBDEVICE_0, &sa, sizeof sa)) goto err;
	hDisp = 0xAB000073;
	if (rm_alloc(hDevice, &hDisp, NV04_DISPLAY_COMMON, NULL, 0)) goto err;

	rm_up = true;
	pr_info(PFX "RM objects ready (disp 0x%08x)\n", hDisp);
	return 0;
err:
	ops.close_gpu(gpu_id, sp, NV_FALSE);
	ops.free_stack(sp);
	return -EIO;
}

static void rm_teardown(void)
{
	if (!rm_up) return;
	rm_free(hDisp, hDevice); rm_free(hSub, hDevice);
	rm_free(hDevice, hClient); rm_free(hClient, hClient);
	ops.close_gpu(gpu_id, sp, NV_FALSE);
	ops.free_stack(sp);
	rm_up = false;
}

static NvU32 find_mux_display(void)
{
	GetSupported gs; MuxStatus ms; NvV32 st; int b; NvU32 found = 0;
	MuxIds mi;

	memset(&mi, 0, sizeof mi);
	if (!usequery) { pr_info(PFX "(skipping QUERY_DISPLAY_IDS_WITH_MUX)\n"); goto skipq; }
	st = rm_ctrl(hDisp, CMD_QUERY_IDS_WITH_MUX, &mi, sizeof mi);
	if (st) pr_info(PFX "QUERY_DISPLAY_IDS_WITH_MUX status 0x%x\n", st);
	else    pr_info(PFX "muxDisplayMask 0x%08x (authoritative)\n", mi.muxDisplayMask);
skipq:

	memset(&gs, 0, sizeof gs);
	st = rm_ctrl(hDisp, CMD_GET_SUPPORTED, &gs, sizeof gs);
	if (st) { pr_err(PFX "GET_SUPPORTED status 0x%x\n", st); return 0; }
	pr_info(PFX "displayMask 0x%08x\n", gs.displayMask);

	for (b = 0; b < 32; b++) {
		NvU32 id = 1u << b;
		DfpInfo di;
		int edp = -1;
		if (!(gs.displayMask & id)) continue;

		memset(&di, 0, sizeof di); di.displayId = id;
		if (!rm_ctrl(hDisp, CMD_DFP_GET_INFO, &di, sizeof di))
			edp = (di.flags >> 15) & 1;   /* EMBEDDED_DISPLAYPORT, bit 15 */

		memset(&ms, 0, sizeof ms); ms.displayId = id;
		st = rm_ctrl(hDisp, CMD_GET_MUX_STATUS, &ms, sizeof ms);
		if (st) {
			pr_info(PFX "  id 0x%08x: eDP=%d dfpFlags=0x%08x  GET_MUX_STATUS 0x%x (not muxed)\n",
				id, edp, di.flags, st);
			continue;
		}
		pr_info(PFX "  id 0x%08x: eDP=%d dfpFlags=0x%08x  muxStatus 0x%08x state=%s mode=%s%s\n",
			id, edp, di.flags, ms.muxStatus, st_name(ms.muxStatus),
			md_name(ms.muxStatus), (edp == 1) ? "  <== EMBEDDED PANEL" : "");
		if (!found) found = id;
	}
	if (found) pr_info(PFX "mux display selected: 0x%08x\n", found);
	return found;
}

/* Force RM to re-detect. NVKMS follows its switch with nvDPNotifyShortPulse()
   to make the DP library re-train and re-detect, but that path needs the dpy in
   pDispEvo->muxDisplays, which MuxInit() only fills for the one machine in its
   hardcoded ACPI table. This is the closest reachable equivalent: an uncached
   GET_CONNECT_STATE makes RM detect over the re-routed AUX instead of returning
   the stale pre-switch answer. */
static void detect_now(NvU32 id, const char *tag)
{
	ConnectState cs;
	NvV32 st;

	memset(&cs, 0, sizeof cs);
	cs.flags = CONNECT_METHOD_DEFAULT;
	cs.displayMask = id;
	st = rm_ctrl(hDisp, CMD_GET_CONNECT_STATE, &cs, sizeof cs);
	pr_info(PFX "[%s] GET_CONNECT_STATE(forced) -> 0x%x displayMask=0x%08x retry=%ums %s\n",
		tag, st, cs.displayMask, cs.retryTimeMs,
		st ? "" : ((cs.displayMask & id) ? "CONNECTED" : "not connected"));
}

static void report(NvU32 id, const char *tag)
{
	MuxStatus ms; NvV32 st;
	memset(&ms, 0, sizeof ms); ms.displayId = id;
	st = rm_ctrl(hDisp, CMD_GET_MUX_STATUS, &ms, sizeof ms);
	if (st) pr_info(PFX "[%s] GET_MUX_STATUS status 0x%x\n", tag, st);
	else pr_info(PFX "[%s] state=%s mode=%s (0x%08x)\n", tag,
		     st_name(ms.muxStatus), md_name(ms.muxStatus), ms.muxStatus);
}

/* dir: 0 = iGPU->dGPU, 1 = dGPU->iGPU */
static NvV32 do_acpi_map(NvU32 id)
{
	AcpiMap *m; NvV32 st;
	if (!acpiid) {
		pr_info(PFX "no ACPI panel id, skipping SET_ACPI_ID_MAPPING\n");
		return 0;
	}
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m) return -1;
	m->mapTable[0].acpiId = acpiid;
	m->mapTable[0].displayId = id;
	m->mapTable[0].dodIndex = 0;
	st = rm_ctrl(hDisp, CMD_SET_ACPI_ID_MAP, m, sizeof(*m));
	pr_info(PFX "SET_ACPI_ID_MAPPING(acpi=0x%08x -> disp 0x%08x) -> 0x%x %s\n",
		acpiid, id, st, st ? "FAILED" : "OK");
	kfree(m);
	return st;
}

static NvV32 do_init(NvU32 id)
{
	InitMuxData init; NvV32 st;
	/* Refuse rather than send ids that are not this panel's: the cost of
	   being wrong here is a reboot, not an error return. */
	if (!manfid || !productid) {
		pr_err(PFX "no panel EDID ids; pass manfid= and productid= (mux-preflight prints them)\n");
		return -1;
	}
	do_acpi_map(id);          /* NVKMS does this first in MuxInit() */
	memset(&init, 0, sizeof init); init.displayId = id;
	init.manfId = (u16)manfid; init.productId = (u16)productid;
	init.tconId = (u16)tconid;
	st = rm_ctrl(hDisp, CMD_INIT_MUX_DATA, &init, sizeof init);
	pr_info(PFX "INIT_MUX_DATA(manf=0x%04x prod=0x%04x tcon=0x%04x) -> 0x%x %s\n",
		manfid, productid, tconid, st, st ? "FAILED" : "OK");
	return st;
}

/* Compose the flags word the way nvRmMuxPre()/nvRmMuxPost() intended to,
   i.e. OR-ing the fields instead of overwriting them. */
static NvU32 mux_flags(NvU32 dir)
{
	return (dir            ? 1u << MUX_F_SWITCH_TYPE_SHIFT   : 0) |
	       (srskip         ? 1u << MUX_F_SR_SKIP_SHIFT       : 0) |
	       (blskip         ? 1u << MUX_F_SKIP_BACKLIGHT_SHIFT : 0);
}

/* dir: 0 = iGPU->dGPU, 1 = dGPU->iGPU.  onlyswitch: skip RUN_PRE/RUN_POST.
   Both return NV_ERR_NOT_SUPPORTED (PRE) / a no-op success (POST) here: they
   carry RMCTRL_FLAGS_ROUTE_TO_PHYSICAL (0x40) and so execute inside GSP
   firmware, unlike SWITCH_DISP_MUX which is RMCTRL_FLAGS_PRIVILEGED (0x4) and
   is implemented in open code (disp_common_ctrl_acpi.c). */
static void run_sequence(NvU32 id, NvU32 dir, NvU32 onlyswitch)
{
	PreMuxOps pre; SwitchMux sw; PostMuxOps post;
	NvU32 flags = mux_flags(dir);
	NvV32 st;

	pr_info(PFX "=== sequence %s on displayId 0x%08x flags 0x%x%s ===\n",
		dir ? "dGPU->iGPU" : "iGPU->dGPU", id, flags,
		onlyswitch ? " (switch only)" : "");

	if (!onlyswitch) {
		memset(&pre, 0, sizeof pre); pre.displayId = id; pre.flags = flags;
		pre.iGpuBrightness = clamp_val(brightness, MUX_BRIGHTNESS_MIN,
					       MUX_BRIGHTNESS_MAX);
		st = rm_ctrl(hDisp, CMD_PRE_MUX_OPS, &pre, sizeof pre);
		pr_info(PFX "RUN_PRE_MUX_OPS -> 0x%x %s preOps=%ums psrEntry=%ums (iGpuBrightness=%u)\n",
			st, st ? "" : "OK", pre.preOpsLatencyMs,
			pre.psrEntryLatencyMs, pre.iGpuBrightness);
	}

	memset(&sw, 0, sizeof sw); sw.displayId = id; sw.flags = flags;
	st = rm_ctrl(hDisp, CMD_SWITCH_MUX, &sw, sizeof sw);
	pr_info(PFX "SWITCH_DISP_MUX -> 0x%x %s muxSwitch=%ums\n",
		st, st ? "" : "OK", sw.muxSwitchLatencyMs);
	/* 0x55 = NV_ERR_NOT_READY. Observed on a reverse switch issued ~4s after
	   the forward one - the mux had not settled yet. Retry once rather than
	   leaving the panel stranded. */
	if (st == 0x55) {
		msleep(500);
		memset(&sw, 0, sizeof sw); sw.displayId = id; sw.flags = flags;
		st = rm_ctrl(hDisp, CMD_SWITCH_MUX, &sw, sizeof sw);
		pr_info(PFX "SWITCH_DISP_MUX retry -> 0x%x %s muxSwitch=%ums\n",
			st, st ? "" : "OK", sw.muxSwitchLatencyMs);
	}
	msleep(100);          /* nvkms-rm.c: nvkms_usleep(100000) AUX settle */
	report(id, "after switch");
	detect_now(id, "after switch");

	if (!onlyswitch) {
		memset(&post, 0, sizeof post); post.displayId = id; post.flags = flags;
		st = rm_ctrl(hDisp, CMD_POST_MUX_OPS, &post, sizeof post);
		pr_info(PFX "RUN_POST_MUX_OPS-> 0x%x %s postOps=%ums psrExit=%ums\n",
			st, st ? "" : "OK", post.postOpsLatencyMs, post.psrExitLatencyMs);
		report(id, "after post");
		detect_now(id, "after post");
	}
}

/* RUN_PRE returns NV_ERR_NOT_SUPPORTED (0x56) here. It carries
   RMCTRL_FLAGS_ROUTE_TO_PHYSICAL so the refusal comes from GSP firmware, but
   the call has skip bits (SR_ENTER_SKIP, SKIP_BACKLIGHT_ENABLE) that NVKMS
   clobbers and therefore never sends. If GSP only lacks the PSR or backlight
   sub-step rather than the whole entry point, one of these combinations will
   return something other than 0x56. Read-only in effect: a control that
   returns an error has performed no action. */
static void probe_pre(NvU32 id)
{
	static const char * const nm[4] = {
		"sr=no  bl=no ", "sr=YES bl=no ", "sr=no  bl=YES", "sr=YES bl=YES" };
	NvU32 dir, i;

	pr_info(PFX "=== RUN_PRE flag sweep on displayId 0x%08x ===\n", id);
	for (dir = 0; dir < 2; dir++) {
		for (i = 0; i < 4; i++) {
			PreMuxOps pre;
			NvU32 flags = dir |
				((i & 1) ? 1u << MUX_F_SR_SKIP_SHIFT : 0) |
				((i & 2) ? 1u << MUX_F_SKIP_BACKLIGHT_SHIFT : 0);
			NvV32 st;

			memset(&pre, 0, sizeof pre);
			pre.displayId = id;
			pre.flags = flags;
			pre.iGpuBrightness = clamp_val(brightness, MUX_BRIGHTNESS_MIN,
						       MUX_BRIGHTNESS_MAX);
			st = rm_ctrl(hDisp, CMD_PRE_MUX_OPS, &pre, sizeof pre);
			pr_info(PFX "  PRE  %s %s flags=0x%x -> 0x%x%s preOps=%ums psrEntry=%ums\n",
				dir ? "d->i" : "i->d", nm[i], flags, st,
				(st == 0) ? " OK <== ACCEPTED" :
				(st == 0x56) ? " NOT_SUPPORTED" : " (other)",
				pre.preOpsLatencyMs, pre.psrEntryLatencyMs);
		}
	}
	/* Same sweep for RUN_POST. Its bit 1:1 is SR_EXIT_SKIP, not SR_ENTER_SKIP;
	   nvRmMuxPost() passes the ENTER name for the EXIT field - same bit, so it
	   is only a naming slip, but it is clobbered by the same '=' bug. */
	for (dir = 0; dir < 2; dir++) {
		for (i = 0; i < 4; i++) {
			PostMuxOps post;
			NvU32 flags = dir |
				((i & 1) ? 1u << MUX_F_SR_SKIP_SHIFT : 0) |
				((i & 2) ? 1u << MUX_F_SKIP_BACKLIGHT_SHIFT : 0);
			NvV32 st;

			memset(&post, 0, sizeof post);
			post.displayId = id;
			post.flags = flags;
			st = rm_ctrl(hDisp, CMD_POST_MUX_OPS, &post, sizeof post);
			pr_info(PFX "  POST %s %s flags=0x%x -> 0x%x%s postOps=%ums psrExit=%ums exitToInactive=%ums\n",
				dir ? "d->i" : "i->d", nm[i], flags, st,
				(st == 0) ? " OK" :
				(st == 0x56) ? " NOT_SUPPORTED" : " (other)",
				post.postOpsLatencyMs, post.psrExitLatencyMs,
				post.psrExitTransitionToInactiveLatencyMs);
		}
	}
	pr_info(PFX "=== sweep done ===\n");
}

static ssize_t nvmuxk_write(struct file *f, const char __user *ubuf,
			    size_t len, loff_t *off)
{
	char buf[32] = {0};
	NvU32 id;

	if (len >= sizeof buf) return -EINVAL;
	if (copy_from_user(buf, ubuf, len)) return -EFAULT;
	strim(buf);

	if (!rm_up) return -ENODEV;

	if (!strcmp(buf, "status")) {
		id = find_mux_display();
		if (id) detect_now(id, "status");
		return len;
	}
	if (!strcmp(buf, "probe-pre")) {
		id = find_mux_display();
		if (!id) return -ENODEV;
		if (do_init(id)) pr_warn(PFX "INIT failed; sweeping anyway\n");
		probe_pre(id);
		return len;
	}
	if (!strcmp(buf, "init")) {
		id = find_mux_display();
		if (!id) return -ENODEV;
		do_init(id);
		report(id, "after init");
		return len;
	}
	if (!strcmp(buf, "dgpu")) {
		id = find_mux_display();
		if (!id) return -ENODEV;
		if (do_init(id)) { pr_err(PFX "INIT failed - not switching\n"); return -EIO; }
		run_sequence(id, 0, 0);
		return len;
	}
	/* switch-only variants for the live (session-preserving) path: RUN_PRE is
	   NV_ERR_NOT_SUPPORTED from GSP anyway, and skipping both keeps the time
	   the panel is unowned to the ~100ms SWITCH_DISP_MUX takes. */
	if (!strcmp(buf, "sw-dgpu") || !strcmp(buf, "sw-igpu")) {
		NvU32 dir = !strcmp(buf, "sw-igpu");
		id = find_mux_display();
		if (!id) return -ENODEV;
		if (do_init(id)) {
			if (!dir) { pr_err(PFX "INIT failed - not switching\n"); return -EIO; }
			pr_warn(PFX "INIT failed on restore; switching anyway\n");
		}
		run_sequence(id, dir, 1);
		return len;
	}
	if (!strcmp(buf, "seq") || !strcmp(buf, "igpu")) {
		id = find_mux_display();
		if (!id) {
			pr_err(PFX "GET_MUX_STATUS fails on every display.\n");
			pr_err(PFX "If it worked earlier this boot, GSP's mux state is wedged\n");
			pr_err(PFX "(a failed INIT_MUX_DATA does this and it survives rmmod).\n");
			pr_err(PFX "REBOOT, then run 'init' before anything else.\n");
			return -ENODEV;
		}
		if (!strcmp(buf, "igpu")) {
			/* restore path: try INIT but proceed regardless - getting the
			   panel back matters more than a clean init */
			if (do_init(id))
				pr_warn(PFX "INIT failed on restore; switching anyway\n");
			run_sequence(id, 1, 0);
			return len;
		}
		/* INIT once, as NVKMS does. The reverse leg must NOT re-init: a
		   failure there could strand the panel on the dGPU. */
		if (do_init(id)) { pr_err(PFX "INIT failed - not switching\n"); return -EIO; }
		run_sequence(id, 0, 0);
		pr_info(PFX "holding 15s before reverting...\n");
		msleep(15000);
		run_sequence(id, 1, 0);
		pr_info(PFX "=== reverted ===\n");
		return len;
	}
	pr_info(PFX "commands: status | init | probe-pre | dgpu | igpu | seq | sw-dgpu | sw-igpu\n");
	return len;
}

static const struct proc_ops nvmuxk_ops = { .proc_write = nvmuxk_write };

static int __init nvmuxk_init(void)
{
	int rc = rm_setup();
	if (rc) return rc;
	if (!proc_create("nvmuxk", 0200, NULL, &nvmuxk_ops)) {
		rm_teardown(); return -ENOMEM; }
	pr_info(PFX "loaded. echo status|init|seq|igpu > /proc/nvmuxk\n");
	return 0;
}

static void __exit nvmuxk_exit(void)
{
	remove_proc_entry("nvmuxk", NULL);
	rm_teardown();
	pr_info(PFX "unloaded\n");
}

module_init(nvmuxk_init);
module_exit(nvmuxk_exit);
