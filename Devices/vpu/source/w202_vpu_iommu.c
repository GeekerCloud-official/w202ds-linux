// SPDX-License-Identifier: GPL-2.0-only
/*
 * W202DS VPU VAU adaptation of the Unisoc Linux 6.18.35 IOMMU driver.
 * VAU v1 was read on all three W202DS codec cores. Page tables remain in
 * coherent RAM while runtime-suspended; a codec MMIO owner holds a lease.
 *
 * Unisoc IOMMU driver
 *
 * Copyright (C) 2020 Unisoc, Inc.
 * Author: Chunyan Zhang <chunyan.zhang@unisoc.com>
 */

#include <linux/clk.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include "w202_vpu.h"
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/iommu.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define SPRD_IOMMU_PAGE_SHIFT	12
#define SPRD_IOMMU_PAGE_SIZE	SZ_4K

#define SPRD_EX_CFG		0x0
#define SPRD_IOMMU_VAOR_BYPASS	BIT(4)
#define SPRD_IOMMU_GATE_EN	BIT(1)
#define SPRD_IOMMU_EN		BIT(0)
#define SPRD_EX_UPDATE		0x4
#define SPRD_EX_FIRST_VPN	0x8
#define SPRD_EX_VPN_RANGE	0xc
#define SPRD_EX_FIRST_PPN	0x10
#define SPRD_EX_DEFAULT_PPN	0x14

#define SPRD_IOMMU_VERSION	0x0
#define SPRD_VERSION_MASK	GENMASK(15, 8)
#define SPRD_VERSION_SHIFT	0x8
#define SPRD_VAU_CFG		0x4
#define SPRD_VAU_UPDATE		0x8
#define SPRD_VAU_AUTH_CFG	0xc
#define SPRD_VAU_FIRST_PPN	0x10
#define SPRD_VAU_DEFAULT_PPN_RD	0x14
#define SPRD_VAU_DEFAULT_PPN_WR	0x18
#define SPRD_VAU_FIRST_VPN	0x1c
#define SPRD_VAU_VPN_RANGE	0x20

enum sprd_iommu_version {
	SPRD_IOMMU_EX,
	SPRD_IOMMU_VAU,
};

/*
 * struct sprd_iommu_device - high-level sprd IOMMU device representation,
 * including hardware information and configuration, also driver data, etc
 *
 * @ver: sprd IOMMU IP version
 * @prot_page_va: protect page base virtual address
 * @prot_page_pa: protect page base physical address, data would be
 *		  written to here while translation fault
 * @base: mapped base address for accessing registers
 * @dev: pointer to basic device structure
 * @iommu: IOMMU core representation
 * @group: IOMMU group
 * @eb: gate clock which controls IOMMU access
 */
struct sprd_iommu_device {
	unsigned long iova_base;
	unsigned long iova_size;
	struct platform_device *codec;
	struct device *power_dev;
	struct clk_bulk_data clocks[5];
	unsigned int nclocks;
	bool clocks_on;
	bool power_on;
	bool hw_active;
	bool broken;
	unsigned int resumes, suspends;
	spinlock_t hw_lock;
	struct sprd_iommu_domain	*dom;
	enum sprd_iommu_version	ver;
	u32			*prot_page_va;
	dma_addr_t		prot_page_pa;
	void __iomem		*base;
	struct device		*dev;
	struct iommu_device	iommu;
	bool			registered;
};

struct sprd_iommu_domain {
	spinlock_t		pgtlock; /* lock for page table */
	struct iommu_domain	domain;
	u32			*pgt_va; /* page table virtual address base */
	dma_addr_t		pgt_pa; /* page table physical address base */
	struct sprd_iommu_device	*sdev;
};

static const struct iommu_ops sprd_iommu_ops;

static struct sprd_iommu_domain *to_sprd_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct sprd_iommu_domain, domain);
}

static inline void
sprd_iommu_write(struct sprd_iommu_device *sdev, unsigned int reg, u32 val)
{
	writel_relaxed(val, sdev->base + reg);
}

static inline u32
sprd_iommu_read(struct sprd_iommu_device *sdev, unsigned int reg)
{
	return readl_relaxed(sdev->base + reg);
}

static inline void
sprd_iommu_update_bits(struct sprd_iommu_device *sdev, unsigned int reg,
		  u32 mask, u32 shift, u32 val)
{
	u32 t = sprd_iommu_read(sdev, reg);

	t = (t & (~(mask << shift))) | ((val & mask) << shift);
	sprd_iommu_write(sdev, reg, t);
}

