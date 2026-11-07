// SPDX-License-Identifier: GPL-2.0-only
/* Qogirn6Pro decoder ABI bridge. Used with the separately loaded VAU provider.
 * Each MMIO owner holds a runtime power/clock lease until its last VMA closes.
 * Register access is deliberately limited to a single privileged file owner.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "w202_vpu.h"
#include "w202_vpu_buffers.h"
#include "w202_vpu_native.h"

#define REG_INT_MASK 0x1004
#define REG_INT_CLEAR 0x1008
#define REG_INT_RAW 0x100c
#define REG_AXI_PAUSE 0x1018
#define REG_AXI_STATUS 0x101c
#define REG_MMU_MASK 0xa0
#define REG_MMU_CLEAR 0xa4
#define REG_MMU_RAW 0xac
#define ABI_MAP_SIZE 0x14000

#define CMD_FREQ_SET _IOW('m', 1, unsigned int)
#define CMD_FREQ_GET _IOR('m', 2, unsigned int)
#define CMD_ENABLE _IO('m', 3)
#define CMD_DISABLE _IO('m', 4)
#define CMD_ACQUIRE _IO('m', 5)
#define CMD_RELEASE _IO('m', 6)
#define CMD_COMPLETE _IO('m', 7)
#define CMD_RESET _IO('m', 8)
#define CMD_HW_INFO _IO('m', 9)
#define CMD_VERSION _IO('m', 10)
#define CMD_IOMMU_STATUS _IO('m', 13)
#define CMD_CODEC_SET _IOW('m', 14, unsigned int)
#define CMD_CODEC_COUNT _IOWR('m', 15, unsigned int)
#define CMD_SCENE_SET _IO('m', 16)
#define CMD_SCENE_GET _IO('m', 17)

struct codec_device {
	struct miscdevice misc;
	void __iomem *base;
	phys_addr_t physical;
	struct regmap *reset;
	u32 reset_reg, reset_mask;
	struct regmap *domain_eb;
	u32 domain_reg, domain_mask;
	struct clk *clock, *parents[5], *original_parent;
	unsigned int num_parents;
	int irq;
	atomic_t opens, interrupts, resets, timeouts, faults;
	atomic_t maps, auto_released, copyout_rollbacks;
	atomic_t native_started, native_inflight, native_cancel_before_irq;
	atomic_t native_cancel_dma_active;
	spinlock_t irq_lock;
	struct mutex power_lock;
	bool powered;
	u32 pending, last_irq;
	wait_queue_head_t wait;
	bool broken;
	struct codec_file *quarantine;
	struct w202_vpu_video *video;
};

struct codec_file {
	struct codec_device *codec;
	struct w202_vpu_buffers buffers;
	struct mutex lock;
	bool acquired, enabled;
	u32 scene, frequency;
};

static void codec_mask_clear(struct codec_device *c)
{
	writel(0, c->base + REG_INT_MASK);
	writel(0, c->base + REG_MMU_MASK);
	writel(0xffff, c->base + REG_INT_CLEAR);
	writel(0xff, c->base + REG_MMU_CLEAR);
	readl(c->base + REG_INT_RAW);
}

static irqreturn_t codec_irq(int irq, void *data)
{
	struct codec_device *c = data;
	u32 status = readl(c->base + REG_INT_RAW) & 0xffff;
	u32 mmu = readl(c->base + REG_MMU_RAW) & 0xff;
	unsigned long flags;

	if (!status && !mmu)
		return IRQ_NONE;
	codec_mask_clear(c);
	spin_lock_irqsave(&c->irq_lock, flags);
	c->pending |= status | (mmu << 16);
	c->last_irq = status | (mmu << 16);
	spin_unlock_irqrestore(&c->irq_lock, flags);
	atomic_inc(&c->interrupts);
	if (mmu || (status & (BIT(0) | BIT(4) | BIT(5) | BIT(13) | BIT(14) | BIT(15))))
		atomic_inc(&c->faults);
	wake_up_interruptible(&c->wait);
	return IRQ_HANDLED;
}

/* Only the decoder's validated reset mask is touched, never the shared top. */
static int codec_reset(struct codec_device *c)
{
	unsigned long flags;
	u32 value;
	int ret;

	disable_irq(c->irq);
	codec_mask_clear(c);
	ret = regmap_update_bits(c->reset, c->reset_reg, c->reset_mask, c->reset_mask);
	if (!ret) {
		udelay(2);
		ret = regmap_update_bits(c->reset, c->reset_reg, c->reset_mask, 0);
	}
	if (!ret)
		ret = readl_poll_timeout(c->base + REG_AXI_STATUS, value,
					!(value & 0x101), 1, 20000);
	if (!ret)
		ret = w202_vpu_iommu_restore(c->misc.parent);
	if (!ret) {
		codec_mask_clear(c);
		atomic_inc(&c->resets);
	} else {
		WRITE_ONCE(c->broken, true);
		dev_err(c->misc.parent, "decoder reset failed: %d\n", ret);
	}
	spin_lock_irqsave(&c->irq_lock, flags);
	c->pending = 0;
	spin_unlock_irqrestore(&c->irq_lock, flags);
	enable_irq(c->irq);
	return ret;
}

