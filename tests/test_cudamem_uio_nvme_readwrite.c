// SPDX-License-Identifier: BSD-3-Clause

/**
 * UIO control: NVMe ↔ GPU P2P with NO IOMMU translation
 * =====================================================
 *
 * Counterpart to test_cudamem_iommu_map_nvme_readwrite.c. Here the NVMe is
 * controlled via the sysfs/UIO path (uio_pci_generic), so DMA uses raw
 * physical/bus addresses with NO IOMMU translation. PRPs are built directly
 * from cuda_heap.phys_lut (GPU BAR1 bus addresses), exactly as the original
 * working UIO path did.
 *
 * Goal: determine whether P2P *read from GPU* (NVMe WRITE command = NVMe reads
 * GPU memory) works when the IOMMU is out of the path.
 *   - If GPU->disk WRITE succeeds here but failed under VFIO  => the failure is
 *     IOMMU-translation-specific.
 *   - If GPU->disk WRITE fails here too                       => the root
 *     complex fundamentally does not forward non-posted P2P reads between these
 *     root ports (translation-independent).
 *
 * Prereqs (run as root before this test):
 *   - NVMe bound to uio_pci_generic
 *   - bus mastering enabled on the NVMe (setpci COMMAND bit 2)
 */

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>

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
};

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
	printf("DEBUG: opc=0x%x prp1=0x%" PRIx64 " (raw GPU bus addr, no translation)\n",
	       opc, (uint64_t)cmd.prp1);

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
rte_init(struct rte *rte)
{
	CUdevice cu_dev;
	int err;

	err = hostmem_config_init(&rte->config);
	if (err) { printf("FAILED: hostmem_config_init(); err(%d)\n", err); return err; }
	err = hostmem_heap_init(&rte->heap, 1024 * 1024 * 128ULL, &rte->config);
	if (err) { printf("FAILED: hostmem_heap_init(); err(%d)\n", err); return err; }
	err = cuInit(0);
	if (err) { printf("FAILED: cuInit(); err(%d)\n", err); return err; }
	err = cuDeviceGet(&cu_dev, 0);
	if (err) { printf("FAILED: cuDeviceGet(); err(%d)\n", err); return err; }
	err = cuCtxCreate(&rte->cu_ctx, 0, cu_dev);
	if (err) { printf("FAILED: cuCtxCreate(); err(%d)\n", err); return err; }
	err = cudamem_config_init(&rte->cuda_config, 0);
	if (err) { printf("FAILED: cudamem_config_init(); err(%d)\n", err); return err; }
	err = cudamem_heap_init(&rte->cuda_heap, 1024 * 1024 * 128ULL, &rte->cuda_config);
	if (err) { printf("FAILED: cudamem_heap_init(); err(%d)\n", err); return err; }
	return 0;
}

int
nvme_init(struct nvme *nvme, const char *bdf, struct rte *rte)
{
	struct nvme_completion cpl = {0};
	struct nvme_command cmd = {0};
	int err;

	err = nvme_controller_open(&nvme->ctrlr, bdf, &rte->heap);
	if (err) { printf("FAILED: nvme_controller_open(); err(%d)\n", err); return err; }

	cmd.opc = 0x6; cmd.cdw10 = 1; /* IDENTIFY controller */
	err = nvme_qpair_submit_sync_contig_prps(&nvme->ctrlr.aq, nvme->ctrlr.heap,
						 nvme->ctrlr.buf, 4096, &cmd,
						 nvme->ctrlr.timeout_ms, &cpl);
	if (err) { printf("FAILED: nvme_qpair_submit_sync(); err(%d)\n", err); return err; }

	err = nvme_controller_create_io_qpair(&nvme->ctrlr, &nvme->ioq, 32);
	if (err) { printf("FAILED: nvme_controller_create_io_qpair(); err(%d)\n", err); return err; }
	return 0;
}

int
main(int argc, char **argv)
{
	struct nvme nvme = {0};
	struct rte rte = {0};
	const size_t buffer_size = 82;
	void *write_buf, *read_buf;
	char *expected, *actual;
	size_t mism;
	int err, werr;

	if (argc != 2) { printf("Usage: %s <PCI-BDF>\n", argv[0]); return 1; }

	err = rte_init(&rte);
	if (err) { printf("FAILED: rte_init(); err(%d)\n", err); return err; }
	err = nvme_init(&nvme, argv[1], &rte);
	if (err) { printf("FAILED: nvme_init(); err(%d)\n", err); return err; }

	write_buf = cudamem_dma_malloc(&rte.cuda_heap, buffer_size);
	read_buf = cudamem_dma_malloc(&rte.cuda_heap, buffer_size);
	expected = malloc(buffer_size);
	actual = malloc(buffer_size);
	if (!write_buf || !read_buf || !expected || !actual) {
		printf("FAILED: alloc\n"); err = ENOMEM; goto exit;
	}

	for (size_t i = 0; i < buffer_size; i++)
		expected[i] = (i % 26) + 'A';
	cuMemcpyHtoD((CUdeviceptr)write_buf, expected, buffer_size);

	/* [1] GPU -> disk WRITE = NVMe reads GPU = P2P READ from GPU  (THE TEST) */
	printf("\n[1] GPU->disk WRITE (P2P read from GPU, no IOMMU)\n");
	werr = nvme_io(&nvme, &rte.cuda_heap, 0x1, write_buf, buffer_size);
	printf("    => GPU->disk WRITE %s\n", werr ? "FAILED" : "OK");

	/* [2] disk -> GPU READ = NVMe writes GPU = P2P WRITE to GPU, verify */
	memset(actual, 0x5A, buffer_size);
	cuMemcpyHtoD((CUdeviceptr)read_buf, actual, buffer_size);
	printf("\n[2] disk->GPU READ (P2P write to GPU, no IOMMU)\n");
	err = nvme_io(&nvme, &rte.cuda_heap, 0x2, read_buf, buffer_size);
	printf("    => disk->GPU READ %s\n", err ? "FAILED" : "OK");
	if (!err) {
		memset(actual, 0, buffer_size);
		cuMemcpyDtoH(actual, (CUdeviceptr)read_buf, buffer_size);
		mism = 0;
		for (size_t i = 0; i < buffer_size; i++)
			if (actual[i] != expected[i]) mism++;
		printf("    => read-back vs written: %zu/%zu mismatch\n", mism, buffer_size);
	}

	printf("\n[RESULT] UIO  GPU->disk(P2P read)=%s  disk->GPU(P2P write)=%s\n",
	       werr ? "FAIL" : "OK", err ? "FAIL" : "OK");
	err = 0;

exit:
	if (write_buf) cudamem_dma_free(&rte.cuda_heap, write_buf);
	if (read_buf) cudamem_dma_free(&rte.cuda_heap, read_buf);
	free(expected);
	free(actual);
	nvme_controller_close(&nvme.ctrlr);
	hostmem_heap_term(&rte.heap);
	cudamem_heap_term(&rte.cuda_heap);
	cuCtxDestroy(rte.cu_ctx);
	return err;
}
