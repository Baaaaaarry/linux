// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Renesas R-Car I2C unit
 *
 * Copyright (C) 2014-19 Wolfram Sang <wsa@sang-engineering.com>
 * Copyright (C) 2011-2019 Renesas Electronics Corporation
 *
 * Copyright (C) 2012-14 Renesas Solutions Corp.
 * Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
 *
 * This file is based on the drivers/i2c/busses/i2c-sh7760.c
 * (c) 2005-2008 MSC Vertriebsges.m.b.H, Manuel Lauss <mlau@msc-ge.com>
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>

/* register offsets */
#define ICSCR	0x00	/* slave ctrl */
#define ICMCR	0x04	/* master ctrl */
#define ICSSR	0x08	/* slave status */
#define ICMSR	0x0C	/* master status */
#define ICSIER	0x10	/* slave irq enable */
#define ICMIER	0x14	/* master irq enable */
#define ICCCR	0x18	/* clock dividers */
#define ICSAR	0x1C	/* slave address */
#define ICMAR	0x20	/* master address */
#define ICRXTX	0x24	/* data port */
#define ICCCR2	0x28	/* Clock control 2 */
#define ICMPR	0x2C	/* SCL mask control */
#define ICHPR	0x30	/* SCL HIGH control */
#define ICLPR	0x34	/* SCL LOW control */
#define ICFBSCR	0x38	/* first bit setup cycle (Gen3) */
#define ICDMAER	0x3c	/* DMA enable (Gen3) */

/* ICSCR */
#define SDBS	BIT(3)	/* slave data buffer select */
#define SIE	BIT(2)	/* slave interface enable */
#define GCAE	BIT(1)	/* general call address enable */
#define FNA	BIT(0)	/* forced non acknowledgment */

/* ICMCR */
#define MDBS	BIT(7)	/* non-fifo mode switch */
#define FSCL	BIT(6)	/* override SCL pin */
#define FSDA	BIT(5)	/* override SDA pin */
#define OBPC	BIT(4)	/* override pins */
#define MIE	BIT(3)	/* master if enable */
#define TSBE	BIT(2)
#define FSB	BIT(1)	/* force stop bit */
#define ESG	BIT(0)	/* enable start bit gen */

/* ICSSR (also for ICSIER) */
#define GCAR	BIT(6)	/* general call received */
#define STM	BIT(5)	/* slave transmit mode */
#define SSR	BIT(4)	/* stop received */
#define SDE	BIT(3)	/* slave data empty */
#define SDT	BIT(2)	/* slave data transmitted */
#define SDR	BIT(1)	/* slave data received */
#define SAR	BIT(0)	/* slave addr received */

/* ICMSR (also for ICMIE) */
#define MNR	BIT(6)	/* nack received */
#define MAL	BIT(5)	/* arbitration lost */
#define MST	BIT(4)	/* sent a stop */
#define MDE	BIT(3)
#define MDT	BIT(2)
#define MDR	BIT(1)
#define MAT	BIT(0)	/* slave addr xfer done */

/* ICDMAER */
#define RSDMAE	BIT(3)	/* DMA Slave Received Enable */
#define TSDMAE	BIT(2)	/* DMA Slave Transmitted Enable */
#define RMDMAE	BIT(1)	/* DMA Master Received Enable */
#define TMDMAE	BIT(0)	/* DMA Master Transmitted Enable */

/* ICCCR2 */
#define FMPE	BIT(7)	/* Fast Mode Plus Enable */
#define CDFD	BIT(2)	/* CDF Disable */
#define HLSE	BIT(1)	/* HIGH/LOW Separate Control Enable */
#define SME	BIT(0)	/* SCL Mask Enable */

/* ICFBSCR */
#define TCYC17	0x0f		/* 17*Tcyc delay 1st bit between SDA and SCL */

#define RCAR_MIN_DMA_LEN	8

/* SCL low/high ratio 5:4 to meet all I2C timing specs (incl safety margin) */
#define RCAR_SCLD_RATIO		5
#define RCAR_SCHD_RATIO		4
/*
 * SMD should be smaller than SCLD/SCHD and is always around 20 in the docs.
 * Thus, we simply use 20 which works for low and high speeds.
 */
#define RCAR_DEFAULT_SMD	20

#define RCAR_BUS_PHASE_START	(MDBS | MIE | ESG)
#define RCAR_BUS_PHASE_DATA	(MDBS | MIE)
#define RCAR_BUS_PHASE_STOP	(MDBS | MIE | FSB)

#define RCAR_IRQ_SEND	(MNR | MAL | MST | MAT | MDE)
#define RCAR_IRQ_RECV	(MNR | MAL | MST | MAT | MDR)
#define RCAR_IRQ_STOP	(MST)

#define ID_LAST_MSG		BIT(0)
#define ID_REP_AFTER_RD		BIT(1)
#define ID_DONE			BIT(2)
#define ID_ARBLOST		BIT(3)
#define ID_NACK			BIT(4)
#define ID_EPROTO		BIT(5)
/* persistent flags */
#define ID_P_FMPLUS		BIT(27)
#define ID_P_NOT_ATOMIC		BIT(28)
#define ID_P_HOST_NOTIFY	BIT(29)
#define ID_P_NO_RXDMA		BIT(30) /* HW forbids RXDMA sometimes */
#define ID_P_PM_BLOCKED		BIT(31)
#define ID_P_MASK		GENMASK(31, 27)

#define ID_SLAVE_NACK		BIT(0)

enum rcar_i2c_type {
	I2C_RCAR_GEN1,
	I2C_RCAR_GEN2,
	I2C_RCAR_GEN3,
	I2C_RCAR_GEN4,
};

struct rcar_i2c_priv {
	u32 flags;
	void __iomem *io;
	struct i2c_adapter adap;
	struct i2c_msg *msg;
	int msgs_left;
	struct clk *clk;

	wait_queue_head_t wait;

	int pos;
	u32 icccr;
	u16 schd;
	u16 scld;
	u8 smd;
	u8 recovery_icmcr;	/* protected by adapter lock */
	enum rcar_i2c_type devtype;
	struct i2c_client *slave;

