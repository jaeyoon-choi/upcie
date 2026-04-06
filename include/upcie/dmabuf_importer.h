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
 * @version 0.4.2
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

struct upcie_dmabuf_map_req {
	char bdf[UPCIE_DMABUF_BDF_LEN];
	__s32 dmabuf_fd;
	__u32 page_size;
	__u32 flags;
	__aligned_u64 user_lut_ptr;
	__u32 lut_capacity;
	__u32 nphys;
	__aligned_u64 map_handle;
};

struct upcie_dmabuf_unmap_req {
	__aligned_u64 map_handle;
};

#define UPCIE_DMABUF_IMPORTER_IOC_MAGIC 'u'

#define UPCIE_DMABUF_MAP _IOWR(UPCIE_DMABUF_IMPORTER_IOC_MAGIC, 0x01, \
			       struct upcie_dmabuf_map_req)
#define UPCIE_DMABUF_UNMAP _IOW(UPCIE_DMABUF_IMPORTER_IOC_MAGIC, 0x02, \
				struct upcie_dmabuf_unmap_req)

#ifndef __KERNEL__
struct upcie_dmabuf_import {
	int importer_fd;
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
#endif

#endif
