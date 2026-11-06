// SPDX-License-Identifier: GPL-2.0-only
/* Read the two DT-described always-on counters; send the vendor time mapping.
 * This diagnostic neither owns nor drains the CH receive streams.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/timekeeping.h>
#include <linux/unaligned.h>

extern int sbuf_status(u8 dst, u8 channel);
extern int sbuf_write(u8 dst, u8 channel, u32 bufid,
		      void *buf, u32 len, int timeout);

static bool send;
module_param(send, bool, 0444);
MODULE_PARM_DESC(send, "Send one 40-byte AP/CH time mapping after reading DT counters");

static u16 shub_checksum(const u8 *data, size_t length)
{
	u32 sum = 0;

	while (length >= 2) {
		sum += get_unaligned_be16(data);
		data += 2;
		length -= 2;
	}
	if (length)
		sum += (u16)*data << 8;
	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

static void __iomem *map_counter(const char *compatible)
{
	struct device_node *node;
	void __iomem *base;

	node = of_find_compatible_node(NULL, NULL, compatible);
	if (!node)
		return NULL;
	base = of_iomap(node, 0);
	of_node_put(node);
	return base;
}

static int __init w202_hub_timesync_init(void)
{
	struct device_node *node;
	void __iomem *syst, *frt;
	u8 frame[52] = {0x7e, 0x7e, 0x7e, 0x7e, 0xff, 0xc4, 0, 40};
	u64 boottime, counter;
	u32 syst_value, low, high, check;
	unsigned long flags;
	int ret, attempt;

	node = of_find_compatible_node(NULL, NULL, "sprd,qogirn6pro-sensorhub");
	if (!node)
		return -ENODEV;
	ret = of_property_read_u32(node, "sipc_sensorhub_id", &check);
	of_node_put(node);
	if (ret || check != 9)
		return -ENODEV;
	ret = sbuf_status(9, 4);
	if (ret)
		return ret;
	syst = map_counter("sprd,syst-timer");
	frt = map_counter("sprd,sysfrt-timer");
	if (!syst || !frt) {
		ret = -ENODEV;
		goto unmap;
	}
	local_irq_save(flags);
	boottime = ktime_get_boottime_ns();
	syst_value = readl(syst + 0xc);
	high = readl(frt + 0xc);
	for (attempt = 0; attempt < 4; attempt++) {
		low = readl(frt + 0x4);
		check = readl(frt + 0xc);
		if (check == high)
			break;
		high = check;
	}
	local_irq_restore(flags);
	if (attempt == 4) {
		ret = -EAGAIN;
		goto unmap;
	}
	counter = (u64)high << 32 | low;
	if (!syst_value || !counter) {
		ret = -ENODATA;
		goto unmap;
	}
	/* cnter_to_boottime, matching the published Android kernel wire ABI.
	 * Exact ns/tick factors: 1e9/1000 and (1953125 >> 6) for 32768 Hz.
	 */
	put_unaligned_le64(boottime, frame + 10);
	put_unaligned_le64(syst_value, frame + 18);
	put_unaligned_le64(counter, frame + 26);
	put_unaligned_le32(1000000, frame + 34);
	put_unaligned_le32(0, frame + 38);
	put_unaligned_le32(1953125, frame + 42);
	put_unaligned_le32(6, frame + 46);
	put_unaligned_be16(shub_checksum(frame, 8), frame + 8);
	put_unaligned_be16(shub_checksum(frame + 10, 40), frame + 50);
	pr_info("w202-hub-timesync: boot_ns=%llu syst_1khz=%u frt_32768hz=%llu payload=%*phN send=%d\n",
		boottime, syst_value, counter, 40, frame + 10, send);
	if (send) {
		ret = sbuf_write(9, 4, 0, frame, sizeof(frame), msecs_to_jiffies(100));
		if (ret == sizeof(frame))
			ret = 0;
		else if (ret >= 0)
			ret = -EIO;
	} else {
		ret = 0;
	}
unmap:
	if (frt)
		iounmap(frt);
	if (syst)
		iounmap(syst);
	return ret;
}

static void __exit w202_hub_timesync_exit(void)
{
}

module_init(w202_hub_timesync_init);
module_exit(w202_hub_timesync_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202DS one-shot AP/CH Sensor Hub hardware time mapping");