	struct resource *res;
	struct dma_chan *dma_tx;
	struct dma_chan *dma_rx;
	struct scatterlist sg;
	enum dma_data_direction dma_direction;

	struct reset_control *rstc;
	int irq;

	struct i2c_client *host_notify_client;
	u8 slave_flags;
};

#define rcar_i2c_priv_to_dev(p)		((p)->adap.dev.parent)
#define rcar_i2c_is_recv(p)		((p)->msg->flags & I2C_M_RD)

static void rcar_i2c_write(struct rcar_i2c_priv *priv, int reg, u32 val)
{
	writel(val, priv->io + reg);
}

static u32 rcar_i2c_read(struct rcar_i2c_priv *priv, int reg)
{
	return readl(priv->io + reg);
}

static void rcar_i2c_clear_irq(struct rcar_i2c_priv *priv, u32 val)
{
	writel(~val & 0x7f, priv->io + ICMSR);
}

static int rcar_i2c_get_scl(struct i2c_adapter *adap)
{
	struct rcar_i2c_priv *priv = i2c_get_adapdata(adap);

	return !!(rcar_i2c_read(priv, ICMCR) & FSCL);
}

static void rcar_i2c_set_scl(struct i2c_adapter *adap, int val)
{
	struct rcar_i2c_priv *priv = i2c_get_adapdata(adap);

	if (val)
		priv->recovery_icmcr |= FSCL;
	else
		priv->recovery_icmcr &= ~FSCL;

	rcar_i2c_write(priv, ICMCR, priv->recovery_icmcr);
}

static void rcar_i2c_set_sda(struct i2c_adapter *adap, int val)
{
	struct rcar_i2c_priv *priv = i2c_get_adapdata(adap);

	if (val)
		priv->recovery_icmcr |= FSDA;
	else
		priv->recovery_icmcr &= ~FSDA;

	rcar_i2c_write(priv, ICMCR, priv->recovery_icmcr);
}

static int rcar_i2c_get_bus_free(struct i2c_adapter *adap)
{
	struct rcar_i2c_priv *priv = i2c_get_adapdata(adap);

	return !(rcar_i2c_read(priv, ICMCR) & FSDA);
}

static struct i2c_bus_recovery_info rcar_i2c_bri = {
	.get_scl = rcar_i2c_get_scl,
	.set_scl = rcar_i2c_set_scl,
	.set_sda = rcar_i2c_set_sda,
	.get_bus_free = rcar_i2c_get_bus_free,
	.recover_bus = i2c_generic_scl_recovery,
};

static void rcar_i2c_init(struct rcar_i2c_priv *priv)
{
	/* reset master mode */
	rcar_i2c_write(priv, ICMIER, 0);
	rcar_i2c_write(priv, ICMCR, MDBS);
	rcar_i2c_write(priv, ICMSR, 0);
	/* start clock */
	if (priv->devtype < I2C_RCAR_GEN3) {
		rcar_i2c_write(priv, ICCCR, priv->icccr);
	} else {
		u32 icccr2 = CDFD | HLSE | SME;

		if (priv->flags & ID_P_FMPLUS)
			icccr2 |= FMPE;

		rcar_i2c_write(priv, ICCCR2, icccr2);
		rcar_i2c_write(priv, ICCCR, priv->icccr);
		rcar_i2c_write(priv, ICMPR, priv->smd);
		rcar_i2c_write(priv, ICHPR, priv->schd);
		rcar_i2c_write(priv, ICLPR, priv->scld);
		rcar_i2c_write(priv, ICFBSCR, TCYC17);
	}
}

static void rcar_i2c_reset_slave(struct rcar_i2c_priv *priv)
{
	rcar_i2c_write(priv, ICSIER, 0);
	rcar_i2c_write(priv, ICSSR, 0);
	rcar_i2c_write(priv, ICSCR, SDBS);
	rcar_i2c_write(priv, ICSAR, 0); /* Gen2: must be 0 if not using slave */
}

static int rcar_i2c_bus_barrier(struct rcar_i2c_priv *priv)
{
	int ret;
	u32 val;

	ret = readl_poll_timeout(priv->io + ICMCR, val, !(val & FSDA), 10,
				 priv->adap.timeout);
	if (ret) {
		/* Waiting did not help, try to recover */
		priv->recovery_icmcr = MDBS | OBPC | FSDA | FSCL;
		ret = i2c_recover_bus(&priv->adap);
	}

	return ret;
}

