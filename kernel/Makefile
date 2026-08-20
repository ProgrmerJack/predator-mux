# Build nvmuxk against the running kernel.
#
# Differences from the original research Makefile, both needed for a build that
# happens somewhere other than the source directory:
#   - $(PWD) is the caller's shell cwd, so `make -C kernel` built the wrong path.
#     Use the directory this Makefile actually lives in.
#   - LLVM=1 was hardcoded because the development kernel (CachyOS) is built with
#     clang, and mixing toolchains fails. Detect it instead so gcc-built kernels
#     (the common case) also work.

obj-m := nvmuxk.o

MDIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR := /lib/modules/$(shell uname -r)/build

# Kernels built with clang must be built against with clang.
LLVM_FLAG := $(shell grep -q 'clang version' /proc/version && echo LLVM=1)

all:
	$(MAKE) -C $(KDIR) M=$(MDIR) $(LLVM_FLAG) \
		KBUILD_EXTRA_SYMBOLS=$(MDIR)/nvidia.symvers modules

clean:
	$(MAKE) -C $(KDIR) M=$(MDIR) $(LLVM_FLAG) clean

.PHONY: all clean
