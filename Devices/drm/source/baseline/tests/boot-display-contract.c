/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the same optional-request selector used by native DRM. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../w202_boot_text.h"

int main(void)
{
	const unsigned int good[20] = {
		1, 2, 0, 0xb0000000, 0x9e4000, 4608000, 1200, 1920, 2400, 1,
		1920, 1200, 120, 84, 1640, 960, 90, 180, 2, 1,
	};
	struct w202_boot_text text;
	unsigned int v[20], i, last;

	/* Normal boots and truncated/extended descriptors select normal fbdev. */
	assert(!w202_boot_text_select(&text, NULL, 0));
	assert(!w202_boot_text_select(&text, good, 19));
	assert(!w202_boot_text_select(&text, good, 21));
	assert(w202_boot_text_select(&text, good, 20));
	w202_boot_text_put(&text, 'X');
	assert(text.cells[0][0] == 'X');
	last = w202_boot_text_offset(&text, 2400, text.rows - 1, text.cols - 1, W202_TEXT_W - 1);
	assert(last + W202_TEXT_H * 2 <= 4608000);

	/* Reject incompatible ABI, addresses, formats and overflowing rectangles. */
	for (i = 0; i < 20; i++) {
		memcpy(v, good, sizeof(v));
		v[i] = ~0u;
		assert(!w202_boot_text_select(&text, v, 20));
	}
	memcpy(v, good, sizeof(v));
	v[14] = 0;
	assert(!w202_boot_text_select(&text, v, 20));
	v[10] = 1280; v[11] = 800;
	v[12] = 80; v[13] = 56; v[14] = 1093; v[15] = 640;
	assert(w202_boot_text_select(&text, v, 20));
	puts("PASS: absent, malformed, overflow, EFI and legacy LK display requests");
	return 0;
}
