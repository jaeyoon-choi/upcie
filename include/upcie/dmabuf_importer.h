// SPDX-License-Identifier: BSD-3-Clause

/**
 * uPCIe dma-buf importer interface
 * =================================
 *
 * Shared ioctl definitions for the helper kernel module that imports a dma-buf
 * on behalf of an NVMe device and returns a page-based DMA LUT.
 *
 * The kernel module keeps the dma-buf attachment mapped until userspace issues
 * an explicit UNMAP or closes the importer file descriptor.
 *
 * @file dmabuf_importer.h
 * @version 0.4.3
 */
#ifndef UPCIE_DMABUF_IMPORTER_H
#define UPCIE_DMABUF_IMPORTER_H

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

#define UPCIE_DMABUF_IMPORTER_DEVICE "/dev/upcie-dmabuf-importer"
#define UPCIE_DMABUF_BDF_LEN 16

#define UPCIE_DMABUF_MAP_F_ANY_BUS_ADDR (1U << 0)
#define UPCIE_DMABUF_MAP_F_ALL_BUS_ADDR (1U << 1)

/* Protection bits for UPCIE_DMABUF_IOMMU_MAP (0 is treated as READ|WRITE). */
#define UPCIE_DMABUF_PROT_READ (1U << 0)
#define UPCIE_DMABUF_PROT_WRITE (1U << 1)

struct upcie_dmabuf_map_req {
	char bdf[UPCIE_DMABUF_BDF_LEN];
	__s32 dmabuf_fd;
	__u32 page_size;
	__u32 flags;
	__aligned_u64 user_lut_ptr;
	__u32 lut_capacity;
	__u32 sg_nents;
	__u32 bus_nents;
	__u32 p2pdma_page_nents;
	__u32 map_none_nents;
	__u32 map_bus_addr_nents;
	__u32 map_thru_host_bridge_nents;
	__u32 map_not_supported_nents;
	__u32 nphys;
	__aligned_u64 map_handle;
};

struct upcie_dmabuf_unmap_req {
	__aligned_u64 map_handle;
};

/*
 * Option-A path: map an already-known array of device-physical addresses
 * (e.g. a udmabuf-derived phys_lut) directly into the IOMMU domain that the
 * target NVMe device currently uses. This is meant for a VFIO-controlled
 * device: userspace picks 'iova_base' (must not collide with its own
 * VFIO_IOMMU_MAP_DMA mappings) and programs PRPs with iova_base + offset.
 */
struct upcie_dmabuf_iommu_map_req {
	char bdf[UPCIE_DMABUF_BDF_LEN];
	__s32 dmabuf_fd;		/* optional lifetime ref; <0 to skip */
	__u32 page_size;
	__u32 nphys;
	__u32 prot;			/* UPCIE_DMABUF_PROT_*; 0 => READ|WRITE */
	__u32 reserved;
	__aligned_u64 iova_base;	/* userspace-chosen base IOVA */
	__aligned_u64 user_phys_ptr;	/* array of 'nphys' __u64 phys addrs */
	__aligned_u64 map_handle;	/* out */
};

#define UPCIE_DMABUF_IMPORTER_IOC_MAGIC 'u'

#define UPCIE_DMABUF_MAP _IOWR(UPCIE_DMABUF_IMPORTER_IOC_MAGIC, 0x01, \
			       struct upcie_dmabuf_map_req)
#define UPCIE_DMABUF_UNMAP _IOW(UPCIE_DMABUF_IMPORTER_IOC_MAGIC, 0x02, \
				struct upcie_dmabuf_unmap_req)
#define UPCIE_DMABUF_IOMMU_MAP _IOWR(UPCIE_DMABUF_IMPORTER_IOC_MAGIC, 0x03, \
				     struct upcie_dmabuf_iommu_map_req)

#ifndef __KERNEL__
struct upcie_dmabuf_import {
	int importer_fd;
	__u32 flags;
	__u32 sg_nents;
	__u32 bus_nents;
	__u32 p2pdma_page_nents;
	__u32 map_none_nents;
	__u32 map_bus_addr_nents;
	__u32 map_thru_host_bridge_nents;
	__u32 map_not_supported_nents;
	__u32 nphys;
	__u64 map_handle;
};