static int rcar_i2c_clock_calculate(struct rcar_i2c_priv *priv)
{
	u32 cdf, round, ick, sum, scl, cdf_width;
	unsigned long rate;
	struct device *dev = rcar_i2c_priv_to_dev(priv);
	struct i2c_timings t = {
		.bus_freq_hz		= I2C_MAX_STANDARD_MODE_FREQ,
		.scl_fall_ns		= 35,
		.scl_rise_ns		= 200,
		.scl_int_delay_ns	= 50,
	};

	/* Fall back to previously used values if not supplied */
	i2c_parse_fw_timings(dev, &t, false);
	priv->smd = RCAR_DEFAULT_SMD;

	/*
	 * calculate SCL clock
	 * see
	 *	ICCCR (and ICCCR2 for Gen3+)
	 *
	 * ick	= clkp / (1 + CDF)
	 * SCL	= ick / (20 + SCGD * 8 + F[(ticf + tr + intd) * ick])
	 *
	 * for Gen3+:
	 * SCL	= clkp / (8 + SMD * 2 + SCLD + SCHD +F[(ticf + tr + intd) * clkp])
	 *
	 * ick  : I2C internal clock < 20 MHz
	 * ticf : I2C SCL falling time
	 * tr   : I2C SCL rising  time
	 * intd : LSI internal delay
	 * clkp : peripheral_clk
	 * F[]  : integer up-valuation
	 */
	rate = clk_get_rate(priv->clk);
	cdf = rate / 20000000;
	cdf_width = (priv->devtype == I2C_RCAR_GEN1) ? 2 : 3;
	if (cdf >= 1U << cdf_width)
		goto err_no_val;

	if (t.bus_freq_hz > I2C_MAX_FAST_MODE_FREQ && priv->devtype >= I2C_RCAR_GEN4)
		priv->flags |= ID_P_FMPLUS;
	else
		priv->flags &= ~ID_P_FMPLUS;

	/* On Gen3+, we use cdf only for the filters, not as a SCL divider */
	ick = rate / (priv->devtype < I2C_RCAR_GEN3 ? (cdf + 1) : 1);

	/*
	 * It is impossible to calculate a large scale number on u32. Separate it.
	 *
	 * F[(ticf + tr + intd) * ick] with sum = (ticf + tr + intd)
	 *  = F[sum * ick / 1000000000]
	 *  = F[(ick / 1000000) * sum / 1000]
	 */
	sum = t.scl_fall_ns + t.scl_rise_ns + t.scl_int_delay_ns;
	round = DIV_ROUND_CLOSEST(ick, 1000000);
	round = DIV_ROUND_CLOSEST(round * sum, 1000);

	if (priv->devtype < I2C_RCAR_GEN3) {
		u32 scgd;
		/*
		 * SCL	= ick / (20 + 8 * SCGD + F[(ticf + tr + intd) * ick])
		 * 20 + 8 * SCGD + F[...] = ick / SCL
		 * SCGD = ((ick / SCL) - 20 - F[...]) / 8
		 * Result (= SCL) should be less than bus_speed for hardware safety
		 */
		scgd = DIV_ROUND_UP(ick, t.bus_freq_hz ?: 1);
		scgd = DIV_ROUND_UP(scgd - 20 - round, 8);
		scl = ick / (20 + 8 * scgd + round);

		if (scgd > 0x3f)
			goto err_no_val;

		dev_dbg(dev, "clk %u/%u(%lu), round %u, CDF: %u, SCGD: %u\n",
			scl, t.bus_freq_hz, rate, round, cdf, scgd);

		priv->icccr = scgd << cdf_width | cdf;
	} else {
		u32 x, sum_ratio = RCAR_SCHD_RATIO + RCAR_SCLD_RATIO;
		/*
		 * SCLD/SCHD ratio and SMD default value are explained above
		 * where they are defined. With these definitions, we can compute
		 * x as a base value for the SCLD/SCHD ratio:
		 *
		 * SCL = clkp / (8 + 2 * SMD + SCLD + SCHD + F[(ticf + tr + intd) * clkp])
		 * SCL = clkp / (8 + 2 * SMD + RCAR_SCLD_RATIO * x
		 *		 + RCAR_SCHD_RATIO * x + F[...])
		 *
		 * with: sum_ratio = RCAR_SCLD_RATIO + RCAR_SCHD_RATIO
		 *
		 * SCL = clkp / (8 + 2 * smd + sum_ratio * x + F[...])
		 * 8 + 2 * smd + sum_ratio * x + F[...] = clkp / SCL
		 * x = ((clkp / SCL) - 8 - 2 * smd - F[...]) / sum_ratio
		 */
		x = DIV_ROUND_UP(rate, t.bus_freq_hz ?: 1);
		x = DIV_ROUND_UP(x - 8 - 2 * priv->smd - round, sum_ratio);
		scl = rate / (8 + 2 * priv->smd + sum_ratio * x + round);

		if (x == 0 || x * RCAR_SCLD_RATIO > 0xffff)
			goto err_no_val;

		priv->icccr = cdf;
		priv->schd = RCAR_SCHD_RATIO * x;
		priv->scld = RCAR_SCLD_RATIO * x;
		if (priv->smd >= priv->schd)
			priv->smd = priv->schd - 1;

		dev_dbg(dev, "clk %u/%u(%lu), round %u, CDF: %u SCHD %u SCLD %u SMD %u\n",
			scl, t.bus_freq_hz, rate, round, cdf, priv->schd, priv->scld, priv->smd);
	}

	return 0;

err_no_val:
	dev_err(dev, "it is impossible to calculate best SCL\n");
	return -EINVAL;
}

/*
 * We don't have a test case but the HW engineers say that the write order of
 * ICMSR and ICMCR depends on whether we issue START or REP_START. So, ICMSR
 * handling is outside of this function. First messages clear ICMSR before this
 * function, interrupt handlers clear the relevant bits after this function.
 */
static void rcar_i2c_prepare_msg(struct rcar_i2c_priv *priv)
{
	int read = !!rcar_i2c_is_recv(priv);
	bool rep_start = !(priv->flags & ID_REP_AFTER_RD);

	priv->pos = 0;
	priv->flags &= ID_P_MASK;

	if (priv->msgs_left == 1)
		priv->flags |= ID_LAST_MSG;

	rcar_i2c_write(priv, ICMAR, i2c_8bit_addr_from_msg(priv->msg));
	if (priv->flags & ID_P_NOT_ATOMIC)
		rcar_i2c_write(priv, ICMIER, read ? RCAR_IRQ_RECV : RCAR_IRQ_SEND);

	if (rep_start)
		rcar_i2c_write(priv, ICMCR, RCAR_BUS_PHASE_START);
}

static void rcar_i2c_first_msg(struct rcar_i2c_priv *priv,
			       struct i2c_msg *msgs, int num)
{
	priv->msg = msgs;
	priv->msgs_left = num;
	rcar_i2c_write(priv, ICMSR, 0); /* must be before preparing msg */
	rcar_i2c_prepare_msg(priv);
}

static void rcar_i2c_next_msg(struct rcar_i2c_priv *priv)
{
	priv->msg++;
	priv->msgs_left--;
	rcar_i2c_prepare_msg(priv);
	/* ICMSR handling must come afterwards in the irq handler */
}

