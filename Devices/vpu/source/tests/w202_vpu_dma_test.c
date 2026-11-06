// SPDX-License-Identifier: GPL-2.0-only
/* Real DMA-API page/SG mappings on each codec. No codec job is submitted. */
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include "w202_vpu.h"

#define TEST_PAGES 8
static unsigned int passed;
module_param(passed, uint, 0444);
static unsigned int high_pages;
module_param(high_pages, uint, 0444);

static int verify_mapping(struct device *dev, struct iommu_domain *domain,
			  dma_addr_t dma, struct page *page)
{
	static const unsigned int offsets[] = { 0, 123, PAGE_SIZE - 1 };
	phys_addr_t pa = page_to_phys(page), actual;
	unsigned int i;

	if (dma < domain->geometry.aperture_start ||
	    dma + PAGE_SIZE - 1 > domain->geometry.aperture_end)
		return -ERANGE;
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		actual = iommu_iova_to_phys(domain, dma + offsets[i]);
		if (actual != pa + offsets[i]) {
			dev_err(dev, "IOVA %pad+%u maps %pa, expected %pa\n",
				&dma, offsets[i], &actual, &pa);
			return -EIO;
		}
	}
	return 0;
}

static int dma_test_probe(struct platform_device *pdev)
{
	/* Interleave allocations so the SG exercises discontiguous physical pages. */
	static const unsigned int order[TEST_PAGES] = { 0, 2, 4, 6, 1, 3, 5, 7 };
	struct device *dev = &pdev->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct page *pages[TEST_PAGES] = {};
	struct scatterlist sgl[TEST_PAGES], *sg;
	dma_addr_t saved[TEST_PAGES], single_dma = 0;
	phys_addr_t pa;
	bool single_mapped = false, sg_mapped = false, noncontiguous = false;
	unsigned int i, j, n, sg_pages, offset, high = 0;
	int ret, nents = 0;

	if (!device_iommu_mapped(dev) || !domain || !iommu_is_dma_domain(domain))
		return dev_err_probe(dev, -ENODEV, "translated DMA domain required\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36));
	if (ret)
		return ret;
	for (i = 0; i < TEST_PAGES; i++) {
		pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pages[i]) {
			ret = -ENOMEM;
			goto out;
		}
		pa = page_to_phys(pages[i]);
		if (pa >= SZ_4G)
			high++;
	}
	pa = page_to_phys(pages[0]);
	single_dma = dma_map_page(dev, pages[0], 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, single_dma)) {
		ret = -EIO;
		goto out;
	}
	single_mapped = true;
	ret = verify_mapping(dev, domain, single_dma, pages[0]);
	if (ret)
		goto out;
	/* Reprogram the actual VAU registers and verify the live mapping survives. */
	ret = w202_vpu_iommu_restore(dev);
	if (ret)
		goto out;
	ret = verify_mapping(dev, domain, single_dma, pages[0]);
	if (ret)
		goto out;
	dma_unmap_page(dev, single_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
	single_mapped = false;
	if (iommu_iova_to_phys(domain, single_dma)) {
		ret = -EIO;
		goto out;
	}
	dev_info(dev, "single page DMA %pad -> PA %pa; offsets/restore/unmap PASS\n",
		 &single_dma, &pa);

	sg_init_table(sgl, TEST_PAGES);
	for (i = 0; i < TEST_PAGES; i++) {
		sg_set_page(&sgl[i], pages[order[i]], PAGE_SIZE, 0);
		if (i && page_to_phys(pages[order[i]]) !=
		    page_to_phys(pages[order[i - 1]]) + PAGE_SIZE)
			noncontiguous = true;
	}
	if (!noncontiguous) {
		ret = -EAGAIN;
		goto out;
	}
	nents = dma_map_sg(dev, sgl, TEST_PAGES, DMA_BIDIRECTIONAL);
	if (!nents) {
		ret = -EIO;
		goto out;
	}
	sg_mapped = true;
	n = 0;
	for_each_sg(sgl, sg, nents, j) {
		if (!IS_ALIGNED(sg_dma_len(sg), PAGE_SIZE)) {
			ret = -EINVAL;
			goto out;
		}
		sg_pages = sg_dma_len(sg) / PAGE_SIZE;
		for (offset = 0; offset < sg_pages; offset++) {
			if (n >= TEST_PAGES) {
				ret = -EOVERFLOW;
				goto out;
			}
			saved[n] = sg_dma_address(sg) + offset * PAGE_SIZE;
			ret = verify_mapping(dev, domain, saved[n], pages[order[n]]);
			if (ret)
				goto out;
			n++;
		}
	}
	if (n != TEST_PAGES) {
		ret = -EIO;
		goto out;
	}
	ret = w202_vpu_iommu_restore(dev);
	if (ret)
		goto out;
	for (i = 0; i < n; i++) {
		ret = verify_mapping(dev, domain, saved[i], pages[order[i]]);
		if (ret)
			goto out;
	}
	dma_unmap_sg(dev, sgl, TEST_PAGES, DMA_BIDIRECTIONAL);
	sg_mapped = false;
	for (i = 0; i < n; i++) {
		if (iommu_iova_to_phys(domain, saved[i])) {
			ret = -EIO;
			goto out;
		}
	}
	passed++;
	high_pages += high;
	dev_info(dev, "8 discontiguous pages / %d DMA segments; high_PA_pages=%u; "
		 "SG/restore/unmap PASS (no codec DMA submitted)\n", nents, high);
	ret = 0;
out:
	if (sg_mapped)
		dma_unmap_sg(dev, sgl, TEST_PAGES, DMA_BIDIRECTIONAL);
	if (single_mapped)
		dma_unmap_page(dev, single_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
	for (i = 0; i < TEST_PAGES; i++)
		if (pages[i])
			__free_page(pages[i]);
	return ret;
}

static const struct of_device_id dma_test_match[] = {
	{ .compatible = "sprd,vpu-enc-core0" },
	{ .compatible = "sprd,vpu-enc-core1" },
	{ .compatible = "sprd,vpu-dec-core0" },
	{},
};
MODULE_DEVICE_TABLE(of, dma_test_match);

static struct platform_driver dma_test_driver = {
	.probe = dma_test_probe,
	.driver = {
		.name = "w202-vpu-dma-test",
		.of_match_table = dma_test_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(dma_test_driver);
MODULE_DESCRIPTION("W202DS codec DMA-API mapping/VAU restore lifetime test");
MODULE_LICENSE("GPL");
MODULE_VERSION("w202ds-vpu-dma-test-v1");
