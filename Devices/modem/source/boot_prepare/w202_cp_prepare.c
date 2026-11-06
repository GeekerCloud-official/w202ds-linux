// SPDX-License-Identifier: GPL-2.0-only
/* Temporary comparison with UMS9620 LK pmic_arm7_RAM_active/pubcp_boot_prepare. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/delay.h>

static bool prepare;
module_param(prepare, bool, 0444);

static int dump(struct regmap *map, u32 base, const u32 *regs, size_t count)
{
 size_t i; int ret; u32 val;
 for (i=0;i<count;i++) {
  ret=regmap_read(map,regs[i],&val); if(ret)return ret;
  pr_notice("W202_CP_PREPARE snapshot %#x=%#x\n",base+regs[i],val);
 }
 return 0;
}
static int change(struct regmap *map,u32 base,u32 reg,u32 mask,u32 value)
{
 int ret;
 pr_notice("W202_CP_PREPARE change %#x mask=%#x value=%#x\n",base+reg,mask,value);
 ret=regmap_update_bits(map,reg,mask,value);
 return ret;
}
static struct regmap *getmap(const char *path)
{
 struct device_node *np=of_find_node_by_path(path); struct regmap *map;
 if(!np)return ERR_PTR(-ENODEV);
 map=syscon_node_to_regmap(np); of_node_put(np); return map;
}
static int __init w202_cp_prepare_init(void)
{
 struct regmap *aon,*pmu; int ret;
 const u32 ar[]={0x8c,0x90,0x124,0x128,0x174,0xcb0};
 const u32 pr[]={0x330,0x344,0x348,0x818,0xb98,0xb9c};
 if(!of_machine_is_compatible("sprd,ums9620"))return -ENODEV;
 aon=getmap("/soc/syscon@64900000"); if(IS_ERR(aon))return PTR_ERR(aon);
 pmu=getmap("/soc/syscon@64910000"); if(IS_ERR(pmu))return PTR_ERR(pmu);
 ret=dump(aon,0x64900000,ar,ARRAY_SIZE(ar));if(ret)return ret;
 ret=dump(pmu,0x64910000,pr,ARRAY_SIZE(pr));if(ret)return ret;
 if(!prepare)return 0;
#define STEP(map,base,reg,mask,val) do {ret=change(map,base,reg,mask,val);if(ret)return ret;}while(0)
 STEP(aon,0x64900000,0x90,BIT(0),BIT(0));
 STEP(aon,0x64900000,0x8c,BIT(0),BIT(0));
 msleep(50);
 STEP(pmu,0x64910000,0xb9c,BIT(0),0);
 STEP(pmu,0x64910000,0xb9c,BIT(1),0);
 STEP(pmu,0x64910000,0x818,BIT(8),0);
 STEP(pmu,0x64910000,0x818,BIT(9),0);
 msleep(50);
 STEP(aon,0x64900000,0x128,BIT(0),0);
 STEP(aon,0x64900000,0x124,BIT(0),0);
 msleep(50);
 STEP(pmu,0x64910000,0x348,BIT(25),0);
 STEP(pmu,0x64910000,0x348,BIT(24),0);
 ret=dump(aon,0x64900000,ar,ARRAY_SIZE(ar));if(ret)return ret;
 ret=dump(pmu,0x64910000,pr,ARRAY_SIZE(pr));if(ret)return ret;
 pr_notice("W202_CP_PREPARE complete\n");
 return 0;
}
static void __exit w202_cp_prepare_exit(void) {}
module_init(w202_cp_prepare_init);
module_exit(w202_cp_prepare_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202DS temporary LK CP preparation comparison");
