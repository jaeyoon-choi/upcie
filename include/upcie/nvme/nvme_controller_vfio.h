// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jaeyoon Choi <j_yoon.choi@samsung.com>

/**
 * VFIO state needed to access a single NVMe controller from user space.
 */
struct vfio_ctx {
	struct vfio_container container;
	struct vfio_group group;
	int device_fd;
	void *bar0;
	size_t bar0_size;
	int iommu_set;
};

static inline int
nvme_vfio_pci_bus_master_enable(int device_fd)
{
	struct vfio_region_info config = {0};
	uint16_t cmd;
	ssize_t ret;
	int err;

	config.index = VFIO_PCI_CONFIG_REGION_INDEX;
	err = vfio_device_get_region_info(device_fd, &config);
	if (err < 0) {
		return -errno;
	}

	ret = pread(device_fd, &cmd, sizeof(cmd), config.offset + PCI_COMMAND);
	if (ret != (ssize_t)sizeof(cmd)) {
		return ret < 0 ? -errno : -EIO;
	}

	if (!(cmd & PCI_COMMAND_MASTER)) {
		cmd |= PCI_COMMAND_MASTER;
		ret = pwrite(device_fd, &cmd, sizeof(cmd), config.offset + PCI_COMMAND);
		if (ret != (ssize_t)sizeof(cmd)) {
			return ret < 0 ? -errno : -EIO;
		}
	}

	return 0;
}

static inline void
nvme_vfio_ctx_init(struct vfio_ctx *vfio)
{
	memset(vfio, 0, sizeof(*vfio));
	vfio->device_fd = -1;
	vfio->group.fd = -1;
	vfio->container.fd = -1;
}

/**
 * Release VFIO resources and undo DMA mappings created for the heap.
 */
static inline int
nvme_vfio_ctx_close(struct vfio_ctx *vfio, struct hostmem_heap *heap)
{
	int err = 0;

	if (vfio->bar0 && vfio->bar0_size) {
		munmap(vfio->bar0, vfio->bar0_size);
	}

	if (vfio->device_fd >= 0) {
		close(vfio->device_fd);
	}

	if (heap && vfio->iommu_set) {
		for (size_t i = 0; i < heap->nphys; ++i) {
			struct vfio_iommu_type1_dma_unmap unmap = {0};

			unmap.argsz = sizeof(unmap);
			unmap.iova = heap->phys_lut[i];
			unmap.size = heap->config->hugepgsz;

			if (vfio_iommu_unmap_dma(&vfio->container, &unmap) < 0 && !err) {
				err = -errno;
			}
		}
	}

	if (vfio->group.fd >= 0) {
		vfio_group_close(&vfio->group);
	}

	if (vfio->container.fd >= 0) {
		vfio_container_close(&vfio->container);
	}

	nvme_vfio_ctx_init(vfio);

	return err;
}

static inline int
nvme_controller_disable_vfio(const struct nvme_controller *ctrlr, const struct vfio_ctx *vfio)
{
	if (!vfio->bar0) {
		return 0;
	}

	nvme_mmio_cc_disable(vfio->bar0);
	if (!ctrlr->timeout_ms) {
		return 0;
	}

	return nvme_mmio_csts_wait_until_not_ready(vfio->bar0, ctrlr->timeout_ms);
}

static inline int
nvme_controller_close_vfio(struct nvme_controller *ctrlr, struct vfio_ctx *vfio)
{
	int close_err;
	int err;

	// Stop the controller before tearing down DMA mappings and BAR access.
	err = nvme_controller_disable_vfio(ctrlr, vfio);

	if (ctrlr->aq.rpool) {
		nvme_qpair_term(&ctrlr->aq);
		memset(&ctrlr->aq, 0, sizeof(ctrlr->aq));
	}

	if (ctrlr->buf) {
		hostmem_dma_free(ctrlr->heap, ctrlr->buf);
		ctrlr->buf = NULL;
	}

	close_err = nvme_vfio_ctx_close(vfio, ctrlr->heap);
	if (close_err && !err) {
		err = close_err;
	}
	memset(ctrlr, 0, sizeof(*ctrlr));

	return err;
}

/**
 * Resolve the IOMMU group ID of the PCI function identified by `bdf`.
 */
