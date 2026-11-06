// SPDX-License-Identifier: GPL-2.0-only
/* Audit the existing CH mapping without modifying shared memory or IPC. */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <asm/memory.h>
#include <asm/pgtable.h>
#include <asm/sysreg.h>

extern void *shmem_ram_vmap_nocache_ex(u8 dst, u16 smem,
				     phys_addr_t start, size_t size);
extern void shmem_ram_unmap_ex(u8 dst, u16 smem, const void *mem);

static unsigned int attribute_index;
static unsigned int mair_byte;
static unsigned int header_ring_count;
static bool ram_pfn_valid;
module_param(attribute_index, uint, 0444);
module_param(mair_byte, uint, 0444);
module_param(header_ring_count, uint, 0444);
module_param(ram_pfn_valid, bool, 0444);

static int mapping_attribute(unsigned long address, u64 *descriptor)
{
	phys_addr_t root_phys;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	/* Fixed M23: 4 KiB pages, 39-bit VA, 48-bit PA. No guessed table format. */
	if (PAGE_SHIFT != 12 || CONFIG_ARM64_VA_BITS != 39 ||
	    CONFIG_ARM64_PA_BITS != 48)
		return -EOPNOTSUPP;
	root_phys = read_sysreg(ttbr1_el1) & GENMASK_ULL(47, PAGE_SHIFT);
	pgd = (pgd_t *)phys_to_virt(root_phys) + pgd_index(address);
	if (pgd_none(READ_ONCE(*pgd)) || pgd_bad(READ_ONCE(*pgd)))
		return -EFAULT;
	p4d = p4d_offset(pgd, address);
	if (p4d_none(READ_ONCE(*p4d)) || p4d_bad(READ_ONCE(*p4d)))
		return -EFAULT;
	pud = pud_offset(p4d, address);
	if (pud_none(READ_ONCE(*pud)))
		return -EFAULT;
	if (pud_sect(READ_ONCE(*pud))) {
		*descriptor = pud_val(READ_ONCE(*pud));
		return 0;
	}
	if (pud_bad(READ_ONCE(*pud)))
		return -EFAULT;
	pmd = pmd_offset(pud, address);
	if (pmd_none(READ_ONCE(*pmd)))
		return -EFAULT;
	if (pmd_sect(READ_ONCE(*pmd))) {
		*descriptor = pmd_val(READ_ONCE(*pmd));
		return 0;
	}
	if (pmd_bad(READ_ONCE(*pmd)))
		return -EFAULT;
	pte = pte_offset_kernel(pmd, address);
	if (!pte_present(READ_ONCE(*pte)))
		return -EFAULT;
	*descriptor = pte_val(READ_ONCE(*pte));
	return 0;
}

static int __init w202_hub_map_audit_init(void)
{
	struct device_node *node;
	void *mapping;
	u32 words[17];
	u64 descriptor;
	int i, result;

	node = of_find_node_by_path("/sprd-sensorhub");
	if (!node)
		return -ENODEV;
	result = of_device_is_compatible(node, "sprd,qogirn6pro-sensorhub") ? 0 : -ENODEV;
	of_node_put(node);
	if (result)
		return result;
	ram_pfn_valid = pfn_valid(PHYS_PFN(0x6500d000));
	mapping = shmem_ram_vmap_nocache_ex(9, 0, 0x6500d000, sizeof(words));
	if (!mapping)
		return -ENOMEM;
	result = mapping_attribute((unsigned long)mapping, &descriptor);
	if (!result) {
		attribute_index = (descriptor & PTE_ATTRINDX_MASK) >> 2;
		mair_byte = (read_sysreg(mair_el1) >> (attribute_index * 8)) & 0xff;
		for (i = 0; i < ARRAY_SIZE(words); i++)
			words[i] = READ_ONCE(((u32 *)mapping)[i]);
		header_ring_count = words[0];
		pr_info("w202-hub-map-audit: pfn_valid=%d attr=%u MAIR=0x%02x rings=%u\n",
			ram_pfn_valid, attribute_index, mair_byte, header_ring_count);
		print_hex_dump(KERN_INFO, "w202-hub-map-header: ", DUMP_PREFIX_OFFSET,
			       16, 4, words, sizeof(words), false);
	}
	shmem_ram_unmap_ex(9, 0, mapping);
	return result;
}

static void __exit w202_hub_map_audit_exit(void) { }
module_init(w202_hub_map_audit_init);
module_exit(w202_hub_map_audit_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202DS read-only CH SBUF mapping attribute audit");
