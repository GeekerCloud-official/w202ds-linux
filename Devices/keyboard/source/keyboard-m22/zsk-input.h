/* SPDX-License-Identifier: GPL-2.0-only */
/* Private input/TTY part of w202_zsk.c. */
#include <linux/bitmap.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/spinlock.h>
#include <linux/tty.h>
#include <linux/tty_ldisc.h>
#include "zsk-protocol.h"

#define ZSK_PAD_X 1177
#define ZSK_PAD_Y 591
#define ZSK_PAD_SLOTS 4

struct w202_zsk_uart {
	struct input_dev *keyboard;
	struct input_dev *touchpad;
	spinlock_t lock;
	struct zsk_parser parser;
	DECLARE_BITMAP(keys, KEY_CNT);
	u64 bytes;
	u64 key_reports;
	u64 pad_reports;
	u64 invalid_reports;
	u64 tty_errors;
};

static DEFINE_MUTEX(w202_zsk_uart_mutex);
static struct w202_zsk_uart *w202_zsk_uart;
static bool w202_zsk_board_online;
static bool w202_zsk_attached;

/* Standard HID usages plus the vendor's function-row usages. */
static const unsigned short w202_zsk_keycodes[256] = {
	[0x04] = KEY_A, [0x05] = KEY_B, [0x06] = KEY_C,
	[0x07] = KEY_D, [0x08] = KEY_E, [0x09] = KEY_F,
	[0x0a] = KEY_G, [0x0b] = KEY_H, [0x0c] = KEY_I,
	[0x0d] = KEY_J, [0x0e] = KEY_K, [0x0f] = KEY_L,
	[0x10] = KEY_M, [0x11] = KEY_N, [0x12] = KEY_O,
	[0x13] = KEY_P, [0x14] = KEY_Q, [0x15] = KEY_R,
	[0x16] = KEY_S, [0x17] = KEY_T, [0x18] = KEY_U,
	[0x19] = KEY_V, [0x1a] = KEY_W, [0x1b] = KEY_X,
	[0x1c] = KEY_Y, [0x1d] = KEY_Z,
	[0x1e] = KEY_1, [0x1f] = KEY_2, [0x20] = KEY_3,
	[0x21] = KEY_4, [0x22] = KEY_5, [0x23] = KEY_6,
	[0x24] = KEY_7, [0x25] = KEY_8, [0x26] = KEY_9,
	[0x27] = KEY_0, [0x28] = KEY_ENTER, [0x29] = KEY_ESC,
	[0x2a] = KEY_BACKSPACE, [0x2b] = KEY_TAB, [0x2c] = KEY_SPACE,
	[0x2d] = KEY_MINUS, [0x2e] = KEY_EQUAL,
	[0x2f] = KEY_LEFTBRACE, [0x30] = KEY_RIGHTBRACE,
	[0x31] = KEY_BACKSLASH, [0x33] = KEY_SEMICOLON,
	[0x34] = KEY_APOSTROPHE, [0x35] = KEY_GRAVE,
	[0x36] = KEY_COMMA, [0x37] = KEY_DOT, [0x38] = KEY_SLASH,
	[0x39] = KEY_CAPSLOCK,
	[0x3a] = KEY_F1, [0x3b] = KEY_F2, [0x3c] = KEY_F3,
	[0x3d] = KEY_F4, [0x3e] = KEY_F5, [0x3f] = KEY_F6,
	[0x40] = KEY_F7, [0x41] = KEY_F8, [0x42] = KEY_F9,
	[0x43] = KEY_F10, [0x44] = KEY_F11, [0x45] = KEY_F12,
	[0x4a] = KEY_HOME, [0x4b] = KEY_PAGEUP, [0x4c] = KEY_DELETE,
	[0x4d] = KEY_END, [0x4e] = KEY_PAGEDOWN,
	[0x4f] = KEY_RIGHT, [0x50] = KEY_LEFT, [0x51] = KEY_DOWN, [0x52] = KEY_UP,
	[0xf0] = KEY_FN, [0xf1] = KEY_SEARCH, [0xf2] = KEY_MUTE,
	[0xf3] = KEY_VOLUMEDOWN, [0xf4] = KEY_VOLUMEUP, [0xf5] = KEY_MICMUTE,
	[0xf6] = KEY_PROG1, [0xf7] = KEY_BRIGHTNESSDOWN,
	[0xf8] = KEY_BRIGHTNESSUP, [0xf9] = KEY_LANGUAGE,
	[0xfa] = KEY_SYSRQ, [0xfb] = KEY_TOUCHPAD_TOGGLE,
	[0xfc] = KEY_VOICECOMMAND, [0xfd] = KEY_PROG2, [0xfe] = KEY_FN_ESC,
};

