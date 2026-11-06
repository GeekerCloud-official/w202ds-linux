// SPDX-License-Identifier: GPL-2.0-only
/*
 * W202DS Qogirn6Pro DSI/DPHY takeover for the independently tested r6p0 KMS.
 * The command path is intentionally explicit: write "start" to dsi_control.
 */
#include <linux/backlight.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/mutex.h>
#include "w202_panel_sync.h"
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "w202_r6p0.h"
#include "w202_dsc.h"

#define DSI_SOFT_RESET			0x04
#define DSI_MASK_PROTOCOL_INT		0x0c
#define DSI_MASK_INTERNAL_INT		0x14
#define DSI_MODE_CFG			0x18
#define DSI_VIRTUAL_CHANNEL_ID		0x1c
#define DSI_DPI_VIDEO_FORMAT		0x20
#define DSI_VIDEO_PKT_CONFIG		0x24
#define DSI_VIDEO_LINE_HBLK_TIME	0x28
#define DSI_VIDEO_LINE_TIME		0x2c
#define DSI_VIDEO_VBLK_LINES		0x30
#define DSI_VIDEO_VACTIVE_LINES		0x34
#define DSI_VID_MODE_CFG		0x38
#define DSI_TIMEOUT_CNT_CLK_CONFIG	0x40
#define DSI_HTX_TO_CONFIG		0x44
#define DSI_LRX_H_TO_CONFIG		0x48
#define DSI_TX_ESC_CLK_CONFIG		0x5c
#define DSI_VBLK_CMD_TRANS_LIMIT	0x64
#define DSI_CMD_MODE_CFG		0x68
#define DSI_GEN_HDR			0x6c
#define DSI_GEN_PLD_DATA		0x70
#define DSI_PHY_CLK_LANE_LP_CTRL	0x74
#define DSI_PHY_INTERFACE_CTRL		0x78
#define DSI_CMD_MODE_STATUS		0x98
#define DSI_PHY_STATUS			0x9c
#define DSI_PHY_MIN_STOP_TIME		0xa0
#define DSI_PHY_LANE_NUM_CONFIG		0xa4
#define DSI_PHY_CLKLANE_TIME_CONFIG	0xa8
#define DSI_PHY_DATALANE_TIME_CONFIG	0xac
#define DSI_MAX_READ_TIME		0xb0
#define DSI_RX_PKT_CHECK_CONFIG		0xb4
#define DSI_TA_EN			0xb8
#define DSI_EOTP_EN			0xbc
#define DSI_VIDEO_NULLPKT_SIZE		0xc0
#define DSI_VIDEO_SIG_DELAY_CONFIG	0xd0
#define DSI_PHY_TST_CTRL0		0xf0
#define DSI_PHY_TST_CTRL1		0xf4

#define DSI_PHY_TESTCLR		BIT(0)
#define DSI_PHY_TESTCLK		BIT(1)
#define DSI_PHY_TESTDIN		GENMASK(7, 0)
#define DSI_PHY_TESTDOUT		GENMASK(15, 8)
#define DSI_PHY_TESTEN		BIT(16)
#define DSI_PHY_RESET_N		BIT(1)
#define DSI_PHY_SHUTDOWN		BIT(0)
#define DSI_PHY_CLK_EN		BIT(2)
#define DSI_PHY_LOCK		BIT(1)
#define DSI_GEN_CMD_WDATA_FIFO_EMPTY BIT(3)
#define DSI_GEN_CMD_CMD_FIFO_EMPTY	BIT(5)
#define DSI_VIDEO_FORMAT_DSC		12
#define DSI_VIDEO_MODE_BURST_SYNC_PULSES 2
#define DSI_CMD_LP_WRITE_MASK	(BIT(8) | BIT(9) | BIT(10) | BIT(14) | \
				 BIT(11) | BIT(12) | BIT(13) | BIT(16) | BIT(17) | \
				 BIT(18) | BIT(19) | BIT(24))

#define W202_DPU_DSC_BASE		0x1a00
#define W202_DSC_CTRL			0x00
#define W202_DSC_PIC_SIZE		0x04
#define W202_DSC_GRP_SIZE		0x08
#define W202_DSC_SLICE_SIZE		0x0c
#define W202_DSC_H_TIMING		0x10
#define W202_DSC_V_TIMING		0x14
#define W202_DSC_CFG0			0x18

#define W202_PHY_VCO_LOW_MHZ		1250
#define W202_PHY_VCO_MID_MHZ		1800
#define W202_PHY_VCO_HIGH_MHZ		2500
#define W202_PHY_REF_CLK_MHZ		26

struct w202_dsi_state {
	struct mutex lock;
	struct clk *dsi_eb;
	struct gpio_desc *reset_gpio;
	struct regmap *dsi_reset_regmap;
	struct regmap *dphy_enable_regmap;
	struct regmap *dphy_power_regmap;
	void __iomem *regs;
	u32 dsi_reset_reg;
	u32 dsi_reset_mask;
	u32 dphy_enable_reg;
	u32 dphy_enable_mask;
	u32 dphy_power_reg;
	u32 dphy_power_mask;
	bool dsi_clock_on;
	bool dphy_power_on;
	bool dphy_gate_on;
	bool active;
	bool panel_sleep;
	bool prepare_attempted;
	bool prepared;
	bool touched;
	bool dpu_stopped;
	bool timing_pending;
	int last_error;
	char state[24];
	u32 command_count;
	u32 pll_freq_khz;
};

struct w202_pll_params {
	u8 refin, cp_s, fdk_s, div, nint, lpf_sel, out_sel, vco_band;
	u8 sdm_en, det_delay;
	u32 fvco_mhz, kint;
};

static inline u32 w202_reg_read(struct w202_dsi_state *s, u32 reg)
{
	return readl(s->regs + reg);
}

static inline void w202_reg_write(struct w202_dsi_state *s, u32 reg, u32 val)
{
	writel(val, s->regs + reg);
}

static inline void w202_reg_update(struct w202_dsi_state *s, u32 reg,
				   u32 mask, u32 val)
{
	u32 old = w202_reg_read(s, reg);

	w202_reg_write(s, reg, (old & ~mask) | (val & mask));
}

static int w202_get_syscon(struct device_node *np, const char *property,
			   struct regmap **map, u32 *offset, u32 *mask)
{
	u32 args[2];

	*map = syscon_regmap_lookup_by_phandle_args(np, property, 2, args);
	if (IS_ERR(*map))
		return PTR_ERR(*map);