static void rcar_i2c_cleanup_dma(struct rcar_i2c_priv *priv, bool terminate)
{
	struct dma_chan *chan = priv->dma_direction == DMA_FROM_DEVICE
		? priv->dma_rx : priv->dma_tx;

	/* only allowed from thread context! */
	if (terminate)
		dmaengine_terminate_sync(chan);

	dma_unmap_single(chan->device->dev, sg_dma_address(&priv->sg),
			 sg_dma_len(&priv->sg), priv->dma_direction);

	/* Gen3+ can only do one RXDMA per transfer and we just completed it */
	if (priv->devtype >= I2C_RCAR_GEN3 &&
	    priv->dma_direction == DMA_FROM_DEVICE)
		priv->flags |= ID_P_NO_RXDMA;

	priv->dma_direction = DMA_NONE;

	/* Disable DMA Master Received/Transmitted, must be last! */
	rcar_i2c_write(priv, ICDMAER, 0);
}

static void rcar_i2c_dma_callback(void *data)
{
	struct rcar_i2c_priv *priv = data;

	priv->pos += sg_dma_len(&priv->sg);

	rcar_i2c_cleanup_dma(priv, false);
}

static bool rcar_i2c_dma(struct rcar_i2c_priv *priv)
{
	struct device *dev = rcar_i2c_priv_to_dev(priv);
	struct i2c_msg *msg = priv->msg;
	bool read = msg->flags & I2C_M_RD;
	enum dma_data_direction dir = read ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	struct dma_chan *chan = read ? priv->dma_rx : priv->dma_tx;
	struct dma_async_tx_descriptor *txdesc;
	dma_addr_t dma_addr;
	dma_cookie_t cookie;
	unsigned char *buf;
	int len;

	/* Do various checks to see if DMA is feasible at all */
	if (!(priv->flags & ID_P_NOT_ATOMIC) || IS_ERR(chan) || msg->len < RCAR_MIN_DMA_LEN ||
	    !(msg->flags & I2C_M_DMA_SAFE) || (read && priv->flags & ID_P_NO_RXDMA))
		return false;

	if (read) {
		/*
		 * The last two bytes needs to be fetched using PIO in
		 * order for the STOP phase to work.
		 */
		buf = priv->msg->buf;
		len = priv->msg->len - 2;
	} else {
		/*
		 * First byte in message was sent using PIO.
		 */
		buf = priv->msg->buf + 1;
		len = priv->msg->len - 1;
	}

	dma_addr = dma_map_single(chan->device->dev, buf, len, dir);
	if (dma_mapping_error(chan->device->dev, dma_addr)) {
		dev_dbg(dev, "dma map failed, using PIO\n");
		return false;
	}

	sg_dma_len(&priv->sg) = len;
	sg_dma_address(&priv->sg) = dma_addr;

	priv->dma_direction = dir;

	txdesc = dmaengine_prep_slave_sg(chan, &priv->sg, 1,
					 read ? DMA_DEV_TO_MEM : DMA_MEM_TO_DEV,
					 DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!txdesc) {
		dev_dbg(dev, "dma prep slave sg failed, using PIO\n");
		rcar_i2c_cleanup_dma(priv, false);
		return false;
	}

	txdesc->callback = rcar_i2c_dma_callback;
	txdesc->callback_param = priv;

	cookie = dmaengine_submit(txdesc);
	if (dma_submit_error(cookie)) {
		dev_dbg(dev, "submitting dma failed, using PIO\n");
		rcar_i2c_cleanup_dma(priv, false);
		return false;
	}

	/* Enable DMA Master Received/Transmitted */
	if (read)
		rcar_i2c_write(priv, ICDMAER, RMDMAE);
	else
		rcar_i2c_write(priv, ICDMAER, TMDMAE);

	dma_async_issue_pending(chan);
	return true;
}

static void rcar_i2c_irq_send(struct rcar_i2c_priv *priv, u32 msr)
{
	struct i2c_msg *msg = priv->msg;
	u32 irqs_to_clear = MDE;

	/* FIXME: sometimes, unknown interrupt happened. Do nothing */
	if (WARN(!(msr & MDE), "spurious irq"))
		return;

	if (msr & MAT)
		irqs_to_clear |= MAT;

	/* Check if DMA can be enabled and take over */
	if (priv->pos == 1 && rcar_i2c_dma(priv))
		return;

	if (priv->pos < msg->len) {
		/*
		 * Prepare next data to ICRXTX register.
		 * This data will go to _SHIFT_ register.
		 *
		 *    *
		 * [ICRXTX] -> [SHIFT] -> [I2C bus]
		 */
		rcar_i2c_write(priv, ICRXTX, msg->buf[priv->pos]);
		priv->pos++;
	} else {
		/*
		 * The last data was pushed to ICRXTX on _PREV_ empty irq.
		 * It is on _SHIFT_ register, and will sent to I2C bus.
		 *
		 *		  *
		 * [ICRXTX] -> [SHIFT] -> [I2C bus]
		 */

		if (priv->flags & ID_LAST_MSG)
			/*
			 * If current msg is the _LAST_ msg,
			 * prepare stop condition here.
			 * ID_DONE will be set on STOP irq.
			 */
			rcar_i2c_write(priv, ICMCR, RCAR_BUS_PHASE_STOP);
		else
			rcar_i2c_next_msg(priv);
	}

	rcar_i2c_clear_irq(priv, irqs_to_clear);
}

