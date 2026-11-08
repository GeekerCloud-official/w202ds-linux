// SPDX-License-Identifier: GPL-2.0-only
/* GNSS power control for the Marlin3 PCIe child already present in W202DS DT.
 * All shared power sequencing remains owned by wcn_bsp. This module does not
 * interpret satellite data, reassign PCIe channels, or reset the common chip.
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rfkill.h>
#include <linux/slab.h>
#include <misc/marlin_platform.h>

#if !IS_ENABLED(CONFIG_RFKILL)
#error "w202ds_gnss_power requires CONFIG_RFKILL=y or m"
#endif

struct w202_gnss_power {
	struct device *dev;
	struct rfkill *rfkill;
	struct mutex lock;
	struct notifier_block reset_notifier;
	atomic_t reset_generation;
	int started_generation;
	int last_error;
	bool owned;
	bool rfkill_ready;
};

/* The BSP exposes a request bitmap, not a receiver fix or electrical status. */
static bool w202_gnss_requested(void)
{
	return !!(marlin_get_power() & BIT(MARLIN_GNSS));
}

static int w202_gnss_stop(struct w202_gnss_power *power)
{
	int ret;

	if (!power->owned)
		return 0;
	if (!w202_gnss_requested()) {
		power->owned = false;
		return 0;
	}
	ret = stop_marlin(MARLIN_GNSS);
	if (!ret)
		power->owned = false;
	return ret;
}

static int w202_gnss_set_block(void *data, bool blocked)
{
	struct w202_gnss_power *power = data;
	int generation, ret;

	mutex_lock(&power->lock);
	if (blocked) {
		ret = w202_gnss_stop(power);
		goto done;
	}
	if (!READ_ONCE(power->rfkill_ready)) {
		ret = -EAGAIN;
		goto done;
	}

	generation = atomic_read(&power->reset_generation);
	if (power->owned && w202_gnss_requested() &&
	    generation == power->started_generation) {
		ret = 0;
		goto done;
	}

	/* A previous WCN fault invalidates our successful-start observation.
	 * Release only our GNSS request before trying the normal start path.
	 */
	ret = w202_gnss_stop(power);
	if (ret)
		goto done;
	if (w202_gnss_requested()) {
		/* The vendor bitmap is not a reference counter. Do not acquire
		 * or later clear a request belonging to an Android HAL/debugger.
		 */
		ret = -EBUSY;
		goto done;
	}

	ret = start_marlin(MARLIN_GNSS);
	/* A failed lower-level rollback may leave our request set. Retain its
	 * ownership so a later block/remove can retry releasing it.
	 */
	power->owned = w202_gnss_requested();
	if (ret)
		goto done;
	if (!power->owned ||
	    generation != atomic_read(&power->reset_generation)) {
		int stop_ret;

		stop_ret = w202_gnss_stop(power);
		if (stop_ret)
			dev_warn(power->dev, "GNSS cleanup failed: %d\n", stop_ret);
		ret = -EIO;
		goto done;
	}
	power->started_generation = generation;

done:
	power->last_error = ret;
	if (ret && !blocked)
		rfkill_set_sw_state(power->rfkill, true);
	mutex_unlock(&power->lock);
	return ret;
}

static const struct rfkill_ops w202_gnss_rfkill_ops = {
	.set_block = w202_gnss_set_block,
};

static int w202_gnss_reset(struct notifier_block *notifier,
			   unsigned long event, void *data)
{
	struct w202_gnss_power *power;

	power = container_of(notifier, struct w202_gnss_power, reset_notifier);
	/* This is an atomic notifier: record invalidation and notify rfkill,
	 * never sleep, stop hardware, or restart a shared WCN subsystem here.
	 * Re-enabling after recovery must pass through set_block again.
	 */
	atomic_inc(&power->reset_generation);
	/* rfkill_register() initializes event work after publishing the device.
	 * Do not queue an event until registration is entirely complete.
	 */
	if (READ_ONCE(power->rfkill_ready))
		rfkill_set_sw_state(power->rfkill, true);
	return NOTIFY_OK;
}

static ssize_t power_requested_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", w202_gnss_requested());
}
static DEVICE_ATTR_RO(power_requested);

static ssize_t last_error_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct w202_gnss_power *power = dev_get_drvdata(dev);
	int error;

	mutex_lock(&power->lock);
	error = power->last_error;
	mutex_unlock(&power->lock);
	return sysfs_emit(buf, "%d\n", error);
}
static DEVICE_ATTR_RO(last_error);

static struct attribute *w202_gnss_attrs[] = {
	&dev_attr_power_requested.attr,
	&dev_attr_last_error.attr,
	NULL,
};
ATTRIBUTE_GROUPS(w202_gnss);

static int w202_gnss_probe(struct platform_device *pdev)
{
	struct w202_gnss_power *power;
	int ret;

	/* The integrated-WCN and Marlin3lite PMIC sequences differ. Do not
	 * bind them using guessed TSEN/TCXO writes copied from another SoC.
	 */
	if (!pdev->dev.parent ||
	    !of_device_is_compatible(pdev->dev.parent->of_node,
				     "unisoc,marlin3_pcie"))
		return -ENODEV;

	power = devm_kzalloc(&pdev->dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;
	power->dev = &pdev->dev;
	mutex_init(&power->lock);
	atomic_set(&power->reset_generation, 0);
	platform_set_drvdata(pdev, power);

	power->rfkill = rfkill_alloc("w202ds-gnss", &pdev->dev, RFKILL_TYPE_GPS,
				    &w202_gnss_rfkill_ops, power);
	if (!power->rfkill)
		return -ENOMEM;
	/* Registering the interface itself must not start satellite reception. */
	rfkill_init_sw_state(power->rfkill, true);
	power->reset_notifier.notifier_call = w202_gnss_reset;
	ret = atomic_notifier_chain_register(&wcn_reset_notifier_list,
					     &power->reset_notifier);
	if (ret)
		goto destroy;
	ret = rfkill_register(power->rfkill);
	if (ret)
		goto unregister_notifier;
	WRITE_ONCE(power->rfkill_ready, true);
	return 0;

unregister_notifier:
	atomic_notifier_chain_unregister(&wcn_reset_notifier_list,
					 &power->reset_notifier);
destroy:
	rfkill_destroy(power->rfkill);
	return ret;
}

static void w202_gnss_remove(struct platform_device *pdev)
{
	struct w202_gnss_power *power = platform_get_drvdata(pdev);
	int ret;

	atomic_notifier_chain_unregister(&wcn_reset_notifier_list,
					 &power->reset_notifier);
	WRITE_ONCE(power->rfkill_ready, false);
	rfkill_unregister(power->rfkill);
	mutex_lock(&power->lock);
	ret = w202_gnss_stop(power);
	mutex_unlock(&power->lock);
	if (ret)
		dev_warn(&pdev->dev, "GNSS request could not be released: %d\n", ret);
	rfkill_destroy(power->rfkill);
}

static const struct of_device_id w202_gnss_of_match[] = {
	{ .compatible = "sprd,gnss" },
	{ }
};
MODULE_DEVICE_TABLE(of, w202_gnss_of_match);

static struct platform_driver w202_gnss_driver = {
	.probe = w202_gnss_probe,
	.remove = w202_gnss_remove,
	.driver = {
		.name = "w202ds-gnss-power",
		.of_match_table = w202_gnss_of_match,
		.dev_groups = w202_gnss_groups,
	},
};
module_platform_driver(w202_gnss_driver);

MODULE_DESCRIPTION("W202DS Marlin3 GNSS power control through rfkill");
MODULE_LICENSE("GPL");
