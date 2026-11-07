// SPDX-License-Identifier: GPL-2.0
/* The TCPM supply controls the internal PD contract. w202-usb-c reports
 * the single system input. Keep the control supply visible to kernel
 * clients without presenting it as another system AC adapter to userspace.
 */
#include <linux/power_supply.h>
#include <linux/slab.h>
#include "controller.h"

static const enum power_supply_property properties[] = {
	POWER_SUPPLY_PROP_SCOPE,
};

static int get_property(struct power_supply *psy,
			const struct power_supply_ext *ext, void *data,
			enum power_supply_property property,
			union power_supply_propval *val)
{
	if (property != POWER_SUPPLY_PROP_SCOPE)
		return -EINVAL;
	val->intval = POWER_SUPPLY_SCOPE_DEVICE;
	return 0;
}

static const struct power_supply_ext extension = {
	.name = "w202-pd-controller",
	.properties = properties,
	.num_properties = ARRAY_SIZE(properties),
	.get_property = get_property,
};

int w202_typec_supply_register(struct w202_typec *pd)
{
	char *name;
	int ret;

	name = kasprintf(GFP_KERNEL, "tcpm-source-psy-%s", dev_name(pd->dev));
	if (!name)
		return -ENOMEM;
	pd->supply = power_supply_get_by_name(name);
	kfree(name);
	if (!pd->supply)
		return -ENODEV;
	ret = power_supply_register_extension(pd->supply, &extension, pd->dev, pd);
	if (ret) {
		power_supply_put(pd->supply);
		pd->supply = NULL;
	}
	return ret;
}

void w202_typec_supply_unregister(struct w202_typec *pd)
{
	if (!pd->supply)
		return;
	power_supply_unregister_extension(pd->supply, &extension);
	power_supply_put(pd->supply);
	pd->supply = NULL;
}
