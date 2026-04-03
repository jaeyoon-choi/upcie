// SPDX-License-Identifier: BSD-3-Clause

#define _UPCIE_WITH_NVME
#include <upcie/upcie_cuda.h>
#include <linux/iommufd.h>

/*
 * Probe the native iommufd path with a controlled A/B comparison:
 *
 * - TEST4:
 *   Map a normal host userspace page and confirm the IOAS setup works.
 * - TEST5:
 *   Reuse the same IOAS and try to map a CUDA device VA from cudamem.
 *
 * This isolates the address-model difference from the rest of the VFIO cdev
 * and iommufd setup.
 *
 * Expected:
 * - If cudamem-backed CUDA VAs are accepted as IOMMU map input, both mappings
 *   should succeed.
 *
 * Observed on the target system:
 * - The host userspace VA maps successfully.
 * - The CUDA device VA fails with -EFAULT under the same IOAS.
 */

static int
vfio_cdev_path_from_bdf(const char *bdf, char *path, size_t path_len)
{
	char sysfs_path[PATH_MAX];
	struct dirent *entry;
	DIR *dir;

	snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/pci/devices/%s/vfio-dev", bdf);

	dir = opendir(sysfs_path);
	if (!dir) {
		return -errno;
	}

	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			continue;
		}

		snprintf(path, path_len, "/dev/vfio/devices/%s", entry->d_name);
		closedir(dir);
		return 0;
	}

	closedir(dir);
	return -ENOENT;
}

static int
iommufd_unmap(int iommu_fd, uint32_t ioas_id, uint64_t iova, uint64_t length)
{
	struct iommu_ioas_unmap unmap = {0};

	unmap.size = sizeof(unmap);
	unmap.ioas_id = ioas_id;
	unmap.iova = iova;
	unmap.length = length;

	if (ioctl(iommu_fd, IOMMU_IOAS_UNMAP, &unmap)) {
		return -errno;
	}

	return 0;
}

