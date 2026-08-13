/*
 * akmci-core.c - Anyka AK39 MCI host controller, request engine
 *
 * Copyright (C) 2010 Anyka, Ltd, All Rights Reserved.
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 *
 * Request sequencing, error handling and completion discipline follow
 * drivers/mmc/host/mmci.c (ARM PrimeCell PL180), from which this controller's
 * programming model is descended. The register vocabulary is Anyka's own and
 * the transfer path is the SoC's shared L2 buffer pool, not a PL180 FIFO.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mmc/host.h>
#include <linux/ratelimit.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/timer.h>

#include <asm/cacheflush.h>

#include "akmci.h"

#define BOTH_DIR	(MMC_DATA_WRITE | MMC_DATA_READ)

static const char *const akmci_xfer_desc[] = {
	[AKMCI_XFER_DMA] = "l2dma",
	[AKMCI_XFER_PIO] = "l2pio",
};

static const char *const akmci_cd_desc[] = {
	[AKMCI_CD_ALWAYS] = "always present",
	[AKMCI_CD_GPIO]   = "gpio",
	[AKMCI_CD_HOOK]   = "board hook",
};

static void akmci_enable_imask(struct akmci_host *host, u32 imask)
{
	u32 newmask = readl(host->base + MCI_MASK_REG);

	writel(newmask | imask, host->base + MCI_MASK_REG);
}

static void akmci_disable_imask(struct akmci_host *host, u32 imask)
{
	u32 newmask = readl(host->base + MCI_MASK_REG);

	writel(newmask & ~imask, host->base + MCI_MASK_REG);
}

static void akmci_clear_imask(struct akmci_host *host)
{
	u32 mask = readl(host->base + MCI_MASK_REG);

	/* The SDIO interrupt is not part of a request and must survive. */
	writel(mask & MCI_SDIOINTMASK, host->base + MCI_MASK_REG);
}

/*
 * Take one L2 common buffer. The pool is shared with SPI, USB and audio, so
 * acquisition is bounded: a busy pool fails the request rather than parking
 * the block queue in an uninterruptible sleep.
 */
static int akmci_l2_acquire(struct akmci_host *host)
{
	unsigned int waited = 0;
	u8 id = AKMCI_L2BUF_NONE;
	int ret;

	for (;;) {
		ret = host->l2->buf_get(host, &id);
		if (ret == 0 && id != AKMCI_L2BUF_NONE) {
			host->l2buf_id = id;
			host->l2_dma_active = 0;
			return 0;
		}

		if (waited >= MCI_L2_ACQUIRE_US)
			break;

		if (in_interrupt() || irqs_disabled())
			udelay(MCI_L2_ACQUIRE_STEP_US);
		else
			usleep_range(MCI_L2_ACQUIRE_STEP_US,
				     2 * MCI_L2_ACQUIRE_STEP_US);
		waited += MCI_L2_ACQUIRE_STEP_US;
	}

	akmci_err_ratelimited(host->dev, "no L2 buffer available\n");
	return -EIO;
}

static void akmci_l2_release(struct akmci_host *host)
{
	if (host->l2buf_id == AKMCI_L2BUF_NONE)
		return;

	host->l2->buf_put(host);
	host->l2buf_id = AKMCI_L2BUF_NONE;
	host->l2_dma_active = 0;
}

static int akmci_l2_wait(struct akmci_host *host)
{
	int ret;

	if (!host->l2_dma_active)
		return 0;

	ret = host->l2->wait(host);
	host->l2_dma_active = 0;

	return ret;
}

/*
 * Hand the next chunk to the L2 buffer. One chunk is at most one block, which
 * is what the controller consumes between DATABLOCKEND interrupts.
 */
