// SPDX-License-Identifier: GPL-2.0-only
/* W202DS CH Sensor Hub: existing SIPC transport to per-chip Linux IIO devices.
 * No firmware download, NV/calibration writes, resets or SIPC ownership changes.
 */
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include "w202_hub_protocol.h"

extern int sbuf_status(u8 dst, u8 channel);
extern int sbuf_read(u8 dst, u8 channel, u32 bufid,
		     void *buf, u32 len, int timeout);
extern int sbuf_write(u8 dst, u8 channel, u32 bufid,
		      void *buf, u32 len, int timeout);

#define HUB_DST 9
#define HUB_CHANNEL 4
#define HUB_PERIOD_NS 200000000
#define HUB_SENSORS 4

struct hub_sensor {
	u8 handle;
	const char *name;
	bool identified;
	bool enabled;
	u32 value_bits[3];
	u64 timestamp_us;
	u64 received_ns;
	u64 sequence;
};

struct w202_hub;
struct hub_rx {
	struct w202_hub *hub;
	unsigned int ring;
	struct task_struct *thread;
	struct hub_parser parser;
};

struct w202_hub {
	struct device *dev;
	struct hub_sensor sensors[HUB_SENSORS];
	struct hub_rx rx[2];
	struct iio_dev *iio[3];
	struct mutex command_lock;
	struct mutex sample_lock;
	spinlock_t data_lock;
	wait_queue_head_t data_wait;
	struct delayed_work sync_work;
	void __iomem *syst;
	void __iomem *frt;
	u32 firmware;
	bool stopping;
};

struct hub_iio_private { struct w202_hub *hub; };

static struct hub_sensor *hub_find_sensor(struct w202_hub *hub, unsigned int handle)
{
	unsigned int i;

	for (i = 0; i < HUB_SENSORS; i++)
		if (hub->sensors[i].handle == handle)
			return &hub->sensors[i];
	return NULL;
}

static int hub_command(struct w202_hub *hub, u8 handle, u8 subtype,
		       const u8 *payload, unsigned int length)
{
	u8 frame[64], no_data = 255;
	int n, ret;

	if (!length) {
		payload = &no_data;
		length = 1;
	}
	n = hub_encode(frame, sizeof(frame), handle, subtype, payload, length);
	if (n < 0)
		return n;
	mutex_lock(&hub->command_lock);
	ret = sbuf_write(HUB_DST, HUB_CHANNEL, 0, frame, n, msecs_to_jiffies(100));
	mutex_unlock(&hub->command_lock);
	if (ret != n)
		return ret < 0 ? ret : -EIO;
	return 0;
}

static void hub_packet(void *context, u8 handle, u8 subtype,
		       const u8 *payload, unsigned int length)
{
	struct w202_hub *hub = context;
	struct hub_sensor *sensor;
	unsigned int offset, i;
	unsigned long flags;
	(void)handle;

	if (subtype == 0xcd && length == 4) {
		WRITE_ONCE(hub->firmware, get_unaligned_le32(payload));
		wake_up_all(&hub->data_wait);
	} else if (subtype == 9 && length == 76) {
		sensor = hub_find_sensor(hub, get_unaligned_le32(payload + 44));
		if (sensor && !strncmp(sensor->name, (const char *)payload, 20))
			WRITE_ONCE(sensor->identified, true);
		wake_up_all(&hub->data_wait);
	} else if (subtype == 4 && length && !(length % 34)) {
		for (offset = 0; offset < length; offset += 34) {
			sensor = hub_find_sensor(hub, get_unaligned_le16(payload + offset));
			if (!sensor)
				continue;
			spin_lock_irqsave(&hub->data_lock, flags);
			for (i = 0; i < 3; i++)
				sensor->value_bits[i] = get_unaligned_le32(payload + offset + 2 + 4 * i);
			sensor->timestamp_us = get_unaligned_le64(payload + offset + 26);
			sensor->received_ns = ktime_get_boottime_ns();
			sensor->sequence++;
			spin_unlock_irqrestore(&hub->data_lock, flags);
		}
		wake_up_all(&hub->data_wait);
	} else if (subtype == 0xc4 && !READ_ONCE(hub->stopping)) {
		mod_delayed_work(system_wq, &hub->sync_work, 0);
	}
}