static int codec_stop(struct codec_file *ctx)
{
	struct codec_device *c = ctx->codec;
	u32 value;
	int ret;

	if (!ctx->enabled)
		return 0;
	writel(3, c->base + REG_AXI_PAUSE);
	ret = readl_poll_timeout(c->base + REG_AXI_STATUS, value,
				!(value & 0x101), 1, 20000);
	/* Reset also covers a client that dies while its hardware job runs. */
	if (codec_reset(c))
		return -EIO;
	ctx->enabled = false;
	return ret;
}

static int codec_context_get(struct codec_device *c, struct codec_file **result)
{
	struct miscdevice *misc = &c->misc;
	struct codec_file *ctx;
	int ret;

	/* MMIO VMAs keep this file alive, so a second owner cannot write its core. */
	if (atomic_cmpxchg(&c->opens, 0, 1))
		return -EBUSY;
	if (READ_ONCE(c->broken)) {
		atomic_set(&c->opens, 0);
		return -EIO;
	}
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		atomic_set(&c->opens, 0);
		return -ENOMEM;
	}
	mutex_lock(&c->power_lock);
	ret = pm_runtime_resume_and_get(misc->parent);
	if (ret < 0)
		goto open_failed;
	ret = w202_vpu_iommu_get(misc->parent);
	if (ret) {
		pm_runtime_put_sync_suspend(misc->parent);
		goto open_failed;
	}
	c->powered = true;
	codec_mask_clear(c);
	enable_irq(c->irq);
	mutex_unlock(&c->power_lock);
	ctx->codec = c;
	mutex_init(&ctx->lock);
	w202_vpu_buffers_init(&ctx->buffers, misc->parent);
	*result = ctx;
	return 0;
open_failed:
	mutex_unlock(&c->power_lock);
	kfree(ctx);
	atomic_set(&c->opens, 0);
	return ret;
}

static int codec_context_put(struct codec_file *ctx)
{
	struct codec_device *c = ctx->codec;
	int ret;

	mutex_lock(&ctx->lock);
	mutex_lock(&c->power_lock);
	/* Close always resets, including MMIO use without an ENABLE ioctl. */
	ret = codec_reset(c);
	if (ret) {
		/* Never free memory which a failed reset could leave DMA-active. */
		c->quarantine = ctx;
		/* release drops fops->owner even on error; preserve the driver, VAU
		 * dependency and device resources until a controlled reboot.
		 */
		__module_get(THIS_MODULE);
		dev_err(c->misc.parent, "retaining decoder buffers after failed reset\n");
		mutex_unlock(&c->power_lock);
		mutex_unlock(&ctx->lock);
		return ret;
	}
	ret = ctx->buffers.count;
	w202_vpu_buffers_destroy(&ctx->buffers);
	atomic_sub(ret, &c->maps);
	atomic_add(ret, &c->auto_released);
	codec_mask_clear(c);
	disable_irq(c->irq);
	c->powered = false;
	w202_vpu_iommu_put(c->misc.parent);
	pm_runtime_mark_last_busy(c->misc.parent);
	pm_runtime_put_autosuspend(c->misc.parent);
	mutex_unlock(&c->power_lock);
	mutex_unlock(&ctx->lock);
	kfree(ctx);
	atomic_set(&c->opens, 0);
	return 0;
}

static int codec_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct codec_device *c = container_of(misc, struct codec_device, misc);
	struct codec_file *ctx;
	int ret = codec_context_get(c, &ctx);

	if (!ret)
		file->private_data = ctx;
	return ret;
}

