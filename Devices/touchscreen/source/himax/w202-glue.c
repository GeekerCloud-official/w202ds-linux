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
    struct device_node *panel;
    bool enabled;

    /* Native Linux identifies the controller through DT, not Android's
     * optional lcd_name boot argument. SPI still performs the chip probe. */
    panel = of_find_compatible_node(NULL, NULL, "himax,hxcommon");
    if (!panel)
        return -ENODEV;
    enabled = of_device_is_available(panel);
    of_node_put(panel);
    return enabled ? himax_common_init() : -ENODEV;
}
static void __exit w202_himax_exit(void) { himax_common_exit(); }
module_init(w202_himax_init);
module_exit(w202_himax_exit);
MODULE_FIRMWARE("Himax_firmware_guoxian.bin");
