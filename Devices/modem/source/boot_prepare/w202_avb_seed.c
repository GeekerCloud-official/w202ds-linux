// SPDX-License-Identifier: GPL-2.0-only
/* Diagnostic: reproduce LK's read-only VbootVerifyInfo / FUNCTYPE_VBOOT_VERIFY_IMG.
 * References: LK platform/common/secureboot/sec_common.c and
 * platform/sprd_shared/trustzone/lk_drv_api.c. No rollback/RPMB write calls.
 */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

static bool run;
module_param(run, bool, 0444);
static int smc_result = -1;
module_param(smc_result, int, 0444);
static unsigned int verify_result = 0xffffffff;
module_param(verify_result, uint, 0444);

/* Every pointer on this wire is a physical u64, including return_data. */
struct verify_info {
	u64 img_name, img_name_len, img_addr, img_len;
	u64 vbmeta_img_addr, vbmeta_img_len, vbmeta_pubkey_addr, vbmeta_pubkey_len;
	u64 vb_cmdline_addr, vb_cmdline_len, return_data, return_data_len;
	u64 unlock_status, kce_decrypt_flag;
};

#define META_OFF 0x1000
#define KEY_OFF 0x11000
#define NAME_OFF 0x12000
#define RESULT_OFF 0x13000
#define CMDLINE_OFF 0x14000
#define IMAGE_OFF 0x18000
#define POOL_SIZE 0x28000

static struct device *dev;
static struct device *trusty_dev;
static void *pool;
static dma_addr_t pool_dma;
extern s32 trusty_std_call32(struct device *dev, u32 nr, u32 a0, u32 a1, u32 a2);

static int live_result_get(char *buf, const struct kernel_param *kp)
{
	dma_rmb();
	return scnprintf(buf, PAGE_SIZE, "%u\n",
		pool ? get_unaligned_le32(pool + RESULT_OFF) : 0xffffffff);
}
static const struct kernel_param_ops live_result_ops = { .get = live_result_get };
module_param_cb(live_result, &live_result_ops, NULL, 0444);

static long verify_on_cpu(void *unused)
{
	struct arm_smccc_res res;
	unsigned long flags;
	unsigned long deadline = jiffies + 10 * HZ;
	struct pm_qos_request qos = {};

	pr_notice("W202_AVB_PREPARE enter cpu=%u ctx=%pad size=%zu\n",
		  smp_processor_id(), &pool_dma, sizeof(struct verify_info));
	dma_wmb();
	cpu_latency_qos_add_request(&qos, 0);
	local_irq_save(flags);
	arm_smccc_smc(0xb4000020, 0x20, pool_dma, sizeof(struct verify_info),
		      0, 0, 0, 0, &res);
	local_irq_restore(flags);
	smc_result = (s32)res.a0;
	pr_notice("W202_AVB_SEED initial smc=%d\n", smc_result);
	do {
		/* Use the loaded Trusty driver's scheduler and log notifier. */
		smc_result = trusty_std_call32(trusty_dev, 0x3c000003, 0, 0, 0);
		dma_rmb();
		if (get_unaligned_le32(pool + RESULT_OFF) != 0xffffffff)
			break;
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));
	cpu_latency_qos_remove_request(&qos);
	dma_rmb();
	verify_result = get_unaligned_le32(pool + RESULT_OFF);
	pr_notice("W202_AVB_PREPARE returned smc=%d verify=%u\n",
		  smc_result, verify_result);
	if (verify_result == 0xffffffff)
		return -EINPROGRESS;
	return (smc_result != 0 && smc_result != -15) || verify_result ? -EKEYREJECTED : 0;
}

static void release_pool(void)
{
	if (pool)
		dma_free_coherent(dev, POOL_SIZE, pool, pool_dma);
	if (dev)
		root_device_unregister(dev);
	if (trusty_dev)
		put_device(trusty_dev);
	pool = NULL;
	dev = NULL;
	trusty_dev = NULL;
}

