// SPDX-License-Identifier: GPL-2.0
/* UMP9620 hardware adapter for the unmodified Linux TCPM state machine.
 * Sink-only, fixed PDO fallback and bounded PPS. No source or VCONN support.
 */
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/slab.h>
#include "controller.h"

static bool activate = true;
module_param(activate, bool, 0444);
MODULE_PARM_DESC(activate, "Activate TCPM by default; 0 selects read-only diagnostics");

static bool reset_pd;
module_param(reset_pd, bool, 0444);
MODULE_PARM_DESC(reset_pd, "Experimental: reset only the PD block once before probe initialization; default off");

static unsigned int target_mv = 9000;
module_param(target_mv, uint, 0444);
MODULE_PARM_DESC(target_mv, "Fixed PDO fallback: 5000, 9000 (default) or experimental 12000 mV; needs prepared charger input OVP");

static bool pps = true;
module_param(pps, bool, 0444);
MODULE_PARM_DESC(pps, "Allow PPS by default, up to source budget (3 A); requires target_mv=9000 and a charging PPS client");

static unsigned int pps_max_mv = 9800;
module_param(pps_max_mv, uint, 0444);
MODULE_PARM_DESC(pps_max_mv, "PPS board ceiling: 9000, validated 9800 (default), or experimental 11000 mV; needs prepared charger OVP");

/* The stock connector advertises DRP/PPS before those paths are implemented.
 * Give TCPM an honest board capability without rewriting the running DT.
 */
static const u32 sink_pdos[] = {
	PDO_FIXED(5000, 3000, PDO_FIXED_USB_COMM),
};
static const struct property_entry connector_properties[] = {
	PROPERTY_ENTRY_STRING("power-role", "sink"),
	PROPERTY_ENTRY_STRING("data-role", "device"),
	PROPERTY_ENTRY_BOOL("self-powered"),
	PROPERTY_ENTRY_U32_ARRAY("sink-pdos", sink_pdos),
	PROPERTY_ENTRY_U32("op-sink-microwatt", 10000000),
	{ }
};

static struct fwnode_handle *create_connector(void)
{
	u32 pdos[] = {
		PDO_FIXED(5000, 3000, PDO_FIXED_USB_COMM),
		PDO_FIXED(target_mv, min(3000U, 18000000 / target_mv), 0),
	};
	struct property_entry properties[] = {
		PROPERTY_ENTRY_STRING("power-role", "sink"),
		PROPERTY_ENTRY_STRING("data-role", "device"),
		PROPERTY_ENTRY_BOOL("self-powered"),
		PROPERTY_ENTRY_U32_ARRAY("sink-pdos", pdos),
		PROPERTY_ENTRY_U32("op-sink-microwatt", 7500000),
		{ }
	};

	return fwnode_create_software_node(target_mv == 5000 ?
		connector_properties : properties, NULL);
}

static struct fwnode_handle *create_pps_connector(void)
{
	u32 pdos[] = {
		PDO_FIXED(5000, 3000, PDO_FIXED_USB_COMM),
		PDO_FIXED(9000, 2000, 0),
		PDO_PPS_APDO(5000, pps_max_mv, 3000),
	};
	struct property_entry properties[] = {
		PROPERTY_ENTRY_STRING("power-role", "sink"),
		PROPERTY_ENTRY_STRING("data-role", "device"),
		PROPERTY_ENTRY_BOOL("self-powered"),
		PROPERTY_ENTRY_U32_ARRAY("sink-pdos", pdos),
		PROPERTY_ENTRY_U32("op-sink-microwatt", 7500000),
		{ }
	};

	return fwnode_create_software_node(properties, NULL);
}

static int check_charger_input(unsigned int required_mv)
{
	static const char * const names[] = { "w202-bq-main", "w202-bq-slave" };
	struct power_supply *psy;
	union power_supply_propval maximum;
	int i, ret;

	if (required_mv == 5000)
		return 0;
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		psy = power_supply_get_by_name(names[i]);
		if (!psy)
			return -EPROBE_DEFER;
		ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_MAX, &maximum);
		power_supply_put(psy);
		if (ret)
			return ret;
		if (maximum.intval < required_mv * 1000)
			return -ERANGE;
	}
	return 0;
}