static void rcar_i2c_irq_recv(struct rcar_i2c_priv *priv, u32 msr)
{
	struct i2c_msg *msg = priv->msg;
	bool recv_len_init = priv->pos == 0 && msg->flags & I2C_M_RECV_LEN;
	u32 irqs_to_clear = MDR;

	/* FIXME: sometimes, unknown interrupt happened. Do nothing */
	if (!(msr & MDR))
		return;

	if (msr & MAT) {
		irqs_to_clear |= MAT;
		/*
		 * Address transfer phase finished, but no data at this point.
		 * Try to use DMA to receive data.
		 */
		rcar_i2c_dma(priv);
	} else if (priv->pos < msg->len) {
		/* get received data */
		u8 data = rcar_i2c_read(priv, ICRXTX);

		msg->buf[priv->pos] = data;
		if (recv_len_init) {
			if (data == 0 || data > I2C_SMBUS_BLOCK_MAX) {
				priv->flags |= ID_DONE | ID_EPROTO;
				return;
			}
			msg->len += msg->buf[0];
			/* Enough data for DMA? */
			if (rcar_i2c_dma(priv))
				return;
			/* new length after RECV_LEN now properly initialized */
			recv_len_init = false;
		}
		priv->pos++;
	}

	/*
	 * If next received data is the _LAST_ and we are not waiting for a new
	 * length because of RECV_LEN, then go to a new phase.
	 */
	if (priv->pos + 1 == msg->len && !recv_len_init) {
		if (priv->flags & ID_LAST_MSG) {
			rcar_i2c_write(priv, ICMCR, RCAR_BUS_PHASE_STOP);
		} else {
			rcar_i2c_write(priv, ICMCR, RCAR_BUS_PHASE_START);
			priv->flags |= ID_REP_AFTER_RD;
		}
	}

	if (priv->pos == msg->len && !(priv->flags & ID_LAST_MSG))
		rcar_i2c_next_msg(priv);

	rcar_i2c_clear_irq(priv, irqs_to_clear);
}

static bool rcar_i2c_slave_irq(struct rcar_i2c_priv *priv)
{
	u32 ssr_raw, ssr_filtered;
	u8 value;
	int ret;

	ssr_raw = rcar_i2c_read(priv, ICSSR) & 0xff;
	ssr_filtered = ssr_raw & rcar_i2c_read(priv, ICSIER);

	if (!ssr_filtered)
		return false;

	/* address detected */
	if (ssr_filtered & SAR) {
		/* read or write request */
		if (ssr_raw & STM) {
			i2c_slave_event(priv->slave, I2C_SLAVE_READ_REQUESTED, &value);
			rcar_i2c_write(priv, ICRXTX, value);
			rcar_i2c_write(priv, ICSIER, SDE | SSR | SAR);
		} else {
			ret = i2c_slave_event(priv->slave, I2C_SLAVE_WRITE_REQUESTED, &value);
			if (ret)
				priv->slave_flags |= ID_SLAVE_NACK;

			rcar_i2c_read(priv, ICRXTX);	/* dummy read */
			rcar_i2c_write(priv, ICSIER, SDR | SSR | SAR);
		}

		/* Clear SSR, too, because of old STOPs to other clients than us */
		rcar_i2c_write(priv, ICSSR, ~(SAR | SSR) & 0xff);
	}

	/* master sent stop */
	if (ssr_filtered & SSR) {
		i2c_slave_event(priv->slave, I2C_SLAVE_STOP, &value);
		rcar_i2c_write(priv, ICSCR, SIE | SDBS); /* clear our NACK */
		priv->slave_flags &= ~ID_SLAVE_NACK;
		rcar_i2c_write(priv, ICSIER, SAR);
		rcar_i2c_write(priv, ICSSR, ~SSR & 0xff);
	}

	/* master wants to write to us */
	if (ssr_filtered & SDR) {
		value = rcar_i2c_read(priv, ICRXTX);
		ret = i2c_slave_event(priv->slave, I2C_SLAVE_WRITE_RECEIVED, &value);
		if (ret)
			priv->slave_flags |= ID_SLAVE_NACK;

		/* Send NACK in case of error, but it will come 1 byte late :( */
		rcar_i2c_write(priv, ICSCR, SIE | SDBS |
			       (priv->slave_flags & ID_SLAVE_NACK ? FNA : 0));
		rcar_i2c_write(priv, ICSSR, ~SDR & 0xff);
	}

	/* master wants to read from us */
	if (ssr_filtered & SDE) {
		i2c_slave_event(priv->slave, I2C_SLAVE_READ_PROCESSED, &value);
		rcar_i2c_write(priv, ICRXTX, value);
		rcar_i2c_write(priv, ICSSR, ~SDE & 0xff);
	}

	return true;
}

/*
 * This driver has a lock-free design because there are IP cores (at least
 * R-Car Gen2) which have an inherent race condition in their hardware design.
 * There, we need to switch to RCAR_BUS_PHASE_DATA as soon as possible after
 * the interrupt was generated, otherwise an unwanted repeated message gets
 * generated. It turned out that taking a spinlock at the beginning of the ISR
 * was already causing repeated messages. Thus, this driver was converted to
 * the now lockless behaviour. Please keep this in mind when hacking the driver.
 * R-Car Gen3 seems to have this fixed but earlier versions than R-Car Gen2 are
 * likely affected. Therefore, we have different interrupt handler entries.
 */
static irqreturn_t rcar_i2c_irq(int irq, struct rcar_i2c_priv *priv, u32 msr)
{
	if (!msr) {
		if (rcar_i2c_slave_irq(priv))
			return IRQ_HANDLED;

		return IRQ_NONE;
	}

	/* Arbitration lost */
	if (msr & MAL) {
		priv->flags |= ID_DONE | ID_ARBLOST;
		goto out;
	}

	/* Nack */
	if (msr & MNR) {
		/* HW automatically sends STOP after received NACK */
		if (priv->flags & ID_P_NOT_ATOMIC)
			rcar_i2c_write(priv, ICMIER, RCAR_IRQ_STOP);
		priv->flags |= ID_NACK;
		goto out;
	}

	/* Stop */
	if (msr & MST) {
		priv->msgs_left--; /* The last message also made it */
		priv->flags |= ID_DONE;
		goto out;
	}

	if (rcar_i2c_is_recv(priv))
		rcar_i2c_irq_recv(priv, msr);
	else
		rcar_i2c_irq_send(priv, msr);

out:
	if (priv->flags & ID_DONE) {
		rcar_i2c_write(priv, ICMIER, 0);
		rcar_i2c_write(priv, ICMSR, 0);
		if (priv->flags & ID_P_NOT_ATOMIC)
			wake_up(&priv->wait);
	}

	return IRQ_HANDLED;
}

