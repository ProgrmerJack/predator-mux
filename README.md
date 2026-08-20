# predator-mux

Runtime display-MUX switching for Acer Predator laptops on Linux: move the
internal panel between the Intel iGPU and the NVIDIA dGPU **without restarting
your session**.

Measured on the development machine: **~1.1 s**, applications untouched,
compositor process unchanged.

```
$ sudo mux-switch dgpu
dark window: 1.04s
brightness matched: 16% -> nvidia_0 (16/100)
OK: panel is on dgpu (card0-eDP-2 connected)
SESSION PRESERVED: kwin pid unchanged (8713)
```

## Why this exists

Every publicly available Linux MUX tool requires a reboot or a full session
restart. `supergfxctl` states that a reboot is always required for the ASUS MUX
because of how it works in ACPI. `vga_switcheroo` predates dynamic muxes and
cannot switch while a modesetting client holds the GPU. NVIDIA proposed a DRM
uAPI for dynamic mux switching in 2022; it has not landed.

This switches the panel while the desktop keeps running.

## Status and scope

Developed and verified on **one machine**: an Acer Predator PHN16S-71
(Intel Arrow Lake-S + RTX 5070 Laptop, BIOS V1.26, PI3DPX8121 mux), Linux 7.1.8,
NVIDIA 610.57.04 open kernel modules, KDE Plasma 6.7.4 on Wayland.

It drives NVIDIA's Resource Manager at kernel privilege. On hardware it does not
understand, it can leave the display dark until reboot.

**Run `mux-preflight` first.** It refuses rather than guesses. A failed
`INIT_MUX_DATA` wedges GSP's mux state: every subsequent `GET_DISP_MUX_STATUS`
returns `NV_ERR_NOT_SUPPORTED`, and that survives unloading the module. Only a
reboot clears it. `INIT_MUX_DATA` also needs the panel's real EDID manufacturer
and product ids, which differ per panel, so preflight reads the EDID and refuses
if it does not match what the module would send.

Keep a way back in: a second TTY, SSH, or a filesystem snapshot.

## Requirements

- A laptop whose internal panel is wired to **both** GPUs (an eDP connector
  appears on each DRM card) and whose firmware reports mux mode `DYNAMIC`
- NVIDIA open kernel modules exporting `nvidia_get_rm_ops`
- Kernel build tree for the running kernel
- For session-preserving switching, a compositor that survives losing all
  outputs and re-probes on udev `change`. Verified on KWin 6.7.4.

## Install

```sh
git clone https://github.com/ProgrmerJack/predator-mux
cd predator-mux
make -C kernel
sudo ./bin/mux-preflight        # must print PASS
```

## Use

```sh
sudo ./bin/mux-switch dgpu      # panel -> NVIDIA, session preserved
sudo ./bin/mux-switch igpu      # panel -> Intel
sudo ./bin/mux-switch           # toggle
```

Both directions auto-revert if the panel does not come back, and fall back to a
compositor restart only if that also fails. Logs go to `/var/log/predator-mux`.

To render on the dGPU as well as scan out from it, use the restart-based
variants (see Limitations):

```sh
sudo ./bin/mux-to-dgpu          # switch, then restart the session on the dGPU
sudo ./bin/mux-to-igpu          # switch back, restart on the iGPU
```

## How it works

1. `nvmuxk`, an out-of-tree module, reaches NVIDIA's RM at kernel privilege
   through the exported `nvidia_get_rm_ops`. The mux controls are
   `RMCTRL_FLAGS_KERNEL_PRIVILEGED` and unreachable from userspace at any uid.
2. It runs NVIDIA's own sequence: `SET_ACPI_ID_MAPPING`, `INIT_MUX_DATA`,
   `RUN_PRE_DISP_MUX_OPERATIONS`, `SWITCH_DISP_MUX`, `RUN_POST_DISP_MUX_OPERATIONS`.
3. Around that, `mux-switch` makes the current owner release the panel and the
   new owner pick it up, driving the compositor with synthetic udev events.

