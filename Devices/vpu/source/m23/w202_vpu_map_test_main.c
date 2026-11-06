// SPDX-License-Identifier: GPL-2.0-only
/* DMA-BUF prep/ownership research frontend. Unsupported codec ioctls fail. */
#include <linux/compat.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/iommu.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "w202_vpu_buffers.h"
#include "w202_vpu.h"

struct map_test_device {
	struct miscdevice misc;
	atomic_t opens, maps, automatic_unmaps, copyout_rollbacks;
	atomic_t verified_pages, high_pages;
};
struct map_test_file {
	struct map_test_device *test;
	struct w202_vpu_buffers buffers;
	/* Serialise the complete ioctl transaction, including userspace copy. */
	struct mutex lock;
};

static int map_test_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct map_test_device *test = container_of(misc, struct map_test_device, misc);
	struct map_test_file *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;
	ctx->test = test;
	w202_vpu_buffers_init(&ctx->buffers, misc->parent);
	mutex_init(&ctx->lock);
	file->private_data = ctx;
	atomic_inc(&test->opens);
	return 0;
}

static int map_test_release(struct inode *inode, struct file *file)
{
	struct map_test_file *ctx = file->private_data;
	unsigned int pending = ctx->buffers.count;

	w202_vpu_buffers_destroy(&ctx->buffers);
	atomic_sub(pending, &ctx->test->maps);
	atomic_add(pending, &ctx->test->automatic_unmaps);
	atomic_dec(&ctx->test->opens);
	kfree(ctx);
	return 0;
}

static long map_test_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct map_test_file *ctx = file->private_data;
	struct map_test_device *test = ctx->test;
	struct w202_vpu_iova map;
	unsigned int pages, high;
	void __user *user = (void __user *)arg;
	int ret;

	if (cmd != W202_VPU_GET_IOVA && cmd != W202_VPU_FREE_IOVA)
		return -ENOTTY;
	if (copy_from_user(&map, user, sizeof(map)))
		return -EFAULT;
	mutex_lock(&ctx->lock);
	if (cmd == W202_VPU_FREE_IOVA) {
		ret = w202_vpu_buffers_unmap(&ctx->buffers, &map);
		if (!ret)
			atomic_dec(&test->maps);
		goto out;
	}
	ret = w202_vpu_buffers_map(&ctx->buffers, &map);
	if (ret)
		goto out;
	/* This frontend validates actual imported pages before returning an IOVA. */
	ret = w202_vpu_iommu_restore(test->misc.parent);
	if (!ret)
		ret = w202_vpu_buffers_verify(&ctx->buffers, &map, &pages, &high);
	if (ret) {
		w202_vpu_buffers_unmap(&ctx->buffers, &map);
		goto out;
	}
	if (copy_to_user(user, &map, sizeof(map))) {
		w202_vpu_buffers_unmap(&ctx->buffers, &map);
		atomic_inc(&test->copyout_rollbacks);
		ret = -EFAULT;
		goto out;
	}
	atomic_inc(&test->maps);
	atomic_add(pages, &test->verified_pages);
	atomic_add(high, &test->high_pages);
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static const struct file_operations map_test_fops = {
	.owner = THIS_MODULE,
	.open = map_test_open,
	.release = map_test_release,
	.unlocked_ioctl = map_test_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct map_test_device *test = dev_get_drvdata(dev);

	return sysfs_emit(buf, "opens=%d maps=%d automatic_unmaps=%d copyout_rollbacks=%d "
			 "verified_pages=%d high_pages=%d\n",
			 atomic_read(&test->opens), atomic_read(&test->maps),
			 atomic_read(&test->automatic_unmaps), atomic_read(&test->copyout_rollbacks),
			 atomic_read(&test->verified_pages), atomic_read(&test->high_pages));
}
static DEVICE_ATTR_RO(state);
static struct attribute *map_test_attrs[] = { &dev_attr_state.attr, NULL };
ATTRIBUTE_GROUPS(map_test);

static int map_test_probe(struct platform_device *pdev)
{
	struct map_test_device *test;
	const char *name = of_device_get_match_data(&pdev->dev);
	struct iommu_domain *domain = iommu_get_domain_for_dev(&pdev->dev);
	int ret;

	if (!domain || !iommu_is_dma_domain(domain))
		return -EPROBE_DEFER;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(36));
	if (ret)
		return ret;
	test = devm_kzalloc(&pdev->dev, sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->misc.minor = MISC_DYNAMIC_MINOR;
	test->misc.name = name;
	test->misc.fops = &map_test_fops;
	test->misc.parent = &pdev->dev;
	test->misc.mode = 0600;
	platform_set_drvdata(pdev, test);
	ret = misc_register(&test->misc);
	if (!ret)
		dev_info(&pdev->dev, "DMA-BUF mapping research only: /dev/%s\n", name);
	return ret;
}

static void map_test_remove(struct platform_device *pdev)
{
	struct map_test_device *test = platform_get_drvdata(pdev);

	/* Bind/unbind attrs suppressed; fops->owner blocks rmmod while files exist. */
	misc_deregister(&test->misc);
}

static const struct of_device_id map_test_match[] = {
	{ .compatible = "sprd,vpu-enc-core0", .data = "w202-vpu-map-enc0" },
	{ .compatible = "sprd,vpu-enc-core1", .data = "w202-vpu-map-enc1" },
	{ .compatible = "sprd,vpu-dec-core0", .data = "w202-vpu-map-dec" },
	{},
};
MODULE_DEVICE_TABLE(of, map_test_match);
static struct platform_driver map_test_driver = {
	.probe = map_test_probe,
	.remove = map_test_remove,
	.driver = {
		.name = "w202-vpu-map-test",
		.of_match_table = map_test_match,
		.suppress_bind_attrs = true,
		.dev_groups = map_test_groups,
	},
};
module_platform_driver(map_test_driver);
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202DS VPU standard DMA-BUF import/ownership research frontend");
MODULE_VERSION("w202ds-vpu-map-test-v1");
