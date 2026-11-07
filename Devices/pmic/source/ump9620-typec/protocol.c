// SPDX-License-Identifier: GPL-2.0
/* FIFO and interrupt adapter; PD policy belongs entirely to Linux TCPM.
 * Register sequences derived from Spreadtrum sc27xx_pd.c (2019).
 */
#include "controller.h"
#include "request-limits.h"

/* Exhausted recovery or I/O failure stops PD, preserving Rd and CC IRQs.
 * Caller holds pd->lock; only an actual CC detach can rearm this latch.
 */
void w202_pd_latch_fault(struct w202_typec *pd, const char *reason)
{
	int ret, mask_ret;

	lockdep_assert_held(&pd->lock);
	if (pd->faulted)
		return;
	pd->faulted = true;
	pd->fault_reason = reason;
	pd->last_fault_reason = reason;
	pd->fault_count++;
	pd->tx_pending = false;
	ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CFG0,
				SC27XX_PD_CTL_EN, 0);
	mask_ret = regmap_write(pd->regmap, pd->base + SC27XX_INT_EN, 0);
	dev_warn(pd->dev, "PD stopped: %s; irq=%llu rx=%llu tx=%llu complete=%llu flags=%04x disable=%d/%d; waiting for CC detach\n",
		 reason, pd->irq_count, pd->rx_count, pd->tx_count,
		 pd->tx_complete_count, pd->last_irq_flags, ret, mask_ret);
}

int w202_pd_flush(struct w202_typec *pd, bool reset_ids)
{
	u32 bits = SC27XX_PD_RX_FLASH | SC27XX_PD_TX_FLASH | SC27XX_PD_RX_ID_CLR;
	int ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CTRL, bits, bits);

	if (!ret && reset_ids)
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_MESG_ID_CFG,
					SC27XX_PD_MESS_ID_MASK, 0);
	return ret;
}

int w202_pd_set_rx(struct tcpc_dev *tcpc, bool on)
{
	struct w202_typec *pd = tcpc_to_w202(tcpc);
	int ret;

	mutex_lock(&pd->lock);
	if (pd->faulted) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (!on)
		pd->tx_pending = false;
	ret = w202_pd_flush(pd, false);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_INT_EN,
					SC27XX_PD_PKG_RV_EN, on ? SC27XX_PD_PKG_RV_EN : 0);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CFG1,
					SC27XX_PD_RX_AUTO_GOOD_CRC,
					on ? SC27XX_PD_RX_AUTO_GOOD_CRC : 0);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CFG0,
					SC27XX_PD_CTL_EN, on ? SC27XX_PD_CTL_EN : 0);
out:
	mutex_unlock(&pd->lock);
	return ret;
}