static int hub_receive(void *data)
{
	struct hub_rx *rx = data;
	u8 bytes[512];
	int ret;

	allow_signal(SIGTERM);
	while (!kthread_should_stop() && !signal_pending(current)) {
		/* Vendor SBUF blocks on mailbox/ring events. SIGTERM permits teardown
		 * without replacing spipe's notifier or destroying the shared ring.
		 */
		ret = sbuf_read(HUB_DST, HUB_CHANNEL, rx->ring, bytes, sizeof(bytes), -1);
		if (ret > 0) {
			hub_feed(&rx->parser, bytes, ret, hub_packet, rx->hub);
		} else if (signal_pending(current) || kthread_should_stop()) {
			break;
		} else {
			dev_err_ratelimited(rx->hub->dev, "ring%u read failed: %d\n", rx->ring, ret);
			/* Back off only after a transport error, not while waiting for data. */
			if (msleep_interruptible(1000))
				break;
		}
	}
	return 0;
}

static int hub_sync_time(struct w202_hub *hub)
{
	u8 payload[40];
	u64 boot_ns, frt;
	u32 syst, high, low, check;
	unsigned long flags;
	int attempt;

	local_irq_save(flags);
	boot_ns = ktime_get_boottime_ns();
	syst = readl(hub->syst + 0xc);
	high = readl(hub->frt + 0xc);
	for (attempt = 0; attempt < 4; attempt++) {
		low = readl(hub->frt + 4);
		check = readl(hub->frt + 0xc);
		if (check == high)
			break;
		high = check;
	}
	local_irq_restore(flags);
	frt = (u64)high << 32 | low;
	if (attempt == 4 || !syst || !frt)
		return -ENODATA;
	put_unaligned_le64(boot_ns, payload);
	put_unaligned_le64(syst, payload + 8);
	put_unaligned_le64(frt, payload + 16);
	put_unaligned_le32(1000000, payload + 24);
	put_unaligned_le32(0, payload + 28);
	put_unaligned_le32(1953125, payload + 32);
	put_unaligned_le32(6, payload + 36);
	return hub_command(hub, 255, 0xc4, payload, sizeof(payload));
}

static void hub_sync_work(struct work_struct *work)
{
	struct w202_hub *hub = container_of(to_delayed_work(work), struct w202_hub, sync_work);
	int ret;

	if (READ_ONCE(hub->stopping))
		return;
	ret = hub_sync_time(hub);
	if (ret)
		dev_err_ratelimited(hub->dev, "time mapping send failed: %d\n", ret);
	if (!READ_ONCE(hub->stopping))
		schedule_delayed_work(&hub->sync_work, msecs_to_jiffies(10000));
}

