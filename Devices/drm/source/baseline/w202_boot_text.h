/* SPDX-License-Identifier: GPL-2.0-only */
/* Text layout for the DRM client's bounded, rotated RGB565 boot window. */
#ifndef W202_BOOT_TEXT_H
#define W202_BOOT_TEXT_H

#define W202_TEXT_COLS 160
#define W202_TEXT_ROWS 50
#define W202_TEXT_W 12
#define W202_TEXT_H 24

struct w202_boot_text {
	unsigned int left, top, cols, rows, col, row;
	unsigned long long dirty;
	unsigned char cells[W202_TEXT_ROWS][W202_TEXT_COLS];
};

static int w202_boot_text_init(struct w202_boot_text *t, const unsigned int v[20])
{
	unsigned int right, bottom, x, y;

	if (v[0] != 1 || v[1] != 2 || v[2] || v[3] != 0xb0000000 ||
	    v[4] != 0x9e4000 || v[5] != 4608000 || v[6] != 1200 ||
	    v[7] != 1920 || v[8] != 2400 || v[9] != 1 ||
	    !((v[10] == 1920 && v[11] == 1200) ||
	      (v[10] == 1280 && v[11] == 800)) ||
	    v[12] > v[10] || v[13] > v[11] ||
	    v[14] > v[10] - v[12] || v[15] > v[11] - v[13] ||
	    v[14] < 128 || v[15] < 80 || v[16] != 90 ||
	    v[17] != 180 || v[18] != 2 || v[19] != 1)
		return -1;
	t->left = (v[12] * 1920 + v[10] - 1) / v[10] + 8;
	t->top = (v[13] * 1200 + v[11] - 1) / v[11] + 8;
	right = (v[12] + v[14]) * 1920 / v[10] - 8;
	bottom = (v[13] + v[15]) * 1200 / v[11] - 8;
	t->cols = (right - t->left) / W202_TEXT_W;
	t->rows = (bottom - t->top) / W202_TEXT_H;
	if (!t->cols || t->cols > W202_TEXT_COLS || !t->rows || t->rows > W202_TEXT_ROWS)
		return -1;
	t->col = t->row = 0;
	t->dirty = (1ULL << t->rows) - 1;
	for (y = 0; y < t->rows; y++)
		for (x = 0; x < t->cols; x++)
			t->cells[y][x] = ' ';
	return 0;
}

/* This is an optional display request, never a prerequisite for normal KMS.
 * Callers decode OF cells first; an absent/malformed request selects fbdev.
 */
static int w202_boot_text_select(struct w202_boot_text *t,
				 const unsigned int *v, unsigned int count)
{
	return v && count == 20 && !w202_boot_text_init(t, v);
}

static void w202_boot_text_newline(struct w202_boot_text *t)
{
	unsigned int x, y;

	t->col = 0;
	if (++t->row < t->rows)
		return;
	t->row = t->rows - 1;
	for (y = 1; y < t->rows; y++)
		for (x = 0; x < t->cols; x++)
			t->cells[y - 1][x] = t->cells[y][x];
	for (x = 0; x < t->cols; x++)
		t->cells[t->row][x] = ' ';
	t->dirty = (1ULL << t->rows) - 1;
}

static void w202_boot_text_put(struct w202_boot_text *t, unsigned char c)
{
	if (c == '\r') {
		t->col = 0;
	} else if (c == '\n') {
		w202_boot_text_newline(t);
	} else if (c == '\t') {
		unsigned int n = 4 - t->col % 4;

		while (n--)
			w202_boot_text_put(t, ' ');
	} else if (c >= 32) {
		if (t->col == t->cols)
			w202_boot_text_newline(t);
		t->cells[t->row][t->col++] = c < 127 ? c : '?';
		t->dirty |= 1ULL << t->row;
	}
}

/* Physical byte offset of one pre-rotated glyph column. Pitch is the actual
 * DRM allocation pitch, which need not equal the inherited framebuffer pitch.
 */
static unsigned int w202_boot_text_offset(const struct w202_boot_text *t,
		unsigned int pitch, unsigned int row, unsigned int col, unsigned int x)
{
	return (t->left + col * W202_TEXT_W + x) * pitch +
		(1200 - t->top - (row + 1) * W202_TEXT_H) * 2;
}
#endif