static int akmci_l2xfer(struct akmci_host *host)
{
	struct mmc_data *data = host->data;
	unsigned int sg_remain;
	unsigned int xferlen;
	int to_card;
	int ret;

	if (data == NULL || host->sg_ptr == NULL || host->size == 0)
		return -EIO;

	to_card = !!(data->flags & MMC_DATA_WRITE);

	while (host->sg_off >= host->sg_ptr->length) {
		if (host->sg_len <= 1)
			return -EIO;

		host->sg_ptr = sg_next(host->sg_ptr);
		if (host->sg_ptr == NULL)
			return -EIO;

		host->sg_len--;
		host->sg_off = 0;
	}

	sg_remain = host->sg_ptr->length - host->sg_off;
	xferlen = min3(sg_remain, (unsigned int)data->blksz, host->size);
	if (xferlen == 0)
		return -EIO;

	if (host->dma_mapped && xferlen >= MCI_L2_DMA_ALIGN) {
		ret = host->l2->xfer_dma(host,
					 sg_dma_address(host->sg_ptr) + host->sg_off,
					 xferlen, to_card);
		if (ret == 0)
			host->l2_dma_active = 1;
	} else {
		ret = host->l2->xfer_cpu(host,
					 sg_virt(host->sg_ptr) + host->sg_off,
					 xferlen, to_card);
	}

	if (ret)
		return ret;

	host->sg_off += xferlen;
	host->data_xfered += xferlen;
	host->size -= xferlen;

	return 0;
}

static void akmci_stop_data(struct akmci_host *host)
{
	struct mmc_data *data = host->data;

	writel(0, host->base + MCI_DMACTRL_REG);
	writel(0, host->base + MCI_DATACTRL_REG);

	akmci_disable_imask(host, MCI_DATAIRQMASKS | MCI_FIFOFULLMASK |
				  MCI_FIFOEMPTYMASK);

	if (data != NULL && host->dma_mapped) {
		enum dma_data_direction ddir = (data->flags & MMC_DATA_WRITE) ?
				DMA_TO_DEVICE : DMA_FROM_DEVICE;

		dma_sync_sg_for_cpu(host->dev, data->sg, data->sg_len, ddir);
		dma_unmap_sg(host->dev, data->sg, data->sg_len, ddir);
	}
	host->dma_mapped = 0;

	host->sg_ptr = NULL;
	host->sg_len = 0;
	host->sg_off = 0;

	host->data = NULL;
}

/*
 * Complete a request exactly once. Everything the request owns - the data
 * phase, the L2 buffer, the watchdog, the controller state - is released here
 * whatever went wrong, and the byte count handed back is all or nothing.
 */
static void akmci_request_end(struct akmci_host *host, struct mmc_request *mrq)
{
	if (WARN_ON_ONCE(mrq == NULL || host->mrq != mrq))
		return;

	writel(0, host->base + MCI_COMMAND_REG);

	del_timer(&host->req_timer);

	if (host->data != NULL) {
		if (host->data->error == 0)
			host->data->error = -EIO;
		akmci_stop_data(host);
	}

	host->mrq = NULL;
	host->cmd = NULL;

	if (host->data_err_flag > 0) {
		host->l2->reset(host);

		writel(MCI_ENABLE | MCI_FAIL_TRIGGER, host->base + MCI_CLK_REG);
		writel(readl(host->base + MCI_CLK_REG) | host->clkreg,
		       host->base + MCI_CLK_REG);
		mdelay(10);
	}

	akmci_l2_release(host);

	if (mrq->data) {
		unsigned int len = mrq->data->blksz * mrq->data->blocks;

		if (mrq->data->error == 0 && host->data_xfered != len)
			mrq->data->error = -EIO;

		if (mrq->data->error || mrq->cmd->error)
			mrq->data->bytes_xfered = 0;
		else
			mrq->data->bytes_xfered = len;
	}

	/* mmc_request_done() may re-enter the driver, so drop the lock. */
	spin_unlock(&host->lock);
	mmc_request_done(host->mmc, mrq);
	spin_lock(&host->lock);
}

/*
 * Complete a request that never reached the controller. host->mrq was never
 * published, so a stray interrupt cannot race with this.
 */
static void akmci_fail_request(struct akmci_host *host,
			       struct mmc_request *mrq, int err)
{
	if (mrq->cmd)
		mrq->cmd->error = err;
	if (mrq->data) {
		mrq->data->error = err;
		mrq->data->bytes_xfered = 0;
	}
	if (mrq->stop)
		mrq->stop->error = err;

	mmc_request_done(host->mmc, mrq);
}