static void notify_supplies(void)
{
	static const char * const names[] = {
		"w202-usb-c", "w202-battery", "w202-bq-main",
	};
	struct power_supply *psy;
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		psy = power_supply_get_by_name(names[i]);
		if (psy) {
			power_supply_changed(psy);
			power_supply_put(psy);
		}
	}
}

static int get_vbus(struct tcpc_dev *tcpc)
{
	struct w202_typec *pd = tcpc_to_w202(tcpc);
	u32 value;
	/* Stock PD uses the Type-C detector's VBUS comparator, independent
	 * of the main charger's cached/held-up input-present indication.
	 */
	int ret = regmap_read(pd->regmap, pd->typec_base + SC27XX_TYPEC_DBG1, &value);

	return ret ? ret : !!(value & SC27XX_TYPEC_VBUS_OK);
}

static int get_current_limit(struct tcpc_dev *tcpc)
{
	/* TCPM calls this for default Rp. Without the callback it reports
	 * zero input budget after PD negotiation fails, suppressing even the
	 * board's existing conservative 500 mA USB fallback. Never use BC1.2
	 * or a previous high-power contract to increase this default.
	 */
	return get_vbus(tcpc) > 0 ? 500 : 0;
}

static int get_cc(struct tcpc_dev *tcpc, enum typec_cc_status *cc1,
		  enum typec_cc_status *cc2)
{
	struct w202_typec *pd = tcpc_to_w202(tcpc);
	enum typec_cc_status rp;
	u32 value;
	int ret = regmap_read(pd->regmap, pd->typec_base + SC27XX_TYPEC_STATUS, &value);

	*cc1 = TYPEC_CC_OPEN;
	*cc2 = TYPEC_CC_OPEN;
	if (ret || (value & SC27XX_STATE_MASK) != 2)
		return ret;
	switch (SC27XX_TYPEC_VBUS_CL(value)) {
	case 0: rp = TYPEC_CC_RP_DEF; break;
	case 1: rp = TYPEC_CC_RP_1_5; break;
	case 2: rp = TYPEC_CC_RP_3_0; break;
	default: return 0;
	}
	if (value & SC27XX_TYPEC_FINAL_SWITCH)
		*cc1 = rp;
	else
		*cc2 = rp;
	return 0;
}

static int set_cc(struct tcpc_dev *tcpc, enum typec_cc_status cc)
{
	struct w202_typec *pd = tcpc_to_w202(tcpc);
	u32 force;
	int ret;

	if (cc != TYPEC_CC_OPEN && cc != TYPEC_CC_RD)
		return -EOPNOTSUPP;
	/* OPEN is a real electrical disconnect. RD returns both switches to
	 * the hardware sink detector so that its status and IRQs remain valid.
	 */
	force = cc == TYPEC_CC_OPEN ? BIT(12) | BIT(14) : 0;
	mutex_lock(&pd->lock);
	if (pd->faulted) {
		mutex_unlock(&pd->lock);
		return -ESHUTDOWN;
	}
	ret = regmap_update_bits(pd->regmap, pd->typec_base + SC27XX_TYPEC_MODE,
				GENMASK(1, 0) | SC27XX_TYPEC_TRY_SRC_EN |
				SC27XX_TYPEC_TRY_SINK_EN, 0);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->typec_base + SC27XX_TYPEC_SW_CFG,
					GENMASK(15, 12), force);
	mutex_unlock(&pd->lock);
	/* Loading with a cable already attached does not generate a new edge.
	 * Re-evaluate CC after TCPM changes detector mode, including the end
	 * of its initial PORT_RESET_WAIT_OFF state. This is an event, no timer.
	 */
	if (!ret && READ_ONCE(pd->ready))
		tcpm_cc_change(pd->port);
	return ret;
}

static int set_polarity(struct tcpc_dev *tcpc, enum typec_cc_polarity polarity)
{
	/* The Type-C detector selects the PHY CC line in hardware. */
	return 0;
}

static int set_vconn(struct tcpc_dev *tcpc, bool on)
{
	return on ? -EOPNOTSUPP : 0;
}

static int set_vbus(struct tcpc_dev *tcpc, bool on, bool charge)
{
	/* Sink path is inherited; battery charging is owned by power_supply.
	 * Never enable the stock OTG regulator or pretend to source VBUS.
	 */
	return on ? -EOPNOTSUPP : 0;
}

