// SPDX-License-Identifier: BSD-3-Clause

/**
 * Option-A: direct NVMe<->GPU P2P DMA under VFIO via kernel iommu_map
 * ==================================================================
 *
 * Unlike test_cudamem_nvme_readwrite.c (which fed the importer's per-page LUT
 * straight into PRPs and failed with Data Transfer Error under vfio-pci), this
 * test asks the helper module to insert the already-known GPU device-physical
 * addresses (cuda_heap.phys_lut, obtained via the udmabuf path) directly into
 * the IOMMU domain that the VFIO-controlled NVMe device uses.
 *
 * The module maps:  IOVA_BASE + i*device_pagesize  ->  phys_lut[i]
 * so PRPs must be built from IOVAs, not from phys_lut. We do that by swapping
 * cuda_heap.phys_lut for an IOVA LUT before issuing I/O.
 *
 * IOVA_BASE must not collide with the host-mem VFIO mappings, which use
 * iova == phys (see nvme_controller_vfio.h). A high base avoids RAM/BAR ranges;
 * tune it for your platform's IOMMU aperture (VFIO_IOMMU_GET_INFO iova_ranges).
 */

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>

/* High IOVA window reserved for GPU pages; keep clear of iova==phys host maps. */
#define UPCIE_TEST_GPU_IOVA_BASE (1ULL << 40)

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
	int importer_fd;
	uint64_t iommu_handle;
	uint64_t *phys_lut_orig;
	uint64_t *iova_lut;
};

static int
iommu_map_cuda_heap(struct nvme *nvme, const char *bdf, struct cudamem_heap *heap)
{
	uint32_t page_size = heap->config->device_pagesize;
	int err;

	nvme->importer_fd = upcie_dmabuf_importer_open();
	if (nvme->importer_fd < 0) {
		printf("FAILED: upcie_dmabuf_importer_open(); err(%d)\n", nvme->importer_fd);
		return nvme->importer_fd;
	}

	/* IOVA LUT used for PRPs: iova_lut[i] = IOVA_BASE + i*device_pagesize */
	nvme->iova_lut = calloc(heap->nphys, sizeof(*nvme->iova_lut));
	if (!nvme->iova_lut) {
		err = -errno;
		printf("FAILED: calloc(iova_lut); err(%d)\n", err);
		goto error;
	}
	for (size_t i = 0; i < heap->nphys; ++i) {
		nvme->iova_lut[i] = UPCIE_TEST_GPU_IOVA_BASE + (uint64_t)i * page_size;
	}

	/* Map the true GPU device-physical addresses into NVMe's VFIO domain. */
	err = upcie_dmabuf_importer_iommu_map(nvme->importer_fd, bdf, heap->dmabuf.fd,
					      UPCIE_TEST_GPU_IOVA_BASE, page_size,
					      heap->nphys, heap->phys_lut,
					      UPCIE_DMABUF_PROT_READ | UPCIE_DMABUF_PROT_WRITE,
					      &nvme->iommu_handle);
	if (err) {
		printf("FAILED: upcie_dmabuf_importer_iommu_map(); err(%d)\n", err);
		goto error;
	}

	/* Build PRPs from IOVAs, not physical addresses. */
	nvme->phys_lut_orig = heap->phys_lut;
	heap->phys_lut = nvme->iova_lut;

	return 0;

error:
	free(nvme->iova_lut);
	nvme->iova_lut = NULL;
	if (nvme->importer_fd >= 0) {
		upcie_dmabuf_importer_close(nvme->importer_fd);
		nvme->importer_fd = -1;
	}
	return err;
}

static void
iommu_unmap_cuda_heap(struct nvme *nvme, struct cudamem_heap *heap)
{
	if (nvme->phys_lut_orig) {
		heap->phys_lut = nvme->phys_lut_orig;
		nvme->phys_lut_orig = NULL;
	}

	/* Unmap from the VFIO domain BEFORE the container is torn down. */
	if (nvme->iommu_handle) {
		int err = upcie_dmabuf_importer_unmap_handle(nvme->importer_fd,
							     nvme->iommu_handle);
		if (err) {
			printf("FAILED: upcie_dmabuf_importer_unmap_handle(); err(%d)\n", err);
		}
		nvme->iommu_handle = 0;
	}

	free(nvme->iova_lut);
	nvme->iova_lut = NULL;

	if (nvme->importer_fd >= 0) {
		upcie_dmabuf_importer_close(nvme->importer_fd);
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

	iommu_unmap_cuda_heap(nvme, &rte->cuda_heap);
	nvme_controller_close_vfio(&nvme->ctrlr, &nvme->vfio);
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

	err = cuDeviceGet(&cu_dev, 0);
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
nvme_io(struct nvme *nvme, struct cudamem_heap *cuda_heap, uint8_t opc, void *buffer,
	size_t buffer_size)
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
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	int err;

	err = nvme_controller_open_vfio(&nvme->ctrlr, &nvme->vfio, bdf, &rte->heap);
	if (err) {
		printf("FAILED: nvme_controller_open_vfio(); err(%d)\n", err);
		return err;
	}

	err = iommu_map_cuda_heap(nvme, bdf, &rte->cuda_heap);
	if (err) {
		nvme_controller_close_vfio(&nvme->ctrlr, &nvme->vfio);
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
		printf("FAILED: nvme_controller_create_io_qpair(); err(%d)\n", err);
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
		return err;
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
