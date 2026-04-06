// SPDX-License-Identifier: GPL-2.0-only

#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/memremap.h>
#include <linux/mmzone.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#include <linux/pci-p2pdma.h>
#else
#include <linux/dma-map-ops.h>
#include <linux/pci-p2pdma.h>
#endif

#include "../include/upcie/dmabuf_importer.h"

struct upcie_dmabuf_map {
	u64 handle;
	u32 page_size;
	u32 nphys;
	struct pci_dev *pdev;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct list_head node;
};

struct upcie_dmabuf_ctx {
	struct mutex lock;
	struct list_head maps;
	u64 next_handle;
};

/*
 * Mirror of drivers/pci/p2pdma.c's internal pagemap wrapper so the debug
 * helper can recover the provider device from a PCI_P2PDMA page.
 */
struct upcie_dmabuf_p2pdma_pagemap {
	struct pci_dev *provider;
	u64 bus_offset;
	struct dev_pagemap pgmap;
};

static void
upcie_dmabuf_map_destroy(struct upcie_dmabuf_map *map)
{
	if (!map)
		return;

	if (map->sgt)
		dma_buf_unmap_attachment(map->attach, map->sgt,
					 DMA_BIDIRECTIONAL);
	if (map->attach)
		dma_buf_detach(map->dmabuf, map->attach);
	if (map->dmabuf)
		dma_buf_put(map->dmabuf);
	if (map->pdev)
		pci_dev_put(map->pdev);

	kfree(map);
}

static int
upcie_dmabuf_parse_bdf(const char *bdf, struct pci_dev **pdev_out)
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

static int
upcie_dmabuf_validate_target(struct pci_dev *pdev)
{
	if (!pdev)
		return -EINVAL;

	/*
	 * This helper relies on the kernel DMA API through dma-buf attachment
	 * mapping. Devices such as vfio-pci set driver_managed_dma because DMA
	 * ownership is intentionally handed to userspace, so the kernel DMA API
	 * mapping we create here will not match the DMA context used by the
	 * device during userspace-driven I/O.
	 */
	if (pdev->driver && pdev->driver->driver_managed_dma) {
		dev_warn(&pdev->dev,
			 "driver '%s' uses driver-managed DMA; kernel dma-buf importer mappings are not valid for this device context\n",
			 pdev->driver->name);
		return -EOPNOTSUPP;
	}

	return 0;
}

static int
upcie_dmabuf_count_pages(struct sg_table *sgt, u32 page_size, u32 *nphys_out)
{
	struct scatterlist *sg;
	u64 nphys = 0;
	int i;

	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		dma_addr_t addr = sg_dma_address(sg);
		unsigned int len = sg_dma_len(sg);

		if (!len)
			continue;
		if (!IS_ALIGNED(addr, page_size) || !IS_ALIGNED(len, page_size))
			return -ERANGE;

		nphys += len / page_size;
		if (nphys > U32_MAX)
			return -EOVERFLOW;
	}

	*nphys_out = (u32)nphys;
	return 0;
}

static void
upcie_dmabuf_accumulate_map_type(struct upcie_dmabuf_map_req *req,
				 enum pci_p2pdma_map_type map_type)
{
	switch (map_type) {
	case PCI_P2PDMA_MAP_BUS_ADDR:
		req->map_bus_addr_nents++;
		break;
	case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:
		req->map_thru_host_bridge_nents++;
		break;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
	case PCI_P2PDMA_MAP_NONE:
		req->map_none_nents++;
		break;
#endif
	case PCI_P2PDMA_MAP_NOT_SUPPORTED:
		req->map_not_supported_nents++;
		break;
	default:
		req->map_none_nents++;
		break;
	}
}

static struct pci_dev *
upcie_dmabuf_p2pdma_provider(struct page *page)
{
	struct dev_pagemap *pgmap;
	struct upcie_dmabuf_p2pdma_pagemap *p2p_pgmap;

	if (!page || !is_pci_p2pdma_page(page))
		return NULL;

	pgmap = page->pgmap;
	if (!pgmap)
		return NULL;

	p2p_pgmap = container_of(pgmap, struct upcie_dmabuf_p2pdma_pagemap, pgmap);
	return p2p_pgmap->provider;
}

static enum pci_p2pdma_map_type
upcie_dmabuf_get_map_type(struct pci_p2pdma_map_state *state,
			  struct device *dev, struct scatterlist *sg)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
	return pci_p2pdma_state(state, dev, sg_page(sg));