static inline int
sprd_iommu_get_version(struct sprd_iommu_device *sdev)
{
	int ver = (sprd_iommu_read(sdev, SPRD_IOMMU_VERSION) &
		   SPRD_VERSION_MASK) >> SPRD_VERSION_SHIFT;

	switch (ver) {
	case SPRD_IOMMU_EX:
	case SPRD_IOMMU_VAU:
		return ver;
	default:
		return -EINVAL;
	}
}

static size_t
sprd_iommu_pgt_size(struct iommu_domain *domain)
{
	return ((domain->geometry.aperture_end -
		 domain->geometry.aperture_start + 1) >>
		SPRD_IOMMU_PAGE_SHIFT) * sizeof(u32);
}

static struct iommu_domain *sprd_iommu_domain_alloc_paging(struct device *dev)
{
	struct sprd_iommu_device *sdev = dev_iommu_priv_get(dev);
	struct sprd_iommu_domain *dom;

	if (!sdev)
		return ERR_PTR(-ENODEV);
	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return ERR_PTR(-ENOMEM);
	spin_lock_init(&dom->pgtlock);
	dom->domain.pgsize_bitmap = SPRD_IOMMU_PAGE_SIZE;
	dom->domain.geometry.aperture_start = sdev->iova_base;
	dom->domain.geometry.aperture_end = sdev->iova_base + sdev->iova_size - 1;
	dom->domain.geometry.force_aperture = true;
	return &dom->domain;
}

static void sprd_iommu_first_vpn(struct sprd_iommu_domain *dom)
{
	struct sprd_iommu_device *sdev = dom->sdev;
	u32 val;
	unsigned int reg;

	if (sdev->ver == SPRD_IOMMU_EX)
		reg = SPRD_EX_FIRST_VPN;
	else
		reg = SPRD_VAU_FIRST_VPN;

	val = dom->domain.geometry.aperture_start >> SPRD_IOMMU_PAGE_SHIFT;
	sprd_iommu_write(sdev, reg, val);
}

static void sprd_iommu_vpn_range(struct sprd_iommu_domain *dom)
{
	struct sprd_iommu_device *sdev = dom->sdev;
	u32 val;
	unsigned int reg;

	if (sdev->ver == SPRD_IOMMU_EX)
		reg = SPRD_EX_VPN_RANGE;
	else
		reg = SPRD_VAU_VPN_RANGE;

	val = (dom->domain.geometry.aperture_end -
	       dom->domain.geometry.aperture_start) >> SPRD_IOMMU_PAGE_SHIFT;
	sprd_iommu_write(sdev, reg, val);
}

static void sprd_iommu_first_ppn(struct sprd_iommu_domain *dom)
{
	u32 val = dom->pgt_pa >> SPRD_IOMMU_PAGE_SHIFT;
	struct sprd_iommu_device *sdev = dom->sdev;
	unsigned int reg;

	if (sdev->ver == SPRD_IOMMU_EX)
		reg = SPRD_EX_FIRST_PPN;
	else
		reg = SPRD_VAU_FIRST_PPN;

	sprd_iommu_write(sdev, reg, val);
}

static void sprd_iommu_default_ppn(struct sprd_iommu_device *sdev)
{
	u32 val = sdev->prot_page_pa >> SPRD_IOMMU_PAGE_SHIFT;

	if (sdev->ver == SPRD_IOMMU_EX) {
		sprd_iommu_write(sdev, SPRD_EX_DEFAULT_PPN, val);
	} else if (sdev->ver == SPRD_IOMMU_VAU) {
		sprd_iommu_write(sdev, SPRD_VAU_DEFAULT_PPN_RD, val);
		sprd_iommu_write(sdev, SPRD_VAU_DEFAULT_PPN_WR, val);
	}
}

static void sprd_iommu_hw_en(struct sprd_iommu_device *sdev, bool en)
{
	unsigned int reg_cfg;
	u32 mask, val;

	if (sdev->ver == SPRD_IOMMU_EX)
		reg_cfg = SPRD_EX_CFG;
	else
		reg_cfg = SPRD_VAU_CFG;

	mask = SPRD_IOMMU_EN | SPRD_IOMMU_GATE_EN;
	val = en ? mask : 0;
	sprd_iommu_update_bits(sdev, reg_cfg, mask, 0, val);
}