static int
iommufd_map(int iommu_fd, uint32_t ioas_id, uint64_t iova, void *user_va, uint64_t length)
{
	struct iommu_ioas_map map = {0};

	map.size = sizeof(map);
	map.flags = IOMMU_IOAS_MAP_FIXED_IOVA | IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE;
	map.ioas_id = ioas_id;
	map.user_va = (uintptr_t)user_va;
	map.length = length;
	map.iova = iova;

	if (ioctl(iommu_fd, IOMMU_IOAS_MAP, &map)) {
		return -errno;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct vfio_device_bind_iommufd bind = {0};
	struct vfio_device_attach_iommufd_pt attach = {0};
	struct vfio_device_detach_iommufd_pt detach = {0};
	struct iommu_ioas_alloc alloc = {0};
	struct iommu_destroy destroy = {0};
	struct cudamem_config cuda_config = {0};
	struct cudamem_heap cuda_heap = {0};
	CUcontext cu_ctx = NULL;
	CUdevice cu_dev;
	void *host_vaddr = MAP_FAILED;
	char vfio_cdev_path[PATH_MAX] = {0};
	int iommu_fd = -1;
	int vfio_fd = -1;
	int attached = 0;
	int cuda_heap_ready = 0;
	int host_map_err = 0;
	int cuda_map_err = 0;
	int err;

	if (argc != 2) {
		printf("Usage: %s <PCI-BDF>\n", argv[0]);
		return 1;
	}

	err = vfio_cdev_path_from_bdf(argv[1], vfio_cdev_path, sizeof(vfio_cdev_path));
	if (err) {
		printf("FAILED: vfio_cdev_path_from_bdf(); err(%d)\n", err);
		return err;
	}

	iommu_fd = open("/dev/iommu", O_RDWR);
	if (iommu_fd < 0) {
		err = -errno;
		printf("FAILED: open(/dev/iommu); err(%d)\n", err);
		return err;
	}

	vfio_fd = open(vfio_cdev_path, O_RDWR);
	if (vfio_fd < 0) {
		err = -errno;
		printf("FAILED: open(%s); err(%d)\n", vfio_cdev_path, err);
		goto exit;
	}

	alloc.size = sizeof(alloc);
	if (ioctl(iommu_fd, IOMMU_IOAS_ALLOC, &alloc)) {
		err = -errno;
		printf("FAILED: IOMMU_IOAS_ALLOC; err(%d)\n", err);
		goto exit;
	}

	bind.argsz = sizeof(bind);
	bind.iommufd = iommu_fd;
	if (ioctl(vfio_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind)) {
		err = -errno;
		printf("FAILED: VFIO_DEVICE_BIND_IOMMUFD; err(%d)\n", err);
		goto exit;
	}

	attach.argsz = sizeof(attach);
	attach.pt_id = alloc.out_ioas_id;
	if (ioctl(vfio_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach)) {
		err = -errno;
		printf("FAILED: VFIO_DEVICE_ATTACH_IOMMUFD_PT; err(%d)\n", err);
		goto exit;
	}
	attached = 1;

	host_vaddr = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (host_vaddr == MAP_FAILED) {
		err = -errno;
		printf("FAILED: mmap(host); err(%d)\n", err);
		goto exit;
	}
	memset(host_vaddr, 0x5A, getpagesize());

	/* TEST4: known-good baseline with an ordinary host userspace VA */
	/*
	 * Expectation: a normal host userspace VA should map successfully, proving
	 * the IOAS setup is valid before we try the CUDA address.
	 * Observation: this mapping succeeds on the target system.
	 */
	host_map_err = iommufd_map(iommu_fd, alloc.out_ioas_id, 0x100000000ULL, host_vaddr, getpagesize());
	if (host_map_err) {
		err = host_map_err;
		printf("FAILED: host user VA map via IOMMU_IOAS_MAP; err(%d)\n", err);
		goto exit;
	}
	printf("IOMMU_IOAS_MAP with host user VA: success\n");

	err = iommufd_unmap(iommu_fd, alloc.out_ioas_id, 0x100000000ULL, getpagesize());
	if (err) {
		printf("FAILED: host IOMMU_IOAS_UNMAP; err(%d)\n", err);
		goto exit;
	}

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

	err = cudamem_heap_init(&cuda_heap, cuda_config.device_pagesize, &cuda_config);
	if (err) {
		printf("FAILED: cudamem_heap_init(); err(%d)\n", err);
		goto exit;
	}
	cuda_heap_ready = 1;

	printf("CUDA heap vaddr: 0x%" PRIx64 "\n", cuda_heap.vaddr);
	printf("CUDA phys_lut[0]: 0x%" PRIx64 "\n", cuda_heap.phys_lut[0]);

	/* TEST5: re-run the same IOAS map, but now with the CUDA device VA */
	/*
	 * Expectation: if cudamem-backed CUDA VAs are valid IOMMU map input, this
	 * should succeed under the same IOAS as the host mapping above.
	 * Observation: the CUDA device VA fails here with -EFAULT.
	 */
	cuda_map_err = iommufd_map(iommu_fd, alloc.out_ioas_id, 0x200000000ULL,
				   (void *)(uintptr_t)cuda_heap.vaddr, cuda_heap.config->device_pagesize);
	if (cuda_map_err) {
		printf("IOMMU_IOAS_MAP with CUDA device VA: err(%d)\n", cuda_map_err);
	} else {
		printf("IOMMU_IOAS_MAP with CUDA device VA: success\n");
		err = iommufd_unmap(iommu_fd, alloc.out_ioas_id, 0x200000000ULL,
				    cuda_heap.config->device_pagesize);
		if (err) {
			printf("FAILED: CUDA IOMMU_IOAS_UNMAP; err(%d)\n", err);
			goto exit;
		}
	}

	err = 0;

exit:
	if (attached) {
		detach.argsz = sizeof(detach);
		if (ioctl(vfio_fd, VFIO_DEVICE_DETACH_IOMMUFD_PT, &detach) && !err) {
			err = -errno;
		}
	}
	if (vfio_fd >= 0) {
		close(vfio_fd);
	}
	if (alloc.out_ioas_id && iommu_fd >= 0) {
		destroy.size = sizeof(destroy);
		destroy.id = alloc.out_ioas_id;
		if (ioctl(iommu_fd, IOMMU_DESTROY, &destroy) && !err) {
			err = -errno;
		}
	}
	if (host_vaddr != MAP_FAILED) {
		munmap(host_vaddr, getpagesize());
	}
	if (cuda_heap_ready) {
		cudamem_heap_term(&cuda_heap);
	}
	if (cu_ctx) {
		cuCtxDestroy(cu_ctx);
	}
	if (iommu_fd >= 0) {
		close(iommu_fd);
	}

	return err;
}
