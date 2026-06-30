// SPDX-License-Identifier: BSD-3-Clause

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>

enum nvme_backend {
	NVME_BACKEND_SYSFS = 0,
	NVME_BACKEND_VFIO,
};

struct rte {
	struct hostmem_config config;
	struct hostmem_heap heap;
	struct cudamem_config cuda_config;
	struct cudamem_heap cuda_heap;
	CUcontext cu_ctx;
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_qpair ioq;
	struct vfio_ctx vfio;
	struct upcie_dmabuf_import cuda_import;
	uint64_t *cuda_heap_phys_lut_orig;
	uint64_t *cuda_heap_phys_lut_imported;
	int importer_fd;
	enum nvme_backend backend;
	int cuda_heap_imported;
};

static int
import_cuda_heap_for_nvme(struct nvme *nvme, const char *bdf, struct cudamem_heap *heap)
{
	int err;

	if (!nvme || !bdf || !heap || !heap->phys_lut || heap->dmabuf.fd < 0) {
		return -EINVAL;
	}

	nvme->importer_fd = upcie_dmabuf_importer_open();
	if (nvme->importer_fd < 0) {
		return nvme->importer_fd;
	}

	nvme->cuda_heap_phys_lut_imported = calloc(heap->nphys, sizeof(*nvme->cuda_heap_phys_lut_imported));
	if (!nvme->cuda_heap_phys_lut_imported) {
		err = -errno;
		printf("FAILED: calloc(cuda_heap_phys_lut_imported); err(%d)\n", err);
		goto error;
	}

	err = upcie_dmabuf_importer_map(nvme->importer_fd, bdf, heap->dmabuf.fd,
					heap->config->device_pagesize, heap->nphys,
					nvme->cuda_heap_phys_lut_imported, &nvme->cuda_import);
	if (err) {
		printf("FAILED: upcie_dmabuf_importer_map(); err(%d)\n", err);
		goto error;
	}

	nvme->cuda_heap_phys_lut_orig = heap->phys_lut;
	heap->phys_lut = nvme->cuda_heap_phys_lut_imported;
	nvme->cuda_heap_imported = 1;

	return 0;

error:
	free(nvme->cuda_heap_phys_lut_imported);
	nvme->cuda_heap_phys_lut_imported = NULL;
	if (nvme->importer_fd >= 0) {
		upcie_dmabuf_importer_close(nvme->importer_fd);
		nvme->importer_fd = -1;
	}
	return err;
}

static void
unimport_cuda_heap_for_nvme(struct nvme *nvme, struct cudamem_heap *heap)
{
	if (!nvme || !heap) {
		return;
	}

	if (nvme->cuda_heap_imported) {
		heap->phys_lut = nvme->cuda_heap_phys_lut_orig;
		nvme->cuda_heap_phys_lut_orig = NULL;
		nvme->cuda_heap_imported = 0;
	}

	if (nvme->cuda_import.map_handle) {
		int err = upcie_dmabuf_importer_unmap(&nvme->cuda_import);

		if (err) {
			printf("FAILED: upcie_dmabuf_importer_unmap(); err(%d)\n", err);
		}
		memset(&nvme->cuda_import, 0, sizeof(nvme->cuda_import));
	}

	free(nvme->cuda_heap_phys_lut_imported);
	nvme->cuda_heap_phys_lut_imported = NULL;

	if (nvme->importer_fd >= 0) {
		int err = upcie_dmabuf_importer_close(nvme->importer_fd);

		if (err) {
			printf("FAILED: upcie_dmabuf_importer_close(); err(%d)\n", err);
		}
		nvme->importer_fd = -1;
	}
}

static void
nvme_cleanup(struct nvme *nvme, struct rte *rte)
{
	if (nvme->ioq.rpool) {
		nvme_qpair_term(&nvme->ioq);
		memset(&nvme->ioq, 0, sizeof(nvme->ioq));
	}

	if (nvme->backend == NVME_BACKEND_VFIO) {
		unimport_cuda_heap_for_nvme(nvme, &rte->cuda_heap);

		nvme_controller_close_vfio(&nvme->ctrlr, &nvme->vfio);
		return;
	}

	nvme_controller_close(&nvme->ctrlr);
}