static void sprd_iommu_cleanup(struct sprd_iommu_domain *dom)
{
	struct sprd_iommu_device *sdev = dom->sdev;
	unsigned long flags;

	if (!sdev)
		return;
	/* Quiesce translation before releasing the page table, never after. */
	spin_lock_irqsave(&sdev->hw_lock, flags);
	if (sdev->dom == dom) {
		if (sdev->hw_active) {
			sprd_iommu_hw_en(sdev, false);
			readl(sdev->base + SPRD_VAU_CFG);
		}
		sdev->dom = NULL;
	}
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
	dma_free_coherent(sdev->dev, sprd_iommu_pgt_size(&dom->domain),
			  dom->pgt_va, dom->pgt_pa);
	dom->pgt_va = NULL;
	dom->sdev = NULL;
}

static void sprd_iommu_domain_free(struct iommu_domain *domain)
{
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);

	sprd_iommu_cleanup(dom);
	kfree(dom);
}

static int sprd_iommu_program(struct sprd_iommu_domain *dom)
{
	struct sprd_iommu_device *sdev = dom->sdev;

	sprd_iommu_hw_en(sdev, false);
	sprd_iommu_first_ppn(dom);
	sprd_iommu_first_vpn(dom);
	sprd_iommu_vpn_range(dom);
	sprd_iommu_default_ppn(sdev);
	dma_wmb();
	/* No out-of-range physical bypass on the codec DMA path. */
	sprd_iommu_update_bits(sdev, SPRD_VAU_CFG, SPRD_IOMMU_VAOR_BYPASS, 0, 0);
	sprd_iommu_hw_en(sdev, true);
	sprd_iommu_write(sdev, SPRD_VAU_UPDATE, 0xffffffff);
	if ((readl(sdev->base + SPRD_VAU_CFG) &
	     (SPRD_IOMMU_EN | SPRD_IOMMU_GATE_EN | SPRD_IOMMU_VAOR_BYPASS)) !=
	    (SPRD_IOMMU_EN | SPRD_IOMMU_GATE_EN) ||
	    readl(sdev->base + SPRD_VAU_FIRST_PPN) != dom->pgt_pa >> SPRD_IOMMU_PAGE_SHIFT ||
	    readl(sdev->base + SPRD_VAU_FIRST_VPN) !=
			dom->domain.geometry.aperture_start >> SPRD_IOMMU_PAGE_SHIFT ||
	    readl(sdev->base + SPRD_VAU_VPN_RANGE) !=
			(dom->domain.geometry.aperture_end - dom->domain.geometry.aperture_start) >>
			SPRD_IOMMU_PAGE_SHIFT) {
		sprd_iommu_hw_en(sdev, false);
		return -EIO;
	}
	return 0;
}

static int sprd_iommu_attach_device(struct iommu_domain *domain, struct device *dev)
{
	struct sprd_iommu_device *sdev = dev_iommu_priv_get(dev);
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);
	struct sprd_iommu_domain *previous;
	size_t size = sprd_iommu_pgt_size(domain);
	unsigned long flags;
	int ret;

	if (!sdev || (dom->sdev && dom->sdev != sdev))
		return -EINVAL;
	ret = pm_runtime_resume_and_get(sdev->dev);
	if (ret < 0)
		return ret;
	if (!dom->pgt_va) {
		dom->pgt_va = dma_alloc_coherent(sdev->dev, size, &dom->pgt_pa, GFP_KERNEL);
		if (!dom->pgt_va) {
			ret = -ENOMEM;
			goto put;
		}
		memset(dom->pgt_va, 0xff, size);
		dom->sdev = sdev;
	}
	spin_lock_irqsave(&sdev->hw_lock, flags);
	previous = sdev->dom;
	sdev->dom = dom;
	ret = sprd_iommu_program(dom);
	if (ret) {
		sdev->dom = NULL;
		if (previous && previous != dom && !sprd_iommu_program(previous))
			sdev->dom = previous;
	}
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
put:
	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);
	return ret;
}

static struct sprd_iommu_device *codec_iommu(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct sprd_iommu_device *sdev;

	/* dev_iommu_ops() is private to the IOMMU core in this kernel. */
	if (!domain || domain->owner != &sprd_iommu_ops)
		return NULL;
	sdev = dev_iommu_priv_get(dev);
	return sdev && dev == &sdev->codec->dev ? sdev : NULL;
}