	*offset = args[0];
	*mask = args[1];
	return 0;
}

static int w202_test_write(struct w202_dsi_state *s, u8 reg, u8 value)
{
	w202_reg_update(s, DSI_PHY_TST_CTRL1, DSI_PHY_TESTEN, DSI_PHY_TESTEN);
	w202_reg_update(s, DSI_PHY_TST_CTRL1, DSI_PHY_TESTDIN, reg);
	w202_reg_update(s, DSI_PHY_TST_CTRL0, DSI_PHY_TESTCLK, DSI_PHY_TESTCLK);
	w202_reg_update(s, DSI_PHY_TST_CTRL0, DSI_PHY_TESTCLK, 0);
	w202_reg_update(s, DSI_PHY_TST_CTRL1, DSI_PHY_TESTEN, 0);
	w202_reg_update(s, DSI_PHY_TST_CTRL1, DSI_PHY_TESTDIN, value);
	w202_reg_update(s, DSI_PHY_TST_CTRL0, DSI_PHY_TESTCLK, DSI_PHY_TESTCLK);
	w202_reg_update(s, DSI_PHY_TST_CTRL0, DSI_PHY_TESTCLK, 0);
	return 0;
}

static int w202_calc_pll(struct w202_pll_params *pll, u32 freq_khz)
{
	u64 tmp;
	u32 potential;
	int i;

	if (!pll || !freq_khz)
		return -EINVAL;

	memset(pll, 0, sizeof(*pll));
	potential = freq_khz / 1000;
	for (i = 0; i < 4; i++) {
		if (potential >= W202_PHY_VCO_LOW_MHZ &&
		    potential <= W202_PHY_VCO_HIGH_MHZ) {
			pll->fvco_mhz = potential;
			pll->out_sel = BIT(i);
			break;
		}
		potential <<= 1;
	}
	if (!pll->fvco_mhz)
		return -ERANGE;

	if (pll->fvco_mhz <= W202_PHY_VCO_MID_MHZ) {
		pll->vco_band = 0;
		pll->lpf_sel = 0;
	} else {
		pll->vco_band = 1;
		pll->lpf_sel = 0;
	}

	pll->nint = pll->fvco_mhz / W202_PHY_REF_CLK_MHZ;
	tmp = (u64)pll->fvco_mhz * 100 * 1000000;
	do_div(tmp, W202_PHY_REF_CLK_MHZ);
	tmp -= (u64)pll->nint * 100 * 1000000;
	tmp *= BIT(20);
	do_div(tmp, 100000000);
	pll->kint = tmp;
	pll->refin = 3;
	pll->sdm_en = 1;
	pll->fdk_s = 1;
	pll->cp_s = 2;
	pll->det_delay = 1;
	return 0;
}

static int w202_program_pll(struct w202_dsi_state *s, u32 freq_khz)
{
	struct w202_pll_params p;
	u8 reg03, reg04, reg06, reg08, reg0b, reg0e, reg0f;
	int ret;

	ret = w202_calc_pll(&p, freq_khz);
	if (ret)
		return ret;

	reg03 = BIT(0) | BIT(1) | (p.lpf_sel << 2);
	reg04 = (p.div & 7) | BIT(3) | (p.cp_s << 5) | (p.fdk_s << 7);
	reg06 = p.nint & 0x7f;
	reg08 = p.vco_band | (p.sdm_en << 1) | (p.refin << 2);
	reg0b = p.out_sel | ((p.kint << 4) & 0xf0);
	reg0e = BIT(4);
	reg0f = p.det_delay & 3;

	ret = w202_test_write(s, 0x03, reg03);
	if (!ret) ret = w202_test_write(s, 0x04, reg04);
	if (!ret) ret = w202_test_write(s, 0x07, 0);
	if (!ret) ret = w202_test_write(s, 0x08, reg08);
	if (!ret) ret = w202_test_write(s, 0x09, p.kint >> 12);
	if (!ret) ret = w202_test_write(s, 0x0a, (p.kint >> 4) & 0xff);
	if (!ret) ret = w202_test_write(s, 0x0b, reg0b);
	if (!ret) ret = w202_test_write(s, 0x0c, 0);
	if (!ret) ret = w202_test_write(s, 0x0d, 0);
	if (!ret) ret = w202_test_write(s, 0x0e, reg0e);
	if (!ret) ret = w202_test_write(s, 0x0f, reg0f);
	if (!ret) ret = w202_test_write(s, 0x06, reg06);
	if (!ret)
		pr_info("w202-dsi: SharkL5 PLL %u kHz VCO=%u MHz N=%u K=0x%x out=%u\n",
			freq_khz, p.fvco_mhz, p.nint, p.kint, p.out_sel);
	return ret;
}

enum w202_timing_id {
	W202_REQUEST_TIME,
	W202_PREPARE_TIME,
	W202_ZERO_TIME,
	W202_TRAIL_TIME,
	W202_EXIT_TIME,
	W202_CLKPOST_TIME,
};

static int w202_program_timing(struct w202_dsi_state *s,
			       enum w202_timing_id id, u8 clk, u8 data)
{
	static const u8 clk_addr[][2] = {
		{ 0x31, 0x90 }, { 0x32, 0x91 }, { 0x33, 0x92 },
		{ 0x34, 0x93 }, { 0x36, 0x95 }, { 0x35, 0x94 },
	};
	static const u8 data_addr[][8] = {
		{ 0x41, 0x51, 0x61, 0x71, 0xa0, 0xb0, 0xc0, 0xd0 },
		{ 0x42, 0x52, 0x62, 0x72, 0xa1, 0xb1, 0xc1, 0xd1 },
		{ 0x43, 0x53, 0x63, 0x73, 0xa2, 0xb2, 0xc2, 0xd2 },
		{ 0x44, 0x54, 0x64, 0x74, 0xa3, 0xb3, 0xc3, 0xd3 },
		{ 0x46, 0x56, 0x66, 0x76, 0xa5, 0xb5, 0xc5, 0xd5 },
		{ 0, 0, 0, 0, 0, 0, 0, 0 },
	};
	int i, ret;

	ret = w202_test_write(s, clk_addr[id][0], clk);
	if (!ret)
		ret = w202_test_write(s, clk_addr[id][1], clk);
	if (ret || id == W202_CLKPOST_TIME)
		return ret;
	for (i = 0; i < ARRAY_SIZE(data_addr[id]); i++) {
		ret = w202_test_write(s, data_addr[id][i], data);
		if (ret)
			return ret;
	}
	return 0;
}