static const unsigned short w202_zsk_modifiers[] = {
	KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT, KEY_LEFTMETA,
	KEY_RIGHTCTRL, KEY_RIGHTSHIFT, KEY_RIGHTALT, KEY_RIGHTMETA,
};

/* Caller holds uart->lock, including for attachment changes and close. */
static void w202_zsk_release_input(struct w202_zsk_uart *uart)
{
	unsigned long key;
	int slot;

	for_each_set_bit(key, uart->keys, KEY_CNT)
		input_report_key(uart->keyboard, key, 0);
	bitmap_zero(uart->keys, KEY_CNT);
	input_sync(uart->keyboard);
	for (slot = 0; slot < ZSK_PAD_SLOTS; slot++) {
		input_mt_slot(uart->touchpad, slot);
		input_mt_report_slot_state(uart->touchpad, MT_TOOL_FINGER, false);
	}
	input_report_key(uart->touchpad, BTN_LEFT, 0);
	input_mt_sync_frame(uart->touchpad);
	input_sync(uart->touchpad);
	uart->parser.used = 0;
}

static void w202_zsk_reset_transport(bool attached)
{
	unsigned long flags;

	WRITE_ONCE(w202_zsk_attached, attached);
	mutex_lock(&w202_zsk_uart_mutex);
	if (w202_zsk_uart) {
		spin_lock_irqsave(&w202_zsk_uart->lock, flags);
		w202_zsk_release_input(w202_zsk_uart);
		spin_unlock_irqrestore(&w202_zsk_uart->lock, flags);
	}
	mutex_unlock(&w202_zsk_uart_mutex);
}

static void w202_zsk_key_report(struct w202_zsk_uart *uart, const u8 *frame)
{
	DECLARE_BITMAP(next, KEY_CNT);
	DECLARE_BITMAP(changed, KEY_CNT);
	unsigned long key;
	unsigned int i;

	/* HID rollover/error usages do not represent a new key state. */
	for (i = 3; i < 9; i++) {
		if (frame[i] && frame[i] <= 3) {
			uart->invalid_reports++;
			return;
		}
	}
	bitmap_zero(next, KEY_CNT);
	for (i = 0; i < ARRAY_SIZE(w202_zsk_modifiers); i++)
		if (frame[1] & BIT(i))
			__set_bit(w202_zsk_modifiers[i], next);
	for (i = 3; i < 9; i++) {
		key = w202_zsk_keycodes[frame[i]];
		if (key)
			__set_bit(key, next);
	}
	bitmap_xor(changed, next, uart->keys, KEY_CNT);
	/* Modifiers must reach the VT before letters in the same report. */
	for (i = 0; i < ARRAY_SIZE(w202_zsk_modifiers); i++) {
		key = w202_zsk_modifiers[i];
		if (test_and_clear_bit(key, changed))
			input_report_key(uart->keyboard, key, test_bit(key, next));
	}
	for_each_set_bit(key, changed, KEY_CNT)
		input_report_key(uart->keyboard, key, test_bit(key, next));
	bitmap_copy(uart->keys, next, KEY_CNT);
	input_sync(uart->keyboard);
	uart->key_reports++;
}