static int
nvme_open_with_vfio_cudamem(struct nvme *nvme, const char *bdf, struct rte *rte)
{
	int err;

	nvme->backend = NVME_BACKEND_VFIO;

	err = nvme_controller_open_vfio(&nvme->ctrlr, &nvme->vfio, bdf, &rte->heap);
	if (err) {
		return err;
	}

	err = import_cuda_heap_for_nvme(nvme, bdf, &rte->cuda_heap);
	if (err) {
		nvme_cleanup(nvme, rte);
		return err;
	}

	return 0;
}

int
rte_init(struct rte *rte)
{
	CUdevice cu_dev;
	int err;
	
	err = hostmem_config_init(&rte->config);
	if (err) {
		printf("FAILED: hostmem_config_init(); err(%d)\n", err);
		return err;
	}

	err = hostmem_heap_init(&rte->heap, 1024 * 1024 * 128ULL, &rte->config);
	if (err) {
		printf("FAILED: hostmem_heap_init(); err(%d)\n", err);
		return err;
	}
	
	err = cuInit(0);
	if (err) {
		printf("FAILED: cuInit(); err(%d)\n", err);
		return err;
	}

	err = cuDeviceGet(&cu_dev, 0); // GPU ID 0
	if (err) {
		printf("FAILED: cuDeviceGet(); err(%d)\n", err);
		return err;
	}

	err = cuCtxCreate(&rte->cu_ctx, 0, cu_dev);
	if (err) {
		printf("FAILED: cuCtxCreate(); err(%d)\n", err);
		return err;
	}

	err = cudamem_config_init(&rte->cuda_config, 0);
	if (err) {
		printf("FAILED: cudamem_config_init(); err(%d)\n", err);
		return err;
	}

	err = cudamem_heap_init(&rte->cuda_heap, 1024 * 1024 * 128ULL, &rte->cuda_config);
	if (err) {
		printf("FAILED: cudamem_heap_init(); err(%d)\n", err);
		return err;
	}

	return 0;
}

int
nvme_io(struct nvme *nvme, struct cudamem_heap *cuda_heap, uint8_t opc, void *buffer, size_t buffer_size)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	struct nvme_request *req;
	uint8_t sc, sct;
	int err;

	req = nvme_request_alloc(nvme->ioq.rpool);
	if (!req) {
		err = errno;
		printf("FAILED: nvme_request_alloc(); err(%d)\n", err);
		return err;
	}
	cmd.cid = req->cid;
	cmd.nsid = 1;
	cmd.opc = opc;
	cmd.cdw10 = 0; ///< SLBA == 0
	cmd.cdw12 = 0; ///< NLB == 0

	nvme_request_prep_command_prps_contig_cuda(req, cuda_heap, buffer, buffer_size, &cmd);

	err = nvme_qpair_enqueue(&nvme->ioq, &cmd);
	if (err) {
		nvme_request_free(nvme->ioq.rpool, req->cid);
		printf("FAILED: nvme_qpair_enqueue(); err(%d)\n", err);
		return err;
	}

	nvme_qpair_sqdb_update(&nvme->ioq);

	err = nvme_qpair_reap_cpl(&nvme->ioq, nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		printf("FAILED: nvme_qpair_reap_cpl(); err(%d)\n", err);
		return err;
	}

	nvme_request_free(nvme->ioq.rpool, cpl.cid);

	sc = (cpl.status & 0x1FE) >> 1;
	sct = (cpl.status & 0xE00) >> 8;
	if (sc) {
		printf("FAILED: Status Code Type(0x%x), Status Code(0x%x)\n", sct, sc);
		err = EIO;
	}

	return err;
}

