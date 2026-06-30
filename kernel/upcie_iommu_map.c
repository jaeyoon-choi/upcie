// SPDX-License-Identifier: GPL-2.0-only
/*
 * upcie_iommu_map — install device-physical addresses into a device's live
 * IOMMU domain from userspace.
 *
 * Problem this solves: to do direct NVMe<->GPU P2P DMA under VFIO isolation,
 * userspace must make GPU memory reachable by the NVMe's DMA. VFIO's userspace
 * map API (VFIO_IOMMU_MAP_DMA) only accepts a pinnable host VA, so GPU memory
 * cannot be registered that way. We already know the GPU's physical addresses
 * (a phys_lut, e.g. from udmabuf); the missing step is putting them into the
 * IOMMU domain the NVMe actually translates through.
 *
 * This misc device exposes exactly that step: given a BDF, a phys_lut and a
 * userspace-chosen IOVA base, it looks up the device's *current* IOMMU domain
 * (for a vfio-pci device this is the VFIO/iommufd-owned domain) and installs
 * iommu_map(iova_base + i*ps -> phys[i]) entries into it. Userspace then writes
 * the IOVA into NVMe PRPs; the IOMMU translates it back to the GPU physical.
 *
 * ioctls (see include/upcie/iommu_map.h):
 *   UPCIE_IOMMU_MAP    install a phys_lut -> IOVA mapping, return a handle
 *   UPCIE_IOMMU_UNMAP  tear a mapping down by handle
 * Mappings are tracked per open fd and torn down on UNMAP or close().
 */

#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "../include/upcie/iommu_map.h"

/* One installed mapping; everything needed to tear it back down exactly. */
struct upcie_iommu_mapping {
	u64 handle;			/* opaque id handed to userspace */
	struct pci_dev *pdev;		/* target device (ref held) */
	struct iommu_domain *domain;	/* domain we mapped into (for unmap) */
	unsigned long iova_base;	/* start IOVA of the mapping */
	size_t mapped_size;		/* bytes actually mapped (unmap length) */
	struct dma_buf *held_dmabuf;	/* optional: keeps GPU memory alive */
	struct list_head node;
};

/* Per-open-fd state: the set of mappings created through this fd. */
struct upcie_iommu_map_ctx {
	struct mutex lock;
	struct list_head maps;
	u64 next_handle;
};

/* Tear down one mapping: iommu_unmap (if the domain still exists), drop the
 * dma_buf/pci refs, and free it. */
static void
upcie_iommu_mapping_destroy(struct upcie_iommu_mapping *map)
{
	if (!map)
		return;

	if (map->domain && map->mapped_size) {
		/*
		 * Only unmap if the device still uses the same domain. If
		 * userspace tore down VFIO first, that domain (and its page
		 * tables) is already gone, so touching it would be a
		 * use-after-free. Userspace is expected to UNMAP before closing
		 * its VFIO container.
		 */
		struct iommu_domain *cur = map->pdev ?
			iommu_get_domain_for_dev(&map->pdev->dev) : NULL;

		if (cur == map->domain)
			iommu_unmap(map->domain, map->iova_base, map->mapped_size);
		else
			pr_warn("upcie-iommu: domain changed before unmap, skipping iommu_unmap (handle=%llu)\n",
				map->handle);
	}
	if (map->held_dmabuf)
		dma_buf_put(map->held_dmabuf);
	if (map->pdev)
		pci_dev_put(map->pdev);
	kfree(map);
}

/* "DDDD:BB:SS.F" string -> struct pci_dev* (takes a ref via pci_get_...). */
static int
upcie_iommu_map_parse_bdf(const char *bdf, struct pci_dev **pdev_out)
{
	unsigned int domain, bus, slot, func;
	struct pci_dev *pdev;

	if (sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &slot, &func) != 4)
		return -EINVAL;
	if (bus > 0xff || slot > PCI_SLOT(~0) || func > PCI_FUNC(~0))
		return -EINVAL;

	pdev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(slot, func));
	if (!pdev)
		return -ENODEV;

	*pdev_out = pdev;
	return 0;
}

/* Look up a mapping by handle; caller must hold ctx->lock. */
static struct upcie_iommu_mapping *
upcie_iommu_map_find_map_locked(struct upcie_iommu_map_ctx *ctx, u64 handle)
{
	struct upcie_iommu_mapping *map;

	list_for_each_entry(map, &ctx->maps, node)
		if (map->handle == handle)
			return map;

	return NULL;
}