#else
	struct page *page = sg_page(sg);
	struct pci_dev *provider;

	if (!page || !is_pci_p2pdma_page(page))
		return PCI_P2PDMA_MAP_NOT_SUPPORTED;

	if (sg_dma_is_bus_address(sg))
		return PCI_P2PDMA_MAP_BUS_ADDR;

	provider = upcie_dmabuf_p2pdma_provider(page);
	if (provider && pci_p2pdma_distance(provider, dev, false) >= 0)
		return PCI_P2PDMA_MAP_THRU_HOST_BRIDGE;

	return PCI_P2PDMA_MAP_NOT_SUPPORTED;
#endif
}

static void
upcie_dmabuf_describe_segments(struct pci_dev *pdev, struct sg_table *sgt,
			       struct upcie_dmabuf_map_req *req)
{
	struct pci_p2pdma_map_state p2pdma_state = {0};
	struct scatterlist *sg;
	u32 bus_nents = 0;
	u32 p2pdma_page_nents = 0;
	int i;

	if (!pdev || !sgt || !req)
		return;

	req->sg_nents = sgt->nents;
	req->bus_nents = 0;
	req->p2pdma_page_nents = 0;
	req->map_none_nents = 0;
	req->map_bus_addr_nents = 0;
	req->map_thru_host_bridge_nents = 0;
	req->map_not_supported_nents = 0;
	req->flags = 0;

	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		struct page *page = sg_page(sg);

		if (sg_dma_is_bus_address(sg))
			bus_nents++;

		if (page && is_pci_p2pdma_page(page))
			p2pdma_page_nents++;

		if (page && is_pci_p2pdma_page(page)) {
			upcie_dmabuf_accumulate_map_type(req,
							 upcie_dmabuf_get_map_type(&p2pdma_state,
									   &pdev->dev, sg));
		} else {
			req->map_none_nents++;
		}
	}

	req->bus_nents = bus_nents;
	req->p2pdma_page_nents = p2pdma_page_nents;
	if (bus_nents)
		req->flags |= UPCIE_DMABUF_MAP_F_ANY_BUS_ADDR;
	if (bus_nents == sgt->nents)
		req->flags |= UPCIE_DMABUF_MAP_F_ALL_BUS_ADDR;
}

static int
upcie_dmabuf_fill_lut(struct sg_table *sgt, u32 page_size, u32 nphys,
		      u64 *lut)
{
	struct scatterlist *sg;
	u32 idx = 0;
	int i;

	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		dma_addr_t addr = sg_dma_address(sg);
		unsigned int len = sg_dma_len(sg);
		unsigned int off;

		if (!len)
			continue;

		for (off = 0; off < len; off += page_size) {
			if (idx >= nphys)
				return -EOVERFLOW;
			lut[idx++] = addr + off;
		}
	}

	return idx == nphys ? 0 : -EINVAL;
}

static struct upcie_dmabuf_map *
upcie_dmabuf_find_map_locked(struct upcie_dmabuf_ctx *ctx, u64 handle)
{
	struct upcie_dmabuf_map *map;

	list_for_each_entry(map, &ctx->maps, node)
		if (map->handle == handle)
			return map;

	return NULL;
}

static long
upcie_dmabuf_ioctl_map(struct file *file, unsigned long arg)
{
	struct upcie_dmabuf_ctx *ctx = file->private_data;
	struct upcie_dmabuf_map_req req;
	struct upcie_dmabuf_map *map = NULL;
	struct pci_dev *pdev = NULL;
	struct dma_buf *dmabuf = NULL;
	struct dma_buf_attachment *attach = NULL;
	struct sg_table *sgt = NULL;
	u64 *lut = NULL;
	u64 handle;
	char bdf[UPCIE_DMABUF_BDF_LEN];
	int err;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (!req.page_size || !is_power_of_2(req.page_size) ||
	    req.page_size < PAGE_SIZE)
		return -EINVAL;

	memcpy(bdf, req.bdf, sizeof(bdf));
	bdf[sizeof(bdf) - 1] = '\0';

	err = upcie_dmabuf_parse_bdf(bdf, &pdev);
	if (err)
		return err;

	err = upcie_dmabuf_validate_target(pdev);
	if (err)
		goto out;

	dmabuf = dma_buf_get(req.dmabuf_fd);
	if (IS_ERR(dmabuf)) {
		err = PTR_ERR(dmabuf);
		dmabuf = NULL;
		goto out;
	}

	attach = dma_buf_attach(dmabuf, &pdev->dev);
	if (IS_ERR(attach)) {
		err = PTR_ERR(attach);
		attach = NULL;
		goto out;
	}

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		err = PTR_ERR(sgt);
		sgt = NULL;
		goto out;
	}

	err = upcie_dmabuf_count_pages(sgt, req.page_size, &req.nphys);
	if (err)
		goto out;

	upcie_dmabuf_describe_segments(pdev, sgt, &req);

	if (!req.user_lut_ptr || req.lut_capacity < req.nphys) {
		err = copy_to_user((void __user *)arg, &req, sizeof(req)) ?
		      -EFAULT : -ENOSPC;
		goto out;
	}

	lut = kcalloc(req.nphys, sizeof(*lut), GFP_KERNEL);
	if (!lut) {
		err = -ENOMEM;
		goto out;
	}

	err = upcie_dmabuf_fill_lut(sgt, req.page_size, req.nphys, lut);
	if (err)
		goto out;

	if (copy_to_user((void __user *)(uintptr_t)req.user_lut_ptr, lut,
			 req.nphys * sizeof(*lut))) {
		err = -EFAULT;
		goto out;
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map) {
		err = -ENOMEM;
		goto out;
	}

	mutex_lock(&ctx->lock);
	handle = ++ctx->next_handle;
	if (!handle)
		handle = ++ctx->next_handle;
	map->handle = handle;
	map->page_size = req.page_size;
	map->nphys = req.nphys;
	map->pdev = pdev;
	map->dmabuf = dmabuf;
	map->attach = attach;
	map->sgt = sgt;
	list_add_tail(&map->node, &ctx->maps);
	mutex_unlock(&ctx->lock);

	req.map_handle = handle;
	err = copy_to_user((void __user *)arg, &req, sizeof(req)) ? -EFAULT : 0;
	if (!err)
		goto out_keep;

	mutex_lock(&ctx->lock);
	list_del(&map->node);
	mutex_unlock(&ctx->lock);
	upcie_dmabuf_map_destroy(map);
	map = NULL;

