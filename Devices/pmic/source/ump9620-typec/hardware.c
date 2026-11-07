// SPDX-License-Identifier: GPL-2.0
/* UMP9620 PHY calibration derived from Spreadtrum sc27xx_pd.c (2019). */
#include <linux/nvmem-consumer.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include "controller.h"

int w202_typec_snapshot(struct w202_typec *pd)
{
	static const u32 addresses[] = {
		0x2008, 0x200c, 0x2378, 0x234c,
		0x380, 0x384, 0x388, 0x38c, 0x3a0, 0x3bc, 0x3d4, 0x3f0,
		0xe08, 0xe10, 0xe14, 0xe18, 0xe30, 0xe50, 0xe54, 0xe58,
	};
	int i, ret;

	BUILD_BUG_ON(ARRAY_SIZE(addresses) > ARRAY_SIZE(pd->saved));
	if (pd->aon_apb) {
		ret = regmap_read(pd->aon_apb,
			UMS9620_REG_AON_APB_MIPI_CSI_POWER_CTRL, &pd->saved_aon_clock);
		if (ret)
			return ret;
		pd->aon_clock_saved = true;
	}
	for (i = 0; i < ARRAY_SIZE(addresses); i++) {
		pd->saved[i].address = addresses[i];
		ret = regmap_read(pd->regmap, addresses[i], &pd->saved[i].value);
		if (ret)
			return ret;
		pd->saved_count++;
		dev_info(pd->dev, "inherited %04x=%04x\n", addresses[i], pd->saved[i].value);
	}
	return 0;
}

void w202_typec_restore(struct w202_typec *pd)
{
	int i, ret;

	/* IRQs must be disabled before restoring the saved interrupt masks.
	 * Restore clocks last. W1C/status/FIFO registers are never restored.
	 */
	for (i = pd->saved_count - 1; i >= 0; i--) {
		ret = regmap_write(pd->regmap, pd->saved[i].address, pd->saved[i].value);
		if (ret)
			dev_err(pd->dev, "Restore %04x failed: %d\n", pd->saved[i].address, ret);
	}
	/* This register is shared with USB PHYs; restore only our clock bit. */
	if (pd->aon_clock_saved) {
		ret = regmap_update_bits(pd->aon_apb,
			UMS9620_REG_AON_APB_MIPI_CSI_POWER_CTRL,
			UMS9620_MASK_AON_APB_R2G_ANALOG_BB_TOP_SINDRV_ENA,
			pd->saved_aon_clock);
		if (ret)
			dev_err(pd->dev, "Restore AON PD clock failed: %d\n", ret);
	}
}

int w202_typec_enable_clock(struct w202_typec *pd)
{
	u32 value;
	int ret;

	if (!pd->aon_apb)
		return 0;
	/* Match stock sc27xx_pd_check_vbus_cc_status on every cable event. */
	ret = regmap_update_bits(pd->aon_apb,
		UMS9620_REG_AON_APB_MIPI_CSI_POWER_CTRL,
		UMS9620_MASK_AON_APB_R2G_ANALOG_BB_TOP_SINDRV_ENA,
		UMS9620_MASK_AON_APB_R2G_ANALOG_BB_TOP_SINDRV_ENA);
	if (!ret)
		ret = regmap_read(pd->aon_apb,
			UMS9620_REG_AON_APB_MIPI_CSI_POWER_CTRL, &value);
	return ret ?: (value & UMS9620_MASK_AON_APB_R2G_ANALOG_BB_TOP_SINDRV_ENA ? 0 : -EIO);
}

static int read_calibration(struct device_node *node, const char *name, u32 *value)
{
	struct nvmem_cell *cell;
	size_t length;
	void *data;
	int ret = 0;

	cell = of_nvmem_cell_get(node, name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);
	data = nvmem_cell_read(cell, &length);
	nvmem_cell_put(cell);
	if (IS_ERR(data))
		return PTR_ERR(data);
	if (length < sizeof(u16))
		ret = -EINVAL;
	else
		*value = get_unaligned_le16(data);
	kfree(data);
	return ret;
}