/* Remove a mapping by handle: unlink it from this fd's list and tear it down. */
static long
upcie_iommu_map_ioctl_unmap(struct file *file, unsigned long arg)
{
	struct upcie_iommu_map_ctx *ctx = file->private_data;
	struct upcie_iommu_unmap_req req;
	struct upcie_iommu_mapping *map;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&ctx->lock);
	map = upcie_iommu_map_find_map_locked(ctx, req.map_handle);
	if (!map) {
		mutex_unlock(&ctx->lock);
		return -ENOENT;
	}

	list_del(&map->node);
	mutex_unlock(&ctx->lock);

	upcie_iommu_mapping_destroy(map);
	return 0;
}

/*
 * Map a caller-provided list of device physical addresses (phys_lut)
 * into the IOMMU domain the target device already uses, and return the IOVA
 * base. For a VFIO-controlled NVMe this is the VFIO/iommufd-owned domain, i.e.
 * the exact translation context the device uses for userspace-driven I/O, so
 * the returned IOVA can be written directly into NVMe PRPs.
 */
static long
upcie_iommu_map_ioctl_map(struct file *file, unsigned long arg)
{
	struct upcie_iommu_map_ctx *ctx = file->private_data;
	struct upcie_iommu_map_req req;
	struct upcie_iommu_mapping *map = NULL;
	struct iommu_domain *domain = NULL;
	struct pci_dev *pdev = NULL;
	struct dma_buf *held = NULL;
	u64 *phys = NULL;
	size_t mapped = 0;
	unsigned long min_pagesz;
	char bdf[UPCIE_IOMMU_MAP_BDF_LEN];
	u64 handle;
	int prot;
	u32 i;
	int err;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req))) {
		pr_err("upcie-iommu: iommu_map copy_from_user(req) failed\n");
		return -EFAULT;
	}

	pr_debug("upcie-iommu: iommu_map req bdf=%.*s page_size=%u nphys=%u iova_base=0x%llx phys_ptr=0x%llx dmabuf_fd=%d prot=0x%x\n",
		(int)sizeof(req.bdf), req.bdf, req.page_size, req.nphys,
		req.iova_base, req.user_phys_ptr, req.dmabuf_fd, req.prot);

	if (!req.page_size || !is_power_of_2(req.page_size) ||
	    req.page_size < PAGE_SIZE)
		return -EINVAL;
	if (!req.nphys || !req.user_phys_ptr)
		return -EINVAL;
	if (req.reserved)
		return -EINVAL;
	if (req.prot & ~UPCIE_IOMMU_MAP_PROT_MASK)
		return -EINVAL;
	if ((req.prot & UPCIE_IOMMU_MAP_PROT_MMIO) &&
	    (req.prot & UPCIE_IOMMU_MAP_PROT_CACHE))
		return -EINVAL;
	if (!IS_ALIGNED(req.iova_base, req.page_size))
		return -EINVAL;
	if (req.nphys > U32_MAX / req.page_size)
		return -EOVERFLOW;

	memcpy(bdf, req.bdf, sizeof(bdf));
	bdf[sizeof(bdf) - 1] = '\0';

	err = upcie_iommu_map_parse_bdf(bdf, &pdev);
	if (err)
		return err;

	/*
	 * The device must already be attached to an IOMMU domain. For a
	 * VFIO-controlled NVMe this returns the VFIO/iommufd-owned domain, i.e.
	 * the exact translation context the device uses for userspace I/O.
	 */
	domain = iommu_get_domain_for_dev(&pdev->dev);
	if (!domain) {
		pr_err("upcie-iommu: no IOMMU domain for %s (device not behind IOMMU / not VFIO-bound?)\n",
		       bdf);
		err = -ENODEV;
		goto err_unwind;
	}
	pr_debug("upcie-iommu: domain=%p type=%u aperture=[0x%llx..0x%llx] pgsize_bitmap=0x%lx for %s\n",
		domain, domain->type, domain->geometry.aperture_start,
		domain->geometry.aperture_end, domain->pgsize_bitmap, bdf);

	if (!domain->pgsize_bitmap) {
		pr_err("upcie-iommu: domain for %s has no supported page sizes\n",
		       bdf);
		err = -EOPNOTSUPP;
		goto err_unwind;
	}
	min_pagesz = 1UL << __ffs(domain->pgsize_bitmap);
	if (req.page_size < min_pagesz) {
		pr_err("upcie-iommu: page_size=%u smaller than domain minimum=%lu for %s (pgsize_bitmap=0x%lx)\n",
		       req.page_size, min_pagesz, bdf, domain->pgsize_bitmap);
		err = -EOPNOTSUPP;
		goto err_unwind;
	}

	/* Intel VT-d returns -EFAULT when iova+size exceeds the domain address
	 * width; pre-check against the aperture so misuse is obvious. */
	{
		u64 range_end = (u64)req.iova_base +
				(u64)req.nphys * req.page_size - 1;

		if (domain->geometry.aperture_end &&
		    range_end > domain->geometry.aperture_end) {
			pr_err("upcie-iommu: iova range [0x%llx..0x%llx] exceeds domain aperture_end=0x%llx\n",
			       req.iova_base, range_end,
			       domain->geometry.aperture_end);
			err = -ERANGE;
			goto err_unwind;
		}
	}

	if (!req.prot) {
		prot = IOMMU_READ | IOMMU_WRITE;
	} else {
		prot = 0;
		if (req.prot & UPCIE_IOMMU_MAP_PROT_READ)
			prot |= IOMMU_READ;
		if (req.prot & UPCIE_IOMMU_MAP_PROT_WRITE)
			prot |= IOMMU_WRITE;
	}
	/*
	 * The memory type describes the target PA, so the caller must select it.
	 * The CUDA path requests MMIO because its LUT contains BAR1 bus addresses
	 * verified against the GPU's PCI resource1; the host-RAM control requests
	 * CACHE instead. This matches the DMA API's resource-vs-RAM distinction.
	 * Intel VT-d ignores these two input bits, while Arm IOMMUs use them to
	 * select device or cacheable-normal page-table attributes.
	 */
	if (req.prot & UPCIE_IOMMU_MAP_PROT_MMIO)
		prot |= IOMMU_MMIO;
	if (req.prot & UPCIE_IOMMU_MAP_PROT_CACHE)
		prot |= IOMMU_CACHE;

	phys = kvmalloc_array(req.nphys, sizeof(*phys), GFP_KERNEL);
	if (!phys) {
		err = -ENOMEM;
		goto err_unwind;
	}
	if (copy_from_user(phys, (void __user *)(uintptr_t)req.user_phys_ptr,
			   (size_t)req.nphys * sizeof(*phys))) {
		pr_err("upcie-iommu: iommu_map copy_from_user(phys[%u]) from 0x%llx failed\n",
		       req.nphys, req.user_phys_ptr);
		err = -EFAULT;
		goto err_unwind;
	}

	if (req.dmabuf_fd >= 0) {
		held = dma_buf_get(req.dmabuf_fd);
		if (IS_ERR(held)) {
			err = PTR_ERR(held);
			pr_err("upcie-iommu: dma_buf_get(fd=%d) failed err=%d\n",
			       req.dmabuf_fd, err);
			held = NULL;
			goto err_unwind;
		}
	}

	for (i = 0; i < req.nphys; i++) {
		unsigned long iova = (unsigned long)req.iova_base +
				     (size_t)i * req.page_size;
		phys_addr_t existing = iommu_iova_to_phys(domain, iova);

		if (existing) {
			pr_err("upcie-iommu: iova=0x%lx already maps to phys=0x%llx at idx=%u\n",
			       iova, (u64)existing, i);
			err = -EEXIST;
			goto err_unwind;
		}

		if (!IS_ALIGNED(phys[i], req.page_size)) {
			err = -ERANGE;
			goto err_unwind;
		}

		/*
		 * AiSIO supports Linux 6.8 and newer.  Pre-6.3 iommu_map() lacks
		 * the GFP argument and is intentionally unsupported here.
		 */
		err = iommu_map(domain, iova, (phys_addr_t)phys[i],
				req.page_size, prot, GFP_KERNEL);
		if (err) {
			pr_err("upcie-iommu: iommu_map(iova=0x%lx phys=0x%llx size=%u prot=%d) failed err=%d at idx=%u\n",
			       iova, phys[i], req.page_size, prot, err, i);
			goto err_unwind;
		}

		mapped += req.page_size;
	}
	pr_debug("upcie-iommu: iommu_map ok: %u pages, iova 0x%llx..0x%llx -> gpu\n",
		req.nphys, req.iova_base,
		req.iova_base + (u64)req.nphys * req.page_size);

	/* Read the page table back through the IOMMU API to prove the mapping is
	 * actually installed and translates to the expected GPU physical. */
	{
		u64 last_page_iova = req.iova_base +
				     (u64)(req.nphys - 1) * req.page_size;
		phys_addr_t p0 = iommu_iova_to_phys(domain,
						    (unsigned long)req.iova_base);
		phys_addr_t pn = iommu_iova_to_phys(domain,
						    (unsigned long)last_page_iova);

		pr_debug("upcie-iommu: verify iova_to_phys: 0x%llx->0x%llx (exp 0x%llx), 0x%llx->0x%llx (exp 0x%llx)\n",
			req.iova_base, (u64)p0, phys[0],
			last_page_iova, (u64)pn, phys[req.nphys - 1]);
		if (p0 != (phys_addr_t)phys[0])
			pr_warn("upcie-iommu: MAPPING MISMATCH at base — page table not as expected!\n");
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		err = -ENOMEM;
		goto err_unwind;
	}

	mutex_lock(&ctx->lock);
	handle = ++ctx->next_handle;
	if (!handle)
		handle = ++ctx->next_handle;
	map->handle = handle;
	map->pdev = pdev;
	map->domain = domain;
	map->iova_base = (unsigned long)req.iova_base;
	map->mapped_size = mapped;
	map->held_dmabuf = held;
	list_add_tail(&map->node, &ctx->maps);
	mutex_unlock(&ctx->lock);

	req.map_handle = handle;
	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		pr_err("upcie-iommu: iommu_map copy_to_user(req) failed\n");
		mutex_lock(&ctx->lock);
		list_del(&map->node);
		mutex_unlock(&ctx->lock);
		/* destroy owns pdev/held/mapping teardown from here */
		upcie_iommu_mapping_destroy(map);
		kvfree(phys);
		return -EFAULT;
	}

	kvfree(phys);
	return 0;