static void w202_zsk_pad_report(struct w202_zsk_uart *uart, const u8 *frame)
{
	const u8 *report = frame;
	unsigned int i, x[ZSK_PAD_SLOTS], y[ZSK_PAD_SLOTS];
	bool valid[ZSK_PAD_SLOTS];

	for (i = 0; i < ZSK_PAD_SLOTS; i++) {
		unsigned int offset = 1 + i * 4;

		valid[i] = report[offset] & BIT(1);
		x[i] = report[offset + 1] | ((report[offset + 2] & 0x0f) << 8);
		y[i] = (report[offset + 3] << 4) | (report[offset + 2] >> 4);
		if (valid[i] && (x[i] > ZSK_PAD_X || y[i] > ZSK_PAD_Y)) {
			uart->invalid_reports++;
			return;
		}
	}
	for (i = 0; i < ZSK_PAD_SLOTS; i++) {
		input_mt_slot(uart->touchpad, i);
		input_mt_report_slot_state(uart->touchpad, MT_TOOL_FINGER, valid[i]);
		if (valid[i]) {
			input_report_abs(uart->touchpad, ABS_MT_POSITION_X, x[i]);
			input_report_abs(uart->touchpad, ABS_MT_POSITION_Y, y[i]);
		}
	}
	input_report_key(uart->touchpad, BTN_LEFT, !!(report[20] & 1));
	input_mt_sync_frame(uart->touchpad);
	input_sync(uart->touchpad);
	uart->pad_reports++;
}

static void w202_zsk_frame(void *ctx, const unsigned char *frame, unsigned int length)
{
	struct w202_zsk_uart *uart = ctx;

	if (length == 9)
		w202_zsk_key_report(uart, frame);
	else
		w202_zsk_pad_report(uart, frame);
}

static void w202_zsk_receive(struct tty_struct *tty, const u8 *data,
			     const u8 *flags, size_t count)
{
	struct w202_zsk_uart *uart = tty->disc_data;
	unsigned long irqflags;
	size_t i;

	spin_lock_irqsave(&uart->lock, irqflags);
	uart->bytes += count;
	if (!READ_ONCE(w202_zsk_attached))
		goto out;
	for (i = 0; i < count; i++) {
		if (flags && flags[i] != TTY_NORMAL) {
			uart->tty_errors++;
			w202_zsk_release_input(uart);
			continue;
		}
		zsk_parser_feed(&uart->parser, data[i], w202_zsk_frame, uart);
	}
out:
	spin_unlock_irqrestore(&uart->lock, irqflags);
}

static void w202_zsk_flush(struct tty_struct *tty)
{
	struct w202_zsk_uart *uart = tty->disc_data;
	unsigned long flags;

	if (!uart)
		return;
	spin_lock_irqsave(&uart->lock, flags);
	w202_zsk_release_input(uart);
	spin_unlock_irqrestore(&uart->lock, flags);
}