int w202_vpu_iommu_get(struct device *dev)
{
	struct sprd_iommu_device *sdev = codec_iommu(dev);
	int ret;

	if (!sdev || !READ_ONCE(sdev->registered))
		return -ENODEV;
	if (READ_ONCE(sdev->broken))
		return -EIO;
	ret = pm_runtime_resume_and_get(sdev->dev);
	return ret < 0 ? ret : 0;
}
EXPORT_SYMBOL_GPL(w202_vpu_iommu_get);

void w202_vpu_iommu_put(struct device *dev)
{
	struct sprd_iommu_device *sdev = codec_iommu(dev);

	if (WARN_ON(!sdev))
		return;
	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);
}
EXPORT_SYMBOL_GPL(w202_vpu_iommu_put);

int w202_vpu_iommu_restore(struct device *dev)
{
	struct sprd_iommu_device *sdev;
	unsigned long flags;
	int ret = w202_vpu_iommu_get(dev);

	if (ret)
		return ret;
	sdev = codec_iommu(dev);
	spin_lock_irqsave(&sdev->hw_lock, flags);
	if (sdev->dom && sdev->hw_active)
		ret = sprd_iommu_program(sdev->dom);
	else
		ret = -ENODEV;
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
	w202_vpu_iommu_put(dev);
	return ret;
}
EXPORT_SYMBOL_GPL(w202_vpu_iommu_restore);

static int sprd_iommu_map(struct iommu_domain *domain, unsigned long iova,
		phys_addr_t paddr, size_t pgsize, size_t pgcount, int prot,
		gfp_t gfp, size_t *mapped)
{
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);
	unsigned long flags, last;
	phys_addr_t physical_last;
	size_t size, i, index;

	*mapped = 0;
	if (pgsize != SPRD_IOMMU_PAGE_SIZE || !pgcount ||
	    check_mul_overflow(pgcount, pgsize, &size) ||
	    check_add_overflow(iova, size - 1, &last) ||
	    check_add_overflow(paddr, (phys_addr_t)size - 1, &physical_last) ||
	    !IS_ALIGNED(iova, pgsize) || !IS_ALIGNED(paddr, pgsize) ||
	    !(prot & (IOMMU_READ | IOMMU_WRITE)) ||
	    physical_last > DMA_BIT_MASK(36) ||
	    (physical_last >> SPRD_IOMMU_PAGE_SHIFT) >= U32_MAX)
		return -EINVAL;
	if (!dom->sdev || !dom->pgt_va)
		return -ENODEV;
	if (iova < domain->geometry.aperture_start || last > domain->geometry.aperture_end)
		return -ERANGE;
	index = (iova - domain->geometry.aperture_start) >> SPRD_IOMMU_PAGE_SHIFT;
	spin_lock_irqsave(&dom->pgtlock, flags);
	for (i = 0; i < pgcount; i++) {
		if (dom->pgt_va[index + i] != cpu_to_le32(U32_MAX)) {
			spin_unlock_irqrestore(&dom->pgtlock, flags);
			return -EEXIST;
		}
	}
	/* Keep the complete physical address until after the page shift. */
	for (i = 0; i < pgcount; i++)
		dom->pgt_va[index + i] = cpu_to_le32((paddr + i * pgsize) >> SPRD_IOMMU_PAGE_SHIFT);
	spin_unlock_irqrestore(&dom->pgtlock, flags);
	*mapped = size;
	return 0;
}

static size_t sprd_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
		size_t pgsize, size_t pgcount, struct iommu_iotlb_gather *gather)
{
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);
	unsigned long flags, last;
	size_t size, index, count;

	if (pgsize != SPRD_IOMMU_PAGE_SIZE || !pgcount ||
	    check_mul_overflow(pgcount, pgsize, &size) ||
	    check_add_overflow(iova, size - 1, &last) ||
	    !IS_ALIGNED(iova, pgsize) || !dom->pgt_va ||
	    iova < domain->geometry.aperture_start || last > domain->geometry.aperture_end)
		return 0;
	index = (iova - domain->geometry.aperture_start) >> SPRD_IOMMU_PAGE_SHIFT;
	spin_lock_irqsave(&dom->pgtlock, flags);
	/* Report only the mapped prefix; never claim to have removed a hole. */
	for (count = 0; count < pgcount; count++)
		if (dom->pgt_va[index + count] == cpu_to_le32(U32_MAX))
			break;
	memset(dom->pgt_va + index, 0xff, count * sizeof(u32));
	spin_unlock_irqrestore(&dom->pgtlock, flags);
	return count * pgsize;
}