int w202_pd_transmit(struct tcpc_dev *tcpc, enum tcpm_transmit_type type,
		     const struct pd_message *msg, unsigned int negotiated_rev)
{
	struct w202_typec *pd = tcpc_to_w202(tcpc);
	u32 payload;
	int i, count = 0, ret;

	if (type != TCPC_TX_SOP && type != TCPC_TX_HARD_RESET)
		return -EOPNOTSUPP;
	if (type == TCPC_TX_SOP && !msg)
		return -EINVAL;
	mutex_lock(&pd->lock);
	if (pd->faulted) {
		ret = -ESHUTDOWN;
		goto out;
	}
	/* TCPM serializes sends. A Hard Reset after its TX timeout must be
	 * allowed to flush that old packet, even if its completion IRQ was lost.
	 */
	if (pd->tx_pending && type != TCPC_TX_HARD_RESET) {
		w202_pd_latch_fault(pd, "previous-tx-incomplete");
		ret = -ESHUTDOWN;
		goto out;
	}
	if (msg && !(le16_to_cpu(msg->header) & PD_HEADER_EXT_HDR) &&
	    pd_header_cnt_le(msg->header) &&
	    pd_header_type_le(msg->header) == PD_DATA_REQUEST &&
	    (pd_header_cnt_le(msg->header) != 1 ||
	     !w202_pd_request_allowed(pd->source_caps, pd->nr_source_caps,
		le32_to_cpu(msg->payload[0]), pd->max_mv, pd->max_ma, pd->allow_pps))) {
		pd->blocked_requests++;
		dev_warn_ratelimited(pd->dev, "PD Request exceeds prepared board limits\n");
		ret = -ERANGE;
		goto out;
	}
	ret = regmap_write(pd->regmap, pd->base + SC27XX_INT_CLR,
		SC27XX_PD_TX_OK_CLR | SC27XX_PD_TX_ERROR_CLR | SC27XX_PD_TX_COLLSION_CLR);
	if (!ret)
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CFG1,
			SC27XX_PD_RETRY_MASK, SC27XX_PD_RETRY(negotiated_rev >= PD_REV30 ? 1 : 3));
	if (ret)
		goto out;
	if (type == TCPC_TX_HARD_RESET) {
		ret = w202_pd_begin_reset(pd);
		pd->tx_pending = false;
	} else {
		count = pd_header_cnt_le(msg->header);
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CTRL,
			SC27XX_PD_TX_FLASH, SC27XX_PD_TX_FLASH);
		if (!ret)
			ret = regmap_write(pd->regmap, pd->base + SC27XX_PD_HEAD_CFG,
				le16_to_cpu(msg->header));
		for (i = 0; !ret && i < count; i++) {
			payload = le32_to_cpu(msg->payload[i]);
			ret = regmap_write(pd->regmap, pd->base + SC27XX_PD_TX_BUF, payload & 0xffff);
			if (!ret)
				ret = regmap_write(pd->regmap, pd->base + SC27XX_PD_TX_BUF, payload >> 16);
		}
	}
	if (ret)
		goto out;
	pd->tx_hard_reset = type == TCPC_TX_HARD_RESET;
	pd->tx_id = msg ? pd_header_msgid_le(msg->header) : 0;
	pd->tx_pending = true;
	ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CTRL,
		pd->tx_hard_reset ? SC27XX_PD_HARD_RESET : SC27XX_PD_TX_START,
		pd->tx_hard_reset ? SC27XX_PD_HARD_RESET : SC27XX_PD_TX_START);
	if (ret)
		pd->tx_pending = false;
	else
		pd->tx_count++;
	dev_dbg(pd->dev, "TX type=%u header=%04x ret=%d\n", type,
		msg ? le16_to_cpu(msg->header) : 0, ret);
out:
	mutex_unlock(&pd->lock);
	return ret;
}

static int receive(struct w202_typec *pd, struct pd_message *msg)
{
	u32 header, lo, hi, status;
	int i, count, ret;

	ret = regmap_read_poll_timeout(pd->regmap, pd->base + SC27XX_PD_STS1,
		status, !(status & SC27XX_PD_RX_EMPTY), 50, 2000);
	if (ret)
		return ret;
	ret = regmap_read(pd->regmap, pd->base + SC27XX_PD_RX_BUF, &header);
	if (ret)
		return ret;
	msg->header = cpu_to_le16(header & 0xffff);
	count = pd_header_cnt_le(msg->header);
	for (i = 0; i < count; i++) {
		ret = regmap_read(pd->regmap, pd->base + SC27XX_PD_RX_BUF, &lo);
		if (!ret)
			ret = regmap_read(pd->regmap, pd->base + SC27XX_PD_RX_BUF, &hi);
		if (ret)
			return ret;
		msg->payload[i] = cpu_to_le32((lo & 0xffff) | (hi << 16));
	}
	if (!(header & PD_HEADER_EXT_HDR) && count &&
	    pd_header_type_le(msg->header) == PD_DATA_SOURCE_CAP) {
		for (i = 0; i < count; i++)
			pd->source_caps[i] = le32_to_cpu(msg->payload[i]);
		pd->nr_source_caps = count;
	}
	dev_dbg(pd->dev, "RX header=%04x fifo=%04x\n", header, status);
	pd->rx_count++;
	/* Keep the subsequent PS_RDY, matching the controller's stock sequence. */
	if (!count && pd_header_type(header) == PD_CTRL_ACCEPT)
		return 0;
	return regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CTRL,
		SC27XX_PD_RX_FLASH, SC27XX_PD_RX_FLASH);
}

