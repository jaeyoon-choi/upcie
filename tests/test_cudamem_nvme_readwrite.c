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
	enum nvme_backend backend;
	int cuda_heap_mapped;
};

static int
vfio_unmap_cudamem_pages(struct vfio_ctx *vfio, struct cudamem_heap *heap, size_t npages)
{
	int err = 0;

	for (size_t i = 0; i < npages; ++i) {
		struct vfio_iommu_type1_dma_unmap unmap = {0};

		unmap.argsz = sizeof(unmap);
		unmap.iova = heap->phys_lut[i];
		unmap.size = heap->config->device_pagesize;

		if (vfio_iommu_unmap_dma(&vfio->container, &unmap) < 0 && !err) {
			err = -errno;
		}
	}

	return err;
}

static int
vfio_map_cudamem_heap(struct vfio_ctx *vfio, struct cudamem_heap *heap)
{
	if (!vfio || !heap || !heap->phys_lut) {
		return -EINVAL;
	}

	for (size_t i = 0; i < heap->nphys; ++i) {
		struct vfio_iommu_type1_dma_map map = {0};

		map.argsz = sizeof(map);
		map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
		map.vaddr = heap->vaddr + (i * heap->config->device_pagesize);
		map.iova = heap->phys_lut[i];
		map.size = heap->config->device_pagesize;

		if (vfio_iommu_map_dma(&vfio->container, &map) < 0) {
			vfio_unmap_cudamem_pages(vfio, heap, i);
			printf("FAILED: vfio_iommu_map_dma(cuda); errno(%d)\n", errno);
			return -errno;
		}
	}

	return 0;
}

static int
vfio_unmap_cudamem_heap(struct vfio_ctx *vfio, struct cudamem_heap *heap)
{
	if (!vfio || !heap || !heap->phys_lut) {
		return -EINVAL;
	}

	return vfio_unmap_cudamem_pages(vfio, heap, heap->nphys);
}

static void
nvme_cleanup(struct nvme *nvme, struct rte *rte)
{
	if (nvme->ioq.rpool) {
		nvme_qpair_term(&nvme->ioq);
		memset(&nvme->ioq, 0, sizeof(nvme->ioq));
	}

	if (nvme->backend == NVME_BACKEND_VFIO) {
		if (nvme->cuda_heap_mapped) {
			vfio_unmap_cudamem_heap(&nvme->vfio, &rte->cuda_heap);
			nvme->cuda_heap_mapped = 0;
		}

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

	err = vfio_map_cudamem_heap(&nvme->vfio, &rte->cuda_heap);
	if (err) {
		nvme_cleanup(nvme, rte);
		return err;
	}

	nvme->cuda_heap_mapped = 1;

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
	struct nvme nvme = {0};
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