static int set_current_limit(struct tcpc_dev *tcpc, u32 ma, u32 mv)
{
	struct w202_typec *pd = tcpc_to_w202(tcpc);

	if (ma > pd->max_ma || mv > pd->max_mv || (mv &&
	    (pd->allow_pps ? mv < 5000 || mv > pd->max_mv :
	     mv != 5000 && mv != 9000 && mv != pd->max_mv)))
		return -ERANGE;
	dev_dbg(pd->dev, "TCPM sink limit %u mV %u mA\n", mv, ma);
	notify_supplies();
	return 0;
}

static int set_roles(struct tcpc_dev *tcpc, bool attached,
		     enum typec_role power, enum typec_data_role data)
{
	struct w202_typec *pd = tcpc_to_w202(tcpc);
	u32 bits = data == TYPEC_HOST ? SC27XX_PD_DATA_ROLE : 0;
	int ret;

	if (attached && (power != TYPEC_SINK || data != TYPEC_DEVICE))
		return -EOPNOTSUPP;
	mutex_lock(&pd->lock);
	if (pd->faulted) {
		mutex_unlock(&pd->lock);
		return -ESHUTDOWN;
	}
	ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CFG0,
				SC27XX_PD_SRC_SINK_MODE, 0);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_HEAD_CFG,
					SC27XX_PD_POWER_ROLE | SC27XX_PD_DATA_ROLE, bits);
	if (!ret && !attached)
		ret = regmap_update_bits(pd->regmap, pd->typec_base + SC27XX_TYPEC_PD_CFG,
					SC27XX_TYPEC_PD_CONSTRACT, 0);
	if (!attached)
		pd->nr_source_caps = 0;
	mutex_unlock(&pd->lock);
	return ret;
}

static int tcpc_init(struct tcpc_dev *tcpc)
{
	/* Hardware setup is checked before tcpm_register_port(), whose init
	 * callback return value is not checked by this kernel's TCPM.
	 */
	return 0;
}

static irqreturn_t typec_irq(int irq, void *data)
{
	struct w202_typec *pd = data;
	u32 events, status, debug;
	int ret, rearmed = 0;

	mutex_lock(&pd->lock);
	ret = regmap_read(pd->regmap, pd->typec_base + 0x18, &events);
	if (!ret)
		ret = regmap_read(pd->regmap, pd->typec_base + SC27XX_TYPEC_STATUS, &status);
	if (!ret)
		ret = regmap_read(pd->regmap, pd->typec_base + SC27XX_TYPEC_DBG1, &debug);
	if (!ret) {
		pd->cc_irq_count++;
		pd->last_cc_events = events;
		pd->last_cc_status = status;
		pd->last_cc_debug = debug;
	}
	if (!ret)
		ret = regmap_write(pd->regmap, pd->typec_base + 0x10,
				   events & SC27XX_TYPEC_INT_CLR_MASK);
	if (!ret && READ_ONCE(pd->ready)) {
		rearmed = w202_typec_enable_clock(pd);
		if (rearmed)
			w202_pd_latch_fault(pd, "cc-clock-io-error");
		else
			rearmed = w202_pd_cc_event(pd, events, status);
	}
	mutex_unlock(&pd->lock);
	if (ret) {
		dev_err_ratelimited(pd->dev, "Type-C IRQ error %d\n", ret);
		return IRQ_HANDLED;
	}
	if (READ_ONCE(pd->ready)) {
		if (rearmed < 0)
			dev_err_ratelimited(pd->dev, "CC detach recovery failed: %d\n", rearmed);
		if (rearmed > 0)
			tcpm_tcpc_reset(pd->port);
		else
			tcpm_cc_change(pd->port);
		tcpm_vbus_change(pd->port);
		notify_supplies();
	}
	return IRQ_HANDLED;
}

