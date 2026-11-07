// SPDX-License-Identifier: GPL-2.0
/* Optional probe-time reset of the PD block, not Type-C or the whole PMIC.
 * Register/bit: UNISOC __regs_ana_ump9620_glb.h, REG_SOFT_RST1[2].
 */
#include "controller.h"

int w202_typec_reset_block(struct w202_typec *pd)
{
	u32 value;
	int ret, release_ret;

	/* Probe only, clocks enabled, before TCPM or IRQs can access PD. */
	ret = regmap_read(pd->regmap, UMP9620_SOFT_RST1, &value);
	if (ret)
		return ret;
	if (value & UMP9620_PD_SOFT_RST)
		return -EBUSY;
	ret = regmap_update_bits(pd->regmap, UMP9620_SOFT_RST1,
			UMP9620_PD_SOFT_RST, UMP9620_PD_SOFT_RST);
	if (!ret)
		usleep_range(1000, 2000);
	/* Even a failed assertion may have reached hardware: release it. */
	release_ret = regmap_update_bits(pd->regmap, UMP9620_SOFT_RST1,
			UMP9620_PD_SOFT_RST, 0);
	if (ret || release_ret)
		return ret ?: release_ret;
	ret = regmap_read(pd->regmap, UMP9620_SOFT_RST1, &value);
	if (!ret && (value & UMP9620_PD_SOFT_RST))
		ret = -EIO;
	if (!ret)
		pd->block_reset_count++;
	return ret;
}