static int sc27xx_pd_rc_ref_cal(struct w202_typec *pd)
{
	u32 vol = 0;
	u32 pd_ref = 0;
	u32 val = 0;
	u32 cfg2_bit7 = 0, cfg0_vth = 0, cfg0_vtl = 0, typec_ibis = 0;
	u32 cfg0, cfg2, cfg0_mask, cfg2_mask = 0;
	int ret;

	vol = (pd->rc_cal >> UMP9620_RC_EFUSE_SHIFT) & 0x7f;
	pd_ref = (pd->ref_cal >> UMP9620_REF_EFUSE_SHIFT) & 0x7;

	/*
	 * According to the datasheet, depending on the calibration
	 * voltage, different register values should be configured.
	 */

	if (vol <= 26)
		val = 0x0;
	if (vol >= 27 && vol <= 37)
		val = 0x1;
	if (vol >= 38 && vol <= 48)
		val = 0x2;
	if (vol >= 49 && vol <= 59)
		val = 0x3;
	if (vol >= 60 && vol <= 69)
		val = 0x4;
	if (vol >= 70 && vol <= 80)
		val = 0x5;
	if (vol >= 81 && vol <= 90)
		val = 0x6;
	if (vol >= 91 && vol <= 127)
		val = 0x7;

	switch (pd_ref) {
	case 0:
		cfg2_bit7 = 0;
		cfg0_vth = 0;
		cfg0_vtl = 2;
		typec_ibis = 0;
		break;
	case 1:
		cfg2_bit7 = 0;
		cfg0_vth = 1;
		cfg0_vtl = 1;
		typec_ibis = 1;
		break;
	case 2:
		cfg2_bit7 = 0;
		cfg0_vth = 1;
		cfg0_vtl = 1;
		typec_ibis = 0;
		break;
	case 3:
		cfg2_bit7 = 1;
		cfg0_vth = 1;
		cfg0_vtl = 1;
		typec_ibis = 0;
		break;
	case 4:
		cfg2_bit7 = 0;
		cfg0_vth = 2;
		cfg0_vtl = 0;
		typec_ibis = 0;
		break;
	}

	if (typec_ibis == 1)
		ret = regmap_update_bits(pd->regmap,
					 pd->typec_base + SC27XX_TYPEC_IBIAS,
					 SC27XX_TYPEC_RX_REF_DECREASE,
					 SC27XX_TYPEC_RX_REF_DECREASE);
	else
		ret = regmap_update_bits(pd->regmap,
					 pd->typec_base + SC27XX_TYPEC_IBIAS,
					 SC27XX_TYPEC_RX_REF_DECREASE, 0);

	cfg0 = (cfg0_vtl << SC27XX_PD_CFG0_VTL_SHIFT) & SC27XX_PD_CFG0_VTL_MASK;
	cfg0 |= (val << SC27XX_PD_CFG0_RCCAL_SHIFT) & SC27XX_PD_CFG0_RCCAL_MASK;
	cfg0 |= cfg0_vth & SC27XX_PD_CFG0_VTH_MASK;
	cfg0_mask = SC27XX_PD_CFG0_RCCAL_MASK |
		    SC27XX_PD_CFG0_VTL_MASK | SC27XX_PD_CFG0_VTH_MASK;

	ret = regmap_update_bits(pd->regmap,
				 pd->base + SC27XX_PD_PHY_CFG0,
				 cfg0_mask, cfg0);
	if (ret < 0)
		return ret;

	cfg2 = (cfg2_bit7 << SC27XX_PD_CFG2_RX_REF_CAL_SHIFT) & 0x80;
	cfg2_mask = SC27XX_PD_CFG2_RX_REF_CAL_BIT;

	return regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_PHY_CFG2,
				  cfg2_mask, cfg2);
}