static inline int
vfio_device_get_iommu_group_id(const char *bdf, int *group_id)
{
	char path[PATH_MAX] = {0};
	char link[PATH_MAX] = {0};
	ssize_t nbytes;
	char *base;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf);

	nbytes = readlink(path, link, sizeof(link) - 1);
	if (nbytes < 0) {
		return -errno;
	}

	base = strrchr(link, '/');
	if (!base || !base[1]) {
		return -EINVAL;
	}

	*group_id = atoi(base + 1);

	return 0;
}

/**
 * Map the hugepage-backed heap into the VFIO IOMMU domain.
 *
 * The current implementation intentionally uses `iova == phys` for each hugepage.
 * This keeps the existing NVMe code paths working because they already pass
 * hostmem_dma_v2p() results to the controller for SQ/CQ/PRP DMA addresses.
 *
 * TODO: decouple IOVA from physical addresses by introducing a dedicated
 *       hostmem_dma_v2iova() path for VFIO-backed DMA.
 */
static inline int
vfio_map_heap(struct vfio_ctx *vfio, struct hostmem_heap *heap)
{
	printf("VFIO DEBUG: map host heap pages=%zu hugepgsz=%u\n",
	       heap->nphys, heap->config->hugepgsz);

	for (size_t i = 0; i < heap->nphys; ++i) {
		struct vfio_iommu_type1_dma_map map = {0};
		void *vaddr = (uint8_t *)heap->memory.virt + (i * heap->config->hugepgsz);

		map.argsz = sizeof(map);
		map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
		map.vaddr = (uintptr_t)vaddr;
		map.iova = heap->phys_lut[i];
		map.size = heap->config->hugepgsz;

		if (vfio_iommu_map_dma(&vfio->container, &map) < 0) {
			printf("VFIO DEBUG: map heap[%zu] failed vaddr=%p iova=0x%" PRIx64
			       " size=%" PRIu64 " errno=%d (%s)\n",
			       i, vaddr, (uint64_t)map.iova, (uint64_t)map.size, errno,
			       strerror(errno));
			UPCIE_DEBUG("FAILED: vfio_iommu_map_dma(); errno(%d)", errno);
			return -errno;
		}

		if (i < 4 || i + 1 == heap->nphys) {
			printf("VFIO DEBUG: map heap[%zu] vaddr=%p iova=0x%" PRIx64
			       " size=%" PRIu64 "\n",
			       i, vaddr, (uint64_t)map.iova, (uint64_t)map.size);
		}
	}

	return 0;
}

/**
 * Open an NVMe controller through VFIO.
 *
 * This sets up the VFIO container/group, maps the DMA heap and BAR0, and then
 * continues with the regular NVMe controller initialization flow.
 */
static inline int
nvme_controller_open_vfio(struct nvme_controller *ctrlr, struct vfio_ctx *vfio, const char *bdf,
			  struct hostmem_heap *heap)
{
	struct vfio_region_info region = {0};
	int api_version = 0;
	int group_id = -1;
	uint64_t cap;
	void *bar0;
	int err;

	memset(ctrlr, 0, sizeof(*ctrlr));
	nvme_vfio_ctx_init(vfio);
	ctrlr->heap = heap;

	ctrlr->buf = hostmem_dma_malloc(ctrlr->heap, 4096);
	if (!ctrlr->buf) {
		UPCIE_DEBUG("FAILED: hostmem_dma_malloc(buf); errno(%d)", errno);
		return -errno;
	}
	memset(ctrlr->buf, 0, 4096);

	nvme_qid_bitmap_init(ctrlr->qids);

	// Set up VFIO resources
	printf("VFIO DEBUG: open bdf=%s\n", bdf);

	err = vfio_device_get_iommu_group_id(bdf, &group_id);
	if (err) {
		printf("VFIO DEBUG: vfio_device_get_iommu_group_id(%s) failed err(%d)\n",
		       bdf, err);
		UPCIE_DEBUG("FAILED: vfio_device_get_iommu_group_id(); err(%d)", err);
		goto fail;
	}
	printf("VFIO DEBUG: iommu_group=%d\n", group_id);

	err = vfio_container_open(&vfio->container);
	if (err) {
		printf("VFIO DEBUG: vfio_container_open() failed err(%d)\n", err);
		UPCIE_DEBUG("FAILED: vfio_container_open(); err(%d)", err);
		goto fail;
	}
	printf("VFIO DEBUG: container fd=%d\n", vfio->container.fd);