static int akmci_start_data(struct akmci_host *host, struct mmc_data *data)
{
	unsigned int datactrl, dmacon;
	unsigned int size;

	if ((data->flags & BOTH_DIR) == BOTH_DIR ||
	    (data->flags & BOTH_DIR) == 0) {
		akmci_err_ratelimited(host->dev, "bad data direction 0x%08x\n",
				    data->flags);
		return -EINVAL;
	}

	size = data->blksz * data->blocks;
	if (data->blksz == 0 || data->blocks == 0 ||
	    data->blksz > MAX_MCI_BLOCK_SIZE ||
	    size > MAX_MCI_REQ_SIZE ||
	    data->sg == NULL || data->sg_len == 0) {
		akmci_err_ratelimited(host->dev,
				    "bad data shape blksz=%u blocks=%u sg_len=%u\n",
				    data->blksz, data->blocks, data->sg_len);
		return -EINVAL;
	}

	/*
	 * MCI_STATUS is read-to-clear. Draining it here discards anything
	 * latched by the command phase, so the first status word the data
	 * phase sees describes the data phase.
	 */
	if (readl(host->base + MCI_STATUS_REG) & MCI_SDIOINT)
		mmc_signal_sdio_irq(host->mmc);

	host->data = data;
	host->size = size;
	host->data_xfered = 0;

	host->sg_len = data->sg_len;
	host->sg_ptr = data->sg;
	host->sg_off = 0;

	/*
	 * The L2 engine only moves whole 64-byte shots, so short control
	 * transfers go through the CPU path and are never mapped for DMA.
	 */
	if (host->cfg.xfer_kind == AKMCI_XFER_DMA &&
	    data->blksz >= MCI_L2_DMA_ALIGN) {
		enum dma_data_direction ddir = (data->flags & MMC_DATA_WRITE) ?
				DMA_TO_DEVICE : DMA_FROM_DEVICE;

		if (dma_map_sg(host->dev, data->sg, data->sg_len, ddir) == 0) {
			akmci_err_ratelimited(host->dev,
					    "failed to map the request sg list\n");
			host->data = NULL;
			return -EIO;
		}
		host->dma_mapped = 1;
	}

	writel(TRANS_DATA_TIMEOUT, host->base + MCI_DATATIMER_REG);
	writel(host->size, host->base + MCI_DATALENGTH_REG);

	dmacon = MCI_DMA_BUFEN | MCI_DMA_SIZE(MCI_L2FIFO_SIZE / 4);
	if (host->cfg.xfer_kind == AKMCI_XFER_DMA)
		dmacon |= MCI_DMA_EN;
	writel(dmacon, host->base + MCI_DMACTRL_REG);

	akmci_enable_imask(host, MCI_DATAIRQMASKS);

	datactrl = MCI_DPSM_ENABLE;

	switch (host->bus_width) {
	case MMC_BUS_WIDTH_8:
		datactrl |= MCI_DPSM_BUSMODE(2);
		break;
	case MMC_BUS_WIDTH_4:
		datactrl |= MCI_DPSM_BUSMODE(1);
		break;
	case MMC_BUS_WIDTH_1:
	default:
		datactrl |= MCI_DPSM_BUSMODE(0);
		break;
	}

	if (data->flags & MMC_DATA_STREAM)
		datactrl |= MCI_DPSM_STREAM;
	else
		datactrl |= MCI_DPSM_BLOCKSIZE(data->blksz);

	if (data->flags & MMC_DATA_READ)
		datactrl |= MCI_DPSM_DIRECTION;

	writel(datactrl, host->base + MCI_DATACTRL_REG);

	if (data->flags & MMC_DATA_WRITE) {
		if (akmci_l2xfer(host)) {
			akmci_stop_data(host);
			return -EIO;
		}
	}

	return 0;
}

static void akmci_start_command(struct akmci_host *host,
				struct mmc_command *cmd)
{
	unsigned int ccon;

	writel(cmd->arg, host->base + MCI_ARGUMENT_REG);
	akmci_enable_imask(host, MCI_CMDIRQMASKS);

	ccon = MCI_CPSM_CMD(cmd->opcode) | MCI_CPSM_ENABLE;
	if (cmd->flags & MMC_RSP_PRESENT) {
		ccon |= MCI_CPSM_RESPONSE;
		if (cmd->flags & MMC_RSP_136)
			ccon |= MCI_CPSM_LONGRSP;
	}

	if (cmd->data)
		ccon |= MCI_CPSM_WITHDATA;

	host->cmd = cmd;

	writel(ccon, host->base + MCI_COMMAND_REG);
}

