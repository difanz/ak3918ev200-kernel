/*
 * akmci.h - Anyka AK39 MCI host controller, private definitions
 *
 * Copyright (C) 2010 Anyka, Ltd, All Rights Reserved.
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __DRIVERS_MMC_HOST_AKMCI_H__
#define __DRIVERS_MMC_HOST_AKMCI_H__

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>

#include "akmci-compat.h"

struct device;
struct mmc_host;
struct mmc_request;
struct mmc_command;
struct mmc_data;

/*
 * Register map. The block is PL180-inspired but not PL180-compatible: 0x038 is
 * a mask register rather than PL180's write-1-to-clear, 0x03c drives the L2
 * engine rather than PL180's MASK0, status bits are renumbered from bit 3 up,
 * and status is cleared by reading it.
 */
#define MCI_CLK_REG			0x004
#define MMC_CLK_DIVL(x)			((x) & 0xff)
#define MMC_CLK_DIVH(x)			(((x) & 0xff) << 8)
#define MCI_CLK_DIV_MASK		0xffff
#define MCI_CLK_ENABLE			(1 << 16)
#define MCI_CLK_PWRSAVE			(1 << 17)
#define MCI_FAIL_TRIGGER		(1 << 19)
#define MCI_ENABLE			(1 << 20)

#define MCI_ARGUMENT_REG		0x008

#define MCI_COMMAND_REG			0x00c
#define MCI_CPSM_ENABLE			(1 << 0)
#define MCI_CPSM_CMD(x)			(((x) & 0x3f) << 1)
#define MCI_CPSM_RESPONSE		(1 << 7)
#define MCI_CPSM_LONGRSP		(1 << 8)
#define MCI_CPSM_PENDING		(1 << 9)
#define MCI_CPSM_RSPCRC_NOCHK		(1 << 10)
#define MCI_CPSM_WITHDATA		(1 << 11)

#define MCI_RESPCMD_REG			0x010
#define MCI_RESPONSE0_REG		0x014
#define MCI_RESPONSE1_REG		0x018
#define MCI_RESPONSE2_REG		0x01c
#define MCI_RESPONSE3_REG		0x020
#define MCI_DATATIMER_REG		0x024
#define MCI_DATALENGTH_REG		0x028

#define MCI_DATACTRL_REG		0x02c
#define MCI_DPSM_ENABLE			(1 << 0)
#define MCI_DPSM_DIRECTION		(1 << 1)
#define MCI_DPSM_STREAM			(1 << 2)
#define MCI_DPSM_BUSMODE(x)		(((x) & 0x3) << 3)
#define MCI_DPSM_BLOCKSIZE(x)		(((x) & 0xfff) << 16)

#define MCI_DATACNT_REG			0x030

#define MCI_STATUS_REG			0x034
#define MCI_RESPCRCFAIL			(1 << 0)
#define MCI_DATACRCFAIL			(1 << 1)
#define MCI_RESPTIMEOUT			(1 << 2)
#define MCI_DATATIMEOUT			(1 << 3)
#define MCI_RESPEND			(1 << 4)
#define MCI_CMDSENT			(1 << 5)
#define MCI_DATAEND			(1 << 6)
#define MCI_DATABLOCKEND		(1 << 7)
#define MCI_STARTBIT_ERR		(1 << 8)
#define MCI_CMDACTIVE			(1 << 9)
#define MCI_TXACTIVE			(1 << 10)
#define MCI_RXACTIVE			(1 << 11)
#define MCI_FIFOFULL			(1 << 12)
#define MCI_FIFOEMPTY			(1 << 13)
#define MCI_FIFOHALFFULL		(1 << 14)
#define MCI_FIFOHALFEMPTY		(1 << 15)
#define MCI_DATATRANS_FINISH		(1 << 16)
#define MCI_SDIOINT			(1 << 17)

