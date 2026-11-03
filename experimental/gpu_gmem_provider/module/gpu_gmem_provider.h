/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef _GPU_GMEM_PROVIDER_H
#define _GPU_GMEM_PROVIDER_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * GGP_NEW_FILE: wrap an NVIDIA dma-buf in a guest_memfd provider file.
 * The new file pins the dma-buf until it and every guest_memfd that uses
 * it are closed.
 */
struct ggp_new_file {
	__s32 dmabuf_fd;	/* in */
	__s32 fd;		/* out: provider file */
	__u64 size;		/* out: bytes, page aligned */
	__u32 nr_ranges;	/* out: BAR1 ranges behind the file */
	__u32 pad;
	__u64 first_phys;	/* out: BAR1 address of the first range */
};

#define GGP_NEW_FILE	_IOWR('G', 0x01, struct ggp_new_file)

#endif /* _GPU_GMEM_PROVIDER_H */