static int hub_read_raw(struct iio_dev *iio, const struct iio_chan_spec *channel,
			int *value, int *value2, long mask)
{
	struct w202_hub *hub = ((struct hub_iio_private *)iio_priv(iio))->hub;
	struct hub_sensor *sensor = hub_find_sensor(hub, channel->address >> 8);
	u8 batch[12] = {0};
	u64 before;
	u32 bits;
	s64 fixed;
	s32 remainder;
	unsigned long flags;
	long waited;
	int ret, disabled;

	if (mask != IIO_CHAN_INFO_PROCESSED || !sensor)
		return -EINVAL;
	if (mutex_lock_interruptible(&hub->sample_lock))
		return -ERESTARTSYS;
	if (READ_ONCE(hub->stopping)) {
		ret = -ENODEV;
		goto unlock;
	}
	before = READ_ONCE(sensor->sequence);
	put_unaligned_le32(HUB_PERIOD_NS, batch);
	ret = hub_command(hub, sensor->handle, 2, batch, sizeof(batch));
	if (ret)
		goto unlock;
	/* Mark ownership before sending so a partial/failed enable is also undone. */
	sensor->enabled = true;
	ret = hub_command(hub, sensor->handle, 0, NULL, 0);
	if (ret)
		goto disable;
	waited = wait_event_interruptible_timeout(hub->data_wait,
		READ_ONCE(hub->stopping) || READ_ONCE(sensor->sequence) != before,
		msecs_to_jiffies(2500));
	if (waited < 0) {
		ret = waited;
		goto disable;
	}
	if (!waited || READ_ONCE(hub->stopping)) {
		ret = READ_ONCE(hub->stopping) ? -ENODEV : -ETIMEDOUT;
		goto disable;
	}
	spin_lock_irqsave(&hub->data_lock, flags);
	bits = sensor->value_bits[channel->address & 255];
	spin_unlock_irqrestore(&hub->data_lock, flags);
	/* Hub magnetic values are microtesla; Linux IIO magnetic units are Gauss.
	 * Other channels retain m/s^2, rad/s and lux in micro-unit fixed point.
	 */
	ret = hub_float_fixed(bits, sensor->handle == 2 ? 10000 : 1000000, &fixed);
	if (!ret && (fixed > (s64)INT_MAX * 1000000 || fixed < (s64)INT_MIN * 1000000))
		ret = -ERANGE;
	if (!ret) {
		*value = div_s64_rem(fixed, 1000000, &remainder);
		*value2 = remainder;
	}
disable:
	disabled = hub_command(hub, sensor->handle, 1, NULL, 0);
	if (!disabled)
		sensor->enabled = false;
	else {
		dev_err(hub->dev, "handle%u disable failed: %d\n", sensor->handle, disabled);
		if (!ret)
			ret = disabled;
	}
unlock:
	mutex_unlock(&hub->sample_lock);
	return ret ? ret : IIO_VAL_INT_PLUS_MICRO;
}

static ssize_t hub_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w202_hub *hub = ((struct hub_iio_private *)iio_priv(dev_to_iio_dev(dev)))->hub;
	unsigned long flags;
	u64 sequence[4], timestamp[4];
	unsigned int i;
	(void)attr;

	spin_lock_irqsave(&hub->data_lock, flags);
	for (i = 0; i < HUB_SENSORS; i++) {
		sequence[i] = hub->sensors[i].sequence;
		timestamp[i] = hub->sensors[i].timestamp_us;
	}
	spin_unlock_irqrestore(&hub->data_lock, flags);
	return sysfs_emit(buf, "firmware=%u samples_1_2_4_5=%llu,%llu,%llu,%llu hub_timestamp_us_1_2_4_5=%llu,%llu,%llu,%llu bad_payloads=%llu,%llu\n",
		READ_ONCE(hub->firmware), sequence[0], sequence[1], sequence[2], sequence[3],
		timestamp[0], timestamp[1], timestamp[2], timestamp[3],
		READ_ONCE(hub->rx[0].parser.bad_payloads), READ_ONCE(hub->rx[1].parser.bad_payloads));
}
static IIO_DEVICE_ATTR_RO(hub_stats, 0);
static struct attribute *hub_iio_attrs[] = { &iio_dev_attr_hub_stats.dev_attr.attr, NULL };
static const struct attribute_group hub_iio_group = { .attrs = hub_iio_attrs };
static const struct iio_info hub_iio_info = { .read_raw = hub_read_raw, .attrs = &hub_iio_group };

#define HUB_AXIS(kind, modifier, handle, axis) { \
	.type = kind, .modified = 1, .channel2 = modifier, \
	.address = ((handle) << 8) | (axis), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) }

