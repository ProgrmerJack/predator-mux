# Build nvmuxk against a kernel that is not necessarily the running one.
#
# Differences from the original research Makefile, all needed for a build that
# happens somewhere other than the source directory, or for a kernel other than
# the one currently booted (DKMS does both):
#
#   - $(PWD) is the caller's shell cwd, so `make -C kernel` built the wrong
#     path. Use the directory this Makefile actually lives in.
#   - LLVM=1 was hardcoded because the development kernel (CachyOS) is built
#     with clang, and mixing toolchains fails. It is now read from the target
#     kernel's own .config, so gcc-built kernels work and a DKMS build for a
#     different kernel gets that kernel's toolchain rather than the running
#     one's.
#   - nvidia.symvers is generated rather than shipped: its CRC has to match the
#     nvidia.ko being built against or the module is rejected at load on a
#     CONFIG_MODVERSIONS kernel. See gen-symvers.sh.

obj-m := nvmuxk.o

MDIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
SYMVERS := $(MDIR)/nvidia.symvers

# Kernels built with clang must be built against with clang. Ask the target
# kernel, not /proc/version, which describes whatever is booted right now.
LLVM_FLAG := $(shell grep -qs '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config && echo LLVM=1)

all: $(SYMVERS)
	$(MAKE) -C $(KDIR) M=$(MDIR) $(LLVM_FLAG) \
		KBUILD_EXTRA_SYMBOLS=$(SYMVERS) modules

# Regenerated whenever the nvidia module is newer than the file, so a driver
# upgrade cannot leave a stale CRC behind.
$(SYMVERS): $(MDIR)/gen-symvers.sh
	sh $(MDIR)/gen-symvers.sh $(SYMVERS) $(KVER)

symvers:
	sh $(MDIR)/gen-symvers.sh $(SYMVERS) $(KVER)

clean:
	$(MAKE) -C $(KDIR) M=$(MDIR) $(LLVM_FLAG) clean
	rm -f $(SYMVERS)

.PHONY: all clean symvers