#define MCI_MASK_REG			0x038
#define MCI_RESPCRCFAILMASK		(1 << 0)
#define MCI_DATACRCFAILMASK		(1 << 1)
#define MCI_RESPTIMEOUTMASK		(1 << 2)
#define MCI_DATATIMEOUTMASK		(1 << 3)
#define MCI_RESPENDMASK			(1 << 4)
#define MCI_CMDSENTMASK			(1 << 5)
#define MCI_DATAENDMASK			(1 << 6)
#define MCI_DATABLOCKENDMASK		(1 << 7)
#define MCI_STARTBIT_ERRMASK		(1 << 8)
#define MCI_CMDACTIVEMASK		(1 << 9)
#define MCI_TXACTIVEMASK		(1 << 10)
#define MCI_RXACTIVEMASK		(1 << 11)
#define MCI_FIFOFULLMASK		(1 << 12)
#define MCI_FIFOEMPTYMASK		(1 << 13)
#define MCI_FIFOHALFFULLMASK		(1 << 14)
#define MCI_FIFOHALFEMPTYMASK		(1 << 15)
#define MCI_DATATRANS_FINISHMASK	(1 << 16)
#define MCI_SDIOINTMASK			(1 << 17)

#define MCI_DMACTRL_REG			0x03c
#define MCI_DMA_BUFEN			(1 << 0)
#define MCI_DMA_ADDR(x)			(((x) & 0x7fff) << 1)
#define MCI_DMA_EN			(1 << 16)
#define MCI_DMA_SIZE(x)			(((x) & 0x7fff) << 17)

#define MCI_FIFO_REG			0x040

#define SDIO_INTRCTR_REG		0x000
#define SDIO_INTR_CTR_ENABLE		(1 << 8)
#define SDIO_INTR_ENABLE		(1 << 17)

#define MCI_CMDIRQMASKS							\
	(MCI_CMDSENTMASK | MCI_RESPENDMASK |				\
	 MCI_RESPCRCFAILMASK | MCI_RESPTIMEOUTMASK)

#define MCI_DATAIRQMASKS						\
	(MCI_DATAENDMASK | MCI_DATABLOCKENDMASK |			\
	 MCI_DATACRCFAILMASK | MCI_DATATIMEOUTMASK)

#define MCI_CMDIRQSTATUS						\
	(MCI_CMDSENT | MCI_RESPEND | MCI_RESPCRCFAIL | MCI_RESPTIMEOUT)

#define MCI_DATAIRQSTATUS						\
	(MCI_DATAEND | MCI_DATABLOCKEND | MCI_DATACRCFAIL |		\
	 MCI_DATATIMEOUT | MCI_STARTBIT_ERR)

/* The inner FIFO is four bytes wide and is not the transfer path. */
#define MCI_FIFOSIZE			4

/* One L2 common buffer is 512 bytes; that is the whole transfer window. */
#define MCI_L2FIFO_SIZE			512
#define MCI_L2_DMA_ALIGN		512

/*
 * MCI_DATALENGTH_REG is sixteen bits wide. 65536 wraps to zero, so the largest
 * expressible request is 65535 bytes, which is 127 whole 512-byte blocks.
 */
#define MAX_MCI_BLOCK_SIZE		512
#define MAX_MCI_BLOCK_COUNT		(65535 / MAX_MCI_BLOCK_SIZE)
#define MAX_MCI_REQ_SIZE		(MAX_MCI_BLOCK_COUNT * MAX_MCI_BLOCK_SIZE)

/*
 * The data timer counts the controller's own clock and the vendor value leaves
 * it effectively disabled, so the software watchdog is what bounds a request.
 */
#define TRANS_DATA_TIMEOUT		0xffffffff
#define MCI_REQ_TIMEOUT			(5 * HZ)

/* Budget for acquiring an L2 common buffer before the request is failed. */
#define MCI_L2_ACQUIRE_US		200000
#define MCI_L2_ACQUIRE_STEP_US		100

/* Invalid L2 buffer id, mirroring the L2 core's own BUF_NULL. */
#define AKMCI_L2BUF_NONE		0xff

enum akmci_dev_kind {
	AKMCI_DEV_MMC = 0,		/* MMC/SD controller instance */
	AKMCI_DEV_SDIO,			/* SDIO controller instance */
};

enum akmci_cd_kind {
	AKMCI_CD_ALWAYS = 0,		/* no usable detect pin; card assumed in */
	AKMCI_CD_GPIO,			/* poll a GPIO the glue has claimed */
	AKMCI_CD_HOOK,			/* glue answers through cfg.card_present */
};