static irqreturn_t rcar_i2c_gen2_irq(int irq, void *ptr)
{
	struct rcar_i2c_priv *priv = ptr;
	u32 msr;

	/* Clear START or STOP immediately, except for REPSTART after read */
	if (likely(!(priv->flags & ID_REP_AFTER_RD)))
		rcar_i2c_write(priv, ICMCR, RCAR_BUS_PHASE_DATA);

	/* Only handle interrupts that are currently enabled */
	msr = rcar_i2c_read(priv, ICMSR);
	if (priv->flags & ID_P_NOT_ATOMIC)
		msr &= rcar_i2c_read(priv, ICMIER);

	return rcar_i2c_irq(irq, priv, msr);
}

static irqreturn_t rcar_i2c_gen3_irq(int irq, void *ptr)
{
	struct rcar_i2c_priv *priv = ptr;
	u32 msr;

	/* Only handle interrupts that are currently enabled */
	msr = rcar_i2c_read(priv, ICMSR);
	if (priv->flags & ID_P_NOT_ATOMIC)
		msr &= rcar_i2c_read(priv, ICMIER);

	/*
	 * Clear START or STOP immediately, except for REPSTART after read or
	 * if a spurious interrupt was detected.
	 */
	if (likely(!(priv->flags & ID_REP_AFTER_RD) && msr))
		rcar_i2c_write(priv, ICMCR, RCAR_BUS_PHASE_DATA);

	return rcar_i2c_irq(irq, priv, msr);
}

static struct dma_chan *rcar_i2c_request_dma_chan(struct device *dev,
					enum dma_transfer_direction dir,
					dma_addr_t port_addr)
{
	struct dma_chan *chan;
	struct dma_slave_config cfg;
	char *chan_name = dir == DMA_MEM_TO_DEV ? "tx" : "rx";
	int ret;

	chan = dma_request_chan(dev, chan_name);
	if (IS_ERR(chan)) {
		dev_dbg(dev, "request_channel failed for %s (%ld)\n",
			chan_name, PTR_ERR(chan));
		return chan;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.direction = dir;
	if (dir == DMA_MEM_TO_DEV) {
		cfg.dst_addr = port_addr;
		cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	} else {
		cfg.src_addr = port_addr;
		cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	}

	ret = dmaengine_slave_config(chan, &cfg);
	if (ret) {
		dev_dbg(dev, "slave_config failed for %s (%d)\n",
			chan_name, ret);
		dma_release_channel(chan);
		return ERR_PTR(ret);
	}

	dev_dbg(dev, "got DMA channel for %s\n", chan_name);
	return chan;
}

static void rcar_i2c_request_dma(struct rcar_i2c_priv *priv,
				 struct i2c_msg *msg)
{
	struct device *dev = rcar_i2c_priv_to_dev(priv);
	bool read;
	struct dma_chan *chan;
	enum dma_transfer_direction dir;

	read = msg->flags & I2C_M_RD;

	chan = read ? priv->dma_rx : priv->dma_tx;
	if (PTR_ERR(chan) != -EPROBE_DEFER)
		return;

	dir = read ? DMA_DEV_TO_MEM : DMA_MEM_TO_DEV;
	chan = rcar_i2c_request_dma_chan(dev, dir, priv->res->start + ICRXTX);

	if (read)
		priv->dma_rx = chan;
	else
		priv->dma_tx = chan;
}

static void rcar_i2c_release_dma(struct rcar_i2c_priv *priv)
{
	if (!IS_ERR(priv->dma_tx)) {
		dma_release_channel(priv->dma_tx);
		priv->dma_tx = ERR_PTR(-EPROBE_DEFER);
	}

	if (!IS_ERR(priv->dma_rx)) {
		dma_release_channel(priv->dma_rx);
		priv->dma_rx = ERR_PTR(-EPROBE_DEFER);
	}
}

/* I2C is a special case, we need to poll the status of a reset */
static int rcar_i2c_do_reset(struct rcar_i2c_priv *priv)
{
	int ret;

	/* Don't reset if a slave instance is currently running */
	if (priv->slave)
		return -EISCONN;

	ret = reset_control_reset(priv->rstc);
	if (ret)
		return ret;

	return read_poll_timeout_atomic(reset_control_status, ret, ret == 0, 1,
					100, false, priv->rstc);
}

static int rcar_i2c_master_xfer(struct i2c_adapter *adap,
				struct i2c_msg *msgs,
				int num)
{
	struct rcar_i2c_priv *priv = i2c_get_adapdata(adap);
	struct device *dev = rcar_i2c_priv_to_dev(priv);
	int i, ret;
	long time_left;

	priv->flags |= ID_P_NOT_ATOMIC;

	pm_runtime_get_sync(dev);

	/* Check bus state before init otherwise bus busy info will be lost */
	ret = rcar_i2c_bus_barrier(priv);
	if (ret < 0)
		goto out;

	/* Gen3+ needs a reset. That also allows RXDMA once */
	if (priv->devtype >= I2C_RCAR_GEN3) {
		ret = rcar_i2c_do_reset(priv);
		if (ret)
			goto out;
		priv->flags &= ~ID_P_NO_RXDMA;
	}

	rcar_i2c_init(priv);

	for (i = 0; i < num; i++)
		rcar_i2c_request_dma(priv, msgs + i);

	rcar_i2c_first_msg(priv, msgs, num);

	time_left = wait_event_timeout(priv->wait, priv->flags & ID_DONE,
				     num * adap->timeout);

	/* cleanup DMA if it couldn't complete properly due to an error */
	if (priv->dma_direction != DMA_NONE)
		rcar_i2c_cleanup_dma(priv, true);

	if (!time_left) {
		rcar_i2c_init(priv);
		ret = -ETIMEDOUT;
	} else if (priv->flags & ID_NACK) {
		ret = -ENXIO;
	} else if (priv->flags & ID_ARBLOST) {
		ret = -EAGAIN;
	} else if (priv->flags & ID_EPROTO) {
		ret = -EPROTO;
	} else {
		ret = num - priv->msgs_left; /* The number of transfer */
	}
out:
	pm_runtime_put(dev);

	if (ret < 0 && ret != -ENXIO)
		dev_err(dev, "error %d : %x\n", ret, priv->flags);

	return ret;
}

static int rcar_i2c_master_xfer_atomic(struct i2c_adapter *adap,
				struct i2c_msg *msgs,
				int num)
{
	struct rcar_i2c_priv *priv = i2c_get_adapdata(adap);
	struct device *dev = rcar_i2c_priv_to_dev(priv);
	unsigned long j;
	bool time_left;
	int ret;

	priv->flags &= ~ID_P_NOT_ATOMIC;

	pm_runtime_get_sync(dev);

	/* Check bus state before init otherwise bus busy info will be lost */
	ret = rcar_i2c_bus_barrier(priv);
	if (ret < 0)
		goto out;

	rcar_i2c_init(priv);
	rcar_i2c_first_msg(priv, msgs, num);

	j = jiffies + num * adap->timeout;
	do {
		u32 msr = rcar_i2c_read(priv, ICMSR);

		msr &= (rcar_i2c_is_recv(priv) ? RCAR_IRQ_RECV : RCAR_IRQ_SEND) | RCAR_IRQ_STOP;

		if (msr) {
			if (priv->devtype < I2C_RCAR_GEN3)
				rcar_i2c_gen2_irq(0, priv);
			else
				rcar_i2c_gen3_irq(0, priv);
		}

		time_left = time_before_eq(jiffies, j);
	} while (!(priv->flags & ID_DONE) && time_left);

	if (!time_left) {
		rcar_i2c_init(priv);
		ret = -ETIMEDOUT;
	} else if (priv->flags & ID_NACK) {
		ret = -ENXIO;
	} else if (priv->flags & ID_ARBLOST) {
		ret = -EAGAIN;
	} else if (priv->flags & ID_EPROTO) {
		ret = -EPROTO;
	} else {
		ret = num - priv->msgs_left; /* The number of transfer */
	}
out:
	pm_runtime_put(dev);

	if (ret < 0 && ret != -ENXIO)
		dev_err(dev, "error %d : %x\n", ret, priv->flags);

	return ret;
}

static int rcar_reg_slave(struct i2c_client *slave)
{
	struct rcar_i2c_priv *priv = i2c_get_adapdata(slave->adapter);

	if (priv->slave)
		return -EBUSY;

	if (slave->flags & I2C_CLIENT_TEN)
		return -EAFNOSUPPORT;

	/* Keep device active for slave address detection logic */
	pm_runtime_get_sync(rcar_i2c_priv_to_dev(priv));

	priv->slave = slave;
	rcar_i2c_write(priv, ICSAR, slave->addr);
	rcar_i2c_write(priv, ICSSR, 0);
	rcar_i2c_write(priv, ICSIER, SAR);
	rcar_i2c_write(priv, ICSCR, SIE | SDBS);

	return 0;
}

static int rcar_unreg_slave(struct i2c_client *slave)
{
	struct rcar_i2c_priv *priv = i2c_get_adapdata(slave->adapter);

	WARN_ON(!priv->slave);

	/* ensure no irq is running before clearing ptr */
	disable_irq(priv->irq);
	rcar_i2c_reset_slave(priv);
	enable_irq(priv->irq);

	priv->slave = NULL;

	pm_runtime_put(rcar_i2c_priv_to_dev(priv));

	return 0;
}

static u32 rcar_i2c_func(struct i2c_adapter *adap)
{
	struct rcar_i2c_priv *priv = i2c_get_adapdata(adap);

	/*
	 * This HW can't do:
	 * I2C_SMBUS_QUICK (setting FSB during START didn't work)
	 * I2C_M_NOSTART (automatically sends address after START)
	 * I2C_M_IGNORE_NAK (automatically sends STOP after NAK)
	 */
	u32 func = I2C_FUNC_I2C | I2C_FUNC_SLAVE |
		   (I2C_FUNC_SMBUS_EMUL_ALL & ~I2C_FUNC_SMBUS_QUICK);

	if (priv->flags & ID_P_HOST_NOTIFY)
		func |= I2C_FUNC_SMBUS_HOST_NOTIFY;

	return func;
}

static const struct i2c_algorithm rcar_i2c_algo = {
	.xfer = rcar_i2c_master_xfer,
	.xfer_atomic = rcar_i2c_master_xfer_atomic,
	.functionality = rcar_i2c_func,
	.reg_slave = rcar_reg_slave,
	.unreg_slave = rcar_unreg_slave,
};

static const struct i2c_adapter_quirks rcar_i2c_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN,
};