	err = vfio_get_api_version(&vfio->container, &api_version);
	if (err) {
		printf("VFIO DEBUG: vfio_get_api_version() failed err(%d)\n", err);
		UPCIE_DEBUG("FAILED: vfio_get_api_version(); err(%d)", err);
		goto fail;
	}
	printf("VFIO DEBUG: api_version=%d expected=%d\n", api_version, VFIO_API_VERSION);
	if (api_version != VFIO_API_VERSION) {
		err = -EINVAL;
		printf("VFIO DEBUG: unexpected api_version=%d expected=%d\n",
		       api_version, VFIO_API_VERSION);
		UPCIE_DEBUG("FAILED: unexpected VFIO_API_VERSION(%d != %d)", api_version,
			    VFIO_API_VERSION);
		goto fail;
	}

	{
		int has_type1 = vfio_check_extension(&vfio->container, VFIO_TYPE1_IOMMU);
		printf("VFIO DEBUG: VFIO_TYPE1_IOMMU extension=%d\n", has_type1);
		if (!has_type1) {
			err = -ENOTSUP;
			printf("VFIO DEBUG: VFIO_TYPE1_IOMMU not supported\n");
			UPCIE_DEBUG("FAILED: VFIO_TYPE1_IOMMU not supported");
			goto fail;
		}
	}

	err = vfio_group_open(group_id, &vfio->group);
	if (err) {
		printf("VFIO DEBUG: vfio_group_open(%d) failed err(%d)\n", group_id, err);
		UPCIE_DEBUG("FAILED: vfio_group_open(%d); err(%d)", group_id, err);
		goto fail;
	}
	printf("VFIO DEBUG: group fd=%d\n", vfio->group.fd);

	err = vfio_group_get_status(&vfio->group);
	if (err < 0) {
		err = -errno;
		printf("VFIO DEBUG: vfio_group_get_status() failed errno=%d (%s)\n",
		       errno, strerror(errno));
		UPCIE_DEBUG("FAILED: vfio_group_get_status(); errno(%d)", errno);
		goto fail;
	}
	printf("VFIO DEBUG: group status flags=0x%x\n", vfio->group.status.flags);

