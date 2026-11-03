# Build the iommufd GPU mapping PoC tools. See README.md.
#
#   make pal  KHDR=/path/to/pal-kernel/usr/include    # PAL RFC kernel
#   make gmem KHDR=/path/to/gmem-kernel/usr/include   # guest_memfd export kernel
#   make ... CUDA13=1                                  # CUDA 13 and later
#   make clean
#
# KHDR is the uapi header tree of the kernel the tools will run on
# ("make headers_install" in that kernel tree puts it in usr/include).

CUDA ?= /usr/local/cuda
KHDR ?= /usr/include
BUILD := build

CFLAGS := -O2 -Wall -I$(KHDR) -I../dmabuf_import/module -I../iommu_map_pa/module -I$(CUDA)/include
LDLIBS := -L$(CUDA)/lib64/stubs -lcuda
ifeq ($(CUDA13),1)
CFLAGS += -include src/cuda13-compat.h
endif

.PHONY: pal gmem clean

pal: | $(BUILD)
	gcc $(CFLAGS) -o $(BUILD)/pal_nvme src/pal_nvme.c $(LDLIBS)
	gcc $(CFLAGS) -o $(BUILD)/pal_probe src/pal_probe.c $(LDLIBS)
	gcc $(CFLAGS) -Wno-unused-result -o $(BUILD)/bar1_cpu_read src/bar1_cpu_read.c $(LDLIBS)

gmem: | $(BUILD)
	gcc $(CFLAGS) -DPAL_NVME_GMEM -I../gpu_gmem_provider/module -o $(BUILD)/pal_nvme src/pal_nvme.c $(LDLIBS)
	gcc $(CFLAGS) -Wno-unused-result -o $(BUILD)/bar1_cpu_read src/bar1_cpu_read.c $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
