// SPDX-License-Identifier: BSD-3-Clause

/**
 * uPCIe iommu-map helper interface
 * ================================
 *
 * Shared ioctl definitions for the helper kernel module that maps an array of
 * device-physical addresses (e.g. a CUDA/udmabuf-derived phys_lut) into the
 * IOMMU domain a VFIO-controlled NVMe already uses, returning an IOVA base that
 * userspace writes into NVMe PRPs.
 *
 * Mappings persist until userspace issues an explicit UNMAP or closes the file
 * descriptor.
 *
 * @file iommu_map.h
 * @version 0.4.4
 */
#ifndef UPCIE_IOMMU_MAP_H
#define UPCIE_IOMMU_MAP_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define UPCIE_IOMMU_MAP_DEVICE "/dev/upcie-iommu-map"
#define UPCIE_IOMMU_MAP_BDF_LEN 16

/* Mapping attributes for UPCIE_IOMMU_MAP (0 is treated as READ|WRITE). */
#define UPCIE_IOMMU_MAP_PROT_READ (1U << 0)
#define UPCIE_IOMMU_MAP_PROT_WRITE (1U << 1)
#define UPCIE_IOMMU_MAP_PROT_MMIO (1U << 2)
#define UPCIE_IOMMU_MAP_PROT_CACHE (1U << 3)

#define UPCIE_IOMMU_MAP_PROT_MASK                                            \
	(UPCIE_IOMMU_MAP_PROT_READ | UPCIE_IOMMU_MAP_PROT_WRITE |            \
	 UPCIE_IOMMU_MAP_PROT_MMIO | UPCIE_IOMMU_MAP_PROT_CACHE)

struct upcie_iommu_unmap_req {
	__aligned_u64 map_handle;
};

/*
 * Map an already-known array of device-physical addresses
 * (e.g. a udmabuf-derived phys_lut) directly into the IOMMU domain that the
 * target NVMe device currently uses. This is meant for a VFIO-controlled
 * device: userspace picks 'iova_base' (must not collide with its own
 * VFIO_IOMMU_MAP_DMA mappings) and programs PRPs with iova_base + offset.
 */
struct upcie_iommu_map_req {
	char bdf[UPCIE_IOMMU_MAP_BDF_LEN];
	__s32 dmabuf_fd;		/* optional lifetime ref; <0 to skip */
	__u32 page_size;
	__u32 nphys;
	__u32 prot;			/* access and memory-type attributes */
	__u32 reserved;			/* must be zero */
	__aligned_u64 iova_base;	/* userspace-chosen base IOVA */
	__aligned_u64 user_phys_ptr;	/* array of 'nphys' __u64 phys addrs */
	__aligned_u64 map_handle;	/* out */
};

#define UPCIE_IOMMU_MAP_IOC_MAGIC 'u'

#define UPCIE_IOMMU_UNMAP _IOW(UPCIE_IOMMU_MAP_IOC_MAGIC, 0x02, \
				struct upcie_iommu_unmap_req)
#define UPCIE_IOMMU_MAP _IOWR(UPCIE_IOMMU_MAP_IOC_MAGIC, 0x03, \
				     struct upcie_iommu_map_req)

#ifndef __KERNEL__
static inline int
upcie_iommu_map_open(void)
{
	int fd = open(UPCIE_IOMMU_MAP_DEVICE, O_RDWR);

	return fd < 0 ? -errno : fd;
}

static inline int
upcie_iommu_map_close(int fd)
{
	if (fd < 0)
		return -EINVAL;

	return close(fd) ? -errno : 0;
}

/* Unmap a handle returned by upcie_iommu_map_add(). */
static inline int
upcie_iommu_map_del(int fd, __u64 map_handle)
{
	struct upcie_iommu_unmap_req req = {0};

	if (fd < 0 || !map_handle)
		return -EINVAL;

	req.map_handle = map_handle;
	return ioctl(fd, UPCIE_IOMMU_UNMAP, &req) < 0 ? -errno : 0;
}

/*
 * Map an array of device-physical addresses (phys_lut) into the
 * IOMMU domain the target NVMe device currently uses. On success the IOMMU
 * translates 'iova_base + i * page_size' to 'phys[i]', so PRPs should be built
 * from iova_base, not from phys[].
 */
static inline int
upcie_iommu_map_add(int fd, const char *bdf, int dmabuf_fd,
				uint64_t iova_base, __u32 page_size, __u32 nphys,
				const uint64_t *phys, __u32 prot,
				uint64_t *map_handle_out)
{
	struct upcie_iommu_map_req req = {0};

	if (fd < 0 || !bdf || !phys || !nphys)
		return -EINVAL;

	strncpy(req.bdf, bdf, sizeof(req.bdf) - 1);
	req.dmabuf_fd = dmabuf_fd;
	req.page_size = page_size;
	req.nphys = nphys;
	req.prot = prot;
	req.iova_base = iova_base;
	req.user_phys_ptr = (__u64)(uintptr_t)phys;

	if (ioctl(fd, UPCIE_IOMMU_MAP, &req) < 0)
		return -errno;

	if (map_handle_out)
		*map_handle_out = req.map_handle;
	return 0;
}
#endif

#endif