	if (!(vfio->group.status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		err = -EBUSY;
		printf("VFIO DEBUG: group not viable flags=0x%x\n",
		       vfio->group.status.flags);
		UPCIE_DEBUG("FAILED: group not viable");
		goto fail;
	}

	err = vfio_group_set_container(&vfio->group, &vfio->container);
	if (err < 0) {
		err = -errno;
		printf("VFIO DEBUG: vfio_group_set_container() failed errno=%d (%s)\n",
		       errno, strerror(errno));
		UPCIE_DEBUG("FAILED: vfio_group_set_container(); errno(%d)", errno);
		goto fail;
	}
	printf("VFIO DEBUG: group attached to container\n");

	err = vfio_set_iommu(&vfio->container, VFIO_TYPE1_IOMMU);
	if (err < 0) {
		err = -errno;
		printf("VFIO DEBUG: vfio_set_iommu(TYPE1) failed errno=%d (%s)\n",
		       errno, strerror(errno));
		UPCIE_DEBUG("FAILED: vfio_set_iommu(); errno(%d)", errno);
		goto fail;
	}
	vfio->iommu_set = 1;
	printf("VFIO DEBUG: TYPE1 IOMMU enabled\n");

	err = vfio_map_heap(vfio, heap);
	if (err) {
		printf("VFIO DEBUG: vfio_map_heap() failed err(%d)\n", err);
		UPCIE_DEBUG("FAILED: vfio_map_heap(); err(%d)", err);
		goto fail;
	}
	printf("VFIO DEBUG: host heap mapped into VFIO IOMMU\n");

	vfio->device_fd = vfio_group_get_device_fd(&vfio->group, bdf);
	if (vfio->device_fd < 0) {
		err = -errno;
		printf("VFIO DEBUG: vfio_group_get_device_fd(%s) failed errno=%d (%s)\n",
		       bdf, errno, strerror(errno));
		UPCIE_DEBUG("FAILED: vfio_group_get_device_fd(); errno(%d)", errno);
		goto fail;
	}
	printf("VFIO DEBUG: device fd=%d\n", vfio->device_fd);

	err = nvme_vfio_pci_bus_master_enable(vfio->device_fd);
	if (err) {
		printf("VFIO DEBUG: nvme_vfio_pci_bus_master_enable() failed err(%d)\n",
		       err);
		UPCIE_DEBUG("FAILED: nvme_vfio_pci_bus_master_enable(); err(%d)", err);
		goto fail;
	}
	printf("VFIO DEBUG: PCI bus mastering enabled\n");

	region.index = VFIO_PCI_BAR0_REGION_INDEX;
	err = vfio_device_get_region_info(vfio->device_fd, &region);
	if (err < 0) {
		err = -errno;
		printf("VFIO DEBUG: vfio_device_get_region_info(BAR0) failed errno=%d (%s)\n",
		       errno, strerror(errno));
		UPCIE_DEBUG("FAILED: vfio_device_get_region_info(); errno(%d)", errno);
		goto fail;
	}
	printf("VFIO DEBUG: BAR0 offset=0x%" PRIx64 " size=0x%" PRIx64
	       " flags=0x%x\n",
	       (uint64_t)region.offset, (uint64_t)region.size, region.flags);

	bar0 = vfio_map_region(vfio->device_fd, region.size, region.offset);
	if (bar0 == MAP_FAILED) {
		err = -errno;
		printf("VFIO DEBUG: vfio_map_region(BAR0) failed errno=%d (%s)\n",
		       errno, strerror(errno));
		UPCIE_DEBUG("FAILED: vfio_map_region(); errno(%d)", errno);
		goto fail;
	}
	printf("VFIO DEBUG: BAR0 mapped at %p\n", bar0);

	vfio->bar0 = bar0;
	vfio->bar0_size = region.size;
	ctrlr->func.bars[0].region = bar0;
	ctrlr->func.bars[0].size = region.size;
	ctrlr->func.bars[0].id = 0;
	ctrlr->func.bars[0].fd = vfio->device_fd;

	// Continue with the common NVMe controller initialization
	cap = nvme_mmio_cap_read(bar0);
	// CAP.TO is encoded in units of 500 ms.
	ctrlr->timeout_ms = nvme_reg_cap_get_to(cap) * 500;
	printf("VFIO DEBUG: NVMe CAP=0x%" PRIx64 " timeout_ms=%d\n",
	       cap, ctrlr->timeout_ms);

	nvme_mmio_cc_disable(bar0);

	err = nvme_mmio_csts_wait_until_not_ready(bar0, ctrlr->timeout_ms);
	if (err) {
		printf("VFIO DEBUG: wait CSTS not ready failed err(%d)\n", err);
		UPCIE_DEBUG("FAILED: nvme_mmio_csts_wait_until_not_ready(); err(%d)", err);
		goto fail;
	}
	printf("VFIO DEBUG: controller disabled\n");

	err = nvme_qpair_init(&ctrlr->aq, 0, 256, bar0, ctrlr->heap);
	if (err) {
		printf("VFIO DEBUG: nvme_qpair_init(aq) failed err(%d)\n", err);
		UPCIE_DEBUG("FAILED: nvme_qpair_init(aq); err(%d)", err);
		goto fail;
	}
	printf("VFIO DEBUG: admin queue initialized sq=%p cq=%p\n",
	       ctrlr->aq.sq, ctrlr->aq.cq);

	nvme_mmio_aq_setup(bar0, hostmem_dma_v2p(heap, ctrlr->aq.sq),
			   hostmem_dma_v2p(heap, ctrlr->aq.cq), ctrlr->aq.depth);

	{
		uint32_t css = (nvme_reg_cap_get_css(cap) & (1 << 6)) ? 0x6 : 0x0;
		uint32_t cc = 0;

		cc = nvme_reg_cc_set_css(cc, css);
		cc = nvme_reg_cc_set_shn(cc, 0x0);
		cc = nvme_reg_cc_set_mps(cc, 0x0);
		cc = nvme_reg_cc_set_ams(cc, 0x0);
		cc = nvme_reg_cc_set_iosqes(cc, 6);
		cc = nvme_reg_cc_set_iocqes(cc, 4);
		cc = nvme_reg_cc_set_en(cc, 0x1);

		nvme_mmio_cc_write(bar0, cc);
	}

	err = nvme_mmio_csts_wait_until_ready(bar0, ctrlr->timeout_ms);
	if (err) {
		printf("VFIO DEBUG: wait CSTS ready failed err(%d)\n", err);
		UPCIE_DEBUG("FAILED: nvme_mmio_csts_wait_until_ready(); err(%d)", err);
		goto fail;
	}
	printf("VFIO DEBUG: controller enabled\n");

	return 0;

fail:
	printf("VFIO DEBUG: nvme_controller_open_vfio(%s) returning err(%d)\n",
	       bdf, err);
	nvme_controller_close_vfio(ctrlr, vfio);

	return err;
}