static ssize_t controller_state_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct w202_typec *pd = dev_get_drvdata(dev);
	u32 status, debug, cfg0, cfg1, phy0, phy1, phy2;
	ssize_t ret;

	mutex_lock(&pd->lock);
	ret = regmap_read(pd->regmap, pd->typec_base + SC27XX_TYPEC_STATUS, &status);
	if (!ret) ret = regmap_read(pd->regmap, pd->typec_base + SC27XX_TYPEC_DBG1, &debug);
	if (!ret) ret = regmap_read(pd->regmap, pd->base + SC27XX_PD_CFG0, &cfg0);
	if (!ret) ret = regmap_read(pd->regmap, pd->base + SC27XX_PD_CFG1, &cfg1);
	if (!ret) ret = regmap_read(pd->regmap, pd->base + SC27XX_PD_PHY_CFG0, &phy0);
	if (!ret) ret = regmap_read(pd->regmap, pd->base + SC27XX_PD_PHY_CFG1, &phy1);
	if (!ret) ret = regmap_read(pd->regmap, pd->base + SC27XX_PD_PHY_CFG2, &phy2);
	if (!ret)
		ret = sysfs_emit(buf, "active=%d typec=%04x debug=%04x cfg0=%04x cfg1=%04x phy=%04x/%04x/%04x irq=%llu rx=%llu tx=%llu errors=%llu complete=%llu flags=%04x fault=%s pps=%d ceiling=%umV/%umA blocked=%llu resets=%llu attempts=%u faults=%llu rearms=%llu last_fault=%s cc_irq=%llu cc_event=%04x cc_status=%04x cc_debug=%04x reset_window=%u block_resets=%llu\n",
			pd->active, status, debug, cfg0, cfg1, phy0, phy1, phy2,
			pd->irq_count, pd->rx_count, pd->tx_count, pd->error_count,
			pd->tx_complete_count, pd->last_irq_flags,
			pd->fault_reason ?: "none", pd->allow_pps, pd->max_mv,
			pd->max_ma, pd->blocked_requests, pd->reset_count,
			pd->reset_attempts, pd->fault_count, pd->rearm_count,
			pd->last_fault_reason ?: "none", pd->cc_irq_count,
			pd->last_cc_events, pd->last_cc_status, pd->last_cc_debug,
			pd->reset_window_attempts, pd->block_reset_count);
	mutex_unlock(&pd->lock);
	return ret;
}
static DEVICE_ATTR_RO(controller_state);

static ssize_t register_snapshot_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct w202_typec *pd = dev_get_drvdata(dev);
	static const u32 addresses[] = {
		0x80, 0x84, 0x88, 0x2008, 0x200c, 0x2010, 0x2014, 0x2018, 0x2378, 0x239c,
		0x380, 0x384, 0x388, 0x38c, 0x394, 0x398, 0x39c, 0x3d4, 0x3e0,
		0xe08, 0xe0c, 0xe10, 0xe14, 0xe18, 0xe1c, 0xe20, 0xe24, 0xe28, 0xe30,
		0xe50, 0xe54, 0xe58,
	};
	u32 value;
	int i, ret, length = 0;

	mutex_lock(&pd->lock);
	for (i = 0; i < ARRAY_SIZE(addresses); i++) {
		ret = regmap_read(pd->regmap, addresses[i], &value);
		if (ret) {
			mutex_unlock(&pd->lock);
			return ret;
		}
		length += sysfs_emit_at(buf, length, "%04x=%04x\n", addresses[i], value);
	}
	mutex_unlock(&pd->lock);
	return length;
}
static DEVICE_ATTR_RO(register_snapshot);

static struct attribute *controller_attrs[] = {
	&dev_attr_controller_state.attr, &dev_attr_register_snapshot.attr, NULL,
};
ATTRIBUTE_GROUPS(controller);

static void stop_controller(struct w202_typec *pd)
{
	if (!pd->active)
		return;
	WRITE_ONCE(pd->ready, false);
	disable_irq(pd->pd_irq);
	disable_irq(pd->typec_irq);
	w202_typec_supply_unregister(pd);
	tcpm_unregister_port(pd->port);
	fwnode_remove_software_node(pd->tcpc.fwnode);
	w202_typec_restore(pd);
	pd->active = false;
}