static const struct of_device_id rcar_i2c_dt_ids[] = {
	{ .compatible = "renesas,i2c-r8a7778", .data = (void *)I2C_RCAR_GEN1 },
	{ .compatible = "renesas,i2c-r8a7779", .data = (void *)I2C_RCAR_GEN1 },
	{ .compatible = "renesas,i2c-r8a7790", .data = (void *)I2C_RCAR_GEN2 },
	{ .compatible = "renesas,i2c-r8a7791", .data = (void *)I2C_RCAR_GEN2 },
	{ .compatible = "renesas,i2c-r8a7792", .data = (void *)I2C_RCAR_GEN2 },
	{ .compatible = "renesas,i2c-r8a7793", .data = (void *)I2C_RCAR_GEN2 },
	{ .compatible = "renesas,i2c-r8a7794", .data = (void *)I2C_RCAR_GEN2 },
	{ .compatible = "renesas,i2c-r8a7795", .data = (void *)I2C_RCAR_GEN3 },
	{ .compatible = "renesas,i2c-r8a7796", .data = (void *)I2C_RCAR_GEN3 },
	/* S4 has no FM+ bit */
	{ .compatible = "renesas,i2c-r8a779f0", .data = (void *)I2C_RCAR_GEN3 },
	{ .compatible = "renesas,rcar-gen1-i2c", .data = (void *)I2C_RCAR_GEN1 },
	{ .compatible = "renesas,rcar-gen2-i2c", .data = (void *)I2C_RCAR_GEN2 },
	{ .compatible = "renesas,rcar-gen3-i2c", .data = (void *)I2C_RCAR_GEN3 },
	{ .compatible = "renesas,rcar-gen4-i2c", .data = (void *)I2C_RCAR_GEN4 },
	{},
};
MODULE_DEVICE_TABLE(of, rcar_i2c_dt_ids);

