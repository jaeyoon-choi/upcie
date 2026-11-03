# Out-of-tree build for the experimental gpu_gmem_provider module (PoC).
# See ../README.md for the kernel and the NVIDIA build it needs.
#
#   make KDIR=/path/to/kernel NVSYMVERS=/path/to/nvidia/Module.symvers
#   make clean
#
# The module calls nv_dma_buf_get_bar1_ranges() from a patched nvidia.ko,
# so it needs that build's Module.symvers and is not part of the DKMS package.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

obj-m += gpu_gmem_provider.o

.PHONY: all clean

all:
	@test -n "$(NVSYMVERS)" || { echo "set NVSYMVERS to the Module.symvers of the patched NVIDIA build (see ../README.md)"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_EXTRA_SYMBOLS=$(NVSYMVERS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
