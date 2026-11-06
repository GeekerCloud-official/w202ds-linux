// SPDX-License-Identifier: GPL-2.0-only
/* Native DRM boot log client. No early-console or handoff symbols. */
#include <linux/console.h>
#include <linux/font.h>
#include <linux/io.h>
#include <linux/iosys-map.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <drm/drm_client.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_print.h>
#include "w202_r6p0.h"
#include "w202_boot_text.h"

struct w202_boot_console {
	struct drm_client_dev client;
	struct drm_client_buffer *buffer;
	struct iosys_map map;
	struct console console;
	struct work_struct work;
	raw_spinlock_t lock;
	struct w202_boot_text text, frame;
	u8 drawn[W202_TEXT_ROWS][W202_TEXT_COLS];
	u16 glyphs[96][W202_TEXT_W][W202_TEXT_H];
	void *inherited;
	bool active, registered, captured, committed;
	u64 frames, cells, render_ns, render_max_ns;
};

static void w202_boot_render(struct work_struct *work)
{
	struct w202_boot_console *b = container_of(work, struct w202_boot_console, work);
	unsigned long flags;
	u64 start, changed = 0, elapsed;
	unsigned int row, col, x, ch;

	raw_spin_lock_irqsave(&b->lock, flags);
	if (!b->active) {
		raw_spin_unlock_irqrestore(&b->lock, flags);
		return;
	}
	b->frame = b->text;
	b->text.dirty = 0;
	raw_spin_unlock_irqrestore(&b->lock, flags);
	start = ktime_get_ns();
	for (row = 0; row < b->frame.rows; row++) {
		if (!(b->frame.dirty & (1ULL << row)))
			continue;
		for (col = 0; col < b->frame.cols; col++) {
			ch = b->frame.cells[row][col];
			if (b->drawn[row][col] == ch)
				continue;
			for (x = 0; x < W202_TEXT_W; x++)
				iosys_map_memcpy_to(&b->map,
					w202_boot_text_offset(&b->frame, b->buffer->pitch, row, col, x),
					b->glyphs[ch - 32][x], W202_TEXT_H * sizeof(u16));
			b->drawn[row][col] = ch;
			changed++;
		}
	}
	/* The native DPU scans a DMA-coherent video framebuffer continuously.
	 * No per-line atomic commit, MMIO kick or synchronous serial write.
	 */
	wmb();
	elapsed = ktime_get_ns() - start;
	if (changed) {
		b->frames++;
		b->cells += changed;
		b->render_ns += elapsed;
		b->render_max_ns = max(b->render_max_ns, elapsed);
	}
	raw_spin_lock_irqsave(&b->lock, flags);
	if (b->active && b->text.dirty)
		schedule_work(&b->work);
	raw_spin_unlock_irqrestore(&b->lock, flags);
}

static void w202_boot_write(struct console *console, const char *s, unsigned int count)
{
	struct w202_boot_console *b = container_of(console, struct w202_boot_console, console);
	unsigned long flags;

	raw_spin_lock_irqsave(&b->lock, flags);
	if (b->active) {
		while (count--)
			w202_boot_text_put(&b->text, *s++);
		if (b->text.dirty)
			schedule_work(&b->work);
	}
	raw_spin_unlock_irqrestore(&b->lock, flags);
}

static void w202_boot_stop(struct w202_boot_console *b)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&b->lock, flags);
	b->active = false;
	raw_spin_unlock_irqrestore(&b->lock, flags);
	if (b->registered) {
		unregister_console(&b->console);
		b->registered = false;
	}
	cancel_work_sync(&b->work);
}

static void w202_boot_unregister(struct drm_client_dev *client)
{
	struct w202_boot_console *b = container_of(client, struct w202_boot_console, client);

	w202_boot_stop(b);
	if (b->inherited)
		memunmap(b->inherited);
	drm_client_buffer_vunmap(b->buffer);
	drm_client_framebuffer_delete(b->buffer);
	drm_client_release(client);
	kvfree(b);
}

static const struct drm_client_funcs w202_boot_client_funcs = {
	.owner = THIS_MODULE,
	.unregister = w202_boot_unregister,
};

