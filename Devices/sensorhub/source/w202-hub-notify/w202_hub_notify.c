// SPDX-License-Identifier: GPL-2.0-only
/* One bounded retry of the existing CH Sensor Hub SBUF doorbell; diagnostic. */
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/types.h>

/* Exact vendor SIPC public wire layout, linux/sipc.h struct smsg. */
struct smsg {
    u8 channel;
    u8 type;
    u16 flag;
    u32 value;
};
extern int sbuf_status(u8 dst, u8 channel);
extern int smsg_send(u8 dst, struct smsg *message, int timeout);

static bool notify;
module_param(notify, bool, 0444);
MODULE_PARM_DESC(notify, "Send one WR pointer doorbell to CHSYS 9/channel4/ring0");

static int __init w202_hub_notify_init(void)
{
    struct device_node *node;
    u32 destination;
    int result;
    struct smsg message = {.channel = 4, .type = 4, .flag = 1, .value = 0};

    BUILD_BUG_ON(sizeof(message) != 8);
    node = of_find_node_by_path("/sprd-sensorhub");
    if (!node)
        return -ENODEV;
    result = of_device_is_compatible(node, "sprd,qogirn6pro-sensorhub") ? 0 : -ENODEV;
    if (!result)
        result = of_property_read_u32(node, "sipc_sensorhub_id", &destination);
    of_node_put(node);
    if (result)
        return result;
    if (destination != 9)
        return -ENODEV;
    result = sbuf_status(9, 4);
    if (result)
        return result;
    if (notify) {
        result = smsg_send(9, &message, msecs_to_jiffies(100));
        if (result)
            return result;
    }
    pr_info("w202-hub-notify: CHSYS9/channel4/ring0 ready; doorbell=%d\n", notify);
    return 0;
}

static void __exit w202_hub_notify_exit(void)
{
}
module_init(w202_hub_notify_init);
module_exit(w202_hub_notify_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202DS bounded Sensor Hub doorbell diagnostic; no resets or ring writes");
