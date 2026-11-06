// SPDX-License-Identifier: GPL-2.0-only
/* Explicit UMP9620 diagnostic recovery fallback. */
#include <linux/io.h>
#include <linux/export.h>
#include <linux/kernel.h>

#define ADI_BASE	0x64400000UL
#define ADI_SIZE	0x30000

static u32 adi_read(void __iomem *b, u32 reg)
{
	int i;

	writel(reg, b + 0x28);
	for (i = 0; i < 1000000; i++) {
		u32 v = readl(b + 0x2c);

		if (!(v & BIT(31)))
			return v & 0xffff;
	}
	return 0;
}

static void adi_write(void __iomem *b, u32 reg, u32 val)
{
	int i;

	for (i = 0; i < 1000000 && !(readl(b + 0x30) & BIT(10)); i++)
		;
	writel(val, b + 0x20000 + reg);
	for (i = 0; i < 1000000 && !(readl(b + 0x30) & BIT(10)); i++)
		;
}

void w202ds_emergency_fastboot(void)
{
	void __iomem *b;
	u32 v;
	b = ioremap(ADI_BASE, ADI_SIZE);
	if (!b)
		return;
	v = adi_read(b, 0x23ac);
	adi_write(b, 0x23ac, (v & ~0xffu) | 0x30);
	v = adi_read(b, 0x23f8);
	adi_write(b, 0x23f8, v | BIT(4));
	v = adi_read(b, 0x2024);
	adi_write(b, 0x2024, v | 1);
	for (;;)
		cpu_relax();
}

EXPORT_SYMBOL_GPL(w202ds_emergency_fastboot);