static int codec_close(struct inode *inode, struct file *file)
{
	return codec_context_put(file->private_data);
}

int w202_vpu_native_begin(struct device *dev, struct codec_file **context)
{
	struct codec_device *c = dev_get_drvdata(dev);
	struct codec_file *ctx;
	int ret;

	ret = codec_context_get(c, &ctx);
	if (ret)
		return ret;
	ctx->acquired = true;
	ret = clk_set_parent(c->clock, c->parents[0]);
	if (!ret)
		ret = codec_reset(c);
	if (ret) {
		codec_context_put(ctx);
		return ret;
	}
	ctx->enabled = true;
	*context = ctx;
	return 0;
}

int w202_vpu_native_import(struct codec_file *ctx, struct dma_buf *buf,
			   struct w202_vpu_iova *map)
{
	int ret = w202_vpu_buffers_import(&ctx->buffers, buf, map);

	if (!ret)
		atomic_inc(&ctx->codec->maps);
	return ret;
}

void w202_vpu_native_wake(struct device *dev)
{
	struct codec_device *c = dev_get_drvdata(dev);

	wake_up_interruptible(&c->wait);
}

int w202_vpu_native_validate_buffers(struct codec_file *ctx,
				     const struct w202_vpu_iova *input,
				     const struct w202_vpu_iova *frame)
{
	return w202_vpu_buffers_validate_native(&ctx->buffers, input, frame);
}

int w202_vpu_native_end(struct codec_file *ctx)
{
	return codec_context_put(ctx);
}

int w202_vpu_native_execute(struct codec_file *ctx,
			    const struct w202_h264_config *config,
			    const atomic_t *abort)
{
	struct codec_device *c = ctx->codec;
	u32 value, bits, n;
	unsigned int i;
	long waited;
	int ret;

	if (atomic_read(abort))
		return -ECANCELED;
	writel(0xd0, c->base + 0x1010);
	writel(0, c->base + 0x1028);
	writel(4, c->base + 0x1020);
	ret = readl_poll_timeout(c->base + 0x8018, value,
				 !(value & BIT(27)), 1, 20000);
	if (ret)
		goto bsm_timeout;
	writel(config->stream_address >> 4, c->base + 0x1080);
	writel(config->stream_address >> 4, c->base + 0x1084);
	writel(6, c->base + 0x8008);
	writel(BIT(31), c->base + 0x8004);
	writel((config->stream_bytes + W202_H264_BSM_PADDING) | BIT(31), c->base + 0x8000);
	for (bits = config->header_bits; bits; bits -= n) {
		if (atomic_read(abort))
			return -ECANCELED;
		n = min(bits, 32U);
		ret = readl_poll_timeout(c->base + 0x8020, value, value & 1, 1, 20000);
		if (ret)
			goto bsm_timeout;
		writel(n << 24, c->base + 0x8008);
		writel((n << 24) | 1, c->base + 0x8008);
		readl(c->base + 0x8010);
	}
	for (i = 0; i < config->count; i++)
		writel(config->regs[i].value, c->base + config->regs[i].offset);
	if (atomic_read(abort))
		return -ECANCELED;
	writel(0x7ff, c->base + REG_INT_CLEAR);
	writel(0x1035, c->base + REG_INT_MASK);
	writel(0xfb, c->base + REG_MMU_MASK);
	writel(0xffffffff, c->base + 0x1038);
	writel(1, c->base + 0x1028);
	writel(10, c->base + 0x1030);
	atomic_inc(&c->native_started);
	atomic_set(&c->native_inflight, 1);
	waited = wait_event_interruptible_timeout(c->wait,
		READ_ONCE(c->pending) || atomic_read(abort), msecs_to_jiffies(200));
	atomic_set(&c->native_inflight, 0);
	if (atomic_read(abort)) {
		/* Cancellation observed after START and before any completion/fault
		 * IRQ. The caller still owns all mappings until native_end resets. */
		if (!READ_ONCE(c->pending)) {
			atomic_inc(&c->native_cancel_before_irq);
			if (readl(c->base + REG_AXI_STATUS) & 0x101)
				atomic_inc(&c->native_cancel_dma_active);
		}
		return -ECANCELED;
	}
	if (waited < 0)
		return waited;
	if (!waited) {
		ret = -ETIMEDOUT;
		goto bsm_timeout;
	}
	value = READ_ONCE(c->pending);
	return (value & 4) && !(value & 0xffffe031) ? 0 : -EIO;
bsm_timeout:
	atomic_inc(&c->timeouts);
	return ret;
}