static int sc27xx_pd_delta_cal(struct w202_typec *pd)
{
	u32 delta_cal = pd->delta_cal;
	u32 vol, vref_sel = 0, ref_cal = 0, delta = 0;
	u32 cfg1, cfg1_mask;

	cfg1_mask = SC27XX_PD_CC1_SW | SC27XX_PD_CC2_SW |
		    SC27XX_PD_CFG1_VREF_SEL_MASK |
		    SC27XX_PD_CFG1_REF_CAL_MASK;

	delta = ((delta_cal >> UMP9620_DELTA_EFUSE_SHIFT) & 0x7f);
	/*
	 * According to the datasheet, delta is efuse caliration
	 * vol = delta * 2 + 1000
	 */
	vol = delta * 2 + 1000;

	/*
	 * According to the datasheet, depending on the calibration
	 * voltage, different register values should be configured.
	 */

	if (vol >= 1185 && vol <= 1195) {
		vref_sel = 0x0;
		ref_cal = 0x0;
	} else if (vol >= 1175 && vol < 1185) {
		vref_sel = 0x0;
		ref_cal = 0x1;
	} else if (vol >= 1165 && vol < 1175) {
		vref_sel = 0x0;
		ref_cal = 0x2;
	} else if (vol >= 1155 && vol < 1165) {
		vref_sel = 0x0;
		ref_cal = 0x3;
	} else if (vol >= 1145 && vol < 1155) {
		vref_sel = 0x0;
		ref_cal = 0x4;
	} else if (vol >= 1135 && vol < 1145) {
		vref_sel = 0x0;
		ref_cal = 0x5;
	} else if (vol >= 1125 && vol < 1135) {
		vref_sel = 0x0;
		ref_cal = 0x6;
	} else if (vol >= 1115 && vol < 1125) {
		vref_sel = 0x1;
		ref_cal = 0x2;
	} else if (vol >= 1105 && vol < 1115) {
		vref_sel = 0x1;
		ref_cal = 0x3;
	} else if (vol >= 1095 && vol < 1105) {
		vref_sel = 0x1;
		ref_cal = 0x4;
	} else if (vol >= 1085 && vol < 1095) {
		vref_sel = 0x1;
		ref_cal = 0x5;
	} else if (vol >= 1075 && vol < 1085) {
		vref_sel = 0x1;
		ref_cal = 0x6;
	} else if (vol >= 1065 && vol < 1075) {
		vref_sel = 0x2;
		ref_cal = 0x2;
	} else if (vol >= 1055 && vol < 1065) {
		vref_sel = 0x2;
		ref_cal = 0x3;
	} else if (vol >= 1045 && vol < 1055) {
		vref_sel = 0x2;
		ref_cal = 0x4;
	} else if (vol >= 1035 && vol < 1045) {
		vref_sel = 0x2;
		ref_cal = 5;
	} else if (vol >= 1025 && vol < 1035) {
		vref_sel = 0x2;
		ref_cal = 0x6;
	} else if (vol >= 1015 && vol < 1025) {
		vref_sel = 0x3;
		ref_cal = 0x2;
	} else if (vol >= 1005 && vol < 1015) {
		vref_sel = 0x3;
		ref_cal = 0x3;
	} else if (vol >= 1000 && vol < 1005) {
		vref_sel = 0x3;
		ref_cal = 0x4;
	}

	cfg1 = SC27XX_PD_CC1_SW | SC27XX_PD_CC2_SW;
	cfg1 |= (vref_sel << SC27XX_PD_CFG1_VREF_SEL_SHIFT)
		& SC27XX_PD_CFG1_VREF_SEL_MASK;
	cfg1 |= (ref_cal << SC27XX_PD_CFG1_REF_CAL_SHIFT)
		& SC27XX_PD_CFG1_REF_CAL_MASK;

	return regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_PHY_CFG1,
				  cfg1_mask, cfg1);
}

