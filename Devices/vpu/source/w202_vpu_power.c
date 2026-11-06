// SPDX-License-Identifier: GPL-2.0-only
/* W202DS Qogirn6Pro VPU genpd providers, based on Unisoc's power sequence.
 * The top domain and its pixel PLL are shared with the live DPU. Never gate
 * them here. Only the three independent codec child domains may transition.
 */
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include "w202_vpu.h"

#define AUTO_SHUTDOWN BIT(24)
#define FORCE_SHUTDOWN BIT(25)
#define SHUTDOWN_MASK (AUTO_SHUTDOWN | FORCE_SHUTDOWN)

struct w202_vpu_pd {
	struct generic_pm_domain genpd;
	struct device_node *node;
	struct regmap *pmu;
	u32 control;
	u32 status;
	u32 status_mask;
	bool initialized;
	bool provider;
	bool linked;
};

static struct w202_vpu_pd domains[] = {
	{ .control = 0x308, .status = 0x510, .status_mask = 0x1f000000 },
	{ .control = 0x30c, .status = 0x514, .status_mask = 0x0000001f },
	{ .control = 0x310, .status = 0x514, .status_mask = 0x00001f00 },
	{ .control = 0x314, .status = 0x514, .status_mask = 0x001f0000 },
};
static const char * const names[] = {
	"w202-vpu-top-display-shared", "w202-vpu-enc0", "w202-vpu-enc1", "w202-vpu-dec",
};
static bool ready;

bool w202_vpu_power_ready(void)
{
	return READ_ONCE(ready);
}
EXPORT_SYMBOL_GPL(w202_vpu_power_ready);

static int w202_vpu_state(struct w202_vpu_pd *pd, u32 *state)
{
	int ret = regmap_read(pd->pmu, pd->status, state);

	if (!ret)
		*state &= pd->status_mask;
	return ret;
}

static int state_get(char *buffer, const struct kernel_param *parameter)
{
	u32 state[ARRAY_SIZE(domains)];
	int i, ret;

	if (!READ_ONCE(ready))
		return -EAGAIN;
	for (i = 0; i < ARRAY_SIZE(domains); i++) {
		ret = w202_vpu_state(&domains[i], &state[i]);
		if (ret)
			return ret;
	}
	return scnprintf(buffer, PAGE_SIZE, "top=%08x enc0=%08x enc1=%08x dec=%08x\n",
			 state[0], state[1], state[2], state[3]);
}
static const struct kernel_param_ops state_ops = { .get = state_get };
module_param_cb(state, &state_ops, NULL, 0444);
MODULE_PARM_DESC(state, "Live PMU states, zero means powered on; read only");

static int change_power(struct w202_vpu_pd *pd, bool on)
{
	u32 before, state;
	int ret, rollback;

	/* The display owns the shared top/PLL; never write its power controls. */
	if (pd == &domains[0]) {
		ret = w202_vpu_state(pd, &state);
		return ret ? ret : (on && !state ? 0 : -EBUSY);
	}
	ret = regmap_read(pd->pmu, pd->control, &before);
	if (ret)
		return ret;
	ret = regmap_update_bits(pd->pmu, pd->control,
				on ? SHUTDOWN_MASK : FORCE_SHUTDOWN,
				on ? 0 : FORCE_SHUTDOWN);
	if (ret)
		goto rollback;
	ret = regmap_read_poll_timeout(pd->pmu, pd->status, state,
				      on ? !(state & pd->status_mask) :
					   !!(state & pd->status_mask),
				      300, 30000);
	if (!ret)
		return 0;
rollback:
	rollback = regmap_update_bits(pd->pmu, pd->control, SHUTDOWN_MASK,
				      before & SHUTDOWN_MASK);
	if (rollback)
		pr_err("w202-vpu: %s power rollback failed: %d\n", pd->genpd.name, rollback);
	return ret;
}

static int power_on(struct generic_pm_domain *domain)
{
	return change_power(container_of(domain, struct w202_vpu_pd, genpd), true);
}

static int power_off(struct generic_pm_domain *domain)
{
	return change_power(container_of(domain, struct w202_vpu_pd, genpd), false);
}

/* A provider cannot disappear while any genpd consumer still uses its ops. */
static int attach_device(struct generic_pm_domain *domain, struct device *dev)
{
	return try_module_get(THIS_MODULE) ? 0 : -ENODEV;
}