static long codec_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct codec_file *ctx = file->private_data;
	struct codec_device *c = ctx->codec;
	void __user *user = (void __user *)arg;
	struct w202_vpu_iova map;
	unsigned long flags;
	unsigned int i;
	u32 value = 0;
	long waited;
	int ret = 0;

	if (mutex_lock_interruptible(&ctx->lock))
		return -ERESTARTSYS;
	if (READ_ONCE(c->broken)) {
		ret = -EIO;
		goto out;
	}
	switch (cmd) {
	case CMD_VERSION:
		value = 0x15;
		goto copy_value;
	case CMD_HW_INFO:
		ret = regmap_read(c->domain_eb, c->domain_reg, &value);
		if (ret) break;
		value &= c->domain_mask;
		goto copy_value;
	case CMD_IOMMU_STATUS:
		ret = iommu_get_domain_for_dev(c->misc.parent) ? 0 : -ENODEV;
		break;
	case CMD_CODEC_SET:
		if (get_user(value, (u32 __user *)user)) ret = -EFAULT;
		else if (value > 31) ret = -EINVAL;
		break;
	case CMD_CODEC_COUNT:
		value = atomic_read(&c->opens);
		goto copy_value;
	case CMD_SCENE_SET:
		if (get_user(value, (u32 __user *)user)) ret = -EFAULT;
		else if (value > 3) ret = -EINVAL;
		else ctx->scene = value;
		break;
	case CMD_SCENE_GET:
		value = ctx->scene;
		goto copy_value;
	case CMD_FREQ_SET:
		if (!ctx->acquired) { ret = -EPERM; break; }
		if (get_user(value, (u32 __user *)user)) { ret = -EFAULT; break; }
		if (value >= c->num_parents) { ret = -EINVAL; break; }
		if (ctx->enabled) { ret = -EBUSY; break; }
		ctx->frequency = value;
		break;
	case CMD_FREQ_GET:
		for (i = 0; i < c->num_parents; i++)
			if (clk_get_rate(c->parents[i]) == clk_get_rate(c->clock)) break;
		if (i == c->num_parents) { ret = -ERANGE; break; }
		value = i;
		goto copy_value;
	case CMD_ACQUIRE:
		if (ctx->acquired) ret = -EALREADY;
		else ctx->acquired = true;
		break;
	case CMD_ENABLE:
		if (!ctx->acquired) { ret = -EPERM; break; }
		if (ctx->enabled) { ret = -EALREADY; break; }
		ret = clk_set_parent(c->clock, c->parents[ctx->frequency]);
		if (!ret) ret = w202_vpu_iommu_restore(c->misc.parent);
		if (!ret) ctx->enabled = true;
		break;
	case CMD_DISABLE:
		if (!ctx->acquired) { ret = -EPERM; break; }
		ret = codec_stop(ctx);
		break;
	case CMD_RELEASE:
		if (!ctx->acquired) { ret = -EPERM; break; }
		ret = codec_stop(ctx);
		if (!ret) ctx->acquired = false;
		break;
	case CMD_RESET:
		if (!ctx->enabled || !ctx->acquired) { ret = -EPERM; break; }
		ret = codec_reset(c);
		break;
	case CMD_COMPLETE:
		if (!ctx->enabled || !ctx->acquired) { ret = -EPERM; break; }
		waited = wait_event_interruptible_timeout(c->wait,
			READ_ONCE(c->pending), msecs_to_jiffies(200));
		spin_lock_irqsave(&c->irq_lock, flags);
		value = c->pending;
		c->pending = 0;
		spin_unlock_irqrestore(&c->irq_lock, flags);
		if (waited < 0) {
			value |= BIT(30);
			ret = waited;
		} else if (!waited) {
			value |= BIT(31);
			atomic_inc(&c->timeouts);
			ret = -ETIMEDOUT;
			codec_mask_clear(c);
		}
		if (put_user(value, (u32 __user *)user)) ret = -EFAULT;
		break;
	case W202_VPU_GET_IOVA:
	case W202_VPU_FREE_IOVA:
		if (copy_from_user(&map, user, sizeof(map))) { ret = -EFAULT; break; }
		if (cmd == W202_VPU_FREE_IOVA) {
			ret = w202_vpu_buffers_unmap(&ctx->buffers, &map);
			if (!ret) atomic_dec(&c->maps);
			break;
		}
		ret = w202_vpu_buffers_map(&ctx->buffers, &map);
		if (!ret && copy_to_user(user, &map, sizeof(map))) {
			w202_vpu_buffers_unmap(&ctx->buffers, &map);
			atomic_inc(&c->copyout_rollbacks);
			ret = -EFAULT;
		}
		if (!ret) atomic_inc(&c->maps);
		break;
	default:
		ret = -ENOTTY;
	}
	goto out;