static int rcar_i2c_probe(struct platform_device *pdev)
{
	struct rcar_i2c_priv *priv;
	struct i2c_adapter *adap;
	struct device *dev = &pdev->dev;
	unsigned long irqflags = 0;
	irqreturn_t (*irqhandler)(int irq, void *ptr) = rcar_i2c_gen3_irq;
	int ret;

	/* Otherwise logic will break because some bytes must always use PIO */
	BUILD_BUG_ON_MSG(RCAR_MIN_DMA_LEN < 3, "Invalid min DMA length");

	priv = devm_kzalloc(dev, sizeof(struct rcar_i2c_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(dev, "cannot get clock\n");
		return PTR_ERR(priv->clk);
	}

	priv->io = devm_platform_get_and_ioremap_resource(pdev, 0, &priv->res);
	if (IS_ERR(priv->io))
		return PTR_ERR(priv->io);

	priv->devtype = (enum rcar_i2c_type)of_device_get_match_data(dev);
	init_waitqueue_head(&priv->wait);

	adap = &priv->adap;
	adap->nr = pdev->id;
	adap->algo = &rcar_i2c_algo;
	adap->class = I2C_CLASS_DEPRECATED;
	adap->retries = 3;
	adap->dev.parent = dev;
	adap->dev.of_node = dev->of_node;
	adap->bus_recovery_info = &rcar_i2c_bri;
	adap->quirks = &rcar_i2c_quirks;
	i2c_set_adapdata(adap, priv);
	strscpy(adap->name, pdev->name, sizeof(adap->name));

	/* Init DMA */
	sg_init_table(&priv->sg, 1);
	priv->dma_direction = DMA_NONE;
	priv->dma_rx = priv->dma_tx = ERR_PTR(-EPROBE_DEFER);

	/* Activate device for clock calculation */
	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);
	ret = rcar_i2c_clock_calculate(priv);
	if (ret < 0) {
		pm_runtime_put(dev);
		goto out_pm_disable;
	}

	/* Bring hardware to known state */
	rcar_i2c_init(priv);
	rcar_i2c_reset_slave(priv);

	/* Stay always active when multi-master to keep arbitration working */
	if (of_property_read_bool(dev->of_node, "multi-master"))
		priv->flags |= ID_P_PM_BLOCKED;
	else
		pm_runtime_put(dev);

	if (of_property_read_bool(dev->of_node, "smbus"))
		priv->flags |= ID_P_HOST_NOTIFY;

	if (priv->devtype < I2C_RCAR_GEN3) {
		irqflags |= IRQF_NO_THREAD;
		irqhandler = rcar_i2c_gen2_irq;
	} else {
		/* R-Car Gen3+ needs a reset before every transfer */
		priv->rstc = devm_reset_control_get_exclusive(&pdev->dev, NULL);
		if (IS_ERR(priv->rstc)) {
			ret = PTR_ERR(priv->rstc);
			goto out_pm_put;
		}

		ret = reset_control_status(priv->rstc);
		if (ret < 0)
			goto out_pm_put;

		/* hard reset disturbs HostNotify local target, so disable it */
		priv->flags &= ~ID_P_HOST_NOTIFY;
	}

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		goto out_pm_put;
	priv->irq = ret;
	ret = devm_request_irq(dev, priv->irq, irqhandler, irqflags, dev_name(dev), priv);
	if (ret < 0) {
		dev_err(dev, "cannot get irq %d\n", priv->irq);
		goto out_pm_put;
	}

	platform_set_drvdata(pdev, priv);

	ret = i2c_add_numbered_adapter(adap);
	if (ret < 0)
		goto out_pm_put;

	if (priv->flags & ID_P_HOST_NOTIFY) {
		priv->host_notify_client = i2c_new_slave_host_notify_device(adap);
		if (IS_ERR(priv->host_notify_client)) {
			ret = PTR_ERR(priv->host_notify_client);
			goto out_del_device;
		}
	}

	dev_info(dev, "probed\n");

	return 0;

 out_del_device:
	i2c_del_adapter(&priv->adap);
 out_pm_put:
	if (priv->flags & ID_P_PM_BLOCKED)
		pm_runtime_put(dev);
 out_pm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static void rcar_i2c_remove(struct platform_device *pdev)
{
	struct rcar_i2c_priv *priv = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	if (priv->host_notify_client)
		i2c_free_slave_host_notify_device(priv->host_notify_client);
	i2c_del_adapter(&priv->adap);
	rcar_i2c_release_dma(priv);
	if (priv->flags & ID_P_PM_BLOCKED)
		pm_runtime_put(dev);
	pm_runtime_disable(dev);
}

static int rcar_i2c_suspend(struct device *dev)
{
	struct rcar_i2c_priv *priv = dev_get_drvdata(dev);

	i2c_mark_adapter_suspended(&priv->adap);
	return 0;
}

static int rcar_i2c_resume(struct device *dev)
{
	struct rcar_i2c_priv *priv = dev_get_drvdata(dev);

	i2c_mark_adapter_resumed(&priv->adap);
	return 0;
}

static const struct dev_pm_ops rcar_i2c_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(rcar_i2c_suspend, rcar_i2c_resume)
};

static struct platform_driver rcar_i2c_driver = {
	.driver	= {
		.name	= "i2c-rcar",
		.of_match_table = rcar_i2c_dt_ids,
		.pm	= pm_sleep_ptr(&rcar_i2c_pm_ops),
	},
	.probe		= rcar_i2c_probe,
	.remove		= rcar_i2c_remove,
};

module_platform_driver(rcar_i2c_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Renesas R-Car I2C bus driver");
MODULE_AUTHOR("Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>");