static const struct iio_chan_spec imu_channels[] = {
	HUB_AXIS(IIO_ACCEL, IIO_MOD_X, 1, 0), HUB_AXIS(IIO_ACCEL, IIO_MOD_Y, 1, 1),
	HUB_AXIS(IIO_ACCEL, IIO_MOD_Z, 1, 2), HUB_AXIS(IIO_ANGL_VEL, IIO_MOD_X, 4, 0),
	HUB_AXIS(IIO_ANGL_VEL, IIO_MOD_Y, 4, 1), HUB_AXIS(IIO_ANGL_VEL, IIO_MOD_Z, 4, 2),
};
static const struct iio_chan_spec magn_channels[] = {
	HUB_AXIS(IIO_MAGN, IIO_MOD_X, 2, 0), HUB_AXIS(IIO_MAGN, IIO_MOD_Y, 2, 1),
	HUB_AXIS(IIO_MAGN, IIO_MOD_Z, 2, 2),
};
static const struct iio_chan_spec light_channels[] = {
	{ .type = IIO_LIGHT, .address = 5 << 8,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) },
};

static bool hub_identified(struct w202_hub *hub)
{
	unsigned int i;

	for (i = 0; i < HUB_SENSORS; i++)
		if (!READ_ONCE(hub->sensors[i].identified))
			return false;
	return true;
}

static void hub_stop(struct w202_hub *hub)
{
	unsigned int i;

	WRITE_ONCE(hub->stopping, true);
	wake_up_all(&hub->data_wait);
	cancel_delayed_work_sync(&hub->sync_work);
	for (i = 0; i < ARRAY_SIZE(hub->iio); i++) {
		if (hub->iio[i]) {
			iio_device_unregister(hub->iio[i]);
			iio_device_free(hub->iio[i]);
			hub->iio[i] = NULL;
		}
	}
	mutex_lock(&hub->sample_lock);
	for (i = 0; i < HUB_SENSORS; i++) {
		if (hub->sensors[i].enabled && hub_command(hub, hub->sensors[i].handle, 1, NULL, 0))
			dev_err(hub->dev, "handle%u final disable failed\n", hub->sensors[i].handle);
	}
	mutex_unlock(&hub->sample_lock);
	for (i = 0; i < ARRAY_SIZE(hub->rx); i++) {
		if (hub->rx[i].thread) {
			get_task_struct(hub->rx[i].thread);
			send_sig(SIGTERM, hub->rx[i].thread, 1);
			kthread_stop(hub->rx[i].thread);
			put_task_struct(hub->rx[i].thread);
			hub->rx[i].thread = NULL;
		}
	}
	/* A receiver may have observed stopping=false just before the first
	 * cancellation. With both receivers joined, no callback can requeue.
	 */
	cancel_delayed_work_sync(&hub->sync_work);
	if (hub->syst)
		iounmap(hub->syst);
	if (hub->frt)
		iounmap(hub->frt);
}

static void __iomem *hub_map_timer(const char *compatible)
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

