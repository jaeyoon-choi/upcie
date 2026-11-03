// SPDX-License-Identifier: GPL-2.0-only
/*
 * guest_memfd PFN mode provider backed by an NVIDIA dma-buf.
 *
 * PoC only: it lets a process outside any guest map CUDA memory through
 * IOMMU_IOAS_MAP_FILE on a guest_memfd. The BAR1 ranges are fixed for the
 * life of the file, so the provider never revokes or restores frames.
 */
#include <linux/anon_inodes.h>
#include <linux/dma-buf.h>
#include <linux/file.h>
#include <linux/guest_memfd.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pgtable.h>
#include <linux/slab.h>

#include "gpu_gmem_provider.h"

/* Exported by the patched nvidia.ko (nv-dmabuf.c). */
struct nv_dma_buf_bar1_range {
	u64 start;
	u64 size;
};
int nv_dma_buf_get_bar1_ranges(struct dma_buf *buf,
			       struct nv_dma_buf_bar1_range **ranges,
			       unsigned int *count);
void nv_dma_buf_put_bar1_ranges(struct dma_buf *buf,
				struct nv_dma_buf_bar1_range *ranges);

struct ggp_range {
	pgoff_t first;		/* file page index of the range */
	unsigned long npages;
	unsigned long base_pfn;
};

struct ggp_info {
	struct mutex lock;		/* gmem_inode */
	struct dma_buf *dmabuf;
	struct nv_dma_buf_bar1_range *bar1;
	struct ggp_range *ranges;
	unsigned int nr_ranges;
	unsigned long npages;
	struct inode *gmem_inode;
};

static const struct ggp_range *ggp_find(struct ggp_info *info, pgoff_t index)
{
	unsigned int lo = 0, hi = info->nr_ranges;

	while (lo < hi) {
		unsigned int mid = lo + (hi - lo) / 2;
		const struct ggp_range *r = &info->ranges[mid];

		if (index < r->first)
			hi = mid;
		else if (index >= r->first + r->npages)
			lo = mid + 1;
		else
			return r;
	}
	return NULL;
}

static void *ggp_gmem_attach(struct inode *gmem_inode,
			     struct file *resource_file)
{
	struct ggp_info *info = resource_file->private_data;

	mutex_lock(&info->lock);
	if (info->gmem_inode) {
		mutex_unlock(&info->lock);
		return ERR_PTR(-EBUSY);
	}
	info->gmem_inode = gmem_inode;
	mutex_unlock(&info->lock);

	return get_file(resource_file);
}

static void ggp_gmem_release(void *provider)
{
	struct file *resource_file = provider;
	struct ggp_info *info = resource_file->private_data;

	mutex_lock(&info->lock);
	info->gmem_inode = NULL;
	mutex_unlock(&info->lock);

	fput(resource_file);
}

static int ggp_gmem_get_pfn(void *provider, pgoff_t index, unsigned long *pfn,
			    int *max_order, u32 *attrs)
{
	struct file *resource_file = provider;
	struct ggp_info *info = resource_file->private_data;
	const struct ggp_range *r = ggp_find(info, index);
	int order;

	if (!r)
		return -EINVAL;

	*pfn = r->base_pfn + (index - r->first);
	if (!max_order)
		return 0;

	/* The largest aligned block around @index that stays in @r. */
	for (order = PUD_ORDER; order; order--) {
		unsigned long first = ALIGN_DOWN(index, 1UL << order);

		if (first >= r->first &&
		    first + (1UL << order) <= r->first + r->npages)
			break;
	}
	*max_order = order;
	return 0;
}

static int ggp_gmem_lookup_pfn_range(void *provider, pgoff_t index,
				     unsigned long *pfn, size_t *nr_pages)
{
	struct file *resource_file = provider;
	struct ggp_info *info = resource_file->private_data;
	const struct ggp_range *r = ggp_find(info, index);

	if (!r)
		return -EINVAL;

	*pfn = r->base_pfn + (index - r->first);
	*nr_pages = r->first + r->npages - index;
	return 0;
}