int w202_typec_calibrate(struct w202_typec *pd)
{
	u32 cc1, cc2;
	int ret;

	ret = read_calibration(pd->dev->of_node, "pdrc_calib", &pd->rc_cal);
	if (!ret)
		ret = read_calibration(pd->dev->of_node, "pddelta_calib", &pd->delta_cal);
	if (!ret)
		ret = read_calibration(pd->typec_node, "typec_cc1_cal", &cc1);
	if (!ret)
		ret = read_calibration(pd->typec_node, "typec_cc2_cal", &cc2);
	if (ret)
		return ret;
	pd->ref_cal = pd->delta_cal;
	if (((pd->ref_cal >> UMP9620_REF_EFUSE_SHIFT) & 7) > 4)
		return -ERANGE;
	ret = regmap_write(pd->regmap, pd->typec_base + 0x3c,
		((cc1 >> 1) & 0x1f) | (((cc2 >> 11) & 0x1f) << 5));
	if (!ret)
		ret = sc27xx_pd_delta_cal(pd);
	if (!ret)
		ret = sc27xx_pd_rc_ref_cal(pd);
	return ret;
}

int w202_typec_hw_init(struct w202_typec *pd)
{
	int ret;

	ret = w202_typec_enable_clock(pd);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, UMP9620_MODULE_EN,
		SC27XX_TYPEC_PD_EN, SC27XX_TYPEC_PD_EN);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, UMP9620_ARM_CLK_EN0,
			SC27XX_CLK_PD_EN | SC27XX_CLK_PD_SEL,
			SC27XX_CLK_PD_EN | SC27XX_CLK_PD_SEL);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, UMP9620_XTL_WAIT_CTRL0,
			SC27XX_XTL_EN, SC27XX_XTL_EN);
	if (!ret && pd->reset_on_probe)
		ret = w202_typec_reset_block(pd);
	if (!ret)
		ret = w202_typec_calibrate(pd);
	/* Stock C-to-C uses digital/analogue 6 MHz, not the AP 26 MHz path. */
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CFG1,
			SC27XX_PD_PHY_13M | SC27XX_PD_RETRY_MASK | SC27XX_PD_AUTO_RETRY |
			SC27XX_PD_HEADER_REG_EN | SC27XX_PD_EN_SOP_RX |
			SC27XX_PD_EN_SOP1_RX | SC27XX_PD_EN_SOP2_RX |
			SC27XX_PD_RX_AUTO_GOOD_CRC | SC27XX_PD_TX_AUTO_GOOD_CRC |
			SC27XX_PD_FRS_DETECT_EN,
			SC27XX_PD_RETRY(3) | SC27XX_PD_AUTO_RETRY |
			SC27XX_PD_HEADER_REG_EN | SC27XX_PD_EN_SOP_RX |
			SC27XX_PD_TX_AUTO_GOOD_CRC);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_PHY_CFG2,
			SC27XX_PD_CFG2_PD_CLK_BIT, 0);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->typec_base + SC27XX_TYPEC_PD_CFG,
			SC27XX_TYPEC_PD_SUPPORT | SC27XX_TYPEC_PD_CONSTRACT,
			SC27XX_TYPEC_PD_SUPPORT);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CFG0,
			SC27XX_PD_BIST_MODE_EN | SC27XX_PD_SRC_SINK_MODE |
			SC27XX_PD_CTL_EN | SC27XX_PD_EN_SOP |
			SC27XX_PD_EN_SOP1_TX | SC27XX_PD_EN_SOP2_TX,
			SC27XX_PD_BIST_MODE_EN | SC27XX_PD_EN_SOP);
	if (!ret)
		ret = w202_pd_flush(pd, true);
	if (!ret)
		ret = regmap_write(pd->regmap, pd->base + SC27XX_INT_CLR, SC27XX_INT_CLR_MASK);
	if (!ret)
		ret = regmap_write(pd->regmap, pd->base + SC27XX_INT_EN,
			SC27XX_INT_EN_MASK);
	if (!ret)
		ret = regmap_write(pd->regmap, pd->typec_base + 0x20, 0xc7f);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, 0x234c, BIT(4), BIT(4));
	if (!ret)
		ret = regmap_write(pd->regmap, pd->typec_base + 0x10, SC27XX_TYPEC_INT_CLR_MASK);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->typec_base + 0x0c, 3, 3);
	return ret;
}