static void akmci_report_data_err(struct akmci_host *host,
				  struct mmc_data *data, const char *err)
{
	akmci_err_ratelimited(host->dev, "data %s: %s, %u of %u bytes left\n",
			    (data->flags & MMC_DATA_READ) ? "read" : "write",
			    err, host->size, data->blksz * data->blocks);
}

static u8 akmci_probe_l2(struct akmci_host *host)
{
	if (host->l2->buf_status == NULL ||
	    host->l2buf_id == AKMCI_L2BUF_NONE)
		return 0;

	return host->l2->buf_status(host);
}

int akmci_probe_format(struct akmci_host *host, char *buf, size_t len)
{
	struct akmci_probe *p = &host->probe;

	return snprintf(buf, len,
			"blockend %u\nblockend_active %u\n"
			"dataend %u\ndataend_nofinish %u\n"
			"cnt_midblock %u\n"
			"wr_dirty %u\nwr_dirty_max %u\n"
			"rd_end_dirty %u\nrd_end_max %u\n",
			p->blockend, p->blockend_active,
			p->dataend, p->dataend_nofinish,
			p->cnt_midblock,
			p->wr_dirty, p->wr_dirty_max,
			p->rd_end_dirty, p->rd_end_max);
}

static void akmci_data_irq(struct akmci_host *host, struct mmc_data *data,
			   unsigned int status)
{
	const unsigned int hw = status;

	if (status & MCI_DATABLOCKEND) {
		u32 cnt = readl(host->base + MCI_DATACNT_REG);

		host->probe.blockend++;
		if (hw & (MCI_TXACTIVE | MCI_RXACTIVE))
			host->probe.blockend_active++;
		if (data->blksz && (cnt % data->blksz) != 0)
			host->probe.cnt_midblock++;

		if (akmci_l2_wait(host)) {
			data->error = -EIO;
			akmci_report_data_err(host, data, "l2 transfer stalled");
			status |= MCI_DATAEND;
			host->data_err_flag = 1;
		} else {
			if (data->flags & MMC_DATA_WRITE) {
				u8 occ = akmci_probe_l2(host);

				if (occ) {
					host->probe.wr_dirty++;
					if (occ > host->probe.wr_dirty_max)
						host->probe.wr_dirty_max = occ;
				}
			}

			if (data->flags & MMC_DATA_WRITE)
				host->l2->clr_status(host);

			if (host->size > 0 && akmci_l2xfer(host)) {
				data->error = -EIO;
				akmci_report_data_err(host, data,
						      "scatterlist exhausted");
				status |= MCI_DATAEND;
				host->data_err_flag = 1;
			}
		}
	}

	if (status & (MCI_DATACRCFAIL | MCI_DATATIMEOUT | MCI_STARTBIT_ERR)) {
		if (status & MCI_DATACRCFAIL) {
			data->error = -EILSEQ;
			akmci_report_data_err(host, data, "crc failure");
		} else if (status & MCI_DATATIMEOUT) {
			data->error = -ETIMEDOUT;
			akmci_report_data_err(host, data, "transfer timeout");
		} else {
			data->error = -EIO;
			akmci_report_data_err(host, data, "start bit error");
		}

		status |= MCI_DATAEND;
		host->data_err_flag = 1;

		if (host->sg_ptr && (data->flags & MMC_DATA_READ))
			flush_dcache_page(sg_page(host->sg_ptr));
	}

	if (status & MCI_DATAEND) {
		if (hw & MCI_DATAEND) {
			host->probe.dataend++;
			if (!(hw & MCI_DATATRANS_FINISH))
				host->probe.dataend_nofinish++;
		}

		if (!data->error && akmci_l2_wait(host)) {
			data->error = -EIO;
			host->data_err_flag = 1;
		}

		if (!data->error && (data->flags & MMC_DATA_READ)) {
			u8 occ = akmci_probe_l2(host);

			if (occ) {
				host->probe.rd_end_dirty++;
				if (occ > host->probe.rd_end_max)
					host->probe.rd_end_max = occ;
			}
		}

		if (!data->error)
			host->data_err_flag = 0;

		akmci_stop_data(host);

		if (!data->stop)
			akmci_request_end(host, data->mrq);
		else
			akmci_start_command(host, data->stop);
	}
}