irqreturn_t w202_pd_irq(int irq, void *data)
{
	struct w202_typec *pd = data;
	struct pd_message msg = {};
	enum tcpm_transmit_status tx_status = TCPC_TX_SUCCESS;
	u32 flags;
	bool rx = false, complete = false, hard_reset = false;
	int ret;

	mutex_lock(&pd->lock);
	pd->irq_count++;
	ret = regmap_read(pd->regmap, pd->base + SC27XX_INT_FLG, &flags);
	if (ret)
		goto error;
	flags &= SC27XX_INT_CLR_MASK;
	pd->last_irq_flags = flags;
	ret = regmap_write(pd->regmap, pd->base + SC27XX_INT_CLR, flags);
	if (ret)
		goto error;
	if (!READ_ONCE(pd->ready) || pd->faulted)
		goto out;
	dev_dbg(pd->dev, "IRQ flags=%04x\n", flags);
	if (flags & SC27XX_PD_HARD_RST_FLAG) {
		complete = pd->tx_pending;
		pd->tx_pending = false;
		tx_status = TCPC_TX_DISCARDED;
		ret = w202_pd_begin_reset(pd);
		hard_reset = !ret;
		goto out;
	}
	if (flags & SC27XX_PD_PKG_RV_FLAG) {
		ret = receive(pd, &msg);
		if (ret)
			goto error;
		if (!pd_header_cnt_le(msg.header) && pd_header_type_le(msg.header) == PD_CTRL_GOOD_CRC) {
			/* Some revisions deliver GoodCRC as well as TX_OK. Complete
			 * once, only for the outstanding message with the same ID.
			 */
			complete = pd->tx_pending && !pd->tx_hard_reset &&
				pd_header_msgid_le(msg.header) == pd->tx_id;
		} else {
			rx = true;
			if (!pd_header_cnt_le(msg.header) &&
			    pd_header_type_le(msg.header) == PD_CTRL_PS_RDY) {
				ret = regmap_update_bits(pd->regmap,
					pd->typec_base + SC27XX_TYPEC_PD_CFG,
					SC27XX_TYPEC_PD_CONSTRACT, SC27XX_TYPEC_PD_CONSTRACT);
				if (ret)
					goto error;
				w202_pd_contract_established(pd);
			}
		}
	}
	if (pd->tx_pending && (flags & (SC27XX_PD_TX_OK_FLAG |
		SC27XX_PD_TX_ERROR_FLAG | SC27XX_PD_TX_COLLSION_FLAG))) {
		complete = true;
		if (flags & (SC27XX_PD_TX_ERROR_FLAG | SC27XX_PD_TX_COLLSION_FLAG))
			tx_status = TCPC_TX_FAILED;
	}
	if (complete) {
		pd->tx_pending = false;
		if (tx_status != TCPC_TX_SUCCESS)
			pd->error_count++;
	}
	if (flags & SC27XX_PD_RX_FIFO_OVERFLOW_FLAG) {
		ret = regmap_update_bits(pd->regmap, pd->base + SC27XX_PD_CTRL,
			SC27XX_PD_RX_FLASH, SC27XX_PD_RX_FLASH);
		pd->error_count++;
	}
	goto out;
error:
	pd->error_count++;
	rx = false;
	complete = pd->tx_pending;
	tx_status = TCPC_TX_FAILED;
	w202_pd_latch_fault(pd, "irq-error");
	dev_err_ratelimited(pd->dev, "PD IRQ error %d\n", ret);
out:
	if (complete)
		pd->tx_complete_count++;
	mutex_unlock(&pd->lock);
	if (complete)
		tcpm_pd_transmit_complete(pd->port, tx_status);
	if (rx)
		tcpm_pd_receive(pd->port, &msg, TCPC_TX_SOP);
	if (hard_reset)
		tcpm_pd_hard_reset(pd->port);
	return IRQ_HANDLED;
}
