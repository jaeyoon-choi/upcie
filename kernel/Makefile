KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

obj-m += upcie_dmabuf_importer.o

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
