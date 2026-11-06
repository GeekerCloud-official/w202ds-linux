// SPDX-License-Identifier: GPL-2.0-only
/* W202DS PWM backlight, explicitly attached after the USB console is ready.
 * Use the original DT curve; preserve LK PWM state until a brightness write.
 */
#include <linux/backlight.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

struct w202_bl {
	struct pwm_device *pwm;
	u32 levels[256];
	u32 scale;
};

static int w202_bl_update(struct backlight_device *bd)
{
	struct w202_bl *bl = bl_get_data(bd);
	struct pwm_state state;
	unsigned int brightness = backlight_get_brightness(bd);
	int ret;

	pwm_get_state(bl->pwm, &state);
	state.duty_cycle = DIV_ROUND_CLOSEST_ULL(
		(u64)state.period * bl->levels[brightness], bl->scale);
	state.enabled = brightness != 0;
	ret = pwm_apply_might_sleep(bl->pwm, &state);
	pr_info("[W202DS-BL] brightness=%u level=%u period=%llu duty=%llu ret=%d\n",
		brightness, bl->levels[brightness], state.period,
		state.duty_cycle, ret);
	return ret;
}

static const struct backlight_ops w202_bl_ops = {
	.update_status = w202_bl_update,
};

static int w202_bl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w202_bl *bl;
	struct backlight_device *bd;
	struct pwm_state state;
	struct backlight_properties props = { .type = BACKLIGHT_RAW,
		.max_brightness = 255 };
	u32 current_level;
	int i, ret;

	if (of_property_count_u32_elems(dev->of_node, "brightness-levels") != 256)
		return -EINVAL;
	bl = devm_kzalloc(dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;
	ret = of_property_read_u32_array(dev->of_node, "brightness-levels", bl->levels, 256);
	if (ret)
		return ret;
	ret = of_property_read_u32(dev->of_node, "sprd,brightness-scale", &bl->scale);
	if (ret || bl->scale != 1023 || bl->levels[0] || bl->levels[255] != 1023)
		return -EINVAL;
	for (i = 1; i < 256; i++)
		if (bl->levels[i] < bl->levels[i - 1] || bl->levels[i] > bl->scale)
			return -EINVAL;
	bl->pwm = devm_pwm_get(dev, "backlight");
	if (IS_ERR(bl->pwm))
		return dev_err_probe(dev, PTR_ERR(bl->pwm), "PWM unavailable\n");
	pwm_get_state(bl->pwm, &state);
	if (!state.period || state.duty_cycle > state.period)
		return -EINVAL;
	current_level = DIV_ROUND_CLOSEST_ULL(state.duty_cycle * bl->scale, state.period);
	/* Find the closest curve entry; no PWM write during probe. */
	for (i = 0; i < 255; i++)
		if (current_level <= (bl->levels[i] + bl->levels[i + 1]) / 2)
			break;
	props.brightness = state.enabled ? i : 0;
	props.power = state.enabled ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;
	bd = devm_backlight_device_register(dev, "w202-backlight", dev, bl,
		&w202_bl_ops, &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);
	pr_info("[W202DS-BL] attached; LK PWM preserved: brightness=%d enabled=%d period=%llu duty=%llu\n",
		props.brightness, state.enabled, state.period, state.duty_cycle);
	return 0;
}

static const struct of_device_id w202_bl_match[] = {
	{ .compatible = "sprd,qogirn6pro-backlight" },
	{}
};

static struct platform_driver w202_bl_driver = {
	.probe = w202_bl_probe,
	.driver = { .name = "w202-backlight", .of_match_table = w202_bl_match },
};

static DEFINE_MUTEX(bl_attach_lock);
static bool bl_registered;

static long w202_bl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	if (cmd != _IO('W', 2))
		return -ENOTTY;
	mutex_lock(&bl_attach_lock);
	if (!bl_registered) {
		ret = platform_driver_register(&w202_bl_driver);
		if (!ret)
			bl_registered = true;
	}
	mutex_unlock(&bl_attach_lock);
	return ret;
}

static const struct file_operations w202_bl_control_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = w202_bl_ioctl,
};

static struct miscdevice w202_bl_control = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "w202blctl",
	.fops = &w202_bl_control_ops,
	.mode = 0600,
};

static int __init w202_bl_init(void)
{
	return misc_register(&w202_bl_control);
}
late_initcall(w202_bl_init);

static void __exit w202_bl_exit(void)
{
	misc_deregister(&w202_bl_control);
	if (bl_registered)
		platform_driver_unregister(&w202_bl_driver);
}
module_exit(w202_bl_exit);
MODULE_DESCRIPTION("W202DS deferred PWM backlight");
MODULE_LICENSE("GPL");