static void akmci_cmd_irq(struct akmci_host *host, struct mmc_command *cmd,
			  unsigned int status)
{
	host->cmd = NULL;

	cmd->resp[0] = readl(host->base + MCI_RESPONSE0_REG);
	cmd->resp[1] = readl(host->base + MCI_RESPONSE1_REG);
	cmd->resp[2] = readl(host->base + MCI_RESPONSE2_REG);
	cmd->resp[3] = readl(host->base + MCI_RESPONSE3_REG);

	if (status & MCI_RESPTIMEOUT)
		cmd->error = -ETIMEDOUT;
	else if ((status & MCI_RESPCRCFAIL) && (cmd->flags & MMC_RSP_CRC))
		cmd->error = -EILSEQ;

	akmci_disable_imask(host, MCI_CMDIRQMASKS);

	if (!cmd->data || cmd->error) {
		if (host->data != NULL) {
			if (host->data->error == 0)
				host->data->error = cmd->error ? : -EIO;
			host->data_err_flag = 1;
			akmci_stop_data(host);
		}
		akmci_request_end(host, cmd->mrq);
	} else if (!(cmd->data->flags & MMC_DATA_READ)) {
		int ret = akmci_start_data(host, cmd->data);

		if (ret) {
			cmd->data->error = ret;
			host->data_err_flag = 1;
			akmci_request_end(host, cmd->mrq);
		}
	}
}

/*
 * MCI_STATUS is cleared by being read, so one read is the only report the
 * driver ever gets for the events it names. Serving both phases from that one
 * word is what allowed a DATAEND latched before the data phase existed to
 * complete a request while its pages were still being transferred; the data
 * bits are therefore dropped whenever they cannot belong to the live data
 * phase.
 */
irqreturn_t akmci_irq(int irq, void *dev_id)
{
	struct akmci_host *host = dev_id;
	u32 status;

	spin_lock(&host->lock);

	status = readl(host->base + MCI_STATUS_REG);

	if (status & MCI_SDIOINT) {
		mmc_signal_sdio_irq(host->mmc);
		status |= readl(host->base + MCI_STATUS_REG);
	}

	if (host->data == NULL)
		status &= ~MCI_DATAIRQSTATUS;

	if ((status & MCI_CMDIRQSTATUS) && host->cmd) {
		struct mmc_data *data = host->data;

		akmci_cmd_irq(host, host->cmd, status);

		if (host->data != data)
			status &= ~MCI_DATAIRQSTATUS;
	}

	if ((status & MCI_DATAIRQSTATUS) && host->data)
		akmci_data_irq(host, host->data, status);

	spin_unlock(&host->lock);

	return IRQ_HANDLED;
}

/*
 * Recover a request the controller stopped reporting progress for. Without
 * this a lost interrupt or an L2 peer that never releases the bus parks the
 * block queue for ever.
 */
static void akmci_req_timeout(akmci_timer_arg_t arg)
{
	struct akmci_host *host = akmci_timer_host(arg, req_timer);
	struct mmc_request *mrq;
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);

	mrq = host->mrq;
	if (mrq != NULL) {
		akmci_err_ratelimited(host->dev,
				    "request timed out (cmd %u, %u bytes left)\n",
				    mrq->cmd ? mrq->cmd->opcode : 0, host->size);

		akmci_clear_imask(host);

		if (host->data != NULL && host->data->error == 0)
			host->data->error = -ETIMEDOUT;
		if (host->cmd != NULL && host->cmd->error == 0)
			host->cmd->error = -ETIMEDOUT;
		if (mrq->cmd != NULL && mrq->cmd->error == 0)
			mrq->cmd->error = -ETIMEDOUT;

		host->data_err_flag = 1;
		akmci_request_end(host, mrq);
	}

	spin_unlock_irqrestore(&host->lock, flags);
}

static int akmci_card_present(struct mmc_host *mmc)
{
	struct akmci_host *host = mmc_priv(mmc);

	switch (host->cfg.cd_kind) {
	case AKMCI_CD_GPIO:
		if (host->cfg.gpio_cd < 0 || host->cfg.gpio_get == NULL)
			return 1;
		return host->cfg.gpio_get(host->cfg.gpio_cd) == 0;
	case AKMCI_CD_HOOK:
		if (host->cfg.card_present == NULL)
			return 1;
		return host->cfg.card_present(host);
	case AKMCI_CD_ALWAYS:
	default:
		return 1;
	}
}