static int sprd_iommu_sync_map(struct iommu_domain *domain, unsigned long iova, size_t size)
{
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);
	unsigned long flags;

	if (!dom->sdev)
		return -ENODEV;
	spin_lock_irqsave(&dom->sdev->hw_lock, flags);
	dma_wmb();
	/* Atomic DMA map/unmap must not resume a sleeping power domain. The
	 * next runtime resume programs the full coherent table and invalidates.
	 */
	if (dom->sdev->hw_active) {
		sprd_iommu_write(dom->sdev, SPRD_VAU_UPDATE, U32_MAX);
		readl(dom->sdev->base + SPRD_VAU_UPDATE);
	}
	spin_unlock_irqrestore(&dom->sdev->hw_lock, flags);
	return 0;
}

static void sprd_iommu_sync(struct iommu_domain *domain,
			    struct iommu_iotlb_gather *iotlb_gather)
{
	sprd_iommu_sync_map(domain, 0, 0);
}

static phys_addr_t sprd_iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	struct sprd_iommu_domain *dom = to_sprd_domain(domain);
	unsigned long flags;
	u32 entry;

	if (!dom->pgt_va || iova < domain->geometry.aperture_start ||
	    iova > domain->geometry.aperture_end)
		return 0;
	spin_lock_irqsave(&dom->pgtlock, flags);
	entry = le32_to_cpu(dom->pgt_va[(iova - domain->geometry.aperture_start) >>
				      SPRD_IOMMU_PAGE_SHIFT]);
	spin_unlock_irqrestore(&dom->pgtlock, flags);
	if (entry == U32_MAX)
		return 0;
	return ((phys_addr_t)entry << SPRD_IOMMU_PAGE_SHIFT) | (iova & (SPRD_IOMMU_PAGE_SIZE - 1));
}

static struct iommu_device *sprd_iommu_probe_device(struct device *dev)
{
	struct sprd_iommu_device *sdev = dev_iommu_priv_get(dev);

	return sdev && READ_ONCE(sdev->registered) && dev == &sdev->codec->dev ?
		&sdev->iommu : ERR_PTR(-ENODEV);
}

static void sprd_iommu_release_device(struct device *dev)
{
	struct sprd_iommu_device *sdev = dev_iommu_priv_get(dev);
	unsigned long flags;

	if (!sdev)
		return;
	spin_lock_irqsave(&sdev->hw_lock, flags);
	if (sdev->hw_active) {
		sprd_iommu_hw_en(sdev, false);
		readl(sdev->base + SPRD_VAU_CFG);
	}
	sdev->dom = NULL;
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
}

static int sprd_iommu_of_xlate(struct device *dev, const struct of_phandle_args *args)
{
	struct platform_device *pdev;
	struct sprd_iommu_device *sdev;

	if (args->args_count)
		return -EINVAL;
	pdev = of_find_device_by_node(args->np);
	if (!pdev)
		return -EPROBE_DEFER;
	sdev = platform_get_drvdata(pdev);
	platform_device_put(pdev);
	if (!sdev || !READ_ONCE(sdev->registered))
		return -EPROBE_DEFER;
	dev_iommu_priv_set(dev, sdev);
	return 0;
}


static const struct iommu_ops sprd_iommu_ops = {
	.domain_alloc_paging = sprd_iommu_domain_alloc_paging,
	.probe_device	= sprd_iommu_probe_device,
	.release_device = sprd_iommu_release_device,
	.device_group	= generic_single_device_group,
	.of_xlate	= sprd_iommu_of_xlate,
	.owner		= THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= sprd_iommu_attach_device,
		.map_pages	= sprd_iommu_map,
		.unmap_pages	= sprd_iommu_unmap,
		.iotlb_sync_map	= sprd_iommu_sync_map,
		.iotlb_sync	= sprd_iommu_sync,
		.iova_to_phys	= sprd_iommu_iova_to_phys,
		.free		= sprd_iommu_domain_free,
	}
};

static const struct of_device_id sprd_iommu_of_match[] = {
	{ .compatible = "unisoc,iommuvaul6p-vsp1" },
	{ .compatible = "unisoc,iommuvaul6p-vsp2" },
	{ .compatible = "unisoc,iommuvaul6p-vsp" },
	{ },
};
MODULE_DEVICE_TABLE(of, sprd_iommu_of_match);

