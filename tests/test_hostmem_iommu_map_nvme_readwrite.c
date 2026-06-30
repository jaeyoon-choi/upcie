// SPDX-License-Identifier: BSD-3-Clause

/**
 * Control experiment: host RAM mapped at a high IOVA
 * ==============================================================
 *
 * This mirrors test_cudamem_iommu_map_nvme_readwrite.c exactly, except the
 * data buffer is ordinary host RAM instead of GPU memory. We take the buffer's
 * REAL physical address and iommu_map it into the VFIO domain at the SAME high
 * IOVA window, then run NVMe I/O through that IOVA.
 *
 * Purpose: isolate "our iommu_map mechanism + high IOVA + IOMMU translation"
 * from "the translation output being a peer device BAR (GPU P2P)".
 *
 *   - If this SUCCEEDS while the GPU version fails, that is direct evidence
 *     that translated requests reach system memory fine, and the GPU failure
 *     is specifically because the translation output (a peer BAR) is not
 *     routed to the peer on this platform.
 *   - If this also FAILS, the problem is in our mechanism / high IOVA, not P2P.
 */

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

/* Same lab-only window as the CUDA test. */
#define UPCIE_TEST_GPU_IOVA_BASE (256ULL << 30)

struct rte {
	struct hostmem_config config;
	struct hostmem_heap heap;	///< controller heap (mapped iova==phys by VFIO)
	struct hostmem_heap data;	///< data buffers, mapped at high IOVA by us
};

struct nvme {
	struct nvme_controller ctrlr;
	struct nvme_qpair ioq;
	struct vfio_ctx vfio;
	int importer_fd;
	uint64_t iommu_handle;
};

static int
iommu_map_data_heap(struct nvme *nvme, const char *bdf, struct hostmem_heap *data)
{
	uint32_t page_size = data->config->hugepgsz;
	int err;

	nvme->importer_fd = upcie_iommu_map_open();
	if (nvme->importer_fd < 0) {
		printf("FAILED: upcie_iommu_map_open(); err(%d)\n", nvme->importer_fd);
		return nvme->importer_fd;
	}

	/* Map the REAL host-RAM physical addresses at the high IOVA window. */
	err = upcie_iommu_map_add(nvme->importer_fd, bdf, -1,
					      UPCIE_TEST_GPU_IOVA_BASE, page_size,
					      data->nphys, data->phys_lut,
					      UPCIE_IOMMU_MAP_PROT_READ |
					      UPCIE_IOMMU_MAP_PROT_WRITE |
					      UPCIE_IOMMU_MAP_PROT_CACHE,
					      &nvme->iommu_handle);
	if (err) {
		printf("FAILED: upcie_iommu_map_add(); err(%d)\n", err);
		upcie_iommu_map_close(nvme->importer_fd);
		nvme->importer_fd = -1;
		return err;
	}

	return 0;
}

static void
nvme_cleanup(struct nvme *nvme)
{
	if (nvme->ioq.rpool) {
		nvme_qpair_term(&nvme->ioq);
		memset(&nvme->ioq, 0, sizeof(nvme->ioq));
	}

	if (nvme->iommu_handle) {
		upcie_iommu_map_del(nvme->importer_fd, nvme->iommu_handle);
		nvme->iommu_handle = 0;
	}
	if (nvme->importer_fd >= 0) {
		upcie_iommu_map_close(nvme->importer_fd);
		nvme->importer_fd = -1;
	}

	nvme_controller_close_vfio(&nvme->ctrlr, &nvme->vfio);
}

int
rte_init(struct rte *rte)
{
	int err;

	err = hostmem_config_init(&rte->config);
	if (err) {
		printf("FAILED: hostmem_config_init(); err(%d)\n", err);
		return err;
	}

	err = hostmem_heap_init(&rte->heap, 1024 * 1024 * 128ULL, &rte->config);
	if (err) {
		printf("FAILED: hostmem_heap_init(ctrlr); err(%d)\n", err);
		return err;
	}

	/* Two hugepages: page 0 = write buffer, page 1 = read buffer. */
	err = hostmem_heap_init(&rte->data, 2ULL * rte->config.hugepgsz, &rte->config);
	if (err) {
		printf("FAILED: hostmem_heap_init(data); err(%d)\n", err);
		return err;
	}

	return 0;
}

/* Issue an NVMe I/O whose data buffer is at the given IOVA (single page, <= page). */
int
nvme_io(struct nvme *nvme, uint8_t opc, uint64_t iova)
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
	cmd.cdw12 = 0; ///< NLB == 0 (1 block)
	cmd.prp1 = iova;
	cmd.prp2 = 0;

	printf("DEBUG: opc=0x%x prp1=0x%" PRIx64 "\n", opc, (uint64_t)cmd.prp1);

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

	err = iommu_map_data_heap(nvme, bdf, &rte->data);
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
		nvme_cleanup(nvme);
		return err;
	}

	err = nvme_controller_create_io_qpair(&nvme->ctrlr, &nvme->ioq, 32);
	if (err) {
		printf("FAILED: nvme_controller_create_io_qpair(); err(%d)\n", err);
		nvme_cleanup(nvme);
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
	const size_t buffer_size = 82;
	char *write_buf, *read_buf;
	uint64_t write_iova, read_iova;
	uint32_t hugepgsz;
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

	hugepgsz = rte.config.hugepgsz;
	/* page 0 = write buffer, page 1 = read buffer */
	write_buf = (char *)rte.data.memory.virt;
	read_buf = (char *)rte.data.memory.virt + hugepgsz;
	write_iova = UPCIE_TEST_GPU_IOVA_BASE;
	read_iova = UPCIE_TEST_GPU_IOVA_BASE + hugepgsz;

	for (size_t i = 0; i < buffer_size; i++) {
		write_buf[i] = (i % 26) + 65;
	}
	memset(read_buf, 0, buffer_size);

	err = nvme_io(&nvme, 0x1, write_iova); ///< WRITE: NVMe reads host RAM via high IOVA
	if (err) {
		printf("FAILED: nvme_io(write); err(%d)\n", err);
		goto exit;
	}

	err = nvme_io(&nvme, 0x2, read_iova); ///< READ: NVMe writes host RAM via high IOVA
	if (err) {
		printf("FAILED: nvme_io(read); err(%d)\n", err);
		goto exit;
	}

	if (memcmp(write_buf, read_buf, buffer_size) != 0) {
		printf("FAILED: written data != read data\n");
		err = EIO;
		goto exit;
	}
	printf("SUCCES: host-RAM @ high-IOVA via iommu_map works (write==read)\n");

exit:
	nvme_cleanup(&nvme);
	hostmem_heap_term(&rte.data);
	hostmem_heap_term(&rte.heap);
	return err;
}
