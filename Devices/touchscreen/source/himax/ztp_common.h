/* No Android/ZTE policy layer in this port. */
#ifndef W202_MINIMAL_ZTP_LOG_H
#define W202_MINIMAL_ZTP_LOG_H
#include <linux/printk.h>
#define TPD_DMESG(fmt, ...) pr_info("[W202DS-HIMAX] " fmt, ##__VA_ARGS__)
#endif