static inline int
upcie_dmabuf_importer_open(void)
{
	int fd = open(UPCIE_DMABUF_IMPORTER_DEVICE, O_RDWR);

	return fd < 0 ? -errno : fd;
}

static inline int
upcie_dmabuf_importer_close(int importer_fd)
{
	if (importer_fd < 0)
		return -EINVAL;

	return close(importer_fd) ? -errno : 0;
}

static inline int
upcie_dmabuf_importer_map(int importer_fd, const char *bdf, int dmabuf_fd,
			   __u32 page_size, __u32 lut_capacity, uint64_t *lut,
			   struct upcie_dmabuf_import *mapping)
{
	struct upcie_dmabuf_map_req req = {0};

	if (importer_fd < 0 || !bdf || !lut || !mapping)
		return -EINVAL;

	strncpy(req.bdf, bdf, sizeof(req.bdf) - 1);
	req.dmabuf_fd = dmabuf_fd;
	req.page_size = page_size;
	req.user_lut_ptr = (__u64)(uintptr_t)lut;
	req.lut_capacity = lut_capacity;

	if (ioctl(importer_fd, UPCIE_DMABUF_MAP, &req) < 0)
		return -errno;

	mapping->importer_fd = importer_fd;
	mapping->flags = req.flags;
	mapping->sg_nents = req.sg_nents;
	mapping->bus_nents = req.bus_nents;
	mapping->p2pdma_page_nents = req.p2pdma_page_nents;
	mapping->map_none_nents = req.map_none_nents;
	mapping->map_bus_addr_nents = req.map_bus_addr_nents;
	mapping->map_thru_host_bridge_nents = req.map_thru_host_bridge_nents;
	mapping->map_not_supported_nents = req.map_not_supported_nents;
	mapping->nphys = req.nphys;
	mapping->map_handle = req.map_handle;
	return 0;
}

static inline int
upcie_dmabuf_importer_unmap(const struct upcie_dmabuf_import *mapping)
{
	struct upcie_dmabuf_unmap_req req = {0};

	if (!mapping || mapping->importer_fd < 0 || !mapping->map_handle)
		return -EINVAL;

	req.map_handle = mapping->map_handle;
	return ioctl(mapping->importer_fd, UPCIE_DMABUF_UNMAP, &req) < 0 ?
	       -errno : 0;
}

/* Unmap any handle (LUT or IOMMU kind) returned by this importer. */
static inline int
upcie_dmabuf_importer_unmap_handle(int importer_fd, __u64 map_handle)
{
	struct upcie_dmabuf_unmap_req req = {0};

	if (importer_fd < 0 || !map_handle)
		return -EINVAL;

	req.map_handle = map_handle;
	return ioctl(importer_fd, UPCIE_DMABUF_UNMAP, &req) < 0 ? -errno : 0;
}

/*
 * Option-A: map an array of device-physical addresses (phys_lut) into the
 * IOMMU domain the target NVMe device currently uses. On success the IOMMU
 * translates 'iova_base + i * page_size' to 'phys[i]', so PRPs should be built
 * from iova_base, not from phys[].
 */
static inline int
upcie_dmabuf_importer_iommu_map(int importer_fd, const char *bdf, int dmabuf_fd,
				uint64_t iova_base, __u32 page_size, __u32 nphys,
				const uint64_t *phys, __u32 prot,
				uint64_t *map_handle_out)
{
	struct upcie_dmabuf_iommu_map_req req = {0};

	if (importer_fd < 0 || !bdf || !phys || !nphys)
		return -EINVAL;

	strncpy(req.bdf, bdf, sizeof(req.bdf) - 1);
	req.dmabuf_fd = dmabuf_fd;
	req.page_size = page_size;
	req.nphys = nphys;
	req.prot = prot;
	req.iova_base = iova_base;
	req.user_phys_ptr = (__u64)(uintptr_t)phys;

	if (ioctl(importer_fd, UPCIE_DMABUF_IOMMU_MAP, &req) < 0)
		return -errno;

	if (map_handle_out)
		*map_handle_out = req.map_handle;
	return 0;
}
#endif

#endif