/* Get references without keeping otherwise idle codec domains powered. */
static int sprd_iommu_get_resources(struct sprd_iommu_device *sdev)
{
	static const char * const paths[] = {
		"/soc/mm/video-codec@32000000", "/soc/mm/enc-core1@32100000",
		"/soc/mm/video-codec@32200000",
	};
	static const char * const names[] = {
		"clk_domain_eb", "clk_dev_eb", "clk_ahb_vsp", "clk_vsp", "clk_ckg_eb",
	};
	static const char * const compatibles[] = {
		"sprd,vpu-enc-core0", "sprd,vpu-enc-core1", "sprd,vpu-dec-core0",
	};
	struct device_node *node, *iommu;
	struct resource res;
	u32 version;
	unsigned int core, i;
	int ret;

	if (!w202_vpu_power_ready())
		return -EPROBE_DEFER;
	ret = of_address_to_resource(sdev->dev->of_node, 0, &res);
	if (ret || res.start < 0x32000000 || res.start > 0x32200000 ||
	    !IS_ALIGNED(res.start - 0x32000000, 0x100000) ||
	    resource_size(&res) != 0x80)
		return -EINVAL;
	core = (res.start - 0x32000000) / 0x100000;
	if (sdev->iova_size != (core == 2 ? 0x40000000 : 0x20000000))
		return -EINVAL;
	node = of_find_node_by_path(paths[core]);
	if (!node || !of_device_is_available(node) ||
	    !of_device_is_compatible(node, compatibles[core]) ||
	    of_property_read_u32(node, "sprd,video_ip_version", &version) || version != 0x15) {
		of_node_put(node);
		return -ENODEV;
	}
	iommu = of_parse_phandle(node, "iommus", 0);
	if (iommu != sdev->dev->of_node) {
		of_node_put(iommu);
		of_node_put(node);
		return -EINVAL;
	}
	of_node_put(iommu);
	sdev->codec = of_find_device_by_node(node);
	of_node_put(node);
	if (!sdev->codec)
		return -EPROBE_DEFER;
	device_lock(&sdev->codec->dev);
	ret = sdev->codec->dev.driver ? -EBUSY : 0;
	device_unlock(&sdev->codec->dev);
	if (ret)
		return ret;
	sdev->power_dev = dev_pm_domain_attach_by_id(&sdev->codec->dev, 0);
	if (IS_ERR(sdev->power_dev)) {
		ret = PTR_ERR(sdev->power_dev);
		sdev->power_dev = NULL;
		return ret;
	}
	if (!sdev->power_dev)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		sdev->clocks[i].id = names[i];
		sdev->clocks[i].clk = of_clk_get_by_name(sdev->codec->dev.of_node, names[i]);
		if (IS_ERR(sdev->clocks[i].clk))
			return PTR_ERR(sdev->clocks[i].clk);
		sdev->nclocks++;
	}
	return 0;
}

/* Runtime callbacks serialize clock/power changes; hw_lock protects MMIO from
 * atomic IOTLB operations and non-waking sysfs snapshots during transitions.
 */
static int sprd_iommu_power_up(struct sprd_iommu_device *sdev)
{
	unsigned long flags;
	int ret;

	ret = pm_runtime_resume_and_get(sdev->power_dev);
	if (ret < 0)
		return ret;
	sdev->power_on = true;
	ret = clk_bulk_prepare_enable(sdev->nclocks, sdev->clocks);
	if (ret)
		goto put_power;
	sdev->clocks_on = true;
	spin_lock_irqsave(&sdev->hw_lock, flags);
	ret = sprd_iommu_get_version(sdev);
	if (ret == SPRD_IOMMU_VAU) {
		sdev->ver = ret;
		ret = sdev->dom ? sprd_iommu_program(sdev->dom) : 0;
	} else {
		ret = -ENODEV;
	}
	if (!ret) {
		sdev->hw_active = true;
		sdev->resumes++;
	}
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
	if (!ret)
		return 0;
	clk_bulk_disable_unprepare(sdev->nclocks, sdev->clocks);
	sdev->clocks_on = false;
put_power:
	pm_runtime_put_sync_suspend(sdev->power_dev);
	sdev->power_on = false;
	return ret;
}

static int sprd_iommu_runtime_resume(struct device *dev)
{
	struct sprd_iommu_device *sdev = dev_get_drvdata(dev);
	int ret = sprd_iommu_power_up(sdev);

	if (ret)
		dev_err(dev, "VAU runtime resume failed: %d\n", ret);
	return ret;
}