err_unwind:
	if (mapped)
		iommu_unmap(domain, (unsigned long)req.iova_base, mapped);
	if (held)
		dma_buf_put(held);
	if (pdev)
		pci_dev_put(pdev);
	kvfree(phys);
	return err;
}

static long
upcie_iommu_map_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case UPCIE_IOMMU_UNMAP:
		return upcie_iommu_map_ioctl_unmap(file, arg);
	case UPCIE_IOMMU_MAP:
		return upcie_iommu_map_ioctl_map(file, arg);
	default:
		return -ENOTTY;
	}
}

static int
upcie_iommu_map_chrdev_open(struct inode *inode, struct file *file)
{
	struct upcie_iommu_map_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->maps);
	file->private_data = ctx;
	return 0;
}

static int
upcie_iommu_map_chrdev_release(struct inode *inode, struct file *file)
{
	struct upcie_iommu_map_ctx *ctx = file->private_data;
	struct upcie_iommu_mapping *map, *tmp;

	if (!ctx)
		return 0;

	list_for_each_entry_safe(map, tmp, &ctx->maps, node) {
		list_del(&map->node);
		upcie_iommu_mapping_destroy(map);
	}

	kfree(ctx);
	return 0;
}

static const struct file_operations upcie_iommu_map_fops = {
	.owner = THIS_MODULE,
	.open = upcie_iommu_map_chrdev_open,
	.release = upcie_iommu_map_chrdev_release,
	.unlocked_ioctl = upcie_iommu_map_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = upcie_iommu_map_ioctl,
#endif
};

static struct miscdevice upcie_iommu_map_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "upcie-iommu-map",
	.fops = &upcie_iommu_map_fops,
	.mode = 0600,
};

static int __init
upcie_iommu_map_init(void)
{
	return misc_register(&upcie_iommu_map_misc);
}

static void __exit
upcie_iommu_map_exit(void)
{
	misc_deregister(&upcie_iommu_map_misc);
}

module_init(upcie_iommu_map_init);
module_exit(upcie_iommu_map_exit);

MODULE_AUTHOR("Jaeyoon Choi <j_yoon.choi@samsung.com>");
MODULE_DESCRIPTION("uPCIe helper: map GPU phys_lut into a device's VFIO IOMMU domain");
MODULE_LICENSE("GPL");

/*
 * MODULE_IMPORT_NS() stringifies its argument through 6.12 but expects a
 * string literal from 6.13 onward. Keep this helper buildable across both.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