/* CUDA owns the contents, so frames are neither zeroed nor freed here. */
static void ggp_gmem_put_pfn_range(void *provider, pgoff_t index,
				   size_t nr_pages)
{
}

static const struct guest_memfd_provider_operations ggp_gmem_provider_ops = {
	.attach		= ggp_gmem_attach,
	.release	= ggp_gmem_release,
	.get_pfn	= ggp_gmem_get_pfn,
	.lookup_pfn_range = ggp_gmem_lookup_pfn_range,
	.put_pfn_range	= ggp_gmem_put_pfn_range,
};

static int ggp_file_release(struct inode *inode, struct file *file)
{
	struct ggp_info *info = file->private_data;

	nv_dma_buf_put_bar1_ranges(info->dmabuf, info->bar1);
	dma_buf_put(info->dmabuf);
	kvfree(info->ranges);
	kfree(info);
	return 0;
}

static const struct file_operations ggp_file_fops = {
	.owner		= THIS_MODULE,
	.release	= ggp_file_release,
	.gmem_provider_ops = &ggp_gmem_provider_ops,
};

static long ggp_new_file(void __user *uarg)
{
	struct ggp_new_file nf;
	struct ggp_info *info;
	struct file *file;
	unsigned int i;
	pgoff_t first = 0;
	int fd, ret;

	if (copy_from_user(&nf, uarg, sizeof(nf)))
		return -EFAULT;
	if (nf.pad)
		return -EINVAL;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	mutex_init(&info->lock);

	info->dmabuf = dma_buf_get(nf.dmabuf_fd);
	if (IS_ERR(info->dmabuf)) {
		ret = PTR_ERR(info->dmabuf);
		goto err_free;
	}

	ret = nv_dma_buf_get_bar1_ranges(info->dmabuf, &info->bar1,
					 &info->nr_ranges);
	if (ret)
		goto err_put_dmabuf;

	ret = -EINVAL;
	if (!info->nr_ranges)
		goto err_put_bar1;

	info->ranges = kvcalloc(info->nr_ranges, sizeof(*info->ranges),
				GFP_KERNEL);
	if (!info->ranges) {
		ret = -ENOMEM;
		goto err_put_bar1;
	}

	for (i = 0; i < info->nr_ranges; i++) {
		u64 start = info->bar1[i].start, size = info->bar1[i].size;

		if (!PAGE_ALIGNED(start) || !size || !PAGE_ALIGNED(size)) {
			ret = -EINVAL;
			goto err_free_ranges;
		}
		info->ranges[i].first = first;
		info->ranges[i].npages = size >> PAGE_SHIFT;
		info->ranges[i].base_pfn = PHYS_PFN(start);
		first += info->ranges[i].npages;
	}
	info->npages = first;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto err_free_ranges;
	}

	file = anon_inode_getfile("[gpu-gmem-provider]", &ggp_file_fops, info,
				  O_RDWR);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err_put_fd;
	}

	nf.fd = fd;
	nf.size = (u64)info->npages << PAGE_SHIFT;
	nf.nr_ranges = info->nr_ranges;
	nf.first_phys = info->bar1[0].start;
	if (copy_to_user(uarg, &nf, sizeof(nf))) {
		/* Closing the file drops the dma-buf through ggp_file_release(). */
		fput(file);
		put_unused_fd(fd);
		return -EFAULT;
	}

	fd_install(fd, file);
	return 0;

err_put_fd:
	put_unused_fd(fd);
err_free_ranges:
	kvfree(info->ranges);
err_put_bar1:
	nv_dma_buf_put_bar1_ranges(info->dmabuf, info->bar1);
err_put_dmabuf:
	dma_buf_put(info->dmabuf);
err_free:
	kfree(info);
	return ret;
}

static long ggp_ctl_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	switch (cmd) {
	case GGP_NEW_FILE:
		return ggp_new_file((void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ggp_ctl_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= ggp_ctl_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice ggp_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "gpu_gmem_provider",
	.fops	= &ggp_ctl_fops,
};

module_misc_device(ggp_misc);

MODULE_DESCRIPTION("guest_memfd provider for NVIDIA dma-bufs (PoC)");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL");