static int w202_zsk_ldisc_open(struct tty_struct *tty)
{
	struct w202_zsk_uart *uart;
	struct input_dev *key, *pad;
	unsigned int i;
	int ret;

	if (strcmp(tty_name(tty), "ttyS2"))
		return -ENODEV;
	mutex_lock(&w202_zsk_uart_mutex);
	if (w202_zsk_uart || !READ_ONCE(w202_zsk_board_online)) {
		ret = -EBUSY;
		goto unlock;
	}
	uart = kzalloc(sizeof(*uart), GFP_KERNEL);
	if (!uart) { ret = -ENOMEM; goto unlock; }
	key = input_allocate_device();
	pad = input_allocate_device();
	if (!key || !pad) { ret = -ENOMEM; goto free_devices; }
	uart->keyboard = key;
	uart->touchpad = pad;
	spin_lock_init(&uart->lock);
	key->name = "W202DS ZTE Keyboard";
	key->phys = "w202-zsk/keyboard";
	key->id.bustype = BUS_RS232;
	key->id.vendor = 0x19d2;
	key->id.product = 0x1987;
	key->id.version = 1;
	key->dev.parent = tty->dev;
	__set_bit(EV_REP, key->evbit);
	for (i = 0; i < ARRAY_SIZE(w202_zsk_keycodes); i++)
		if (w202_zsk_keycodes[i])
			input_set_capability(key, EV_KEY, w202_zsk_keycodes[i]);
	for (i = 0; i < ARRAY_SIZE(w202_zsk_modifiers); i++)
		input_set_capability(key, EV_KEY, w202_zsk_modifiers[i]);
	pad->name = "W202DS ZTE Touchpad";
	pad->phys = "w202-zsk/touchpad";
	pad->id.bustype = BUS_RS232;
	pad->id.vendor = 0x19d2;
	pad->id.product = 0x2023;
	pad->id.version = 1;
	pad->dev.parent = tty->dev;
	__set_bit(INPUT_PROP_POINTER, pad->propbit);
	__set_bit(INPUT_PROP_BUTTONPAD, pad->propbit);
	input_set_capability(pad, EV_KEY, BTN_LEFT);
	input_set_abs_params(pad, ABS_X, 0, ZSK_PAD_X, 0, 0);
	input_set_abs_params(pad, ABS_Y, 0, ZSK_PAD_Y, 0, 0);
	input_set_abs_params(pad, ABS_MT_POSITION_X, 0, ZSK_PAD_X, 0, 0);
	input_set_abs_params(pad, ABS_MT_POSITION_Y, 0, ZSK_PAD_Y, 0, 0);
	input_abs_set_res(pad, ABS_X, ZSK_PAD_X / 85);
	input_abs_set_res(pad, ABS_Y, ZSK_PAD_Y / 43);
	input_abs_set_res(pad, ABS_MT_POSITION_X, ZSK_PAD_X / 85);
	input_abs_set_res(pad, ABS_MT_POSITION_Y, ZSK_PAD_Y / 43);
	ret = input_mt_init_slots(pad, ZSK_PAD_SLOTS, INPUT_MT_POINTER | INPUT_MT_DROP_UNUSED);
	if (ret)
		goto free_devices;
	ret = input_register_device(key);
	if (ret)
		goto free_devices;
	ret = input_register_device(pad);
	if (ret) {
		input_unregister_device(key);
		key = NULL;
		goto free_devices;
	}
	tty->receive_room = 65536;
	tty->disc_data = uart;
	w202_zsk_uart = uart;
	mutex_unlock(&w202_zsk_uart_mutex);
	return 0;
free_devices:
	input_free_device(pad);
	input_free_device(key);
	kfree(uart);
unlock:
	mutex_unlock(&w202_zsk_uart_mutex);
	return ret;
}

static void w202_zsk_ldisc_close(struct tty_struct *tty)
{
	struct w202_zsk_uart *uart = tty->disc_data;

	mutex_lock(&w202_zsk_uart_mutex);
	w202_zsk_flush(tty);
	w202_zsk_uart = NULL;
	tty->disc_data = NULL;
	input_unregister_device(uart->touchpad);
	input_unregister_device(uart->keyboard);
	kfree(uart);
	mutex_unlock(&w202_zsk_uart_mutex);
}

static ssize_t w202_zsk_uart_state(char *buf, int offset)
{
	struct w202_zsk_uart *uart;
	unsigned long flags;
	ssize_t len;

	mutex_lock(&w202_zsk_uart_mutex);
	uart = w202_zsk_uart;
	if (!uart) {
		len = sysfs_emit_at(buf, offset, "uart_open=0\n");
	} else {
		spin_lock_irqsave(&uart->lock, flags);
		len = sysfs_emit_at(buf, offset,
			"uart_open=1 bytes=%llu key_reports=%llu pad_reports=%llu invalid=%llu tty_errors=%llu frames=%lu checksum_errors=%lu bad_pad_reports=%lu discarded=%lu\n",
			uart->bytes, uart->key_reports, uart->pad_reports,
			uart->invalid_reports, uart->tty_errors, uart->parser.frames,
			uart->parser.bad_checksums, uart->parser.bad_pad_reports,
			uart->parser.discarded);
		spin_unlock_irqrestore(&uart->lock, flags);
	}
	mutex_unlock(&w202_zsk_uart_mutex);
	return len;
}

/* N_DEVELOPMENT is explicitly reserved for manual out-of-tree testing. */
static struct tty_ldisc_ops w202_zsk_ldisc = {
	.owner = THIS_MODULE,
	.num = N_DEVELOPMENT,
	.name = "w202-zsk",
	.open = w202_zsk_ldisc_open,
	.close = w202_zsk_ldisc_close,
	.receive_buf = w202_zsk_receive,
	.flush_buffer = w202_zsk_flush,
	.hangup = w202_zsk_flush,
};
