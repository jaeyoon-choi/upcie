// SPDX-License-Identifier: BSD-3-Clause

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>

/*
 * Compare two VFIO type1 mapping attempts against the same cudamem-backed
 * buffer:
 *
 * - TEST1:
 *   Pass the CUDA device VA directly as map.vaddr.
 * - TEST2:
 *   If the exporter supports it, mmap the dma-buf into a host VA.
 * - TEST3:
 *   Retry the VFIO map through that host userspace address instead.
 *
 * Expected:
 * - Either the direct CUDA VA path works, or the dma-buf mmap path provides
 *   a host userspace VA that can be mapped through VFIO.
 *
 * Observed on the target system:
 * - The direct CUDA VA path fails with -EFAULT.
 * - The dma-buf mmap fallback itself fails, so no host-VA retry is available.
 */

static int
vfio_unmap_dma_pages(struct vfio_ctx *vfio, size_t npages, size_t pagesize, uint64_t *phys_lut)
{
	int err = 0;

	for (size_t i = 0; i < npages; ++i) {
		struct vfio_iommu_type1_dma_unmap unmap = {0};

		unmap.argsz = sizeof(unmap);
		unmap.iova = phys_lut[i];
		unmap.size = pagesize;

		if (vfio_iommu_unmap_dma(&vfio->container, &unmap) < 0 && !err) {
			err = -errno;
		}
	}

	return err;
}

static int
vfio_map_dma_pages(struct vfio_ctx *vfio, uintptr_t base_vaddr, size_t npages, size_t pagesize,
		   uint64_t *phys_lut)
{
	for (size_t i = 0; i < npages; ++i) {
		struct vfio_iommu_type1_dma_map map = {0};

		map.argsz = sizeof(map);
		map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
		map.vaddr = base_vaddr + (i * pagesize);
		map.iova = phys_lut[i];
		map.size = pagesize;

		if (vfio_iommu_map_dma(&vfio->container, &map) < 0) {
			int err = -errno;

			vfio_unmap_dma_pages(vfio, i, pagesize, phys_lut);
			return err;
		}
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct hostmem_config host_config = {0};
	struct hostmem_heap host_heap = {0};
	struct cudamem_config cuda_config = {0};
	struct cudamem_heap cuda_heap = {0};
	struct nvme_controller ctrlr = {0};
	struct vfio_ctx vfio = {0};
	CUcontext cu_ctx = NULL;
	CUdevice cu_dev;
	void *dmabuf_map = MAP_FAILED;
	int host_heap_ready = 0;
	int cuda_heap_ready = 0;
	int vfio_ready = 0;
	int direct_err = 0;
	int mmap_err = 0;
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = hostmem_config_init(&host_config);
	if (err) {
		printf("FAILED: hostmem_config_init(); err(%d)\n", err);
		return err;
	}

	err = hostmem_heap_init(&host_heap, 1024 * 1024 * 128ULL, &host_config);
	if (err) {
		printf("FAILED: hostmem_heap_init(); err(%d)\n", err);
		return err;
	}
	host_heap_ready = 1;

	err = cuInit(0);
	if (err) {
		printf("FAILED: cuInit(); err(%d)\n", err);
		goto exit;
	}

	err = cuDeviceGet(&cu_dev, 0);
	if (err) {
		printf("FAILED: cuDeviceGet(); err(%d)\n", err);
		goto exit;
	}

	err = cuCtxCreate(&cu_ctx, 0, cu_dev);
	if (err) {
		printf("FAILED: cuCtxCreate(); err(%d)\n", err);
		goto exit;
	}

	err = cudamem_config_init(&cuda_config, 0);
	if (err) {
		printf("FAILED: cudamem_config_init(); err(%d)\n", err);
		goto exit;
	}

	err = cudamem_heap_init(&cuda_heap, 8 * cuda_config.device_pagesize, &cuda_config);
	if (err) {
		printf("FAILED: cudamem_heap_init(); err(%d)\n", err);
		goto exit;
	}
	cuda_heap_ready = 1;

	err = nvme_controller_open_vfio(&ctrlr, &vfio, argv[1], &host_heap);
	if (err) {
		printf("FAILED: nvme_controller_open_vfio(); err(%d)\n", err);
		goto exit;
	}
	vfio_ready = 1;

	printf("CUDA heap vaddr: 0x%" PRIx64 "\n", cuda_heap.vaddr);
	printf("CUDA dma-buf fd: %d\n", cuda_heap.dmabuf.fd);
	printf("CUDA phys_lut[0]: 0x%" PRIx64 "\n", cuda_heap.phys_lut[0]);

	/* TEST1: direct VFIO map using the CUDA device VA */
	/*
	 * Expectation: the CUDA buffer VA could be registered directly for DMA.
	 * Observation: VFIO rejects the CUDA device VA here with -EFAULT.
	 */
	direct_err = vfio_map_dma_pages(&vfio, cuda_heap.vaddr, cuda_heap.nphys,
					 cuda_heap.config->device_pagesize, cuda_heap.phys_lut);
	if (!direct_err) {
		printf("VFIO map with CUDA device VA: success\n");
		vfio_unmap_dma_pages(&vfio, cuda_heap.nphys, cuda_heap.config->device_pagesize,
				     cuda_heap.phys_lut);
	} else {
		printf("VFIO map with CUDA device VA: err(%d)\n", direct_err);
	}

	/* TEST2: try to obtain a host userspace VA by mmap'ing the exported dma-buf */
	/*
	 * If a host-visible mmap exists for the exported dma-buf, it gives us a
	 * real userspace VA to compare against the failing CUDA device VA path.
	 *
	 * Expectation: mmap succeeds and provides a host VA for a second VFIO map
	 * attempt.
	 * Observation: mmap itself fails on the target system, so this fallback
	 * path cannot be exercised.
	 */
	dmabuf_map = mmap(NULL, cuda_heap.size, PROT_READ | PROT_WRITE, MAP_SHARED, cuda_heap.dmabuf.fd, 0);
	if (dmabuf_map == MAP_FAILED) {
		err = -errno;
		printf("FAILED: mmap(cuda dmabuf); err(%d)\n", err);
		goto exit;
	}

	printf("dma-buf mmap vaddr: %p\n", dmabuf_map);

	/* TEST3: retry the VFIO map through the host VA returned by TEST2 */
	mmap_err = vfio_map_dma_pages(&vfio, (uintptr_t)dmabuf_map, cuda_heap.nphys,
				       cuda_heap.config->device_pagesize, cuda_heap.phys_lut);
	if (mmap_err) {
		err = mmap_err;
		printf("FAILED: VFIO map with dma-buf mmap VA; err(%d)\n", err);
		goto exit;
	}

	printf("VFIO map with dma-buf mmap VA: success\n");

	err = vfio_unmap_dma_pages(&vfio, cuda_heap.nphys, cuda_heap.config->device_pagesize,
				    cuda_heap.phys_lut);
	if (err) {
		printf("FAILED: vfio_unmap_dma_pages(); err(%d)\n", err);
		goto exit;
	}

	err = 0;

exit:
	if (dmabuf_map != MAP_FAILED) {
		munmap(dmabuf_map, cuda_heap.size);
	}

	if (vfio_ready) {
		nvme_controller_close_vfio(&ctrlr, &vfio);
	}
	if (cuda_heap_ready) {
		cudamem_heap_term(&cuda_heap);
	}
	if (host_heap_ready) {
		hostmem_heap_term(&host_heap);
	}
	if (cu_ctx) {
		cuCtxDestroy(cu_ctx);
	}

	return err;
}
