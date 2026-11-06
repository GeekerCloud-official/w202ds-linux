// SPDX-License-Identifier: GPL-2.0-only
/* W202DS ZTE serial keyboard attachment and power control.
 * Board GPIO levels follow the vendor zsk_input.c, not its stale comment:
 * ID low means attached, and EN high powers the keyboard.
 */
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include "zsk-input.h"

struct w202_zsk {
	struct device *dev;
	struct gpio_desc *id;
	struct gpio_desc *enable;
	struct delayed_work detect_work;
	struct mutex lock;
	int irq;
	bool stopping;
	bool attached;
	bool powered;
	u64 changes;
};

static void w202_zsk_power_off(void *data)
{
	struct w202_zsk *zsk = data;

	gpiod_set_raw_value_cansleep(zsk->enable, 0);
}

static void w202_zsk_detect_work(struct work_struct *work)
{
	struct w202_zsk *zsk = container_of(to_delayed_work(work),
						struct w202_zsk, detect_work);
	int raw;
	bool attached;

	mutex_lock(&zsk->lock);
	if (zsk->stopping)
		goto out;
	raw = gpiod_get_raw_value_cansleep(zsk->id);
	if (raw < 0) {
		dev_err_ratelimited(zsk->dev, "reading keyboard ID: %d\n", raw);
		goto out;
	}
	attached = !raw;
	if (attached != zsk->attached || attached != zsk->powered) {
		w202_zsk_reset_transport(attached);
		gpiod_set_raw_value_cansleep(zsk->enable, attached);
		zsk->attached = attached;
		zsk->powered = attached;
		zsk->changes++;
		dev_info(zsk->dev, "attached=%u power=%u (ID raw=%d)\n",
			 zsk->attached, zsk->powered, raw);
	}
out:
	mutex_unlock(&zsk->lock);
}

static irqreturn_t w202_zsk_irq(int irq, void *data)
{
	struct w202_zsk *zsk = data;

	mod_delayed_work(system_wq, &zsk->detect_work, msecs_to_jiffies(100));
	return IRQ_HANDLED;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct w202_zsk *zsk = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&zsk->lock);
	len = sysfs_emit(buf, "attached=%u powered=%u id_raw=%d en_raw=%d changes=%llu\n",
			 zsk->attached, zsk->powered,
			 gpiod_get_raw_value_cansleep(zsk->id),
			 gpiod_get_raw_value_cansleep(zsk->enable), zsk->changes);
	mutex_unlock(&zsk->lock);
	len += w202_zsk_uart_state(buf, len);
	return len;
}
static DEVICE_ATTR_RO(state);

static struct attribute *w202_zsk_attrs[] = {
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(w202_zsk);

static int w202_zsk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w202_zsk *zsk;
	int id_gpio, enable_gpio, ret;

	zsk = devm_kzalloc(dev, sizeof(*zsk), GFP_KERNEL);
	if (!zsk)
		return -ENOMEM;
	zsk->dev = dev;
	mutex_init(&zsk->lock);
	INIT_DELAYED_WORK(&zsk->detect_work, w202_zsk_detect_work);
	platform_set_drvdata(pdev, zsk);

	/* Support the unmodified factory DT's legacy property names. */
	id_gpio = of_get_named_gpio(dev->of_node, "zte,keyboard_id", 0);
	if (id_gpio < 0)
		return dev_err_probe(dev, id_gpio, "keyboard ID GPIO\n");
	ret = devm_gpio_request_one(dev, id_gpio, GPIOF_IN, "w202-keyboard-id");
	if (ret)
		return dev_err_probe(dev, ret, "requesting keyboard ID\n");
	zsk->id = gpio_to_desc(id_gpio);

	enable_gpio = of_get_named_gpio(dev->of_node, "zte,keyboard_en", 0);
	if (enable_gpio < 0)
		return dev_err_probe(dev, enable_gpio, "keyboard enable GPIO\n");
	ret = devm_gpio_request_one(dev, enable_gpio, GPIOF_OUT_INIT_LOW,
				    "w202-keyboard-enable");
	if (ret)
		return dev_err_probe(dev, ret, "requesting keyboard enable\n");
	zsk->enable = gpio_to_desc(enable_gpio);
	ret = devm_add_action_or_reset(dev, w202_zsk_power_off, zsk);
	if (ret)
		return ret;
	zsk->irq = gpiod_to_irq(zsk->id);
	if (zsk->irq < 0)
		return dev_err_probe(dev, zsk->irq, "keyboard attachment IRQ\n");
	ret = devm_request_threaded_irq(dev, zsk->irq, NULL, w202_zsk_irq,
			IRQF_ONESHOT | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"w202-keyboard-id", zsk);
	if (ret)
		return dev_err_probe(dev, ret, "requesting keyboard attachment IRQ\n");

	WRITE_ONCE(w202_zsk_board_online, true);
	mod_delayed_work(system_wq, &zsk->detect_work, msecs_to_jiffies(100));
	dev_info(dev, "serial keyboard attachment and power control ready\n");
	return 0;
}

static void w202_zsk_remove(struct platform_device *pdev)
{
	struct w202_zsk *zsk = platform_get_drvdata(pdev);

	disable_irq(zsk->irq);
	mutex_lock(&zsk->lock);
	zsk->stopping = true;
	mutex_unlock(&zsk->lock);
	cancel_delayed_work_sync(&zsk->detect_work);
	WRITE_ONCE(w202_zsk_board_online, false);
	w202_zsk_reset_transport(false);
}

static const struct of_device_id w202_zsk_of_match[] = {
	{ .compatible = "zte,zsk_keyboard" },
	{},
};
MODULE_DEVICE_TABLE(of, w202_zsk_of_match);

static struct platform_driver w202_zsk_driver = {
	.probe = w202_zsk_probe,
	.remove = w202_zsk_remove,
	.driver = {
		.name = "w202-zsk",
		.of_match_table = w202_zsk_of_match,
		.dev_groups = w202_zsk_groups,
	},
};
static int __init w202_zsk_init(void)
{
	int ret = tty_register_ldisc(&w202_zsk_ldisc);

	if (ret)
		return ret;
	ret = platform_driver_register(&w202_zsk_driver);
	if (ret)
		tty_unregister_ldisc(&w202_zsk_ldisc);
	return ret;
}

static void __exit w202_zsk_exit(void)
{
	platform_driver_unregister(&w202_zsk_driver);
	tty_unregister_ldisc(&w202_zsk_ldisc);
}
module_init(w202_zsk_init);
module_exit(w202_zsk_exit);

MODULE_DESCRIPTION("W202DS ZTE serial keyboard and touchpad");
MODULE_LICENSE("GPL");