enum akmci_xfer_kind {
	AKMCI_XFER_DMA = 0,		/* L2 buffer filled by the L2 DMA engine */
	AKMCI_XFER_PIO,			/* L2 buffer filled by the CPU */
};

struct akmci_host;

/*
 * The L2 backend. The controller never touches system memory itself: every
 * block passes through one 512-byte buffer out of the SoC's shared L2 pool,
 * which is also used by SPI, USB and audio. Each call reports failure so the
 * host can fail the request instead of charging bytes the card never saw.
 */
struct akmci_l2_ops {
	int  (*buf_get)(struct akmci_host *host, u8 *buf_id);
	void (*buf_put)(struct akmci_host *host);
	int  (*xfer_dma)(struct akmci_host *host, dma_addr_t phys,
			 unsigned int len, int to_card);
	int  (*xfer_cpu)(struct akmci_host *host, void *virt,
			 unsigned int len, int to_card);
	int  (*wait)(struct akmci_host *host);
	void (*clr_status)(struct akmci_host *host);
	void (*reset)(struct akmci_host *host);
	u8   (*buf_status)(struct akmci_host *host);
};

/*
 * Diagnostic counters. This is not a shipping feature: it exists to answer
 * three register-semantics questions that source cannot settle, and it is
 * removed once they are answered.
 */
struct akmci_probe {
	unsigned int	blockend;	/* DATABLOCKEND interrupts */
	unsigned int	blockend_active;/* ... with TX/RXACTIVE still set */
	unsigned int	dataend;	/* DATAEND interrupts, hardware ones */
	unsigned int	dataend_nofinish;/* ... without DATATRANS_FINISH */
	unsigned int	cnt_midblock;	/* DATACNT not on a block boundary */
	unsigned int	wr_dirty;	/* write: L2 not empty before refill */
	unsigned int	wr_dirty_max;	/* worst such occupancy, 64B groups */
	unsigned int	rd_end_dirty;	/* read: L2 not empty at DATAEND */
	unsigned int	rd_end_max;
};

struct akmci_board_cfg {
	int		dev_kind;	/* enum akmci_dev_kind */
	int		cd_kind;	/* enum akmci_cd_kind */
	int		xfer_kind;	/* enum akmci_xfer_kind */
	int		data_lines;	/* 1 or 4 */
	int		cap_highspeed;
	u32		max_speed_hz;
	u32		extra_caps;
	int		gpio_cd;	/* claimed by the glue, or -ENOSYS */
	int		gpio_wp;	/* claimed by the glue, or -ENOSYS */
	int		(*gpio_get)(int gpio);
	int		(*card_present)(struct akmci_host *host);
};

struct akmci_host {
	struct device		*dev;
	struct mmc_host		*mmc;
	void __iomem		*base;
	unsigned long		asic_clk;

	struct akmci_board_cfg	cfg;
	const struct akmci_l2_ops *l2;
	void			*glue;

	spinlock_t		lock;
	struct mmc_request	*mrq;
	struct mmc_command	*cmd;
	struct mmc_data		*data;

	unsigned char		bus_mode;
	unsigned char		bus_width;
	unsigned long		bus_clock;
	unsigned long		clkreg;

	unsigned int		size;
	unsigned int		data_xfered;
	struct scatterlist	*sg_ptr;
	unsigned int		sg_len;
	unsigned int		sg_off;

	int			dma_mapped;
	int			l2_dma_active;
	int			data_err_flag;
	u8			l2buf_id;

	struct akmci_probe	probe;

	struct timer_list	req_timer;
};

/*
 * Glue contract. The glue allocates the host, fills in base, asic_clk, cfg,
 * l2 and dev, then adds it. akmci_irq() is the controller interrupt handler.
 */
struct akmci_host *akmci_host_alloc(struct device *dev);
void akmci_host_free(struct akmci_host *host);
int akmci_host_add(struct akmci_host *host);
void akmci_host_del(struct akmci_host *host);
irqreturn_t akmci_irq(int irq, void *dev_id);
void akmci_card_event(struct akmci_host *host);
int akmci_probe_format(struct akmci_host *host, char *buf, size_t len);

#endif /* __DRIVERS_MMC_HOST_AKMCI_H__ */