int w202_boot_console_prepare(struct w202_dpu *d)
{
	struct device_node *chosen;
	struct w202_boot_console *b;
	u32 v[20];
	unsigned int ch, x, y;
	const u8 *font = font_vga_8x16.data;
	int ret;

	if (d->boot_console)
		return 0;
	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return 0;
	if (!of_find_property(chosen, "pocketjs,boot-display", NULL)) {
		of_node_put(chosen);
		return 0; /* Ordinary boot: use full-screen fbdev. */
	}
	ret = of_property_count_u32_elems(chosen, "pocketjs,boot-display");
	if (ret == ARRAY_SIZE(v))
		ret = of_property_read_u32_array(chosen, "pocketjs,boot-display", v, ARRAY_SIZE(v));
	else
		ret = -EINVAL;
	of_node_put(chosen);
	if (ret) {
		drm_warn(&d->drm, "invalid optional boot-display property; using normal fbdev\n");
		return 0;
	}
	b = kvzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	if (!w202_boot_text_select(&b->text, v, ARRAY_SIZE(v))) {
		drm_warn(&d->drm, "unsupported optional boot-display layout; using normal fbdev\n");
		kvfree(b);
		return 0;
	}
	raw_spin_lock_init(&b->lock);
	INIT_WORK(&b->work, w202_boot_render);
	for (ch = 32; ch < 128; ch++)
		for (x = 0; x < W202_TEXT_W; x++)
			for (y = 0; y < W202_TEXT_H; y++)
				b->glyphs[ch - 32][x][W202_TEXT_H - 1 - y] =
					font[ch * 16 + y * 16 / W202_TEXT_H] &
					(0x80 >> (x * 8 / W202_TEXT_W)) ? 0xdedb : 0;
	b->inherited = memremap(0xb0000000, 4608000, MEMREMAP_WB);
	if (!b->inherited) {
		ret = -ENOMEM;
		goto free;
	}
	ret = drm_client_init(&d->drm, &b->client, "w202-boot-log", &w202_boot_client_funcs);
	if (ret)
		goto unmap;
	b->buffer = drm_client_framebuffer_create(&b->client, 1200, 1920, DRM_FORMAT_RGB565);
	if (IS_ERR(b->buffer)) {
		ret = PTR_ERR(b->buffer);
		goto release;
	}
	ret = drm_client_buffer_vmap(b->buffer, &b->map);
	if (ret)
		goto delete;
	if (b->buffer->pitch < 2400) {
		ret = -EINVAL;
		drm_client_buffer_vunmap(b->buffer);
		goto delete;
	}
	strscpy(b->console.name, "drmboot", sizeof(b->console.name));
	b->console.write = w202_boot_write;
	b->console.flags = CON_ENABLED | CON_PRINTBUFFER;
	b->console.index = 0;
	d->boot_console = b;
	drm_client_register(&b->client);
	return 0;
delete:
	drm_client_framebuffer_delete(b->buffer);
release:
	drm_client_release(&b->client);
unmap:
	memunmap(b->inherited);
free:
	kvfree(b);
	return ret;
}

void w202_boot_console_capture(struct w202_dpu *d)
{
	struct w202_boot_console *b = d->boot_console;
	unsigned int y;

	/* Called after generic aperture removal has drained the old writer,
	 * before resetting the DPU. Preserve every pixel outside the log window.
	 */
	if (!b || b->captured)
		return;
	for (y = 0; y < 1920; y++)
		iosys_map_memcpy_to(&b->map, y * b->buffer->pitch,
				   b->inherited + y * 2400, 2400);
	wmb();
	memunmap(b->inherited);
	b->inherited = NULL;
	b->captured = true;
}

int w202_boot_console_start(struct w202_dpu *d)
{
	struct w202_boot_console *b = d->boot_console;
	struct drm_mode_set *set;
	u64 timeouts = d->latch_timeouts, updates = d->updates;
	int ret;

	if (b->committed)
		return 0;
	if (!b->captured)
		return -EINVAL;
	ret = drm_client_modeset_probe(&b->client, 1200, 1920);
	if (ret)
		return ret;
	mutex_lock(&b->client.modeset_mutex);
	drm_client_for_each_modeset(set, &b->client)
		if (set->num_connectors)
			set->fb = b->buffer->fb;
	mutex_unlock(&b->client.modeset_mutex);
	ret = drm_client_modeset_commit(&b->client);
	if (ret)
		return ret;
	if (d->latch_timeouts != timeouts || d->updates == updates)
		return -ETIMEDOUT;
	b->committed = true;
	b->active = true;
	register_console(&b->console);
	b->registered = true;
	pr_info("w202-drm: native boot log active in %ux%u cells; full screen awaits login\n",
		b->text.cols, b->text.rows);
	return 0;
}

void w202_boot_console_stop(struct w202_dpu *d)
{
	if (d->boot_console)
		w202_boot_stop(d->boot_console);
}

bool w202_boot_console_active(struct w202_dpu *d)
{
	return d->boot_console && READ_ONCE(d->boot_console->active);
}

ssize_t w202_boot_console_status(struct w202_dpu *d, char *buf)
{
	struct w202_boot_console *b = d->boot_console;

	if (!b)
		return sysfs_emit(buf, "available=0 active=0\n");
	return sysfs_emit(buf, "available=1 active=%u captured=%u committed=%u frames=%llu cells=%llu render_ns=%llu render_max_ns=%llu pitch=%u\n",
		READ_ONCE(b->active), b->captured, b->committed, READ_ONCE(b->frames),
		READ_ONCE(b->cells), READ_ONCE(b->render_ns), READ_ONCE(b->render_max_ns), b->buffer->pitch);
}
