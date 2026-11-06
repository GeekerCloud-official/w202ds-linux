// SPDX-License-Identifier: GPL-2.0-only
/* W202DS Hall input: stock GPIO binding, measured active-low lid switch. */
#include <linux/atomic.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

struct w202_hall {
	struct gpio_desc *line;
	struct input_dev *input;
	struct mutex report_lock;
	atomic64_t irq_count;
};

static int w202_hall_report(struct w202_hall *hall)
{
	int raw;

	mutex_lock(&hall->report_lock);
	raw = gpiod_get_raw_value_cansleep(hall->line);
	if (raw >= 0) {
		input_report_switch(hall->input, SW_LID, !raw);
		input_sync(hall->input);
	}
	mutex_unlock(&hall->report_lock);
	return raw < 0 ? raw : 0;
}

static irqreturn_t w202_hall_irq(int irq, void *data)
{
	struct w202_hall *hall = data;

	atomic64_inc(&hall->irq_count);
	if (w202_hall_report(hall))
		dev_err_ratelimited(&hall->input->dev, "Hall GPIO read failed\n");
	return IRQ_HANDLED;
}

static ssize_t raw_level_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct w202_hall *hall = dev_get_drvdata(dev);
	int raw = gpiod_get_raw_value_cansleep(hall->line);

	return raw < 0 ? raw : sysfs_emit(buf, "%d\n", raw);
}
static DEVICE_ATTR_RO(raw_level);

static ssize_t closed_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct w202_hall *hall = dev_get_drvdata(dev);
	int raw = gpiod_get_raw_value_cansleep(hall->line);

	return raw < 0 ? raw : sysfs_emit(buf, "%d\n", !raw);
}
static DEVICE_ATTR_RO(closed);

static ssize_t irq_count_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct w202_hall *hall = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lld\n", atomic64_read(&hall->irq_count));
}
static DEVICE_ATTR_RO(irq_count);

static struct attribute *w202_hall_attrs[] = {
	&dev_attr_raw_level.attr,
	&dev_attr_closed.attr,
	&dev_attr_irq_count.attr,
	NULL,
};
ATTRIBUTE_GROUPS(w202_hall);

static void w202_hall_disable_supply(void *data)
{
	regulator_disable(data);
}

static int w202_hall_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct of_phandle_args args;
	struct resource resource;
	struct regulator *supply;
	struct w202_hall *hall;
	int gpio, irq, result;

	/* The polarity was physically verified only on this exact W202DS line. */
	result = of_parse_phandle_with_args(dev->of_node, "ah,gpio_irq",
					  "#gpio-cells", 0, &args);
	if (result)
		return dev_err_probe(dev, result, "Hall GPIO binding missing\n");
	result = of_address_to_resource(args.np, 0, &resource);
	of_node_put(args.np);
	if (result || resource.start != 0x64170000 || args.args_count != 2 ||
	    args.args[0] != 148 || args.args[1] != 0)
		return dev_err_probe(dev, -ENODEV, "Unverified Hall GPIO mapping\n");

	hall = devm_kzalloc(dev, sizeof(*hall), GFP_KERNEL);
	if (!hall)
		return -ENOMEM;
	mutex_init(&hall->report_lock);
	atomic64_set(&hall->irq_count, 0);
	platform_set_drvdata(pdev, hall);

	/* Legacy resolver is needed for the stock, nonstandard ah,gpio_irq name. */
	gpio = of_get_named_gpio(dev->of_node, "ah,gpio_irq", 0);
	if (gpio < 0)
		return dev_err_probe(dev, gpio, "Hall GPIO not ready\n");
	result = devm_gpio_request_one(dev, gpio, GPIOF_IN, "w202ds-hall");
	if (result)
		return dev_err_probe(dev, result, "Cannot acquire Hall input\n");
	hall->line = gpio_to_desc(gpio);
	if (!hall->line)
		return -ENODEV;

	/* Original W202DS DT has no supply; never invent a regulator mapping. */
	if (of_find_property(dev->of_node, "vddio-supply", NULL)) {
		supply = devm_regulator_get(dev, "vddio");
		if (IS_ERR(supply))
			return dev_err_probe(dev, PTR_ERR(supply), "Hall supply unavailable\n");
		result = regulator_enable(supply);
		if (result)
			return dev_err_probe(dev, result, "Hall supply enable failed\n");
		result = devm_add_action_or_reset(dev, w202_hall_disable_supply, supply);
		if (result)
			return result;
	}

	irq = gpiod_to_irq(hall->line);
	if (irq < 0)
		return dev_err_probe(dev, irq, "Hall IRQ unavailable\n");
	hall->input = devm_input_allocate_device(dev);
	if (!hall->input)
		return -ENOMEM;
	hall->input->name = "w202ds-hall";
	hall->input->phys = "w202ds-hall/input0";
	hall->input->id.bustype = BUS_HOST;
	input_set_capability(hall->input, EV_SW, SW_LID);
	result = input_register_device(hall->input);
	if (result)
		return dev_err_probe(dev, result, "Hall input registration failed\n");
	result = devm_request_threaded_irq(dev, irq, NULL, w202_hall_irq,
		IRQF_ONESHOT | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
		"w202ds-hall", hall);
	if (result)
		return dev_err_probe(dev, result, "Hall IRQ request failed\n");
	result = w202_hall_report(hall);
	if (result)
		return dev_err_probe(dev, result, "Initial Hall GPIO read failed\n");
	dev_info(dev, "GPIO148 both-edge input; low=closed, high=open\n");
	return 0;
}

static const struct of_device_id w202_hall_of_match[] = {
	{ .compatible = "ah,hall_ic" },
	{ }
};
MODULE_DEVICE_TABLE(of, w202_hall_of_match);

static struct platform_driver w202_hall_driver = {
	.probe = w202_hall_probe,
	.driver = {
		.name = "w202ds-hall",
		.of_match_table = w202_hall_of_match,
		.dev_groups = w202_hall_groups,
	},
};
module_platform_driver(w202_hall_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202DS GPIO148 Hall lid switch, event driven, verified active low");
