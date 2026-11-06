// SPDX-License-Identifier: GPL-2.0-only
/* Standard DMA-BUF imports, owned by a file context, independent of fd reuse. */
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include "w202_vpu_buffers.h"

struct w202_vpu_mapping {
	struct list_head list;
	struct dma_buf *buf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t iova;
	size_t size;
};

static void mapping_destroy(struct w202_vpu_mapping *m)
{
	if (m->sgt)
		dma_buf_unmap_attachment_unlocked(m->attach, m->sgt, DMA_BIDIRECTIONAL);
	if (m->attach)
		dma_buf_detach(m->buf, m->attach);
	if (m->buf)
		dma_buf_put(m->buf);
	kfree(m);
}

void w202_vpu_buffers_init(struct w202_vpu_buffers *ctx, struct device *dev)
{
	ctx->dev = get_device(dev);
	mutex_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->maps);
	ctx->count = 0;
}

void w202_vpu_buffers_destroy(struct w202_vpu_buffers *ctx)
{
	struct w202_vpu_mapping *m, *tmp;

	mutex_lock(&ctx->lock);
	list_for_each_entry_safe(m, tmp, &ctx->maps, list) {
		list_del(&m->list);
		mapping_destroy(m);
	}
	ctx->count = 0;
	mutex_unlock(&ctx->lock);
	put_device(ctx->dev);
}

int w202_vpu_buffers_map(struct w202_vpu_buffers *ctx, struct w202_vpu_iova *map)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(ctx->dev);
	struct w202_vpu_mapping *m;
	struct scatterlist *sg;
	dma_addr_t next, end;
	size_t total = 0;
	unsigned int i;
	int ret;

	if (map->fd < 0 || map->need_cache_sync > 1)
		return -EINVAL;
	if (!domain || !iommu_is_dma_domain(domain))
		return -ENODEV;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	mutex_lock(&ctx->lock);
	if (ctx->count >= 64) {
		ret = -ENOSPC;
		goto fail;
	}
	m->buf = dma_buf_get(map->fd);
	if (IS_ERR(m->buf)) {
		ret = PTR_ERR(m->buf);
		m->buf = NULL;
		goto fail;
	}
	m->size = m->buf->size;
	if (!m->size || m->size > SZ_256M || !IS_ALIGNED(m->size, PAGE_SIZE)) {
		ret = -EINVAL;
		goto fail;
	}
	m->attach = dma_buf_attach(m->buf, ctx->dev);
	if (IS_ERR(m->attach)) {
		ret = PTR_ERR(m->attach);
		m->attach = NULL;
		goto fail;
	}
	m->sgt = dma_buf_map_attachment_unlocked(m->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(m->sgt)) {
		ret = PTR_ERR(m->sgt);
		m->sgt = NULL;
		goto fail;
	}
	if (!m->sgt->nents) {
		ret = -EIO;
		goto fail;
	}
	m->iova = sg_dma_address(m->sgt->sgl);
	next = m->iova;
	for_each_sgtable_dma_sg(m->sgt, sg, i) {
		/* The register ABI requires one continuous device address span. */
		if (!sg_dma_len(sg) || sg_dma_address(sg) != next ||
		    check_add_overflow(next, (dma_addr_t)sg_dma_len(sg), &next) ||
		    check_add_overflow(total, (size_t)sg_dma_len(sg), &total)) {
			ret = -ERANGE;
			goto fail;
		}
	}
	if (total != m->size || check_add_overflow(m->iova, (dma_addr_t)total - 1, &end) ||
	    m->iova < domain->geometry.aperture_start || end > domain->geometry.aperture_end) {
		ret = -ERANGE;
		goto fail;
	}
	list_add_tail(&m->list, &ctx->maps);
	ctx->count++;
	map->size = m->size;
	map->iova_addr = m->iova;
	/* Do not return uninitialised ABI padding. */
	map->reserved0 = 0;
	memset(map->reserved, 0, sizeof(map->reserved));
	mutex_unlock(&ctx->lock);
	return 0;
fail:
	mapping_destroy(m);
	mutex_unlock(&ctx->lock);
	return ret;
}

int w202_vpu_buffers_unmap(struct w202_vpu_buffers *ctx, const struct w202_vpu_iova *map)
{
	struct w202_vpu_mapping *m;
	int ret = -ENOENT;

	mutex_lock(&ctx->lock);
	list_for_each_entry(m, &ctx->maps, list) {
		if (m->iova == map->iova_addr && m->size == map->size) {
			list_del(&m->list);
			ctx->count--;
			/* Use saved attachment/direction, never the untrusted FREE flag/fd. */
			mapping_destroy(m);
			ret = 0;
			break;
		}
	}
	mutex_unlock(&ctx->lock);
	return ret;
}

/* Research acceptance: compare the exporter SG physical pages with live VAU PTEs.
 * No private exporter/ION pointer is inspected. This submits no codec DMA job.
 */
int w202_vpu_buffers_verify(struct w202_vpu_buffers *ctx, const struct w202_vpu_iova *map,
			    unsigned int *pages, unsigned int *high_pages)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(ctx->dev);
	struct w202_vpu_mapping *m;
	struct scatterlist *sg;
	size_t position, offset;
	phys_addr_t pa;
	unsigned int i;
	int ret = -ENOENT;

	*pages = *high_pages = 0;
	if (!domain)
		return -ENODEV;
	mutex_lock(&ctx->lock);
	list_for_each_entry(m, &ctx->maps, list) {
		if (m->iova != map->iova_addr || m->size != map->size)
			continue;
		position = 0;
		ret = -EIO;
		for_each_sgtable_sg(m->sgt, sg, i) {
			if (!IS_ALIGNED(sg->offset, PAGE_SIZE) ||
			    !IS_ALIGNED(sg->length, PAGE_SIZE) || sg->length > m->size - position)
				goto out;
			for (offset = 0; offset < sg->length; offset += PAGE_SIZE) {
				pa = sg_phys(sg) + offset;
				if (iommu_iova_to_phys(domain, m->iova + position + offset) != pa ||
				    iommu_iova_to_phys(domain, m->iova + position + offset + PAGE_SIZE - 1) !=
						pa + PAGE_SIZE - 1)
					goto out;
				(*pages)++;
				if (pa >= SZ_4G)
					(*high_pages)++;
			}
			position += sg->length;
		}
		if (position == m->size)
			ret = 0;
		break;
	}
out:
	mutex_unlock(&ctx->lock);
	return ret;
}