static int sprd_iommu_runtime_suspend(struct device *dev)
{
	struct sprd_iommu_device *sdev = dev_get_drvdata(dev);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&sdev->hw_lock, flags);
	if (sdev->hw_active) {
		sprd_iommu_hw_en(sdev, false);
		readl(sdev->base + SPRD_VAU_CFG);
		sdev->hw_active = false;
	}
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
	if (sdev->clocks_on) {
		clk_bulk_disable_unprepare(sdev->nclocks, sdev->clocks);
		sdev->clocks_on = false;
	}
	ret = sdev->power_on ? pm_runtime_put_sync_suspend(sdev->power_dev) : 0;
	sdev->power_on = false;
	if (ret < 0) {
		/* A failed suspend must not advertise ACTIVE with dead clocks. */
		if (sprd_iommu_power_up(sdev))
			WRITE_ONCE(sdev->broken, true);
		dev_err(dev, "VAU power suspend failed: %d; broken=%u\n", ret, sdev->broken);
		return ret;
	}
	sdev->suspends++;
	return 0;
}

static void sprd_iommu_put_resources(struct sprd_iommu_device *sdev)
{
	if (sdev->clocks_on) {
		clk_bulk_disable_unprepare(sdev->nclocks, sdev->clocks);
		sdev->clocks_on = false;
	}
	while (sdev->nclocks)
		clk_put(sdev->clocks[--sdev->nclocks].clk);
	if (sdev->power_dev) {
		if (sdev->power_on)
			pm_runtime_put_sync_suspend(sdev->power_dev);
		dev_pm_domain_detach(sdev->power_dev, true);
		sdev->power_dev = NULL;
		sdev->power_on = false;
	}
	if (sdev->codec) {
		platform_device_put(sdev->codec);
		sdev->codec = NULL;
	}
}

static void sprd_iommu_stop_pm(struct sprd_iommu_device *sdev)
{
	pm_runtime_barrier(sdev->dev);
	pm_runtime_disable(sdev->dev);
	if (sdev->clocks_on || sdev->power_on)
		sprd_iommu_runtime_suspend(sdev->dev);
	pm_runtime_set_suspended(sdev->dev);
}

static int sprd_iommu_probe(struct platform_device *pdev)
{
	struct sprd_iommu_device *sdev;
	struct device *dev = &pdev->dev;
	u32 start, size;
	int ret;

	sdev = devm_kzalloc(dev, sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;
	sdev->dev = dev;
	spin_lock_init(&sdev->hw_lock);
	ret = of_property_read_u32(dev->of_node, "iova-base", &start);
	if (ret)
		return ret;
	ret = of_property_read_u32(dev->of_node, "iova-size", &size);
	if (ret || start != 0x20000000 || (size != 0x20000000 && size != 0x40000000))
		return -EINVAL;
	sdev->iova_base = start;
	sdev->iova_size = size;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36));
	if (ret)
		return ret;
	sdev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sdev->base))
		return PTR_ERR(sdev->base);
	ret = sprd_iommu_get_resources(sdev);
	if (ret)
		goto put_resources;
	platform_set_drvdata(pdev, sdev);
	pm_runtime_set_autosuspend_delay(dev, 250);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	/* Registration may recursively probe a codec and attach its default domain. */
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto stop_pm;
	sdev->prot_page_va = dma_alloc_coherent(dev, SPRD_IOMMU_PAGE_SIZE,
					       &sdev->prot_page_pa, GFP_KERNEL);
	if (!sdev->prot_page_va) {
		ret = -ENOMEM;
		goto put_pm;
	}
	ret = iommu_device_sysfs_add(&sdev->iommu, dev, NULL, "%s", dev_name(dev));
	if (ret)
		goto put_pm;
	/* Registration can trigger client probing, so all state is ready first. */
	sdev->registered = true;
	ret = iommu_device_register(&sdev->iommu, &sprd_iommu_ops, dev);
	if (ret) {
		sdev->registered = false;
		goto remove_sysfs;
	}
	dev_info(dev, "VAU v1 ready: IOVA %08x+%08x; 250ms runtime autosuspend\n", start, size);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return 0;
remove_sysfs:
	iommu_device_sysfs_remove(&sdev->iommu);
put_pm:
	pm_runtime_put_sync_suspend(dev);