static int hub_probe(struct platform_device *pdev)
{
	static const char *const names[] = {"w202-lsm6dsoq", "w202-mmc5603", "w202-ltr578"};
	static const struct iio_chan_spec *const channels[] = {imu_channels, magn_channels, light_channels};
	static const unsigned int counts[] = {ARRAY_SIZE(imu_channels), ARRAY_SIZE(magn_channels), ARRAY_SIZE(light_channels)};
	struct w202_hub *hub;
	struct iio_dev *iio;
	u32 dst;
	u8 normal = 2;
	unsigned int i;
	int ret;
	long waited;

	ret = of_property_read_u32(pdev->dev.of_node, "sipc_sensorhub_id", &dst);
	if (ret || dst != HUB_DST)
		return -ENODEV;
	if (sbuf_status(HUB_DST, HUB_CHANNEL))
		return -EPROBE_DEFER;
	hub = devm_kzalloc(&pdev->dev, sizeof(*hub), GFP_KERNEL);
	if (!hub)
		return -ENOMEM;
	hub->dev = &pdev->dev;
	hub->sensors[0] = (struct hub_sensor){ .handle = 1, .name = "acc_lsm6dsoq" };
	hub->sensors[1] = (struct hub_sensor){ .handle = 2, .name = "mag_mmc5603" };
	hub->sensors[2] = (struct hub_sensor){ .handle = 4, .name = "gyro_lsm6dsoq" };
	hub->sensors[3] = (struct hub_sensor){ .handle = 5, .name = "light_ltr578" };
	mutex_init(&hub->command_lock);
	mutex_init(&hub->sample_lock);
	spin_lock_init(&hub->data_lock);
	init_waitqueue_head(&hub->data_wait);
	INIT_DELAYED_WORK(&hub->sync_work, hub_sync_work);
	platform_set_drvdata(pdev, hub);
	hub->syst = hub_map_timer("sprd,syst-timer");
	hub->frt = hub_map_timer("sprd,sysfrt-timer");
	if (!hub->syst || !hub->frt) {
		ret = -ENODEV;
		goto stop;
	}
	for (i = 0; i < ARRAY_SIZE(hub->rx); i++) {
		hub->rx[i].hub = hub;
		hub->rx[i].ring = i;
		hub->rx[i].thread = kthread_run(hub_receive, &hub->rx[i], "w202-hub-rx%u", i);
		if (IS_ERR(hub->rx[i].thread)) {
			ret = PTR_ERR(hub->rx[i].thread);
			hub->rx[i].thread = NULL;
			goto stop;
		}
	}
	ret = hub_command(hub, 255, 0xcd, NULL, 0);
	if (ret)
		goto stop;
	waited = wait_event_interruptible_timeout(hub->data_wait, READ_ONCE(hub->firmware), HZ);
	if (waited <= 0) {
		ret = waited ? waited : -ETIMEDOUT;
		goto stop;
	}
	/* This wire layout is validated only for the actual 20250605 CH image. */
	if (READ_ONCE(hub->firmware) != 20250605) {
		ret = -EPROTONOSUPPORT;
		goto stop;
	}
	ret = hub_command(hub, 255, 9, NULL, 0);
	if (ret)
		goto stop;
	waited = wait_event_interruptible_timeout(hub->data_wait, hub_identified(hub), HZ);
	if (waited <= 0) {
		ret = waited ? waited : -ENODEV;
		goto stop;
	}
	ret = hub_sync_time(hub);
	if (ret)
		goto stop;
	ret = hub_command(hub, 255, 0xc5, &normal, 1);
	if (ret)
		goto stop;
	for (i = 0; i < ARRAY_SIZE(hub->iio); i++) {
		iio = iio_device_alloc(&pdev->dev, sizeof(struct hub_iio_private));
		if (!iio) {
			ret = -ENOMEM;
			goto stop;
		}
		((struct hub_iio_private *)iio_priv(iio))->hub = hub;
		iio->name = names[i];
		iio->info = &hub_iio_info;
		iio->channels = channels[i];
		iio->num_channels = counts[i];
		iio->modes = INDIO_DIRECT_MODE;
		ret = iio_device_register(iio);
		if (ret) {
			iio_device_free(iio);
			goto stop;
		}
		hub->iio[i] = iio;
	}
	schedule_delayed_work(&hub->sync_work, msecs_to_jiffies(10000));
	dev_info(&pdev->dev, "CH firmware %u; LSM6DSOQ/MMC5603/LTR578 direct IIO ready\n", hub->firmware);
	return 0;
stop:
	hub_stop(hub);
	return dev_err_probe(&pdev->dev, ret, "CH direct IIO setup failed\n");
}

static void hub_remove(struct platform_device *pdev)
{
	hub_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id hub_match[] = {
	{ .compatible = "sprd,qogirn6pro-sensorhub" }, {}
};
MODULE_DEVICE_TABLE(of, hub_match);
static struct platform_driver hub_driver = {
	.probe = hub_probe, .remove = hub_remove,
	.driver = { .name = "w202-hub-iio", .of_match_table = hub_match },
};
module_platform_driver(hub_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202DS CH Sensor Hub direct IIO for LSM6DSOQ, MMC5603 and LTR578");