static int __init w202_avb_prepare_init(void)
{
	const struct firmware *meta = NULL, *key = NULL;
	struct verify_info *info;
	int ret;

	static_assert(sizeof(struct verify_info) == 112);
	if (!of_machine_is_compatible("sprd,ums9620"))
		return -ENODEV;
	trusty_dev = bus_find_device_by_name(&platform_bus_type, NULL, "trusty");
	if (!trusty_dev || !trusty_dev->driver) {
		if (trusty_dev)
			put_device(trusty_dev);
		trusty_dev = NULL;
		return -ENODEV;
	}
	dev = root_device_register("w202-avb-seed");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		dev = NULL;
		put_device(trusty_dev);
		trusty_dev = NULL;
		return ret;
	}
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto out;
	ret = request_firmware(&meta, "w202-diag/vbmeta.bin", dev);
	if (ret)
		goto out;
	ret = request_firmware(&key, "w202-diag/vboot-key-hash.bin", dev);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (meta->size != 0x10000 || key->size != 32 ||
	    memcmp(meta->data, "AVB0", 4) ||
	    get_unaligned_be32(meta->data + 120))
		goto out;
	pool = dma_alloc_coherent(dev, POOL_SIZE, &pool_dma, GFP_KERNEL);
	ret = -ENOMEM;
	if (!pool)
		goto out;
	ret = -ERANGE;
	if (upper_32_bits(pool_dma + POOL_SIZE - 1) || !IS_ALIGNED(pool_dma, 4096))
		goto out;
	memset(pool, 0, POOL_SIZE);
	memcpy(pool + META_OFF, meta->data, meta->size);
	memcpy(pool + IMAGE_OFF, meta->data, meta->size);
	memcpy(pool + KEY_OFF, key->data, key->size);
	memcpy(pool + NAME_OFF, "vbmeta", sizeof("vbmeta"));
	put_unaligned_le32(0xffffffff, pool + RESULT_OFF);
	info = pool;
	info->img_name = pool_dma + NAME_OFF;
	info->img_name_len = 36;
	/* Use the ordinary AVB request path so its read callback seeds KBC's copy.
	 * Request only the signed top-level metadata, without starting a CP core.
	 */
	info->img_addr = pool_dma + IMAGE_OFF;
	info->img_len = 0x4000;
	info->vbmeta_img_addr = pool_dma + META_OFF;
	info->vbmeta_img_len = 0x4000; /* exact DHTB payload size of this input */
	info->vbmeta_pubkey_addr = pool_dma + KEY_OFF;
	info->vbmeta_pubkey_len = 32;
	info->vb_cmdline_addr = pool_dma + CMDLINE_OFF;
	info->vb_cmdline_len = 4096;
	info->return_data = pool_dma + RESULT_OFF;
	info->return_data_len = 16;
	/* Keep AVB verification strict for this diagnostic. */
	info->unlock_status = 0;
	pr_notice("W202_AVB_PREPARE inputs ready pool=%pad run=%u\n", &pool_dma, run);
	ret = 0;
	if (run) {
		cpus_read_lock();
		ret = cpu_online(0) ? work_on_cpu(0, verify_on_cpu, NULL) : -ENODEV;
		cpus_read_unlock();
	}
out:
	release_firmware(key);
	release_firmware(meta);
	if (ret == -EINPROGRESS) {
		/* A yielded secure call may still hold our physical pointers. */
		__module_get(THIS_MODULE);
		pr_err("W202_AVB_PREPARE pending: retain buffers/module until reboot\n");
		return 0;
	}
	if (ret)
		release_pool();
	return ret;
}

static void __exit w202_avb_prepare_exit(void)
{
	release_pool();
}
module_init(w202_avb_prepare_init);
module_exit(w202_avb_prepare_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("W202DS temporary LK AVB initialization comparison");
