KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

obj-m += upcie_iommu_map.o

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