static int probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w202_typec *pd;
	int ret;

	if (target_mv != 5000 && target_mv != 9000 && target_mv != 12000)
		return -EINVAL;
	if (pps && target_mv != 9000)
		return -EINVAL;
	if (pps_max_mv != 9000 && pps_max_mv != 9800 && pps_max_mv != 11000)
		return -EINVAL;
	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;
	pd->dev = dev;
	pd->allow_pps = pps;
	pd->reset_on_probe = reset_pd;
	pd->max_mv = pps ? pps_max_mv : target_mv;
	pd->max_ma = 3000;
	pd->regmap = dev_get_regmap(dev->parent, NULL);
	if (!pd->regmap)
		return -EPROBE_DEFER;
	if (of_property_read_bool(dev->of_node, "sprd,syscon-aon-apb")) {
		pd->aon_apb = syscon_regmap_lookup_by_phandle(dev->of_node,
							 "sprd,syscon-aon-apb");
		if (IS_ERR(pd->aon_apb))
			return PTR_ERR(pd->aon_apb);
	}
	mutex_init(&pd->lock);
	platform_set_drvdata(pdev, pd);
	ret = of_property_read_u32_index(dev->of_node, "reg", 0, &pd->base);
	if (!ret)
		ret = of_property_read_u32_index(dev->of_node, "reg", 1, &pd->typec_base);
	if (ret || pd->base != 0xe00 || pd->typec_base != 0x380)
		return -EINVAL;
	pd->typec_node = of_parse_phandle(dev->of_node, "extcon", 0);
	if (!pd->typec_node)
		return -EINVAL;
	ret = w202_typec_snapshot(pd);
	if (ret || !activate)
		goto put_node;
	ret = check_charger_input(pd->max_mv);
	if (ret) {
		dev_err(dev, "Charger input OVP not prepared for %u mV: %d\n", pd->max_mv, ret);
		goto put_node;
	}
	pd->pd_irq = platform_get_irq(pdev, 0);
	pd->typec_irq = of_irq_get(pd->typec_node, 0);
	if (pd->pd_irq < 0 || pd->typec_irq < 0) {
		ret = pd->pd_irq < 0 ? pd->pd_irq : pd->typec_irq;
		goto put_node;
	}
	ret = w202_typec_hw_init(pd);
	if (ret)
		goto restore;
	pd->tcpc = (struct tcpc_dev) {
		.init = tcpc_init, .get_vbus = get_vbus, .get_cc = get_cc,
		.get_current_limit = get_current_limit,
		.set_cc = set_cc, .set_polarity = set_polarity,
		.set_vconn = set_vconn, .set_vbus = set_vbus,
		.set_current_limit = set_current_limit, .set_roles = set_roles,
		.set_pd_rx = w202_pd_set_rx, .pd_transmit = w202_pd_transmit,
	};
	pd->tcpc.fwnode = pps ? create_pps_connector() : create_connector();
	if (IS_ERR(pd->tcpc.fwnode)) {
		ret = PTR_ERR(pd->tcpc.fwnode);
		goto restore;
	}
	ret = devm_request_threaded_irq(dev, pd->pd_irq, NULL, w202_pd_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, "w202-ump9620-pd", pd);
	if (!ret)
		ret = devm_request_threaded_irq(dev, pd->typec_irq, NULL, typec_irq,
						IRQF_ONESHOT | IRQF_NO_AUTOEN, "w202-ump9620-typec", pd);
	if (ret)
		goto remove_node;
	pd->port = tcpm_register_port(dev, &pd->tcpc);
	if (IS_ERR(pd->port)) {
		ret = PTR_ERR(pd->port);
		goto remove_node;
	}
	ret = w202_typec_supply_register(pd);
	if (ret) {
		tcpm_unregister_port(pd->port);
		goto remove_node;
	}
	pd->active = true;
	WRITE_ONCE(pd->ready, true);
	enable_irq(pd->pd_irq);
	enable_irq(pd->typec_irq);
	tcpm_cc_change(pd->port);
	tcpm_vbus_change(pd->port);
	dev_info(dev, "Linux TCPM active: sink, fixed PDO target %u mV, bounded PPS %s\n",
		 target_mv, pps ? "allowed" : "off");
	ret = 0;
	goto put_node;
remove_node:
	fwnode_remove_software_node(pd->tcpc.fwnode);
restore:
	w202_typec_restore(pd);
put_node:
	of_node_put(pd->typec_node);
	pd->typec_node = NULL;
	return ret;
}

static void remove_controller(struct platform_device *pdev)
{
	stop_controller(platform_get_drvdata(pdev));
}

static const struct of_device_id matches[] = {
	{ .compatible = "sprd,ump9620-pd" }, { }
};
MODULE_DEVICE_TABLE(of, matches);
static struct platform_driver controller_driver = {
	.probe = probe, .remove = remove_controller, .shutdown = remove_controller,
	.driver = { .name = "w202-ump9620-typec", .of_match_table = matches,
		    .dev_groups = controller_groups },
};
module_platform_driver(controller_driver);
MODULE_DESCRIPTION("W202DS UMP9620 Type-C and PD hardware for Linux TCPM");
MODULE_AUTHOR("W202DS BSP contributors");
MODULE_LICENSE("GPL");