static void akmci_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct akmci_host *host = mmc_priv(mmc);
	struct mmc_data *data;
	unsigned long flags;
	int ret;

	if (akmci_card_present(mmc) == 0) {
		dev_dbg(host->dev, "no medium present\n");
		akmci_fail_request(host, mrq, -ENOMEDIUM);
		return;
	}

	host->data_err_flag = 0;
	host->data_xfered = 0;

	data = mrq->data ? mrq->data : (mrq->cmd ? mrq->cmd->data : NULL);
	if (data != NULL) {
		ret = akmci_l2_acquire(host);
		if (ret) {
			akmci_fail_request(host, mrq, ret);
			return;
		}
	}

	spin_lock_irqsave(&host->lock, flags);

	host->mrq = mrq;
	mod_timer(&host->req_timer, jiffies + MCI_REQ_TIMEOUT);

	ret = 0;
	if (mrq->data && (mrq->data->flags & MMC_DATA_READ))
		ret = akmci_start_data(host, mrq->data);

	if (ret) {
		mrq->cmd->error = ret;
		mrq->data->error = ret;
		host->data_err_flag = 1;
		akmci_request_end(host, mrq);
	} else {
		akmci_start_command(host, mrq->cmd);
	}

	spin_unlock_irqrestore(&host->lock, flags);
}

/*
 * The bus clock is asic_clk / ((DIVH + 1) * (DIVL + 1)). The vendor splits the
 * requested divisor additively, which under-clocks the bus; the split is kept
 * because it is what this board has been characterised at, and only the
 * out-of-range cases are clamped.
 */
static void akmci_set_clk(struct akmci_host *host, struct mmc_ios *ios)
{
	u32 clk, div;
	u32 clk_div_h, clk_div_l;

	if (ios->clock == 0) {
		clk = readl(host->base + MCI_CLK_REG);
		clk &= ~MCI_CLK_ENABLE;
		writel(clk, host->base + MCI_CLK_REG);

		host->bus_clock = 0;
		host->clkreg = clk;
		return;
	}

	clk = readl(host->base + MCI_CLK_REG);
	clk |= MCI_CLK_ENABLE;
	clk &= ~MCI_CLK_DIV_MASK;

	div = host->asic_clk / ios->clock;
	if (host->asic_clk % ios->clock)
		div += 1;

	if (div < 2)
		div = 2;
	div -= 2;

	clk_div_h = div / 2;
	clk_div_l = div - clk_div_h;

	if (clk_div_h > 255)
		clk_div_h = 255;
	if (clk_div_l > 255)
		clk_div_l = 255;

	clk |= MMC_CLK_DIVL(clk_div_l) | MMC_CLK_DIVH(clk_div_h);
	writel(clk, host->base + MCI_CLK_REG);

	host->bus_clock = host->asic_clk / ((clk_div_h + 1) * (clk_div_l + 1));
	host->clkreg = clk;
}

static void akmci_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct akmci_host *host = mmc_priv(mmc);

	host->bus_mode = ios->bus_mode;
	host->bus_width = ios->bus_width;

	if (ios->clock != host->bus_clock)
		akmci_set_clk(host, ios);
}

static int akmci_get_ro(struct mmc_host *mmc)
{
	struct akmci_host *host = mmc_priv(mmc);

	if (host->cfg.gpio_wp < 0 || host->cfg.gpio_get == NULL)
		return 0;

	return host->cfg.gpio_get(host->cfg.gpio_wp) == 0;
}

static void akmci_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct akmci_host *host = mmc_priv(mmc);
	unsigned long flags;
	unsigned int reg1, reg2;

	if (host->cfg.dev_kind != AKMCI_DEV_SDIO)
		return;

	spin_lock_irqsave(&host->lock, flags);

	reg1 = readl(host->base + MCI_MASK_REG);
	reg2 = readl(host->base + SDIO_INTRCTR_REG);

	if (enable) {
		reg1 |= SDIO_INTR_ENABLE;
		reg2 |= SDIO_INTR_CTR_ENABLE;
	} else {
		reg1 &= ~SDIO_INTR_ENABLE;
		reg2 &= ~SDIO_INTR_CTR_ENABLE;
	}

	writel(reg2, host->base + SDIO_INTRCTR_REG);
	writel(reg1, host->base + MCI_MASK_REG);

	spin_unlock_irqrestore(&host->lock, flags);
}