stop_pm:
	sprd_iommu_stop_pm(sdev);
	if (sdev->prot_page_va)
		dma_free_coherent(dev, SPRD_IOMMU_PAGE_SIZE, sdev->prot_page_va, sdev->prot_page_pa);
	platform_set_drvdata(pdev, NULL);
put_resources:
	sprd_iommu_put_resources(sdev);
	return ret;
}

static void sprd_iommu_remove(struct platform_device *pdev)
{
	struct sprd_iommu_device *sdev = platform_get_drvdata(pdev);

	/* Retire clients/domains before their page tables or power are released. */
	if (sdev->registered) {
		sdev->registered = false;
		iommu_device_unregister(&sdev->iommu);
		iommu_device_sysfs_remove(&sdev->iommu);
	}
	sprd_iommu_stop_pm(sdev);
	dma_free_coherent(sdev->dev, SPRD_IOMMU_PAGE_SIZE, sdev->prot_page_va, sdev->prot_page_pa);
	platform_set_drvdata(pdev, NULL);
	sprd_iommu_put_resources(sdev);
}

/*
 * IOMMU core pins ops->owner even for an unbound codec. Explicit retirement
 * drops those references without force-unloading a provider. Bind attrs are
 * suppressed; retirement refuses a bound codec (including VFIO). IOMMUFD is
 * disabled in the pinned M23 configuration.
 */
static ssize_t retire_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct sprd_iommu_device *sdev = dev_get_drvdata(dev);
	bool retire;
	int ret = kstrtobool(buf, &retire);

	if (ret || !retire)
		return -EINVAL;
	device_lock(&sdev->codec->dev);
	if (sdev->codec->dev.driver) {
		ret = -EBUSY;
		goto unlock;
	}
	if (sdev->registered) {
		WRITE_ONCE(sdev->registered, false);
		iommu_device_unregister(&sdev->iommu);
		iommu_device_sysfs_remove(&sdev->iommu);
		dev_info(dev, "VAU retired; idle client/domain references released\n");
	}
	ret = count;
unlock:
	device_unlock(&sdev->codec->dev);
	return ret;
}
static DEVICE_ATTR_WO(retire);

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sprd_iommu_device *sdev = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t count;

	spin_lock_irqsave(&sdev->hw_lock, flags);
	/* Live register fields are zero/invalid while suspended; do not wake it. */
	count = sysfs_emit(buf, "registered=%u domain=%u active=%u broken=%u resumes=%u suspends=%u live_regs=%u cfg=%08x first_ppn=%08x "
			  "first_vpn=%08x vpn_range=%08x\n",
			  READ_ONCE(sdev->registered), !!sdev->dom,
			  sdev->hw_active, READ_ONCE(sdev->broken), sdev->resumes,
			  READ_ONCE(sdev->suspends), sdev->hw_active,
			  sdev->hw_active ? readl(sdev->base + SPRD_VAU_CFG) : 0,
			  sdev->hw_active ? readl(sdev->base + SPRD_VAU_FIRST_PPN) : 0,
			  sdev->hw_active ? readl(sdev->base + SPRD_VAU_FIRST_VPN) : 0,
			  sdev->hw_active ? readl(sdev->base + SPRD_VAU_VPN_RANGE) : 0);
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
	return count;
}
static DEVICE_ATTR_RO(state);

static struct attribute *sprd_iommu_attrs[] = {
	&dev_attr_retire.attr, &dev_attr_state.attr, NULL,
};
ATTRIBUTE_GROUPS(sprd_iommu);

static const struct dev_pm_ops sprd_iommu_pm_ops = {
	SET_RUNTIME_PM_OPS(sprd_iommu_runtime_suspend, sprd_iommu_runtime_resume, NULL)
};

static struct platform_driver sprd_iommu_driver = {
	.driver	= {
		.name		= "w202-vpu-iommu",
		.of_match_table	= sprd_iommu_of_match,
		.suppress_bind_attrs = true,
		.dev_groups = sprd_iommu_groups,
		.pm = &sprd_iommu_pm_ops,
	},
	.probe	= sprd_iommu_probe,
	.remove = sprd_iommu_remove,
};
module_platform_driver(sprd_iommu_driver);

MODULE_DESCRIPTION("W202DS codec-only VAU v1 IOMMU research driver");
MODULE_VERSION("w202ds-vpu-iommu-v2-pm");
MODULE_LICENSE("GPL");