copy_value:
	ret = put_user(value, (u32 __user *)user);
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static int codec_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct codec_file *ctx = file->private_data;
	size_t size = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff || !size || size > ABI_MAP_SIZE ||
	    !(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	vm_flags_clear(vma, VM_MAYEXEC);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return io_remap_pfn_range(vma, vma->vm_start, ctx->codec->physical >> PAGE_SHIFT,
				 size, vma->vm_page_prot);
}

static const struct file_operations codec_fops = {
	.owner = THIS_MODULE,
	.open = codec_open,
	.release = codec_close,
	.unlocked_ioctl = codec_ioctl,
	.mmap = codec_mmap,
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct codec_device *c = dev_get_drvdata(dev);
	u32 axi = 0, raw = 0, mmu = 0;
	ssize_t count;

	mutex_lock(&c->power_lock);
	if (c->powered) {
		axi = readl(c->base + REG_AXI_STATUS);
		raw = readl(c->base + REG_INT_RAW);
		mmu = readl(c->base + REG_MMU_RAW);
	}
	count = sysfs_emit(buf, "opens=%d maps=%d auto_released=%d copyout_rollbacks=%d interrupts=%d resets=%d timeouts=%d faults=%d "
			 "broken=%u quarantined=%u quarantined_maps=%u powered=%u live_regs=%u clock=%lu pending=%08x last_irq=%08x axi=%08x raw=%08x mmu=%08x "
			 "native_started=%d native_inflight=%d native_cancel_before_irq=%d native_cancel_dma_active=%d\n",
			 atomic_read(&c->opens), atomic_read(&c->maps),
			 atomic_read(&c->auto_released), atomic_read(&c->copyout_rollbacks),
			 atomic_read(&c->interrupts),
			 atomic_read(&c->resets), atomic_read(&c->timeouts),
			 atomic_read(&c->faults), READ_ONCE(c->broken), !!READ_ONCE(c->quarantine),
			 c->quarantine ? READ_ONCE(c->quarantine->buffers.count) : 0,
			 c->powered, c->powered,
			 clk_get_rate(c->clock),
			 READ_ONCE(c->pending), READ_ONCE(c->last_irq), axi, raw, mmu,
			 atomic_read(&c->native_started), atomic_read(&c->native_inflight),
			 atomic_read(&c->native_cancel_before_irq), atomic_read(&c->native_cancel_dma_active));
	mutex_unlock(&c->power_lock);
	return count;
}
static DEVICE_ATTR_RO(state);
static struct attribute *codec_attrs[] = { &dev_attr_state.attr, NULL };
ATTRIBUTE_GROUPS(codec);

