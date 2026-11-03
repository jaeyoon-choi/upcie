// SPDX-License-Identifier: BSD-3-Clause
/*
 * pal_probe: map a CUDA allocation into an iommufd IOAS through its dma-buf.
 *
 * Steps:
 *   1. cuMemAlloc(size) and export it with cuMemGetHandleForAddressRange()
 *      as a dma-buf fd.
 *   2. Open /dev/iommu, allocate an IOAS.
 *   3. Optionally bind a VFIO cdev device (--vfio /dev/vfio/devices/vfioN)
 *      to the iommufd and attach it to a HWPT on that IOAS, so the mapping
 *      lands in a hardware domain and shows up in the iommu:map tracepoint.
 *   4. IOMMU_IOAS_MAP_FILE with the dma-buf fd. Print the IOVA or the errno.
 *   5. Optionally hold the mapping for --hold seconds, then unmap.
 *
 * Build: gcc -O2 -Wall -I/usr/local/cuda/include -o pal_probe pal_probe.c -lcuda
 * Needs access to /dev/iommu (root, or chown the node).
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/iommufd.h>
#include <linux/vfio.h>

#include <cuda.h>

#define IOMMUFD_PATH "/dev/iommu"

static const char *cu_err(CUresult r)
{
	const char *s = "?";

	cuGetErrorString(r, &s);
	return s;
}

#define CU_CHECK(call)                                                        \
	do {                                                                  \
		CUresult _r = (call);                                         \
		if (_r != CUDA_SUCCESS) {                                     \
			fprintf(stderr, "%s failed: %s\n", #call, cu_err(_r));  \
			return 1;                                             \
		}                                                             \
	} while (0)

static int ioctl_or_err(int fd, unsigned long req, void *arg, const char *name)
{
	int rc = ioctl(fd, req, arg);

	if (rc < 0) {
		fprintf(stderr, "%s failed: %s (%d)\n", name, strerror(errno), errno);
		return -errno;
	}
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--size BYTES] [--vfio /dev/vfio/devices/vfioN] [--hold SEC] [--no-write]\n"
		"  --size      allocation size, default 1 GiB\n"
		"  --vfio      VFIO cdev to bind and attach to the IOAS before mapping\n"
		"  --hold      keep the mapping for SEC seconds before unmapping\n"
		"  --no-write  map read-only\n",
		argv0);
}

int main(int argc, char **argv)
{
	size_t size = 1ULL << 30;
	const char *vfio_path = NULL;
	int hold = 0;
	int writable = 1;
	int c;
	static const struct option opts[] = {
		{ "size", required_argument, NULL, 's' },
		{ "vfio", required_argument, NULL, 'v' },
		{ "hold", required_argument, NULL, 'H' },
		{ "no-write", no_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 }
	};

	while ((c = getopt_long(argc, argv, "s:v:H:rh", opts, NULL)) != -1) {
		switch (c) {
		case 's': size = strtoull(optarg, NULL, 0); break;
		case 'v': vfio_path = optarg; break;
		case 'H': hold = atoi(optarg); break;
		case 'r': writable = 0; break;
		default: usage(argv[0]); return 2;
		}
	}

	/* 1. CUDA allocation exported as a dma-buf */
	CUdevice dev;
	CUcontext ctx;
	CUdeviceptr dptr;
	int dmabuf_fd = -1;

	CU_CHECK(cuInit(0));
	CU_CHECK(cuDeviceGet(&dev, 0));
	CU_CHECK(cuCtxCreate(&ctx, 0, dev));
	CU_CHECK(cuMemAlloc(&dptr, size));
	CU_CHECK(cuMemGetHandleForAddressRange(&dmabuf_fd, dptr, size,
					       CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0));
	printf("cuda: dptr=0x%llx size=%zu dmabuf_fd=%d\n",
	       (unsigned long long)dptr, size, dmabuf_fd);

	/* 2. iommufd and an IOAS */
	int iommufd = open(IOMMUFD_PATH, O_RDWR);

	if (iommufd < 0) {
		fprintf(stderr, "open %s failed: %s\n", IOMMUFD_PATH, strerror(errno));
		return 1;
	}

	struct iommu_ioas_alloc ioas_alloc = { .size = sizeof(ioas_alloc) };

	if (ioctl_or_err(iommufd, IOMMU_IOAS_ALLOC, &ioas_alloc, "IOMMU_IOAS_ALLOC"))
		return 1;
	printf("iommufd: ioas_id=%u\n", ioas_alloc.out_ioas_id);

	/* 3. Optional VFIO cdev attach, so that a hardware domain exists */
	int vfio_fd = -1;

	if (vfio_path) {
		vfio_fd = open(vfio_path, O_RDWR);
		if (vfio_fd < 0) {
			fprintf(stderr, "open %s failed: %s\n", vfio_path, strerror(errno));
			return 1;
		}

		struct vfio_device_bind_iommufd bind = {
			.argsz = sizeof(bind), .iommufd = iommufd,
		};
		if (ioctl_or_err(vfio_fd, VFIO_DEVICE_BIND_IOMMUFD, &bind,
				 "VFIO_DEVICE_BIND_IOMMUFD"))
			return 1;

		struct iommu_hwpt_alloc hwpt = {
			.size = sizeof(hwpt), .dev_id = bind.out_devid,
			.pt_id = ioas_alloc.out_ioas_id,
		};
		if (ioctl_or_err(iommufd, IOMMU_HWPT_ALLOC, &hwpt, "IOMMU_HWPT_ALLOC"))
			return 1;

		struct vfio_device_attach_iommufd_pt attach = {
			.argsz = sizeof(attach), .pt_id = hwpt.out_hwpt_id,
		};
		if (ioctl_or_err(vfio_fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach,
				 "VFIO_DEVICE_ATTACH_IOMMUFD_PT"))
			return 1;
		printf("vfio: %s devid=%u hwpt=%u attached\n", vfio_path,
		       bind.out_devid, hwpt.out_hwpt_id);
	}

	/* 4. Map the dma-buf into the IOAS */
	struct iommu_ioas_map_file map = {
		.size = sizeof(map),
		.flags = IOMMU_IOAS_MAP_READABLE | (writable ? IOMMU_IOAS_MAP_WRITEABLE : 0),
		.ioas_id = ioas_alloc.out_ioas_id,
		.fd = dmabuf_fd,
		.start = 0,
		.length = size,
	};
	int rc = ioctl(iommufd, IOMMU_IOAS_MAP_FILE, &map);

	if (rc < 0) {
		printf("IOMMU_IOAS_MAP_FILE: FAILED errno=%d (%s)\n", errno, strerror(errno));
		return 3;
	}
	printf("IOMMU_IOAS_MAP_FILE: OK iova=0x%llx length=0x%llx\n",
	       (unsigned long long)map.iova, (unsigned long long)map.length);

	/* 5. Hold, then unmap */
	if (hold > 0) {
		printf("holding %d s (pid %d)\n", hold, getpid());
		fflush(stdout);
		sleep(hold);
	}

	struct iommu_ioas_unmap unmap = {
		.size = sizeof(unmap), .ioas_id = ioas_alloc.out_ioas_id,
		.iova = map.iova, .length = size,
	};
	if (ioctl_or_err(iommufd, IOMMU_IOAS_UNMAP, &unmap, "IOMMU_IOAS_UNMAP"))
		return 1;
	printf("IOMMU_IOAS_UNMAP: OK\n");

	if (vfio_fd >= 0)
		close(vfio_fd);
	close(iommufd);
	close(dmabuf_fd);
	cuMemFree(dptr);
	cuCtxDestroy(ctx);
	return 0;
}