static u32 w202_average(u32 a, u32 b)
{
	return min(a, b) + abs((s32)b - (s32)a) / 2;
}

static int w202_program_dphy_timing(struct w202_dsi_state *s, u32 freq_khz)
{
	const u32 scale = 100;
	const u32 factor = 2;
	u32 ui, byteck, half_byteck, constant, prepare;
	u8 clk, data;
	int ret;

	if (!freq_khz)
		return -EINVAL;
	ui = 1000 * scale / (freq_khz / 1000);
	byteck = ui << 3;
	half_byteck = ui << 2;
	constant = ui << 1;

	clk = DIV_ROUND_UP(50 * scale * (factor << 1), byteck) - 2;
	data = clk;
	ret = w202_program_timing(s, W202_REQUEST_TIME, clk, data);
	if (ret)
		return ret;

	prepare = w202_average(38 * scale, 95 * scale);
	clk = DIV_ROUND_UP(prepare, half_byteck) - 1;
	data = DIV_ROUND_UP(w202_average(40 * scale + 4 * ui,
						85 * scale + 6 * ui), half_byteck) - 1;
	ret = w202_program_timing(s, W202_PREPARE_TIME, clk, data);
	if (ret)
		return ret;

	clk = DIV_ROUND_UP(300 * scale * factor + prepare -
			   525 * byteck / 100, byteck) - 2;
	data = DIV_ROUND_UP((145 * scale + 10 * ui) * factor +
			    w202_average(40 * scale + 4 * ui,
					 85 * scale + 6 * ui) -
			    525 * byteck / 100, byteck) - 2;
	ret = w202_program_timing(s, W202_ZERO_TIME, clk, data);
	if (ret)
		return ret;

	clk = DIV_ROUND_UP(60 * scale * factor - constant, half_byteck);
	data = DIV_ROUND_UP(max(8 * ui, 60 * scale + 4 * ui) * 3 / 2 -
			    constant, half_byteck) - 2;
	ret = w202_program_timing(s, W202_TRAIL_TIME, clk, data);
	if (ret)
		return ret;

	clk = DIV_ROUND_UP(100 * scale * factor, byteck) - 2;
	data = clk;
	ret = w202_program_timing(s, W202_EXIT_TIME, clk, data);
	if (ret)
		return ret;

	clk = DIV_ROUND_UP((60 * scale + 52 * ui) * factor, byteck) - 2;
	return w202_program_timing(s, W202_CLKPOST_TIME, clk, clk);
}

static int w202_wait_pll(struct w202_dsi_state *s)
{
	int i;

	for (i = 0; i < 50000; i++) {
		if (w202_reg_read(s, DSI_PHY_STATUS) & DSI_PHY_LOCK)
			return 0;
		udelay(3);
	}
	return -ETIMEDOUT;
}

static int w202_dphy_init(struct w202_dsi_state *s, u32 freq_khz, u32 lanes)
{
	int ret;

	w202_reg_update(s, DSI_PHY_INTERFACE_CTRL, DSI_PHY_RESET_N, 0);
	w202_reg_update(s, DSI_PHY_INTERFACE_CTRL, DSI_PHY_SHUTDOWN, 0);
	w202_reg_update(s, DSI_PHY_INTERFACE_CTRL, DSI_PHY_CLK_EN, 0);
	w202_reg_update(s, DSI_PHY_TST_CTRL0, DSI_PHY_TESTCLR, 0);
	w202_reg_update(s, DSI_PHY_TST_CTRL0, DSI_PHY_TESTCLR,
			DSI_PHY_TESTCLR);
	w202_reg_update(s, DSI_PHY_TST_CTRL0, DSI_PHY_TESTCLR, 0);

	ret = w202_program_pll(s, freq_khz);
	if (ret)
		return ret;
	ret = w202_program_dphy_timing(s, freq_khz);
	if (ret)
		return ret;

	w202_reg_update(s, DSI_PHY_INTERFACE_CTRL, DSI_PHY_SHUTDOWN,
			DSI_PHY_SHUTDOWN);
	w202_reg_update(s, DSI_PHY_INTERFACE_CTRL, DSI_PHY_RESET_N,
			DSI_PHY_RESET_N);
	w202_reg_write(s, DSI_PHY_MIN_STOP_TIME, 0x1c);
	w202_reg_update(s, DSI_PHY_INTERFACE_CTRL, DSI_PHY_CLK_EN,
			DSI_PHY_CLK_EN);
	w202_reg_write(s, DSI_PHY_LANE_NUM_CONFIG, lanes - 1);

	return w202_wait_pll(s);
}

static int w202_dsi_host_init(struct w202_dsi_state *s,
			      u32 byte_clk_khz, u32 esc_clk_khz)
{
	u32 div, max_read, data_hs2lp, data_lp2hs, clk_hs2lp, clk_lp2hs;

	if (!byte_clk_khz || !esc_clk_khz)
		return -EINVAL;
	w202_reg_write(s, DSI_SOFT_RESET, 0);
	w202_reg_write(s, DSI_MASK_PROTOCOL_INT, ~0U);
	w202_reg_write(s, DSI_MASK_INTERNAL_INT, ~0U);
	w202_reg_write(s, DSI_MODE_CFG, 1);
	w202_reg_update(s, DSI_EOTP_EN, BIT(0) | BIT(1), 0);
	w202_reg_update(s, DSI_RX_PKT_CHECK_CONFIG, BIT(0) | BIT(1), BIT(0) | BIT(1));
	w202_reg_write(s, DSI_TA_EN, 1);
	w202_reg_update(s, DSI_VIRTUAL_CHANNEL_ID, GENMASK(3, 0), 0);

	div = DIV_ROUND_UP(byte_clk_khz, esc_clk_khz);
	w202_reg_write(s, DSI_TX_ESC_CLK_CONFIG, div);
	/* LK uses the vendor default 0x8000 ns; preserve this board profile. */
	max_read = DIV_ROUND_UP(0x8000U * (u64)byte_clk_khz, 1000000);
	w202_reg_write(s, DSI_MAX_READ_TIME, max_read);
	data_hs2lp = DIV_ROUND_CLOSEST(120 * byte_clk_khz, 1000000);
	data_lp2hs = DIV_ROUND_CLOSEST(500 * byte_clk_khz, 1000000);
	clk_hs2lp = 4;
	clk_lp2hs = 15;
	w202_reg_write(s, DSI_PHY_DATALANE_TIME_CONFIG,
			(data_hs2lp << 16) | data_lp2hs);
	w202_reg_write(s, DSI_PHY_CLKLANE_TIME_CONFIG,
			(clk_hs2lp << 16) | clk_lp2hs);
	w202_reg_write(s, DSI_SOFT_RESET, 1);
	pr_info("w202-dsi: host ready, escape divider=%u read-time=%u\n",
		div, max_read);
	return 0;
}