The session survives because KWin substitutes a virtual `PlaceholderOutput` when
it has no real outputs and drops it when one returns, so the interval where
neither GPU owns the panel is a case it already handles.

## Findings

**`RUN_PRE_DISP_MUX_OPERATIONS` is not unsupported; NVIDIA's driver cannot reach
the working call.** With default flags it returns `NV_ERR_NOT_SUPPORTED` (0x56),
which reads like a missing firmware feature. Sweeping the documented flag space
shows it succeeds in both directions whenever `SR_ENTER_SKIP=YES`; only the
PSR-entry sub-step is refused. That combination is unreachable from NVKMS,
because `nvRmMuxPre()` in `nvkms-rm.c` does:

```c
params.flags = DRF_DEF(0073_CTRL_DFP, _DISP_MUX_FLAGS, _SR_ENTER_SKIP, _NO);
if (state == MUX_STATE_DISCRETE) {
    params.flags = NV0073_CTRL_DFP_DISP_MUX_FLAGS_SWITCH_TYPE_IGPU_TO_DGPU;  /* '=' not '|=' */
```

The skip bit is overwritten immediately after being set. Both constants are `0`,
so it is numerically harmless, but the flag is dead code upstream.
`nvRmMuxPost()` repeats the pattern and additionally passes the `SR_ENTER_SKIP`
name for the `SR_EXIT_SKIP` field.

**Writing a DRM connector's sysfs `status` does not notify userspace.**
`drm_sysfs.c status_store()` sets `connector->force` and re-runs `fill_modes()`;
it never calls `drm_kms_helper_hotplug_event()`. Follow it with
`udevadm trigger --action=change /sys/class/drm/cardN`, or the compositor never
learns anything happened.

**Mux state is not connector state.** `SWITCH_DISP_MUX` returning `NV_OK` with
`state=DISCRETE_GPU` does not mean the DRM connector came up. They are separate
layers and must be verified separately.

**NVKMS ships a manual mux API that is dead on almost all hardware.**
`NVKMS_IOCTL_SWITCH_MUX` requires the display to be in `pDispEvo->muxDisplays`,
which `MuxInit()` populates only through a hardcoded ACPI-id table holding a
single entry (`acpiId = 0x8001a420`). NVIDIA's own comment describes it as "a
poor-man's alternative to the WDDM driver's
`CDisplayMgr::NVInitializeACPIToDeviceMaskMap()`".

## Limitations

- **A live switch moves scanout, not rendering.** KWin binds its render device
  to `primaryGpu()->renderDevice()` at backend construction
  (`drm_egl_backend.cpp`), and `primaryGpu()` is `m_gpus.front()`, fixed at
  init. Outputs on any other GPU get a `MultiGpuSwapchain` copy. The
  restart-based scripts avoid this by making the dGPU primary through
  `KWIN_DRM_DEVICES`. Plasma 6.8 removes the copy (KWin MR !7101, dma-buf v6).
- **The panel blanks for roughly a second.** A glitch-free handoff needs the
  panel held in PSR across the switch. i915 tears PSR down on the CRTC-disable
  path (`intel_psr_disable()`), and `intel_psr_pause()` calls `intel_psr_exit()`.
  There is no in-tree way to hand a panel to another driver while it
  self-refreshes; that is what the unlanded DRM mux uAPI was for.
- Wayland and KWin only. Other compositors are untested.
- Panel EDID ids are compile-time defaults. Other panels must pass `manfid=`
  and `productid=`; preflight prints the correct values.

## Author

Abduxoliq Ashuraliyev, Independent Researcher, Tashkent, Uzbekistan
ORCID [0009-0003-5482-5526](https://orcid.org/0009-0003-5482-5526)

## Licence

`kernel/nvmuxk.c` is `Dual MIT/GPL`. Everything else is MIT. See `LICENSE`.

Not affiliated with, endorsed by, or supported by NVIDIA or Acer.