static int codec_probe(struct platform_device *pdev)
{
	static const char * const names[] = {
		"clk_src_256m", "clk_src_307m2", "clk_src_384m", "clk_src_512m", "clk_src_680m",
	};
	struct device *dev = &pdev->dev;
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct codec_device *c;
	u32 reset_args[2], version;
	int i, ret;

	if (!w202_vpu_power_ready() || !iommu_get_domain_for_dev(dev))
		return -EPROBE_DEFER;
	if (!res || res->start != 0x32200000 || resource_size(res) != 0xc000 ||
	    of_property_read_u32(dev->of_node, "sprd,video_ip_version", &version) || version != 0x15)
		return -EINVAL;
	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c) return -ENOMEM;
	c->reset = syscon_regmap_lookup_by_phandle_args(dev->of_node, "reset-syscon", 2, reset_args);
	if (IS_ERR(c->reset)) return PTR_ERR(c->reset);
	if (reset_args[0] != 4 || reset_args[1] != 0x26020) return -EINVAL;
	c->reset_reg = reset_args[0];
	c->reset_mask = reset_args[1];
	c->domain_eb = syscon_regmap_lookup_by_phandle_args(dev->of_node,
		"vsp-domain-eb-syscon", 2, reset_args);
	if (IS_ERR(c->domain_eb)) return PTR_ERR(c->domain_eb);
	if (reset_args[0] || reset_args[1] != BIT(21)) return -EINVAL;
	c->domain_reg = reset_args[0];
	c->domain_mask = reset_args[1];
	c->physical = res->start;
	/* The separate VAU provider owns the overlapping first 0x80 bytes. */
	c->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!c->base) return -ENOMEM;
	c->clock = devm_clk_get(dev, "clk_vsp");
	if (IS_ERR(c->clock)) return PTR_ERR(c->clock);
	c->original_parent = clk_get_parent(c->clock);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		c->parents[i] = devm_clk_get(dev, names[i]);
		if (IS_ERR(c->parents[i])) return PTR_ERR(c->parents[i]);
		c->num_parents++;
	}
	spin_lock_init(&c->irq_lock);
	mutex_init(&c->power_lock);
	init_waitqueue_head(&c->wait);
	c->misc = (struct miscdevice){
		.minor = MISC_DYNAMIC_MINOR, .name = "sprd_vsp", .mode = 0600,
		.parent = dev, .fops = &codec_fops,
	};
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36));
	if (ret) return ret;
	c->irq = platform_get_irq(pdev, 0);
	if (c->irq < 0) return c->irq;
	/* platform_probe also attached this physical device to the child genpd.
	 * Its reference must be runtime-managed as well as the VAU's virtual one.
	 */
	pm_runtime_set_autosuspend_delay(dev, 250);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) goto disable_pm;
	ret = w202_vpu_iommu_get(dev);
	if (ret) goto put_codec;
	codec_mask_clear(c);
	/* Never enter an MMIO IRQ handler when the owner has allowed power off. */
	ret = devm_request_irq(dev, c->irq, codec_irq, IRQF_NO_AUTOEN, "w202-vpu-dec", c);
	if (ret) goto put_power;
	platform_set_drvdata(pdev, c);
	ret = misc_register(&c->misc);
	if (!ret) {
		c->video = w202_vpu_video_register(dev);
		if (IS_ERR(c->video)) {
			ret = PTR_ERR(c->video);
			misc_deregister(&c->misc);
		} else {
			dev_info(dev, "decoder ABI and V4L2 share exclusive runtime power lease\n");
		}
	}
put_power:
	w202_vpu_iommu_put(dev);
put_codec:
	if (ret) {
		pm_runtime_put_sync_suspend(dev);
	} else {
		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_autosuspend(dev);
	}
disable_pm:
	if (ret) pm_runtime_disable(dev);
	return ret;
}

static void codec_remove(struct platform_device *pdev)
{
	struct codec_device *c = platform_get_drvdata(pdev);
	int ret;

	w202_vpu_video_unregister(c->video);
	misc_deregister(&c->misc);
	synchronize_irq(c->irq);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0) goto disable_pm;
	ret = w202_vpu_iommu_get(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "cannot resume decoder for clock-parent restoration: %d\n", ret);
		goto put_codec;
	}
	codec_mask_clear(c);
	if (c->original_parent && clk_set_parent(c->clock, c->original_parent))
		dev_err(&pdev->dev, "could not restore decoder clock parent\n");
	w202_vpu_iommu_put(&pdev->dev);
put_codec:
	pm_runtime_put_sync_suspend(&pdev->dev);
disable_pm:
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id codec_match[] = {
	{ .compatible = "sprd,vpu-dec-core0" }, {},
};
MODULE_DEVICE_TABLE(of, codec_match);
static struct platform_driver codec_driver = {
	.probe = codec_probe, .remove = codec_remove,
	.driver = { .name = "w202-vpu-codec", .of_match_table = codec_match,
		.suppress_bind_attrs = true, .dev_groups = codec_groups },
};
module_platform_driver(codec_driver);
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202DS native decoder ioctl, reset, IRQ and standard DMA-BUF bridge");
MODULE_VERSION("w202ds-vpu-codec-v9-v4l2-poc-origin");