static int w202_wait_fifo(struct w202_dsi_state *s, u32 mask)
{
	int i;

	for (i = 0; i < 20000; i++) {
		if ((w202_reg_read(s, DSI_CMD_MODE_STATUS) & mask) == mask)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int w202_generic_write(struct w202_dsi_state *s,
			      const u8 *payload, u16 len)
{
	u8 type, wc0, wc1;
	u32 word;
	int i, j, ret;

	if (!payload && len)
		return -EINVAL;
	ret = w202_wait_fifo(s, DSI_GEN_CMD_WDATA_FIFO_EMPTY);
	if (ret)
		return ret;

	if (len > 2) {
		for (i = 0; i < len; i += j) {
			word = 0;
			for (j = 0; j < 4 && i + j < len; j++)
				word |= (u32)payload[i + j] << (j * 8);
			w202_reg_write(s, DSI_GEN_PLD_DATA, word);
		}
		wc0 = len & 0xff;
		wc1 = len >> 8;
		type = 0x29;
	} else {
		wc0 = len ? payload[0] : 0;
		wc1 = len > 1 ? payload[1] : 0;
		type = len == 0 ? 0x03 : (len == 1 ? 0x13 : 0x23);
	}

	ret = w202_wait_fifo(s, DSI_GEN_CMD_CMD_FIFO_EMPTY);
	if (ret)
		return ret;
	w202_reg_write(s, DSI_GEN_HDR, type | (wc0 << 8) | (wc1 << 16));
	return w202_wait_fifo(s, DSI_GEN_CMD_CMD_FIFO_EMPTY |
			      DSI_GEN_CMD_WDATA_FIFO_EMPTY);
}

static int w202_panel_stream(struct w202_dsi_state *s,
			     struct device_node *panel_cfg,
			     const char *property)
{
	const u8 *commands;
	int length, offset = 0;
	int count = 0;

	commands = of_get_property(panel_cfg, property, &length);
	if (!commands || length < 4)
		return -ENOENT;

	while (offset < length) {
		u8 data_type, wait_ms;
		u16 payload_len;
		int ret;

		if (length - offset < 4)
			return -EINVAL;
		data_type = commands[offset];
		wait_ms = commands[offset + 1];
		payload_len = ((u16)commands[offset + 2] << 8) |
			      commands[offset + 3];
		offset += 4;
		if (!payload_len || payload_len > 128 || payload_len > length - offset)
			return -EINVAL;
		if (data_type != 0x05 && data_type != 0x15 && data_type != 0x39)
			return -EOPNOTSUPP;
		ret = w202_generic_write(s, commands + offset, payload_len);
		if (ret)
			return ret;
		offset += payload_len;
		count++;
		if (wait_ms)
			msleep(wait_ms);
	}
	if (!strcmp(property, "sprd,initial-command")) {
		if (count != 64 || length != 796)
			return -EINVAL;
		s->command_count = count;
	} else if ((!strcmp(property, "sprd,sleep-in-command") && count != 3) ||
		   (!strcmp(property, "sprd,sleep-out-command") && count != 2)) {
		return -EINVAL;
	}
	return 0;
}

/* Fixed HX83102J profile, verified against the panel's E1 PPS prefix and
 * a read-only M10 hardware snapshot (2026-10-20). The generic vendor DSC
 * calculator defaults to xmit_delay=475, whereas this panel requires 512.
 * Write the complete known-good encoder profile; do not inherit LK registers.
 */
static int w202_config_dsc(struct w202_dpu *d, struct w202_dsi_state *s)
{
	static const u32 profile[] = {
		0x2000000b, 0x078004b0, 0x001400c8, 0x05000fa0,
		0x09c0a004, 0x58001c08, 0x306c8200, 0x00080202,
		0x0494050e, 0x10f01800, 0x003316b6, 0x382a1c0e,
		0x69625446, 0x7b797770, 0x00007e7d, 0x01000102,
		0x09be0940, 0x19fa19fc, 0x1a3819f8, 0x1ab61a78,
		0x2b342af6, 0x3b742b74, 0x00006bf4,
	};
	int i;

	/* Configuration first, enable last, while DPU is stopped. */
	for (i = 1; i < ARRAY_SIZE(profile); i++)
		writel(profile[i], d->regs + W202_DPU_DSC_BASE + i * 4);
	writel(profile[0], d->regs + W202_DPU_DSC_BASE);
	for (i = 0; i < ARRAY_SIZE(profile); i++)
		if (readl(d->regs + W202_DPU_DSC_BASE + i * 4) != profile[i])
			return -EIO;
	pr_info("w202-dsi: Linux programmed all 23 DSC registers: 8bpc/8bpp, 1200x1920, slices 600x20, xmit_delay=512\n");
	return 0;
}

static int w202_validate_panel(struct device_node *np)
{
	static const u8 panel_pps[] = {
		0xe1, 0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x07,
		0x80, 0x02, 0x58, 0x00, 0x14, 0x02, 0x58, 0x02,
		0x58, 0x02, 0x00, 0x02, 0x2c, 0x00, 0x20, 0x02,
		0x02, 0x00, 0x08, 0x00, 0x0c, 0x05, 0x0e, 0x04,
		0x94, 0x18, 0x00, 0x10, 0xf0, 0x03, 0x0c, 0x20,
		0x00, 0x06, 0x0b, 0x0b, 0x33,
	};
	const u8 *cmd;
	int len, off = 0, count = 0, pps_count = 0;

	if (!of_property_read_bool(np, "sprd,use-gen-write") ||
	    of_property_read_bool(np, "sprd,use-dcs-write"))
		return -EINVAL;
	cmd = of_get_property(np, "sprd,initial-command", &len);
	if (!cmd || len != 796)
		return -EINVAL;
	while (off < len) {
		u16 n;

		if (len - off < 4)
			return -EINVAL;
		n = ((u16)cmd[off + 2] << 8) | cmd[off + 3];
		if ((cmd[off] != 0x05 && cmd[off] != 0x15 && cmd[off] != 0x39) ||
		    !n || n > 128 || n > len - off - 4)
			return -EINVAL;
		off += 4;
		if (n == sizeof(panel_pps) && !memcmp(cmd + off, panel_pps, n))
			pps_count++;
		off += n;
		count++;
	}
	return count == 64 && pps_count == 1 ? 0 : -EINVAL;
}

static int w202_video_config(struct w202_dsi_state *s)
{
	const u32 hactive = 1200, vactive = 1920;
	const u32 hsync = 4, hbp = 160, hfp = 156;
	const u32 vfp = 1408, vbp = 28, vsync = 8;
	const u32 byte_clk_khz = 112000;
	const u32 pixel_clk_hz = 307200000;
	u32 ratio, hline, hs_to, div, hsa_cycles, hbp_cycles;

	ratio = byte_clk_khz * 1000 / (pixel_clk_hz / 1000);
	hline = hactive + hsync + hbp + hfp;
	hsa_cycles = hsync * ratio / 1000;
	hbp_cycles = hbp * ratio / 1000;

	w202_reg_write(s, DSI_SOFT_RESET, 0);
	w202_reg_update(s, DSI_DPI_VIDEO_FORMAT, GENMASK(5, 0),
			DSI_VIDEO_FORMAT_DSC);
	w202_reg_update(s, DSI_VID_MODE_CFG, GENMASK(1, 0),
			DSI_VIDEO_MODE_BURST_SYNC_PULSES);
	w202_reg_update(s, DSI_VID_MODE_CFG, GENMASK(13, 8) | BIT(15),
			GENMASK(13, 8) | BIT(15));
	w202_reg_write(s, DSI_VBLK_CMD_TRANS_LIMIT, 0x80);
	w202_reg_write(s, DSI_VIDEO_SIG_DELAY_CONFIG,
			95 * hline * ratio / 100000);
	w202_reg_write(s, DSI_VIDEO_LINE_TIME, hline * ratio / 1000);
	w202_reg_write(s, DSI_VIDEO_LINE_HBLK_TIME,
			(hsa_cycles << 16) | hbp_cycles);
	w202_reg_write(s, DSI_VIDEO_VACTIVE_LINES, vactive);
	w202_reg_write(s, DSI_VIDEO_VBLK_LINES,
			/* The r1p0 host has a 10-bit VFP field; match vendor bitfield
		 * truncation. DPU/DSC retain the complete 1408-line porch. */
			(vfp & 0x3ff) | ((vbp & 0x3ff) << 10) |
			((vsync & 0x3ff) << 20));
	w202_reg_write(s, DSI_VIDEO_PKT_CONFIG, hactive);
	w202_reg_write(s, DSI_VIDEO_NULLPKT_SIZE, 0);
	w202_reg_write(s, DSI_TIMEOUT_CNT_CLK_CONFIG, 0);

	hs_to = hline * vactive + 2;
	for (div = 0x80; div < hs_to && div > 2; div--) {
		if (!(hs_to % div)) {
			w202_reg_write(s, DSI_TIMEOUT_CNT_CLK_CONFIG, div);
			w202_reg_write(s, DSI_LRX_H_TO_CONFIG, hs_to / div);
			w202_reg_write(s, DSI_HTX_TO_CONFIG, hs_to / div);
			break;
		}
	}
	w202_reg_write(s, DSI_MASK_PROTOCOL_INT, ~0U);
	w202_reg_write(s, DSI_MASK_INTERNAL_INT, ~0U);
	w202_reg_write(s, DSI_SOFT_RESET, 1);

	return 0;
}

static int w202_panel_reset(struct w202_dsi_state *s)
{
	int ret;

	ret = gpiod_direction_output(s->reset_gpio, 1);
	if (ret)
		return ret;
	msleep(5);
	gpiod_set_value_cansleep(s->reset_gpio, 0);
	msleep(2);
	gpiod_set_value_cansleep(s->reset_gpio, 1);
	msleep(55);
	return 0;
}

static int w202_set_panel_brightness(struct w202_dsi_state *s)
{
	static const u8 brightness[] = { 0x51, 0x06, 0x66 };
	static const u8 control[] = { 0x53, 0x2c };
	int ret;

	ret = w202_generic_write(s, brightness, sizeof(brightness));
	if (ret)
		return ret;
	return w202_generic_write(s, control, sizeof(control));
}

static int w202_dsi_prepare_state(struct w202_dpu *d)
{
	struct w202_dsi_state *s = d->dsi;
	struct device_node *dsi_np = NULL, *dphy_np = NULL, *panel_np = NULL;
	struct device_node *panel_cfg = NULL;
	struct resource res;
	int ret;

	if (s->prepared)
		return 0;
	if (s->prepare_attempted)
		return -EALREADY;
	s->prepare_attempted = true;
	dsi_np = of_find_node_by_path("/soc/dpuvsp/dsi@31300000");
	dphy_np = of_find_node_by_path("/soc/dpuvsp/dphy");
	panel_cfg = of_find_node_by_path(
		"/lcds/lcd_guoxian_hx83102j_huaxing_nopen_mipi_himax");
	if (!dsi_np || !dphy_np || !panel_cfg) {
		ret = -ENODEV;
		goto out;
	}
	if (!of_device_is_compatible(dsi_np, "sprd,qogirn6pro-dsi-host") ||
	    !of_device_is_compatible(dphy_np, "sprd,qogirn6pro-dsi-phy")) {
		ret = -ENODEV;
		goto out;
	}
	panel_np = of_get_child_by_name(dsi_np, "panel");
	if (!panel_np || !of_device_is_compatible(panel_np,
						 "sprd,generic-mipi-panel")) {
		ret = -ENODEV;
		goto out;
	}
	if (of_address_to_resource(dsi_np, 0, &res)) {
		ret = -EINVAL;
		goto out;
	}
	s->regs = devm_ioremap_resource(d->dev, &res);
	if (IS_ERR(s->regs)) {
		ret = PTR_ERR(s->regs);
		s->regs = NULL;
		goto out;
	}
	ret = w202_get_syscon(dsi_np, "reset-syscon", &s->dsi_reset_regmap,
			      &s->dsi_reset_reg, &s->dsi_reset_mask);
	if (ret)
		goto out;
	ret = w202_get_syscon(dphy_np, "enable-syscon", &s->dphy_enable_regmap,
			      &s->dphy_enable_reg, &s->dphy_enable_mask);
	if (ret)
		goto out;
	ret = w202_get_syscon(dphy_np, "power-syscon", &s->dphy_power_regmap,
			      &s->dphy_power_reg, &s->dphy_power_mask);
	if (ret)
		goto out;
	s->dsi_eb = of_clk_get_by_name(dsi_np, "clk_dsi0_eb");
	if (IS_ERR(s->dsi_eb)) {
		ret = PTR_ERR(s->dsi_eb);
		s->dsi_eb = NULL;
		goto out;
	}
	s->reset_gpio = devm_fwnode_gpiod_get_index(d->dev,
						    of_fwnode_handle(panel_np),
						    "reset", 0, GPIOD_ASIS,
						    "w202-panel-reset");
	if (IS_ERR(s->reset_gpio)) {
		ret = PTR_ERR(s->reset_gpio);
		s->reset_gpio = NULL;
		goto out;
	}
	s->prepared = true;
	ret = 0;
out:
	of_node_put(panel_np);
	of_node_put(panel_cfg);
	of_node_put(dphy_np);
	of_node_put(dsi_np);
	return ret;
}

/* Acquire the inherited DSI gate without reset, mode or panel commands. */
int w202_dsi_claim_clocks(struct w202_dpu *d)
{
	struct w202_dsi_state *s;
	int ret;

	lockdep_assert_held(&d->hw_lock);
	if (!d->dsi) {
		d->dsi = devm_kzalloc(d->dev, sizeof(*d->dsi), GFP_KERNEL);
		if (!d->dsi)
			return -ENOMEM;
		mutex_init(&d->dsi->lock);
		strscpy(d->dsi->state, "inherited", sizeof(d->dsi->state));
	}
	s = d->dsi;
	mutex_lock(&s->lock);
	ret = w202_dsi_prepare_state(d);
	if (!ret && !s->dsi_clock_on) {
		ret = clk_prepare_enable(s->dsi_eb);
		if (!ret)
			s->dsi_clock_on = true;
	}
	mutex_unlock(&s->lock);
	return ret;
}

int w202_dsi_start(struct w202_dpu *d)
{
	struct w202_dsi_state *s;
	struct device_node *dsi_np = NULL, *panel_cfg = NULL;
	u32 lanes = 4, freq_khz = 896000, esc_khz = 20000;
	int ret;

	if (!d || !d->dev || !d->regs)
		return -EINVAL;
	if (!d->dsi) {
		d->dsi = devm_kzalloc(d->dev, sizeof(*d->dsi), GFP_KERNEL);
		if (!d->dsi)
			return -ENOMEM;
		mutex_init(&d->dsi->lock);
		strscpy(d->dsi->state, "inherited", sizeof(d->dsi->state));
	}
	s = d->dsi;
	mutex_lock(&s->lock);
	if (s->active) {
		ret = 0;
		goto out_unlock;
	}
	if (s->touched) {
		ret = -EALREADY;
		goto out_unlock;
	}
	if (readl(d->regs) != 0x600) {
		ret = -EBUSY;
		strscpy(s->state, "DPU-state-mismatch", sizeof(s->state));
		goto out_unlock;
	}
	ret = w202_dsi_prepare_state(d);
	if (ret) {
		strscpy(s->state, "precheck-failed", sizeof(s->state));
		goto out_unlock;
	}
	dsi_np = of_find_node_by_path("/soc/dpuvsp/dsi@31300000");
	panel_cfg = of_find_node_by_path(
		"/lcds/lcd_guoxian_hx83102j_huaxing_nopen_mipi_himax");
	if (!dsi_np || !panel_cfg) {
		ret = -ENODEV;
		goto fail_nodes;
	}
	of_property_read_u32(panel_cfg, "sprd,dsi-lane-number", &lanes);
	of_property_read_u32(panel_cfg, "sprd,phy-bit-clock", &freq_khz);
	of_property_read_u32(panel_cfg, "sprd,phy-escape-clock", &esc_khz);
	if (lanes != 4 || freq_khz != 896000 || esc_khz != 20000) {
		ret = -EINVAL;
		goto fail_nodes;
	}
	ret = w202_validate_panel(panel_cfg);
	if (ret)
		goto fail_nodes;
	strscpy(s->state, "clock-enable", sizeof(s->state));

	if (!s->dsi_clock_on) {
		ret = clk_prepare_enable(s->dsi_eb);
		if (ret)
			goto fail_nodes;
		s->dsi_clock_on = true;
	}
	/* Serialize against DRM scanout and wait for the stop IRQ before reset. */
	strscpy(s->state, "dpu-stop", sizeof(s->state));
	reinit_completion(&d->stopped);
	writel(BIT(0), d->regs + 0x254);
	writel(readl(d->regs + 0x08) | BIT(1), d->regs + 0x08);
	if (!wait_for_completion_timeout(&d->stopped, msecs_to_jiffies(500))) {
		/* DSI has not been touched; resume the existing rescue scanout. */
		writel((readl(d->regs + 0x08) & ~BIT(1)) | BIT(0) | BIT(4),
		       d->regs + 0x08);
		ret = -ETIMEDOUT;
		goto fail_nodes;
	}
	s->dpu_stopped = true;
	s->touched = true;
	strscpy(s->state, "dpu-reset-init", sizeof(s->state));
	ret = w202_dpu_hw_init(d);
	if (ret)
		goto fail_nodes;
	strscpy(s->state, "host-reset", sizeof(s->state));
	ret = regmap_update_bits(s->dsi_reset_regmap, s->dsi_reset_reg,
				s->dsi_reset_mask, s->dsi_reset_mask);
	if (ret)
		goto fail_nodes;
	udelay(10);
	ret = regmap_update_bits(s->dsi_reset_regmap, s->dsi_reset_reg,
				s->dsi_reset_mask, 0);
	if (ret)
		goto fail_nodes;
	/* Qogirn6Pro DPHY power is active-low; the gate uses bits 0 and 1. */
	ret = regmap_update_bits(s->dphy_power_regmap, s->dphy_power_reg,
				s->dphy_power_mask, 0);
	if (ret)
		goto fail_nodes;
	s->dphy_power_on = true;
	udelay(100);
	ret = regmap_update_bits(s->dphy_enable_regmap, s->dphy_enable_reg,
				s->dphy_enable_mask, s->dphy_enable_mask);
	if (ret)
		goto fail_nodes;
	s->dphy_gate_on = true;
	strscpy(s->state, "host-init", sizeof(s->state));
	ret = w202_dsi_host_init(s, freq_khz / 8, esc_khz);
	if (ret)
		goto fail_nodes;
	/* Match the vendor sequence: program DPI timings before powering D-PHY. */
	ret = w202_video_config(s);
	if (ret)
		goto fail_nodes;
	w202_reg_write(s, DSI_MODE_CFG, 1);
	strscpy(s->state, "phy-init", sizeof(s->state));
	ret = w202_dphy_init(s, freq_khz, lanes);
	if (ret)
		goto fail_nodes;
	s->pll_freq_khz = freq_khz;

	strscpy(s->state, "dsc-init", sizeof(s->state));
	ret = w202_config_dsc(d, s);
	if (ret)
		goto fail_nodes;
	w202_reg_write(s, DSI_MODE_CFG, 1);
	w202_reg_update(s, DSI_CMD_MODE_CFG, DSI_CMD_LP_WRITE_MASK,
			DSI_CMD_LP_WRITE_MASK);
	strscpy(s->state, "panel-reset", sizeof(s->state));
	ret = w202_panel_reset(s);
	if (ret)
		goto fail_nodes;
	strscpy(s->state, "panel-commands", sizeof(s->state));
	ret = w202_panel_stream(s, panel_cfg, "sprd,initial-command");
	if (ret)
		goto fail_nodes;
	ret = w202_set_panel_brightness(s);
	if (ret)
		goto fail_nodes;
	/* Vendor order: video mode, host state reset, continuous HS clock. */
	strscpy(s->state, "video-handoff", sizeof(s->state));
	w202_reg_write(s, DSI_MODE_CFG, 2);
	w202_reg_write(s, DSI_SOFT_RESET, 0);
	udelay(100);
	w202_reg_write(s, DSI_SOFT_RESET, 1);
	w202_reg_update(s, DSI_PHY_CLK_LANE_LP_CTRL, BIT(0), BIT(0));
	if (w202_reg_read(s, DSI_PHY_LANE_NUM_CONFIG) != lanes - 1) {
		ret = -EIO;
		goto fail_nodes;
	}
	ret = w202_wait_pll(s);
	if (ret)
		goto fail_nodes;
	/* After a DPU reset the global timing/DSC groups must be committed
	 * with the first scanout, before an unconfigured frame can run. */
	reinit_completion(&d->updated);
	reinit_completion(&d->configured);
	writel(BIT(16) | BIT(18), d->regs + 0x254);
	writel(BIT(0) | BIT(2) | BIT(4), d->regs + 0x08);
	s->dpu_stopped = false;
	if (!wait_for_completion_timeout(&d->updated, msecs_to_jiffies(500))) {
		ret = -ETIMEDOUT;
		strscpy(s->state, "scanout-timeout", sizeof(s->state));
		goto fail_nodes;
	}
	if (!wait_for_completion_timeout(&d->configured, msecs_to_jiffies(500))) {
		ret = -ETIMEDOUT;
		strscpy(s->state, "global-latch-timeout", sizeof(s->state));
		goto fail_nodes;
	}
	s->active = true;
	strscpy(s->state, "linux-active", sizeof(s->state));
	drm_info(&d->drm,
		 "Qogirn6Pro DSI/DPHY initialized by Linux: 4 lanes, 896 MHz, panel commands=%u, DSI=%08x PHY=%08x DSC=%08x\n",
		 s->command_count, w202_reg_read(s, DSI_MODE_CFG),
		 w202_reg_read(s, DSI_PHY_STATUS),
		 readl(d->regs + W202_DPU_DSC_BASE + W202_DSC_CTRL));
	ret = 0;
	goto fail_nodes;

fail_nodes:
	of_node_put(panel_cfg);
	of_node_put(dsi_np);
	if (ret) {
		s->last_error = ret;
		drm_err(&d->drm,
			"Linux DSI takeover failed (%d), stage=%s stopped=%u; DSI=%08x PHY=%08x\n",
			ret, s->state, s->dpu_stopped, s->dsi_clock_on ? w202_reg_read(s, DSI_MODE_CFG) : 0,
			s->dsi_clock_on ? w202_reg_read(s, DSI_PHY_STATUS) : 0);
	}
out_unlock:
	mutex_unlock(&s->lock);
	return ret;
}

/* Vendor dpu_vrr(): stop scanout, update DPU/DSC/DSI porches, latch
 * global configuration, resume. No panel commands, sleep or reset.
 * Handle initial DRM acquisition with an already running physical scanout.
 */
int w202_dsi_set_mode(struct w202_dpu *d, const struct drm_display_mode *mode)
{
 struct w202_dsi_state *s = d->dsi;
 u32 vfp, timing, host;
 int ret = 0;

 lockdep_assert_held(&d->hw_lock);
 if (drm_mode_equal(mode, &d->mode))
  return 0;
 if (!drm_mode_equal(mode, &d->modes[0]) &&
     !drm_mode_equal(mode, &d->modes[1]))
  return -EINVAL;
 if (!s)
  return -ENODEV;
 mutex_lock(&s->lock);
 if (!s->active && (!s->panel_sleep || !s->dpu_stopped)) {
  ret = -EBUSY;
  goto out;
 }
 if (s->active) {
  reinit_completion(&d->stopped);
  writel(BIT(0), d->regs + 0x254);
  writel(readl(d->regs + 0x08) | BIT(1), d->regs + 0x08);
  if (!wait_for_completion_timeout(&d->stopped, msecs_to_jiffies(500))) {
   ret = -ETIMEDOUT;
   goto out;
  }
  s->dpu_stopped = true;
 }
 vfp = mode->vsync_start - mode->vdisplay;
 timing = vfp << 20 | 28 << 8 | 8;
 host = (vfp & 0x3ff) | 28 << 10 | 8 << 20;
 writel(timing, d->regs + 0x268);
 writel(timing, d->regs + W202_DPU_DSC_BASE + W202_DSC_V_TIMING);
 w202_reg_write(s, DSI_VIDEO_VBLK_LINES, host);
 if (readl(d->regs + 0x268) != timing ||
     readl(d->regs + W202_DPU_DSC_BASE + W202_DSC_V_TIMING) != timing ||
     w202_reg_read(s, DSI_VIDEO_VBLK_LINES) != host) {
  ret = -EIO;
  goto out;
 }
 drm_mode_copy(&d->mode, mode);
 s->timing_pending = true;
 if (s->active) {
  reinit_completion(&d->updated);
  reinit_completion(&d->configured);
  writel(BIT(16) | BIT(18), d->regs + 0x254);
  writel((readl(d->regs + 0x08) & ~BIT(1)) | BIT(0) | BIT(2) | BIT(4),
         d->regs + 0x08);
  if (!wait_for_completion_timeout(&d->updated, msecs_to_jiffies(500)) ||
      !wait_for_completion_timeout(&d->configured, msecs_to_jiffies(500))) {
   ret = -ETIMEDOUT;
   goto out;
  }
  s->dpu_stopped = false;
  s->timing_pending = false;
 }
 drm_info(&d->drm, "DRM timing selected: %dHz vfp=%u total=%u, DPU/DSC=%08x DSI=%08x; no panel commands\n",
          drm_mode_vrefresh(mode), vfp, mode->vtotal, timing, host);
out:
 if (ret)
  s->last_error = ret;
 mutex_unlock(&s->lock);
 return ret;
}

/* Called with hw_lock held by the DRM commit path. Retained-power standby
 * deliberately uses sleep-in/out, not panel reset: the Himax touch firmware
 * shares this panel's reset domain and must survive ordinary DRM blanking.
 */
int w202_dsi_set_power(struct w202_dpu *d, bool on)
{
	struct w202_dsi_state *s = d->dsi;
	struct device_node *np;
	int ret = 0;

	lockdep_assert_held(&d->hw_lock);
	if (!s || !s->touched)
		return -ENODEV;
	mutex_lock(&s->lock);
	if (s->active == on) {
		/* Retry a previously failed touch wake on the next real commit. */
		if (on)
			ret = w202_panel_set_awake(true);
		goto out;
	}
	if (!s->panel_sleep && !s->active) {
		ret = -EIO;
		goto out;
	}
	np = of_find_node_by_path(
		"/lcds/lcd_guoxian_hx83102j_huaxing_nopen_mipi_himax");
	if (!np) {
		ret = -ENODEV;
		goto out;
	}
	if (!on) {
		/* Drain touch IRQ processing before the shared controller sleeps.
		 * On a partial display failure leave it quiet until a real wake. */
		ret = w202_panel_set_awake(false);
		if (ret)
			goto put_node;
		ret = backlight_disable(d->backlight);
		if (ret)
			goto put_node;
		reinit_completion(&d->stopped);
		writel(BIT(0), d->regs + 0x254);
		writel(readl(d->regs + 0x08) | BIT(1), d->regs + 0x08);
		if (!wait_for_completion_timeout(&d->stopped, msecs_to_jiffies(500))) {
			ret = -ETIMEDOUT;
			goto put_node;
		}
		s->dpu_stopped = true;
		w202_reg_write(s, DSI_MODE_CFG, 1);
		ret = w202_panel_stream(s, np, "sprd,sleep-in-command");
		if (!ret) {
			s->panel_sleep = true;
			s->active = false;
			strscpy(s->state, "linux-standby", sizeof(s->state));
		}
	} else {
		w202_reg_write(s, DSI_MODE_CFG, 1);
		ret = w202_panel_stream(s, np, "sprd,sleep-out-command");
		if (ret)
			goto put_node;
		ret = w202_set_panel_brightness(s);
		if (ret)
			goto put_node;
		w202_reg_write(s, DSI_MODE_CFG, 2);
		w202_reg_write(s, DSI_SOFT_RESET, 0);
		udelay(100);
		w202_reg_write(s, DSI_SOFT_RESET, 1);
		w202_reg_update(s, DSI_PHY_CLK_LANE_LP_CTRL, BIT(0), BIT(0));
		ret = w202_wait_pll(s);
		if (ret)
			goto put_node;
		reinit_completion(&d->updated);
		if (s->timing_pending)
			reinit_completion(&d->configured);
		writel(BIT(16) | BIT(18), d->regs + 0x254);
		writel((readl(d->regs + 0x08) & ~BIT(1)) | BIT(0) |
		       (s->timing_pending ? BIT(2) : 0), d->regs + 0x08);
		writel(readl(d->regs + 0x08) | BIT(4), d->regs + 0x08);
		if (!wait_for_completion_timeout(&d->updated, msecs_to_jiffies(500))) {
			ret = -ETIMEDOUT;
			goto put_node;
		}
		if (s->timing_pending &&
		    !wait_for_completion_timeout(&d->configured, msecs_to_jiffies(500))) {
			ret = -ETIMEDOUT;
			goto put_node;
		}
		s->timing_pending = false;
		s->dpu_stopped = false;
		s->panel_sleep = false;
		s->active = true;
		ret = backlight_enable(d->backlight);
		strscpy(s->state, "linux-active", sizeof(s->state));
		if (!ret)
			ret = w202_panel_set_awake(true);
	}
put_node:
	of_node_put(np);
	if (ret)
		s->last_error = ret;
	else
		drm_info(&d->drm, "DRM panel %s completed without reset\n",
			 on ? "wake" : "standby");
out:
	mutex_unlock(&s->lock);
	return ret;
}

ssize_t w202_dsi_status(struct w202_dpu *d, char *buf)
{
	struct w202_dsi_state *s;
	u32 mode = 0, phy = 0, dsc = 0;

	if (!d || !buf)
		return -EINVAL;
	s = d->dsi;
	if (s && s->regs && s->dsi_clock_on) {
		mode = w202_reg_read(s, DSI_MODE_CFG);
		phy = w202_reg_read(s, DSI_PHY_STATUS);
		dsc = readl(d->regs + W202_DPU_DSC_BASE + W202_DSC_CTRL);
	}
	return sysfs_emit(buf, "state=%s error=%d stopped=%u commands=%u pll_khz=%u refresh_hz=%u vtotal=%u dsi_mode=%08x phy_status=%08x dsc_ctrl=%08x\n",
			  s ? s->state : "inherited", s ? s->last_error : 0,
			  s ? s->dpu_stopped : 0, s ? s->command_count : 0,
			  s ? s->pll_freq_khz : 0, drm_mode_vrefresh(&d->mode),
			  d->mode.vtotal, mode, phy, dsc);
}