static void detach_device(struct generic_pm_domain *domain, struct device *dev)
{
	module_put(THIS_MODULE);
}

static int validate_syscon(struct w202_vpu_pd *pd, const char *property,
			   u32 offset, u32 mask)
{
	struct regmap *map;
	u32 args[2];

	map = syscon_regmap_lookup_by_phandle_args(pd->node, property, 2, args);
	if (IS_ERR(map))
		return PTR_ERR(map);
	if (args[0] != offset || args[1] != mask || (pd->pmu && pd->pmu != map))
		return -EINVAL;
	pd->pmu = map;
	return 0;
}

static void remove_domains(void)
{
	int i, ret;

	WRITE_ONCE(ready, false);
	for (i = ARRAY_SIZE(domains) - 1; i >= 0; i--) {
		struct w202_vpu_pd *pd = &domains[i];

		if (pd->provider) {
			of_genpd_del_provider(pd->node);
			pd->provider = false;
		}
		if (pd->linked) {
			ret = pm_genpd_remove_subdomain(&domains[0].genpd, &pd->genpd);
			WARN_ON(ret);
			pd->linked = false;
		}
		if (pd->initialized) {
			ret = pm_genpd_remove(&pd->genpd);
			WARN_ON(ret);
			pd->initialized = false;
		}
		of_node_put(pd->node);
		pd->node = NULL;
	}
}

static int __init w202_vpu_power_init(void)
{
	struct device_node *parent;
	char path[64];
	u32 state;
	int i, ret;

	/* Check every expected board register before publishing any provider. */
	for (i = 0; i < ARRAY_SIZE(domains); i++) {
		struct w202_vpu_pd *pd = &domains[i];

		snprintf(path, sizeof(path), "/soc/mm/power-domain@%d", i);
		pd->node = of_find_node_by_path(path);
		if (!pd->node || !of_device_is_compatible(pd->node, "sprd,vpu-pd") ||
		    !of_device_is_available(pd->node)) {
			ret = -ENODEV;
			goto fail;
		}
		ret = validate_syscon(pd, "pmu-vpu-force-shutdown-syscon", pd->control, FORCE_SHUTDOWN);
		if (ret)
			goto fail;
		ret = validate_syscon(pd, "pmu-vpu-auto-shutdown-syscon", pd->control, AUTO_SHUTDOWN);
		if (ret)
			goto fail;
		ret = validate_syscon(pd, "pmu-pwr-status-syscon", pd->status, pd->status_mask);
		if (ret)
			goto fail;
		parent = of_parse_phandle(pd->node, "power-domains", 0);
		if ((i && parent != domains[0].node) || (!i && parent))
			ret = -EINVAL;
		of_node_put(parent);
		if (ret)
			goto fail;
		ret = w202_vpu_state(pd, &state);
		if (ret)
			goto fail;
		if (!i && state) {
			pr_err("w202-vpu: shared display domain is not on; refusing to alter it\n");
			ret = -EBUSY;
			goto fail;
		}
		pd->genpd.name = names[i];
		pd->genpd.power_on = power_on;
		pd->genpd.power_off = power_off;
		pd->genpd.attach_dev = attach_device;
		pd->genpd.detach_dev = detach_device;
		if (!i)
			pd->genpd.flags = GENPD_FLAG_ALWAYS_ON;
		ret = pm_genpd_init(&pd->genpd, NULL, !!state);
		if (ret)
			goto fail;
		pd->initialized = true;
	}
	for (i = 1; i < ARRAY_SIZE(domains); i++) {
		ret = pm_genpd_add_subdomain(&domains[0].genpd, &domains[i].genpd);
		if (ret)
			goto fail;
		domains[i].linked = true;
	}
	for (i = 0; i < ARRAY_SIZE(domains); i++) {
		ret = of_genpd_add_provider_simple(domains[i].node, &domains[i].genpd);
		if (ret)
			goto fail;
		domains[i].provider = true;
	}
	WRITE_ONCE(ready, true);
	pr_info("w202-vpu: four genpd providers ready; shared display power and PLL preserved\n");
	return 0;
fail:
	remove_domains();
	return ret;
}

module_init(w202_vpu_power_init);
module_exit(remove_domains);
MODULE_DESCRIPTION("W202DS VPU child power domains with shared DPU preservation");
MODULE_LICENSE("GPL");
MODULE_VERSION("w202ds-vpu-power-v2");
