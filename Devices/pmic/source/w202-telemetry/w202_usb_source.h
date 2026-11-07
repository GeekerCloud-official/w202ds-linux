/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_USB_SOURCE_H
#define W202_USB_SOURCE_H

/* Read-only UMP9620 register decoding. Units are uA. No PD contract implied.
 * CHARGE_STATUS[2]=VBUS, [11]=BC1.2 done, [7:5]=SDP/DCP/CDP.
 * TYPEC_STATUS[4:0]=state (2 attached sink), [7:6]=live source Rp.
 * TYPEC_EN[0]=enabled, TYPEC_DBG1[8]=VBUS_OK.
 */
static inline unsigned int w202_usb_source_current(unsigned int charge_status,
		unsigned int typec_enable, unsigned int typec_status, unsigned int typec_debug)
{
	unsigned int available = 500000;
	unsigned int bc_type = charge_status & 0xe0;

	if (!(charge_status & 0x04))
		return 0;
	if ((charge_status & 0x800) && (bc_type == 0x40 || bc_type == 0x20))
		available = 1500000;
	if ((typec_enable & 1) && (typec_status & 0x1f) == 2 && (typec_debug & 0x100)) {
		switch ((typec_status >> 6) & 3) {
		case 1:
			return 1500000;
		case 2:
			return 3000000;
		}
	}
	return available;
}

/* TCPM CURRENT_NOW is the negotiated/requested input budget, not measured
 * current. Non-PD must also respect the live Type-C/BC1.2 ceiling.
 */
static inline unsigned int w202_usb_sink_budget(unsigned int advertised,
		unsigned int negotiated, int pd_contract)
{
	if (negotiated > 3000000)
		negotiated = 3000000;
	return pd_contract || negotiated < advertised ? negotiated : advertised;
}

#endif
