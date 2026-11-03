# Out-of-tree build for the dmabuf_import module.
#
#   make            # build dmabuf_import.ko against the running kernel
#   make KDIR=/path/to/kernel/build   # or a specific kernel tree
#   make clean
#
# DKMS calls this via dkms.conf (MAKE[0]); it passes KDIR as
# ${kernel_source_dir}.

obj-m += dmabuf_import.o

# The DMA-buf mapping types series renames the importer entry points. Detect
# it by one of the new names, since mainline has <linux/dma-buf-mapping.h> too.
ccflags-y += $(if $(shell grep -s dma_buf_sgt_dynamic_attach $(srctree)/include/linux/dma-buf.h),-DDMABUF_IMPORT_MAPPING_TYPES)

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
