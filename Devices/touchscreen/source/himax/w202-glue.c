// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of.h>
#include <linux/firmware.h>
#include <linux/string.h>
char himax_firmware_name[50] = "Himax_firmware_guoxian.bin";
char himax_mp_firmware_name[50] = "Himax_mp_firmware_guoxian.bin";
char hx_criteria_csv_name[30];
struct firmware *himax_adb_upgrade_firmware;
extern int himax_common_init(void);
extern void himax_common_exit(void);
static int __init w202_himax_init(void)
{
    struct device_node *chosen = of_find_node_by_path("/chosen");
    const char *args;
    int ret;
    if (!chosen) return -ENODEV;
    ret = of_property_read_string(chosen, "bootargs", &args);
    if (!ret && !strstr(args, "lcd_name=lcd_guoxian_hx83102j_huaxing_nopen_mipi_himax "))
        ret = -ENODEV;
    of_node_put(chosen);
    return ret ? ret : himax_common_init();
}
static void __exit w202_himax_exit(void) { himax_common_exit(); }
module_init(w202_himax_init);
module_exit(w202_himax_exit);
MODULE_FIRMWARE("Himax_firmware_guoxian.bin");