int
nvme_init(struct nvme *nvme, const char *bdf, struct rte *rte)
{
	char driver_name[NAME_MAX + 1] = {0};
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	int err;

	err = pci_device_get_driver_name(bdf, driver_name, sizeof(driver_name));
	if (err) {
		printf("FAILED: pci_device_get_driver_name(); err(%d)\n", err);
		return err;
	}
	printf("NVMe DEBUG: bdf=%s driver='%s'\n", bdf, driver_name);

	if (!strcmp(driver_name, "vfio-pci")) {
		err = nvme_open_with_vfio_cudamem(nvme, bdf, rte);
	} else if (!strcmp(driver_name, "uio_pci_generic")) {
		nvme->backend = NVME_BACKEND_SYSFS;
		err = nvme_controller_open(&nvme->ctrlr, bdf, &rte->heap);
	} else {
		printf("FAILED: unsupported driver '%s'\n", driver_name);
		return -ENOTSUP;
	}

	if (err) {
		printf("FAILED: nvme_device_open(); err(%d)\n", err);
		return err;
	}

	cmd.opc = 0x6; ///< IDENTIFY
	cmd.cdw10 = 1; // CNS=1: Identify Controller

	err = nvme_qpair_submit_sync_contig_prps(&nvme->ctrlr.aq, nvme->ctrlr.heap,
						 nvme->ctrlr.buf, 4096, &cmd,
						 nvme->ctrlr.timeout_ms, &cpl);
	if (err) {
		printf("FAILED: nvme_qpair_submit_sync(); err(%d)\n", err);
		nvme_cleanup(nvme, rte);
		return err;
	}

	err = nvme_controller_create_io_qpair(&nvme->ctrlr, &nvme->ioq, 32);
	if (err) {
		printf("FAILED: nvme_device_create_io_qpair(); err(%d)\n", err);
		nvme_cleanup(nvme, rte);
		return err;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme nvme = {
		.importer_fd = -1,
	};
	struct rte rte = {0};
	const size_t buffer_size = 82 * sizeof(char);
	void *write_buf = NULL, *read_buf = NULL;	///< CUDA IO buffers
	char *expected = NULL, *actual = NULL;		///< HOST buffers for comparison
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = rte_init(&rte);
	if (err) {
		printf("FAILED: rte_init(); err(%d)\n", err);
		return err;
	}

	err = nvme_init(&nvme, argv[1], &rte);
	if (err) {
		printf("FAILED: nvme_init(); err(%d)\n", err);
		goto exit;
	}

	write_buf = cudamem_dma_malloc(&rte.cuda_heap, buffer_size);
	if (!write_buf) {
		err = errno;
		printf("FAILED: cudamem_dma_malloc(write_buf); err(%d)\n", err);
		goto exit;
	}

	read_buf = cudamem_dma_malloc(&rte.cuda_heap, buffer_size);
	if (!read_buf) {
		err = errno;
		printf("FAILED: cudamem_dma_malloc(read_buf); err(%d)\n", err);
		goto exit;
	}

	expected = malloc(buffer_size);
	if (!expected) {
		err = errno;
		printf("FAILED: malloc(expected); err(%d)\n", err);
		goto exit;
	}

	actual = malloc(buffer_size);
	if (!actual) {
		err = errno;
		printf("FAILED: malloc(actual); err(%d)\n", err);
		goto exit;
	}

	// Fill buffer with ascii characters
	for (size_t i = 0; i < buffer_size; i++) {
		expected[i] = (i % 26) + 65;
	}
	
	memset(actual, 0, buffer_size);

	err = cuMemcpyHtoD((CUdeviceptr)write_buf, expected, buffer_size);
	if (err) {
		printf("FAILED: cuMemcpyHtoD(expected -> write_buf); err(%d)\n", err);
		goto exit;
	}

	err = cuMemcpyHtoD((CUdeviceptr)read_buf, actual, buffer_size);
	if (err) {
		printf("FAILED: cuMemcpyHtoD(actual -> read_buf); err(%d)\n", err);
		goto exit;
	}

	err = nvme_io(&nvme, &rte.cuda_heap, 0x1, write_buf, buffer_size);
	if (err) {
		printf("FAILED: nvme_io(write); err(%d)\n", err);
		goto exit;
	}

	err = nvme_io(&nvme, &rte.cuda_heap, 0x2, read_buf, buffer_size);
	if (err) {
		printf("FAILED: nvme_io(read); err(%d)\n", err);
		goto exit;
	}

	
	err = cuMemcpyDtoH(actual, (CUdeviceptr)read_buf, buffer_size);
	if (err) {
		printf("FAILED: cuMemcpyDtoH(read_buf -> actual); err(%d)\n", err);
		goto exit;
	}

	for (size_t i = 0; i < buffer_size; i++) {
		if (expected[i] != actual[i]) {
			printf("FAILED: written data != read data\n");
			printf("Wrote: %s\n", expected);
			printf("Read: %s\n", actual);
			goto exit;
		}
	}
	printf("SUCCES: written data == read data\n");

exit:
	cudamem_dma_free(&rte.cuda_heap, write_buf);
	cudamem_dma_free(&rte.cuda_heap, read_buf);
	free(expected);
	free(actual);
	nvme_cleanup(&nvme, &rte);
	hostmem_heap_term(&rte.heap);
	cudamem_heap_term(&rte.cuda_heap);
	cuCtxDestroy(rte.cu_ctx);

	return err;
}
