// SPDX-License-Identifier: GPL-2.0-only
/* Diagnostic fbdev over the M4-proven LK scanout. No display MMIO or modeset.
 * Registration is explicitly deferred until the USB shell requests it.
 * RAM is mapped WB (the stock reservation is part of the linear map), and
 * each CPU drawing operation is cleaned to the PoC for LK's DMA scanout.
 */
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <asm/cacheflush.h>

#define LOGO_BASE 0xb0000000ULL
#define FRAME_W 1200
#define FRAME_H 1920
#define FRAME_PITCH (FRAME_W * 2)
#define FRAME_SIZE (FRAME_PITCH * FRAME_H)
#define ATTACH_FBCON _IO('W', 1)

static DEFINE_MUTEX(attach_lock);
static struct fb_info *logo_info;

static void flush_rows(struct fb_info *info, u32 y, u32 height)
{
	unsigned long start, end;

	if (y >= FRAME_H || !height)
		return;
	height = min_t(u32, height, FRAME_H - y);
	start = (unsigned long)info->screen_buffer + y * FRAME_PITCH;
	end = start + height * FRAME_PITCH;
	dcache_clean_poc(start, end);
}

static void logo_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	sys_fillrect(info, rect);
	flush_rows(info, rect->dy, rect->height);
}

static void logo_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	sys_copyarea(info, area);
	flush_rows(info, area->dy, area->height);
}

static void logo_imageblit(struct fb_info *info, const struct fb_image *image)
{
	sys_imageblit(info, image);
	flush_rows(info, image->dy, image->height);
}

static ssize_t logo_fb_write(struct fb_info *info, const char __user *buf,
			     size_t count, loff_t *pos)
{
	ssize_t ret = fb_sys_write(info, buf, count, pos);

	/* Also publish a partial copy if fb_sys_write reports an error. */
	flush_rows(info, 0, FRAME_H);
	return ret;
}

static int logo_setcolreg(unsigned int index, unsigned int red,
			  unsigned int green, unsigned int blue,
			  unsigned int transp, struct fb_info *info)
{
	if (index >= 16)
		return -EINVAL;
	((u32 *)info->pseudo_palette)[index] =
		((red >> 11) << 11) | ((green >> 10) << 5) | (blue >> 11);
	return 0;
}

static int logo_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	if (var->xres != FRAME_W || var->yres != FRAME_H ||
	    var->xres_virtual != FRAME_W || var->yres_virtual != FRAME_H ||
	    var->bits_per_pixel != 16 || var->xoffset || var->yoffset ||
	    var->rotate || var->grayscale || var->nonstd)
		return -EINVAL;
	var->red = (struct fb_bitfield){ .offset = 11, .length = 5 };
	var->green = (struct fb_bitfield){ .offset = 5, .length = 6 };
	var->blue = (struct fb_bitfield){ .offset = 0, .length = 5 };
	var->transp = (struct fb_bitfield){};
	return 0;
}

static int logo_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	/* A normal userspace mmap cannot guarantee cache publishing on stores. */
	return -EOPNOTSUPP;
}

static const struct fb_ops logo_fb_ops = {
	.owner = THIS_MODULE,
	.fb_read = fb_sys_read,
	.fb_write = logo_fb_write,
	.fb_fillrect = logo_fillrect,
	.fb_copyarea = logo_copyarea,
	.fb_imageblit = logo_imageblit,
	.fb_check_var = logo_check_var,
	.fb_setcolreg = logo_setcolreg,
	.fb_mmap = logo_mmap,
};

static int attach_framebuffer(void)
{
	struct fb_info *info;
	void *memory;
	int ret;

	if (logo_info)
		return 0;
	pr_info("[W202DS-FBCON] deferred attach begin; reserved RAM only\n");
	memory = memremap(LOGO_BASE, FRAME_SIZE, MEMREMAP_WB);
	if (!memory)
		return -ENOMEM;
	info = framebuffer_alloc(16 * sizeof(u32), NULL);
	if (!info) {
		memunmap(memory);
		return -ENOMEM;
	}
	info->screen_buffer = memory;
	info->screen_size = FRAME_SIZE;
	info->pseudo_palette = info->par;
	info->flags = FBINFO_VIRTFB | FBINFO_READS_FAST;
	info->fbops = &logo_fb_ops;
	strscpy(info->fix.id, "W202DS-LKFB", sizeof(info->fix.id));
	info->fix.smem_start = LOGO_BASE;
	info->fix.smem_len = FRAME_SIZE;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = FRAME_PITCH;
	info->fix.accel = FB_ACCEL_NONE;
	info->var.xres = info->var.xres_virtual = FRAME_W;
	info->var.yres = info->var.yres_virtual = FRAME_H;
	info->var.bits_per_pixel = 16;
	info->var.width = 147;
	info->var.height = 236;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.vmode = FB_VMODE_NONINTERLACED;
	logo_check_var(&info->var, info);
	ret = fb_alloc_cmap(&info->cmap, 16, 0);
	if (ret)
		goto release;
	ret = register_framebuffer(info);
	if (ret) {
		fb_dealloc_cmap(&info->cmap);
		goto release;
	}
	logo_info = info;
	pr_info("[W202DS-FBCON] fb%d registered, 1200x1920 RGB565 stride2400; LK scanout preserved\n",
		info->node);
	return 0;
release:
	framebuffer_release(info);
	memunmap(memory);
	return ret;
}

static long logo_control_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	int ret;

	if (cmd != ATTACH_FBCON)
		return -ENOTTY;
	mutex_lock(&attach_lock);
	ret = attach_framebuffer();
	mutex_unlock(&attach_lock);
	return ret;
}

static const struct file_operations logo_control_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = logo_control_ioctl,
};

static struct miscdevice logo_control = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "w202fbcon",
	.fops = &logo_control_ops,
	.mode = 0600,
};

static int __init logo_console_init(void)
{
	struct device_node *chosen, *logo;
	struct resource r;
	const char *args;
	int ret;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return -ENODEV;
	ret = of_property_read_string(chosen, "bootargs", &args);
	if (!ret && (!strstr(args, "lcd_name=lcd_guoxian_hx83102j_huaxing_nopen_mipi_himax ") ||
		     !strstr(args, "lcd_base=b0000000 ") ||
		     !strstr(args, "logo_bpix=8 ") ||
		     !strstr(args, "androidboot.lcd_flip=1 ")))
		ret = -ENODEV;
	of_node_put(chosen);
	if (ret)
		return ret;
	logo = of_find_node_by_path("/reserved-memory/logobuffer@b0000000");
	if (!logo)
		return -ENODEV;
	ret = of_address_to_resource(logo, 0, &r);
	if (!ret && (r.start != LOGO_BASE || resource_size(&r) != 0x9e4000 ||
		     of_property_read_bool(logo, "reusable") ||
		     of_property_read_bool(logo, "no-map")))
		ret = -EINVAL;
	of_node_put(logo);
	if (ret)
		return ret;
	ret = misc_register(&logo_control);
	if (!ret)
		pr_info("[W202DS-FBCON] control ready; no framebuffer access before USB command\n");
	return ret;
}
late_initcall(logo_console_init);
