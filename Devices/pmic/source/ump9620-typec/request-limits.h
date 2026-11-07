/* SPDX-License-Identifier: GPL-2.0 */
#ifndef W202_PD_REQUEST_LIMITS_H
#define W202_PD_REQUEST_LIMITS_H

/* Final board limit before a TCPM Request enters the FIFO. In this kernel
 * TCPM's PPS selector uses the source APDO without intersecting sink limits.
 * Decode USB PD wire units here; no protocol state machine or I/O.
 */
static inline int w202_pd_request_allowed(const unsigned int *caps,
		unsigned int count, unsigned int rdo, unsigned int max_mv,
		unsigned int max_ma, int allow_pps)
{
	unsigned int pos = (rdo >> 28) & 7;
	unsigned int pdo, mv, ma, maximum;

	if (!pos || pos > count || count > 7)
		return 0;
	pdo = caps[pos - 1];
	switch (pdo >> 30) {
	case 0: /* Fixed PDO: 50 mV / 10 mA; no give-back requests. */
		mv = ((pdo >> 10) & 0x3ff) * 50;
		ma = ((rdo >> 10) & 0x3ff) * 10;
		maximum = (rdo & 0x3ff) * 10;
		return !(rdo & (1U << 27)) && mv <= max_mv &&
			(mv == 5000 || mv == 9000 || mv == max_mv) &&
			ma && ma <= maximum && maximum <= max_ma &&
			mv * maximum <= 18000000 && /* Stock fixed-PD 18 W ceiling. */
			maximum <= (pdo & 0x3ff) * 10;
	case 3: /* PPS APDO: source 100 mV / 50 mA, RDO 20 mV / 50 mA. */
		mv = ((rdo >> 9) & 0x7ff) * 20;
		ma = (rdo & 0x7f) * 50;
		return allow_pps && !((pdo >> 28) & 3) &&
			mv >= 5000 && mv <= max_mv && ma && ma <= max_ma &&
			mv >= ((pdo >> 8) & 0xff) * 100 &&
			mv <= ((pdo >> 17) & 0xff) * 100 &&
			ma <= (pdo & 0x7f) * 50;
	default:
		return 0;
	}
}
#endif