static struct mmc_host_ops akmci_ops = {
	AKMCI_MMC_OPS_CLAIM_HOOKS
	.request	 = akmci_request,
	.set_ios	 = akmci_set_ios,
	.get_ro		 = akmci_get_ro,
	.get_cd		 = akmci_card_present,
	.enable_sdio_irq = akmci_enable_sdio_irq,
};

void akmci_card_event(struct akmci_host *host)
{
	mmc_detect_change(host->mmc, AKMCI_DETECT_DELAY);
}

struct akmci_host *akmci_host_alloc(struct device *dev)
{
	struct akmci_host *host;
	struct mmc_host *mmc;

	mmc = mmc_alloc_host(sizeof(struct akmci_host), dev);
	if (!mmc)
		return NULL;

	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->dev = dev;

	spin_lock_init(&host->lock);

	host->l2buf_id = AKMCI_L2BUF_NONE;
	host->cfg.gpio_cd = -ENOSYS;
	host->cfg.gpio_wp = -ENOSYS;

	akmci_timer_setup(&host->req_timer, akmci_req_timeout, host);

	return host;
}

void akmci_host_free(struct akmci_host *host)
{
	mmc_free_host(host->mmc);
}

/*
 * Queue limits. Every byte the card sees passes through a single 512-byte L2
 * buffer, and the data length register is sixteen bits wide, so the block
 * layer is told 512-byte blocks, 127 of them, and one segment: a segment that
 * ended mid-block would leave the controller waiting for a block the driver
 * could not finish assembling.
 */
static void akmci_init_limits(struct akmci_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct akmci_board_cfg *cfg = &host->cfg;

	mmc->ops = &akmci_ops;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;

	mmc->caps = 0;
	if (cfg->data_lines == 4 || cfg->data_lines == 8)
		mmc->caps |= MMC_CAP_4_BIT_DATA;
	if (cfg->dev_kind == AKMCI_DEV_SDIO)
		mmc->caps |= MMC_CAP_SDIO_IRQ;
	if (cfg->cap_highspeed)
		mmc->caps |= MMC_CAP_SD_HIGHSPEED | MMC_CAP_MMC_HIGHSPEED;
	mmc->caps |= cfg->extra_caps;

	mmc->f_min = host->asic_clk / (255 + 1 + 255 + 1);
	mmc->f_max = host->asic_clk / (0 + 1 + 0 + 1);
	if (cfg->max_speed_hz && mmc->f_max > cfg->max_speed_hz)
		mmc->f_max = cfg->max_speed_hz;

	mmc->max_segs = 1;
	mmc->max_blk_size = MAX_MCI_BLOCK_SIZE;
	mmc->max_blk_count = MAX_MCI_BLOCK_COUNT;
	mmc->max_req_size = MAX_MCI_REQ_SIZE;
	mmc->max_seg_size = MAX_MCI_REQ_SIZE;
}

int akmci_host_add(struct akmci_host *host)
{
	int ret;

	if (WARN_ON(host->base == NULL || host->l2 == NULL ||
		    host->asic_clk == 0))
		return -EINVAL;

	akmci_init_limits(host);

	writel(MCI_ENABLE | MCI_FAIL_TRIGGER, host->base + MCI_CLK_REG);
	akmci_clear_imask(host);

	ret = mmc_add_host(host->mmc);
	if (ret)
		return ret;

	dev_info(host->dev, "%s: %s transfer, %u-bit bus, card detect: %s\n",
		 mmc_hostname(host->mmc),
		 akmci_xfer_desc[host->cfg.xfer_kind],
		 host->cfg.data_lines,
		 akmci_cd_desc[host->cfg.cd_kind]);

	return 0;
}

void akmci_host_del(struct akmci_host *host)
{
	unsigned long flags;

	mmc_remove_host(host->mmc);

	spin_lock_irqsave(&host->lock, flags);
	akmci_clear_imask(host);
	spin_unlock_irqrestore(&host->lock, flags);

	del_timer_sync(&host->req_timer);
	akmci_l2_release(host);
}
