// SPDX-License-Identifier: GPL-2.0-only
/* W202DS: all four AW88166s share AON GPIO61. */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>

struct w202_amp_bank {
	struct generic_pm_domain domain;
	struct gpio_desc *reset;
};

static int w202_amp_attach(struct generic_pm_domain *domain, struct device *dev)
{
	/* Keep the GPIO owner loaded until the last codec has detached. */
	return try_module_get(THIS_MODULE) ? 0 : -ENODEV;
}

static void w202_amp_detach(struct generic_pm_domain *domain, struct device *dev)
{
	module_put(THIS_MODULE);
}

static void w202_amp_release(void *data)
{
	struct w202_amp_bank *bank = data;

	gpiod_set_value_cansleep(bank->reset, 1);
}

static int w202_amp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w202_amp_bank *bank;
	int ret;

	bank = devm_kzalloc(dev, sizeof(*bank), GFP_KERNEL);
	if (!bank)
		return -ENOMEM;

	/* Active-low in DT: asserted before any codec can probe. */
	bank->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(bank->reset))
		return dev_err_probe(dev, PTR_ERR(bank->reset), "shared amplifier reset\n");
	ret = devm_add_action_or_reset(dev, w202_amp_release, bank);
	if (ret)
		return ret;
	usleep_range(2000, 2500);
	gpiod_set_value_cansleep(bank->reset, 0);
	usleep_range(5000, 6000);

	/*
	 * AW88166 powers itself down through its registers. Resetting this bank
	 * at runtime would destroy the DSP state of every other attached codec.
	 * Keep reset deasserted through runtime/system suspend and codec reprobe.
	 */
	bank->domain.name = dev_name(dev);
	bank->domain.flags = GENPD_FLAG_ALWAYS_ON;
	bank->domain.attach_dev = w202_amp_attach;
	bank->domain.detach_dev = w202_amp_detach;
	ret = pm_genpd_init(&bank->domain, NULL, false);
	if (ret)
		return ret;
	ret = of_genpd_add_provider_simple(dev->of_node, &bank->domain);
	if (ret) {
		pm_genpd_remove(&bank->domain);
		return ret;
	}
	platform_set_drvdata(pdev, bank);
	return 0;
}

static void w202_amp_remove(struct platform_device *pdev)
{
	struct w202_amp_bank *bank = platform_get_drvdata(pdev);

	of_genpd_del_provider(pdev->dev.of_node);
	pm_genpd_remove(&bank->domain);
}

static const struct of_device_id w202_amp_match[] = {
	{ .compatible = "zte,w202ds-amplifier-bank" },
	{ }
};
MODULE_DEVICE_TABLE(of, w202_amp_match);

static struct platform_driver w202_amp_driver = {
	.probe = w202_amp_probe,
	.remove = w202_amp_remove,
	.driver = {
		.name = "w202-audio-amp",
		.of_match_table = w202_amp_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(w202_amp_driver);
MODULE_DESCRIPTION("W202DS shared AW88166 reset owner");
MODULE_LICENSE("GPL");
