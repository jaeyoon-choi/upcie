// SPDX-License-Identifier: BSD-3-Clause

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>

int
main(int argc, char **argv)
{
	struct upcie_dmabuf_import mapping = {0};
	struct cudamem_config config = {0};
	struct cudamem_heap heap = {0};
	CUcontext cu_ctx;
	CUdevice cu_dev;
	uint64_t *lut = NULL;
	int importer_fd = -1;
	int err;

	if (argc < 2) {
		printf("usage: %s <NVMe BDF>\n", argv[0]);
		return EINVAL;
	}

	err = cuInit(0);
	if (err) {
		printf("FAILED: cuInit(); err(%d)\n", err);
		return err;
	}

	err = cuDeviceGet(&cu_dev, 0);
	if (err) {
		printf("FAILED: cuDeviceGet(); err(%d)\n", err);
		return err;
	}

	err = cuCtxCreate(&cu_ctx, 0, cu_dev);
	if (err) {
		printf("FAILED: cuCtxCreate(); err(%d)\n", err);
		return err;
	}

	err = cudamem_config_init(&config, 0);
	if (err) {
		printf("FAILED: cudamem_config_init(); err(%d)\n", err);
		goto out_ctx;
	}

	err = cudamem_heap_init(&heap, 8 * config.device_pagesize, &config);
	if (err) {
		printf("FAILED: cudamem_heap_init(); err(%d)\n", err);
		goto out_ctx;
	}

	lut = calloc(heap.nphys, sizeof(*lut));
	if (!lut) {
		err = errno;
		printf("FAILED: calloc(lut); errno(%d)\n", err);
		goto out_heap;
	}

	importer_fd = upcie_dmabuf_importer_open();
	if (importer_fd < 0) {
		err = -importer_fd;
		printf("FAILED: upcie_dmabuf_importer_open(); err(%d)\n", err);
		goto out_lut;
	}

	err = upcie_dmabuf_importer_map(importer_fd, argv[1], heap.dmabuf.fd,
					 heap.config->device_pagesize, heap.nphys, lut,
					 &mapping);
	if (err) {
		printf("FAILED: upcie_dmabuf_importer_map(); err(%d)\n", err);
		goto out_importer;
	}

	printf("imported LUT:\n");
	printf("  nphys: %u\n", mapping.nphys);
	for (uint32_t i = 0; i < mapping.nphys; ++i)
		printf("  - 0x%" PRIx64 "\n", lut[i]);

	err = upcie_dmabuf_importer_unmap(&mapping);
	if (err) {
		printf("FAILED: upcie_dmabuf_importer_unmap(); err(%d)\n", err);
		goto out_importer;
	}

out_importer:
	if (importer_fd >= 0)
		upcie_dmabuf_importer_close(importer_fd);
out_lut:
	free(lut);
out_heap:
	cudamem_heap_term(&heap);
out_ctx:
	cuCtxDestroy(cu_ctx);

	return err;
}