out:
	kfree(lut);
	if (!map) {
		if (sgt)
			dma_buf_unmap_attachment(attach, sgt,
						 DMA_BIDIRECTIONAL);
		if (attach)
			dma_buf_detach(dmabuf, attach);
		if (dmabuf)
			dma_buf_put(dmabuf);
		if (pdev)
			pci_dev_put(pdev);
	}
	return err;

out_keep:
	kfree(lut);
	return 0;
}

static long
upcie_dmabuf_ioctl_unmap(struct file *file, unsigned long arg)
{
	struct upcie_dmabuf_ctx *ctx = file->private_data;
	struct upcie_dmabuf_unmap_req req;
	struct upcie_dmabuf_map *map;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&ctx->lock);
	map = upcie_dmabuf_find_map_locked(ctx, req.map_handle);
	if (!map) {
		mutex_unlock(&ctx->lock);
		return -ENOENT;
	}

	list_del(&map->node);
	mutex_unlock(&ctx->lock);

	upcie_dmabuf_map_destroy(map);
	return 0;
}

static long
upcie_dmabuf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case UPCIE_DMABUF_MAP:
		return upcie_dmabuf_ioctl_map(file, arg);
	case UPCIE_DMABUF_UNMAP:
		return upcie_dmabuf_ioctl_unmap(file, arg);
	default:
		return -ENOTTY;
	}
}

static int
upcie_dmabuf_open(struct inode *inode, struct file *file)
{
	struct upcie_dmabuf_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->maps);
	file->private_data = ctx;
	return 0;
}

static int
upcie_dmabuf_release(struct inode *inode, struct file *file)
{
	struct upcie_dmabuf_ctx *ctx = file->private_data;
	struct upcie_dmabuf_map *map, *tmp;

	if (!ctx)
		return 0;

	list_for_each_entry_safe(map, tmp, &ctx->maps, node) {
		list_del(&map->node);
		upcie_dmabuf_map_destroy(map);
	}

	kfree(ctx);
	return 0;
}

static const struct file_operations upcie_dmabuf_fops = {
	.owner = THIS_MODULE,
	.open = upcie_dmabuf_open,
	.release = upcie_dmabuf_release,
	.unlocked_ioctl = upcie_dmabuf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = upcie_dmabuf_ioctl,
#endif
};

static struct miscdevice upcie_dmabuf_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "upcie-dmabuf-importer",
	.fops = &upcie_dmabuf_fops,
	.mode = 0600,
};

static int __init
upcie_dmabuf_init(void)
{
	return misc_register(&upcie_dmabuf_misc);
}

static void __exit
upcie_dmabuf_exit(void)
{
	misc_deregister(&upcie_dmabuf_misc);
}

module_init(upcie_dmabuf_init);
module_exit(upcie_dmabuf_exit);

MODULE_AUTHOR("OpenAI Codex");
MODULE_DESCRIPTION("uPCIe dma-buf importer helper for NVMe-target DMA LUTs");
MODULE_LICENSE("GPL");

/*
 * MODULE_IMPORT_NS() stringifies its argument in some 6.8-based header trees
 * but expects a string literal in newer upstream-style headers. Keep this
 * helper buildable across both.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
