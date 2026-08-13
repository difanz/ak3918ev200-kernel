// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Anyka AK39EV330/AK3918EV200 encrypt block: AES offload for the Linux
 * crypto API.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>

#include <asm/unaligned.h>
#include <mach/map.h>

#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <crypto/scatterwalk.h>

#include "ak-encrypt-regs.h"

#define AK_ENC_QUEUE_LEN	50
/* The engine's own rate is not yet known, so the software bound is generous
 * per byte and still well inside the board watchdog.
 */
#define AK_ENC_WATCHDOG_MS	1000
#define AK_ENC_BOUNCE_MAX	16384

/* The encrypt block's bit in the system controller's module reset word. */
#define AK_ENC_RESET_CON	(AK_VA_SYSCTRL + 0x20)
#define AK_ENC_RESET_BIT	14

#define FLAGS_BUSY		0
#define FLAGS_CLAIMED		1

enum ak_enc_path {
	AK_ENC_PATH_DIRECT,	/* one mapped DMA segment covers the request */
	AK_ENC_PATH_BOUNCE,	/* copied through the coherent staging pair */
	AK_ENC_PATH_PIO,	/* one block per interrupt through GRP_INPUT */
};

struct ak_enc_dev {
	struct device		*dev;
	void __iomem		*base;
	struct clk		*clk;
	int			irq;

	spinlock_t		lock;
	struct crypto_queue	queue;
	struct tasklet_struct	done_task;
	struct timer_list	watchdog;
	unsigned long		flags;
	u32			int_status;

	struct ablkcipher_request *req;

	bool			force_pio;
	unsigned int		max_xfer;
	unsigned int		bounce_len;
	void			*bounce_in;
	void			*bounce_out;
	dma_addr_t		bounce_in_dma;
	dma_addr_t		bounce_out_dma;

	struct dentry		*debug;
	atomic_t		stat_requests;
	atomic_t		stat_hw;
	atomic_t		stat_fallback;
	atomic_t		stat_bounce;
	atomic_t		stat_pio;
	atomic_t		stat_timeouts;
	atomic_t		stat_errors;
	atomic_t		stat_kbytes;
};

struct ak_enc_ctx {
	struct ak_enc_dev	*dd;
	struct crypto_ablkcipher *fallback;
	unsigned int		mode;
	unsigned int		keylen;
	u8			key[AES_MAX_KEY_SIZE];
};

struct ak_enc_reqctx {
	enum ak_enc_path	path;
	unsigned int		offset;
	unsigned int		chunk;
	unsigned int		src_nents;
	unsigned int		dst_nents;
	/* Must stay last: the fallback's own request context follows it. */
	struct ablkcipher_request fallback_req;
};

struct ak_enc_alg {
	unsigned int		mode;
	struct crypto_alg	alg;
};

static struct ak_enc_dev *ak_enc_singleton;

static unsigned int priority = 50;
module_param(priority, uint, 0444);
MODULE_PARM_DESC(priority, "cra_priority of the registered algorithms");

/* The engine's fixed cost per request loses to table-driven software AES on
 * this core below this length, so short requests go to software.
 */
static unsigned int hw_min_bytes = 1024;
module_param(hw_min_bytes, uint, 0644);
MODULE_PARM_DESC(hw_min_bytes, "route requests shorter than this to software");

/* DATALEN tops out at 16 KiB, so that is the longest transfer programmed;
 * every path segments above it.
 */
static unsigned int max_xfer = 16384;
module_param(max_xfer, uint, 0444);
MODULE_PARM_DESC(max_xfer, "longest single engine transfer, in bytes");

static unsigned int bounce_bytes = 4096;
module_param(bounce_bytes, uint, 0444);
MODULE_PARM_DESC(bounce_bytes, "size of each staging buffer, in bytes");

static bool force_pio;
module_param(force_pio, bool, 0644);
MODULE_PARM_DESC(force_pio, "use the single-group register path, not DMA");

static inline void ak_enc_write(struct ak_enc_dev *dd, u32 val, u32 reg)
{
	writel(val, dd->base + reg);
}

static inline u32 ak_enc_read(struct ak_enc_dev *dd, u32 reg)
{
	return readl(dd->base + reg);
}

/*
 * The gate clock's enable op pulses this same bit, so probe gets a reset for
 * free; this exists for the recovery path, which cannot sleep.
 */
static void ak_enc_soft_reset(void)
{
	unsigned long flags;
	u32 val;

	local_irq_save(flags);
	val = __raw_readl(AK_ENC_RESET_CON);
	__raw_writel(val | (1u << AK_ENC_RESET_BIT), AK_ENC_RESET_CON);
	local_irq_restore(flags);

	mdelay(5);

	local_irq_save(flags);
	val = __raw_readl(AK_ENC_RESET_CON);
	__raw_writel(val & ~(1u << AK_ENC_RESET_BIT), AK_ENC_RESET_CON);
	local_irq_restore(flags);
}

static u32 ak_enc_alg_sel(unsigned int keylen)
{
	switch (keylen) {
	case AES_KEYSIZE_192:
		return AKENC_ALG_SEL(AKENC_ALG_AES192);
	case AES_KEYSIZE_256:
		return AKENC_ALG_SEL(AKENC_ALG_AES256);
	default:
		return AKENC_ALG_SEL(AKENC_ALG_AES128);
	}
}

static void ak_enc_load_words(struct ak_enc_dev *dd, u32 reg, const u8 *src,
			      unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len / 4; i++)
		ak_enc_write(dd, get_unaligned_le32(src + i * 4), reg + i * 4);
}

static void ak_enc_store_words(struct ak_enc_dev *dd, u32 reg, u8 *dst,
			       unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len / 4; i++)
		put_unaligned_le32(ak_enc_read(dd, reg + i * 4), dst + i * 4);
}

static void ak_enc_program_data(struct ak_enc_dev *dd,
				struct ablkcipher_request *req,
				struct ak_enc_reqctx *rctx)
{
	u8 block[AES_BLOCK_SIZE];

	switch (rctx->path) {
	case AK_ENC_PATH_DIRECT:
		rctx->chunk = min(req->nbytes - rctx->offset, dd->max_xfer);
		ak_enc_write(dd, sg_dma_address(req->src) + rctx->offset,
			     AKENC_PLAINT_ADDR);
		ak_enc_write(dd, sg_dma_address(req->dst) + rctx->offset,
			     AKENC_CIPHER_ADDR);
		ak_enc_write(dd, rctx->chunk, AKENC_DATALEN);
		break;
	case AK_ENC_PATH_BOUNCE:
		rctx->chunk = min(req->nbytes - rctx->offset, dd->bounce_len);
		dma_sync_single_for_cpu(dd->dev, dd->bounce_in_dma,
					rctx->chunk, DMA_TO_DEVICE);
		scatterwalk_map_and_copy(dd->bounce_in, req->src, rctx->offset,
					 rctx->chunk, 0);
		dma_sync_single_for_device(dd->dev, dd->bounce_in_dma,
					   rctx->chunk, DMA_TO_DEVICE);
		dma_sync_single_for_device(dd->dev, dd->bounce_out_dma,
					   rctx->chunk, DMA_FROM_DEVICE);
		ak_enc_write(dd, dd->bounce_in_dma, AKENC_PLAINT_ADDR);
		ak_enc_write(dd, dd->bounce_out_dma, AKENC_CIPHER_ADDR);
		ak_enc_write(dd, rctx->chunk, AKENC_DATALEN);
		break;
	case AK_ENC_PATH_PIO:
		rctx->chunk = AES_BLOCK_SIZE;
		scatterwalk_map_and_copy(block, req->src, rctx->offset,
					 rctx->chunk, 0);
		ak_enc_load_words(dd, AKENC_GRP_INPUT(1), block, rctx->chunk);
		break;
	}
}

static void ak_enc_harvest_data(struct ak_enc_dev *dd,
				struct ablkcipher_request *req,
				struct ak_enc_reqctx *rctx)
{
	u8 block[AES_BLOCK_SIZE];

	switch (rctx->path) {
	case AK_ENC_PATH_DIRECT:
		break;
	case AK_ENC_PATH_BOUNCE:
		dma_sync_single_for_cpu(dd->dev, dd->bounce_out_dma,
					rctx->chunk, DMA_FROM_DEVICE);
		scatterwalk_map_and_copy(dd->bounce_out, req->dst,
					 rctx->offset, rctx->chunk, 1);
		break;
	case AK_ENC_PATH_PIO:
		ak_enc_store_words(dd, AKENC_GRP_OUTPUT(1), block,
				   rctx->chunk);
		scatterwalk_map_and_copy(block, req->dst, rctx->offset,
					 rctx->chunk, 1);
		break;
	}
}

/*
 * IV_MODE loads the vector register; clearing it for every chunk after the
 * first is what keeps CBC chained across a segmented transfer.
 */
static void ak_enc_launch(struct ak_enc_dev *dd,
			  struct ablkcipher_request *req,
			  struct ak_enc_reqctx *rctx, bool first)
{
	struct crypto_ablkcipher *tfm = crypto_ablkcipher_reqtfm(req);
	struct ak_enc_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	u32 con;

	ak_enc_program_data(dd, req, rctx);

	if (first) {
		ak_enc_write(dd, AKENC_TIMEOUT_MAX, AKENC_TIMEOUT);
		if (crypto_ablkcipher_ivsize(tfm))
			ak_enc_load_words(dd, AKENC_VEC_INPUT(1), req->info,
					  crypto_ablkcipher_ivsize(tfm));
		ak_enc_load_words(dd, AKENC_KEY_INPUT(1), ctx->key,
				  ctx->keylen);

		con = AKENC_INT_EN | AKENC_TIMEOUT_INT_EN | AKENC_CLK_EN |
		      AKENC_AES_BIT_SEQ | AKENC_IV_MODE |
		      ak_enc_alg_sel(ctx->keylen) |
		      AKENC_WIDTH_SEL(AKENC_WIDTH_FULL) |
		      AKENC_OPT_MODE(ctx->mode);
		if (rctx->path != AK_ENC_PATH_PIO)
			con |= AKENC_MULT_GRP;
	} else {
		con = ak_enc_read(dd, AKENC_CONTROL);
		con &= ~(AKENC_IV_MODE | AKENC_START);
		con |= AKENC_INT_EN | AKENC_TIMEOUT_INT_EN;
	}
	ak_enc_write(dd, con, AKENC_CONTROL);

	clear_bit(FLAGS_CLAIMED, &dd->flags);
	mod_timer(&dd->watchdog, jiffies + msecs_to_jiffies(
		  AK_ENC_WATCHDOG_MS + rctx->chunk / 64));

	con = ak_enc_read(dd, AKENC_CONTROL);
	ak_enc_write(dd, con | AKENC_START, AKENC_CONTROL);
}

static void ak_enc_unmap(struct ak_enc_dev *dd,
			 struct ablkcipher_request *req,
			 struct ak_enc_reqctx *rctx)
{
	if (rctx->path != AK_ENC_PATH_DIRECT)
		return;
	dma_unmap_sg(dd->dev, req->dst, rctx->dst_nents, DMA_FROM_DEVICE);
	dma_unmap_sg(dd->dev, req->src, rctx->src_nents, DMA_TO_DEVICE);
}

static int ak_enc_handle_queue(struct ak_enc_dev *dd,
			       struct ablkcipher_request *req);

static void ak_enc_finish(struct ak_enc_dev *dd, int err)
{
	struct ablkcipher_request *req = dd->req;
	struct crypto_ablkcipher *tfm = crypto_ablkcipher_reqtfm(req);
	struct ak_enc_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	struct ak_enc_reqctx *rctx = ablkcipher_request_ctx(req);
	unsigned long flags;

	ak_enc_unmap(dd, req, rctx);

	/* Callers chain a CBC stream by feeding the last ciphertext block back
	 * as the next request's IV.
	 */
	if (!err && ctx->mode == AKENC_MODE_CBC &&
	    crypto_ablkcipher_ivsize(tfm) == AES_BLOCK_SIZE)
		scatterwalk_map_and_copy(req->info, req->dst,
					 req->nbytes - AES_BLOCK_SIZE,
					 AES_BLOCK_SIZE, 0);

	spin_lock_irqsave(&dd->lock, flags);
	dd->req = NULL;
	clear_bit(FLAGS_BUSY, &dd->flags);
	spin_unlock_irqrestore(&dd->lock, flags);

	if (!err)
		atomic_add(req->nbytes / 1024, &dd->stat_kbytes);

	req->base.complete(&req->base, err);
	ak_enc_handle_queue(dd, NULL);
}

static void ak_enc_recover(struct ak_enc_dev *dd)
{
	u32 con = ak_enc_read(dd, AKENC_CONTROL);

	con &= ~(AKENC_INT_EN | AKENC_TIMEOUT_INT_EN | AKENC_START);
	ak_enc_write(dd, con | AKENC_STOP, AKENC_CONTROL);
	ak_enc_soft_reset();
}

static void ak_enc_done_task(unsigned long data)
{
	struct ak_enc_dev *dd = (struct ak_enc_dev *)data;
	struct ablkcipher_request *req = dd->req;
	struct ak_enc_reqctx *rctx;
	u32 status = dd->int_status;

	del_timer(&dd->watchdog);
	if (!req)
		return;
	rctx = ablkcipher_request_ctx(req);

	if (!(status & AKENC_INT_DONE)) {
		ak_enc_recover(dd);
		if (status & AKENC_INT_TIMEOUT) {
			atomic_inc(&dd->stat_timeouts);
			dev_err_ratelimited(dd->dev, "engine timeout\n");
			ak_enc_finish(dd, -ETIMEDOUT);
		} else {
			atomic_inc(&dd->stat_errors);
			dev_err_ratelimited(dd->dev, "engine stalled\n");
			ak_enc_finish(dd, -EIO);
		}
		return;
	}

	ak_enc_harvest_data(dd, req, rctx);
	rctx->offset += rctx->chunk;

	if (rctx->offset < req->nbytes) {
		ak_enc_launch(dd, req, rctx, false);
		return;
	}

	ak_enc_finish(dd, 0);
}

static void ak_enc_claim(struct ak_enc_dev *dd, u32 status)
{
	if (test_and_set_bit(FLAGS_CLAIMED, &dd->flags))
		return;
	dd->int_status = status;
	tasklet_schedule(&dd->done_task);
}

static void ak_enc_watchdog(unsigned long data)
{
	struct ak_enc_dev *dd = (struct ak_enc_dev *)data;

	if (test_bit(FLAGS_BUSY, &dd->flags))
		ak_enc_claim(dd, AKENC_INT_TIMEOUT);
}

/*
 * The interrupt is masked here rather than acknowledged: the status register's
 * clearing rule is not established, and a level-triggered source that the
 * handler cannot clear would live-lock the core before the tasklet ran.
 */
static irqreturn_t ak_enc_irq(int irq, void *dev_id)
{
	struct ak_enc_dev *dd = dev_id;
	u32 status, con;

	(void)irq;

	status = ak_enc_read(dd, AKENC_INT_STATUS);
	con = ak_enc_read(dd, AKENC_CONTROL);
	ak_enc_write(dd, con & ~(AKENC_INT_EN | AKENC_TIMEOUT_INT_EN |
				 AKENC_START), AKENC_CONTROL);

	if (!test_bit(FLAGS_BUSY, &dd->flags))
		return IRQ_HANDLED;

	ak_enc_claim(dd, status);
	return IRQ_HANDLED;
}

static int ak_enc_start(struct ak_enc_dev *dd, struct ablkcipher_request *req)
{
	struct ak_enc_reqctx *rctx = ablkcipher_request_ctx(req);

	dd->req = req;
	rctx->offset = 0;
	rctx->chunk = 0;
	rctx->src_nents = 0;
	rctx->dst_nents = 0;
	rctx->path = dd->force_pio ? AK_ENC_PATH_PIO : AK_ENC_PATH_BOUNCE;

	if (rctx->path == AK_ENC_PATH_BOUNCE &&
	    sg_nents_for_len(req->src, req->nbytes) == 1 &&
	    sg_nents_for_len(req->dst, req->nbytes) == 1) {
		int ns, nd = 0;

		ns = dma_map_sg(dd->dev, req->src, 1, DMA_TO_DEVICE);
		if (ns)
			nd = dma_map_sg(dd->dev, req->dst, 1, DMA_FROM_DEVICE);

		if (ns && nd &&
		    !((sg_dma_address(req->src) |
		       sg_dma_address(req->dst)) & 3) &&
		    sg_dma_len(req->src) >= req->nbytes &&
		    sg_dma_len(req->dst) >= req->nbytes) {
			rctx->src_nents = 1;
			rctx->dst_nents = 1;
			rctx->path = AK_ENC_PATH_DIRECT;
		} else {
			if (nd)
				dma_unmap_sg(dd->dev, req->dst, 1,
					     DMA_FROM_DEVICE);
			if (ns)
				dma_unmap_sg(dd->dev, req->src, 1,
					     DMA_TO_DEVICE);
		}
	}

	atomic_inc(&dd->stat_hw);
	if (rctx->path == AK_ENC_PATH_BOUNCE)
		atomic_inc(&dd->stat_bounce);
	else if (rctx->path == AK_ENC_PATH_PIO)
		atomic_inc(&dd->stat_pio);

	ak_enc_launch(dd, req, rctx, true);
	return -EINPROGRESS;
}

static int ak_enc_handle_queue(struct ak_enc_dev *dd,
			       struct ablkcipher_request *req)
{
	struct crypto_async_request *async_req, *backlog;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&dd->lock, flags);
	if (req)
		ret = ablkcipher_enqueue_request(&dd->queue, req);
	if (test_bit(FLAGS_BUSY, &dd->flags)) {
		spin_unlock_irqrestore(&dd->lock, flags);
		return ret;
	}
	backlog = crypto_get_backlog(&dd->queue);
	async_req = crypto_dequeue_request(&dd->queue);
	if (!async_req) {
		spin_unlock_irqrestore(&dd->lock, flags);
		return ret;
	}
	set_bit(FLAGS_BUSY, &dd->flags);
	spin_unlock_irqrestore(&dd->lock, flags);

	if (backlog)
		backlog->complete(backlog, -EINPROGRESS);

	ak_enc_start(dd, ablkcipher_request_cast(async_req));
	return ret;
}

static int ak_enc_fallback(struct ablkcipher_request *req, bool encrypt)
{
	struct crypto_ablkcipher *tfm = crypto_ablkcipher_reqtfm(req);
	struct ak_enc_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	struct ak_enc_reqctx *rctx = ablkcipher_request_ctx(req);

	if (ctx->dd)
		atomic_inc(&ctx->dd->stat_fallback);

	ablkcipher_request_set_tfm(&rctx->fallback_req, ctx->fallback);
	ablkcipher_request_set_callback(&rctx->fallback_req, req->base.flags,
					req->base.complete, req->base.data);
	ablkcipher_request_set_crypt(&rctx->fallback_req, req->src, req->dst,
				     req->nbytes, req->info);

	return encrypt ? crypto_ablkcipher_encrypt(&rctx->fallback_req) :
			 crypto_ablkcipher_decrypt(&rctx->fallback_req);
}

/*
 * The published control map has no decrypt direction, so only encryption ever
 * reaches the engine; decryption is a software cipher under a hardware name.
 */
static bool ak_enc_can_hw(struct ak_enc_dev *dd, struct ak_enc_ctx *ctx,
			  struct ablkcipher_request *req)
{
	return dd && ctx->keylen && req->nbytes &&
	       req->nbytes >= hw_min_bytes &&
	       !(req->nbytes % AES_BLOCK_SIZE);
}

static int ak_enc_encrypt(struct ablkcipher_request *req)
{
	struct crypto_ablkcipher *tfm = crypto_ablkcipher_reqtfm(req);
	struct ak_enc_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	struct ak_enc_dev *dd = ctx->dd;

	if (dd)
		atomic_inc(&dd->stat_requests);
	if (!ak_enc_can_hw(dd, ctx, req))
		return ak_enc_fallback(req, true);

	return ak_enc_handle_queue(dd, req);
}

static int ak_enc_decrypt(struct ablkcipher_request *req)
{
	struct crypto_ablkcipher *tfm = crypto_ablkcipher_reqtfm(req);
	struct ak_enc_ctx *ctx = crypto_ablkcipher_ctx(tfm);

	if (ctx->dd)
		atomic_inc(&ctx->dd->stat_requests);
	return ak_enc_fallback(req, false);
}

static int ak_enc_setkey(struct crypto_ablkcipher *tfm, const u8 *key,
			 unsigned int keylen)
{
	struct ak_enc_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	int err;

	if (keylen != AES_KEYSIZE_128 && keylen != AES_KEYSIZE_192 &&
	    keylen != AES_KEYSIZE_256) {
		crypto_ablkcipher_set_flags(tfm, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	}

	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;

	crypto_ablkcipher_clear_flags(ctx->fallback, CRYPTO_TFM_REQ_MASK);
	crypto_ablkcipher_set_flags(ctx->fallback,
				    crypto_ablkcipher_get_flags(tfm) &
				    CRYPTO_TFM_REQ_MASK);
	err = crypto_ablkcipher_setkey(ctx->fallback, key, keylen);
	crypto_ablkcipher_set_flags(tfm,
				    crypto_ablkcipher_get_flags(ctx->fallback) &
				    CRYPTO_TFM_RES_MASK);
	return err;
}

static int ak_enc_cra_init(struct crypto_tfm *tfm)
{
	struct ak_enc_ctx *ctx = crypto_tfm_ctx(tfm);
	struct ak_enc_alg *ealg = container_of(tfm->__crt_alg,
					       struct ak_enc_alg, alg);
	const char *name = crypto_tfm_alg_name(tfm);

	ctx->dd = ak_enc_singleton;
	ctx->mode = ealg->mode;

	ctx->fallback = crypto_alloc_ablkcipher(name, 0, CRYPTO_ALG_ASYNC |
						CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(ctx->fallback))
		return PTR_ERR(ctx->fallback);

	tfm->crt_ablkcipher.reqsize = sizeof(struct ak_enc_reqctx) +
			crypto_ablkcipher_reqsize(ctx->fallback);
	return 0;
}

static void ak_enc_cra_exit(struct crypto_tfm *tfm)
{
	struct ak_enc_ctx *ctx = crypto_tfm_ctx(tfm);

	crypto_free_ablkcipher(ctx->fallback);
	memzero_explicit(ctx->key, sizeof(ctx->key));
}

static struct ak_enc_alg ak_enc_algs[] = {
	{
		.mode = AKENC_MODE_ECB,
		.alg = {
			.cra_name		= "ecb(aes)",
			.cra_driver_name	= "ecb-aes-ak",
			.cra_blocksize		= AES_BLOCK_SIZE,
			.cra_ctxsize		= sizeof(struct ak_enc_ctx),
			.cra_alignmask		= 3,
			.cra_type		= &crypto_ablkcipher_type,
			.cra_module		= THIS_MODULE,
			.cra_init		= ak_enc_cra_init,
			.cra_exit		= ak_enc_cra_exit,
			.cra_u.ablkcipher = {
				.min_keysize	= AES_MIN_KEY_SIZE,
				.max_keysize	= AES_MAX_KEY_SIZE,
				.setkey		= ak_enc_setkey,
				.encrypt	= ak_enc_encrypt,
				.decrypt	= ak_enc_decrypt,
			},
		},
	},
	{
		.mode = AKENC_MODE_CBC,
		.alg = {
			.cra_name		= "cbc(aes)",
			.cra_driver_name	= "cbc-aes-ak",
			.cra_blocksize		= AES_BLOCK_SIZE,
			.cra_ctxsize		= sizeof(struct ak_enc_ctx),
			.cra_alignmask		= 3,
			.cra_type		= &crypto_ablkcipher_type,
			.cra_module		= THIS_MODULE,
			.cra_init		= ak_enc_cra_init,
			.cra_exit		= ak_enc_cra_exit,
			.cra_u.ablkcipher = {
				.min_keysize	= AES_MIN_KEY_SIZE,
				.max_keysize	= AES_MAX_KEY_SIZE,
				.ivsize		= AES_BLOCK_SIZE,
				.setkey		= ak_enc_setkey,
				.encrypt	= ak_enc_encrypt,
				.decrypt	= ak_enc_decrypt,
			},
		},
	},
};

static int ak_enc_register_algs(struct ak_enc_dev *dd)
{
	unsigned int i, j;
	int err;

	for (i = 0; i < ARRAY_SIZE(ak_enc_algs); i++) {
		ak_enc_algs[i].alg.cra_priority = priority;
		ak_enc_algs[i].alg.cra_flags = CRYPTO_ALG_TYPE_ABLKCIPHER |
					       CRYPTO_ALG_ASYNC |
					       CRYPTO_ALG_KERN_DRIVER_ONLY |
					       CRYPTO_ALG_NEED_FALLBACK;
		err = crypto_register_alg(&ak_enc_algs[i].alg);
		if (err) {
			dev_err(dd->dev, "cannot register %s: %d\n",
				ak_enc_algs[i].alg.cra_driver_name, err);
			for (j = 0; j < i; j++)
				crypto_unregister_alg(&ak_enc_algs[j].alg);
			return err;
		}
	}
	return 0;
}

static int ak_enc_stats_show(struct seq_file *s, void *data)
{
	struct ak_enc_dev *dd = s->private;

	(void)data;
	seq_printf(s, "requests %d\n", atomic_read(&dd->stat_requests));
	seq_printf(s, "hw %d\n", atomic_read(&dd->stat_hw));
	seq_printf(s, "fallbacks %d\n", atomic_read(&dd->stat_fallback));
	seq_printf(s, "bounces %d\n", atomic_read(&dd->stat_bounce));
	seq_printf(s, "pio %d\n", atomic_read(&dd->stat_pio));
	seq_printf(s, "timeouts %d\n", atomic_read(&dd->stat_timeouts));
	seq_printf(s, "errors %d\n", atomic_read(&dd->stat_errors));
	seq_printf(s, "kbytes %d\n", atomic_read(&dd->stat_kbytes));
	seq_printf(s, "bounce_len %u\n", dd->bounce_len);
	seq_printf(s, "max_xfer %u\n", dd->max_xfer);
	return 0;
}

static int ak_enc_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, ak_enc_stats_show, inode->i_private);
}

static const struct file_operations ak_enc_stats_fops = {
	.owner		= THIS_MODULE,
	.open		= ak_enc_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int ak_enc_probe(struct platform_device *pdev)
{
	struct ak_enc_dev *dd;
	struct resource *res;
	u32 mode;
	int err;

	if (ak_enc_singleton)
		return -EBUSY;

	dd = devm_kzalloc(&pdev->dev, sizeof(*dd), GFP_KERNEL);
	if (!dd)
		return -ENOMEM;

	dd->dev = &pdev->dev;
	spin_lock_init(&dd->lock);
	crypto_init_queue(&dd->queue, AK_ENC_QUEUE_LEN);
	tasklet_init(&dd->done_task, ak_enc_done_task, (unsigned long)dd);
	setup_timer(&dd->watchdog, ak_enc_watchdog, (unsigned long)dd);

	dd->force_pio = force_pio;
	if (!of_property_read_u32(pdev->dev.of_node, "encrypt-mode", &mode) &&
	    mode == 0)
		dd->force_pio = true;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOENT;
	dd->base = devm_ioremap(&pdev->dev, res->start, AKENC_IO_SIZE);
	if (!dd->base)
		return -ENOMEM;

	dd->irq = platform_get_irq(pdev, 0);
	if (dd->irq < 0)
		return dd->irq;

	dd->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(dd->clk))
		return PTR_ERR(dd->clk);
	err = clk_prepare_enable(dd->clk);
	if (err)
		return err;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		goto err_clk;

	dd->max_xfer = max_t(unsigned int, max_xfer, AES_BLOCK_SIZE);
	dd->max_xfer &= ~(AES_BLOCK_SIZE - 1u);
	dd->bounce_len = clamp_t(unsigned int, bounce_bytes, AES_BLOCK_SIZE,
				 AK_ENC_BOUNCE_MAX);
	dd->bounce_len &= ~(AES_BLOCK_SIZE - 1u);
	dd->bounce_len = min(dd->bounce_len, dd->max_xfer);
	/* Cached staging with an explicit sync per transfer: an uncached copy
	 * costs more on this core than the cache maintenance does.
	 */
	dd->bounce_in = kmalloc(dd->bounce_len, GFP_KERNEL);
	dd->bounce_out = kmalloc(dd->bounce_len, GFP_KERNEL);
	if (!dd->bounce_in || !dd->bounce_out) {
		err = -ENOMEM;
		goto err_bounce;
	}
	dd->bounce_in_dma = dma_map_single(&pdev->dev, dd->bounce_in,
					   dd->bounce_len, DMA_TO_DEVICE);
	if (dma_mapping_error(&pdev->dev, dd->bounce_in_dma)) {
		err = -ENOMEM;
		goto err_bounce;
	}
	dd->bounce_out_dma = dma_map_single(&pdev->dev, dd->bounce_out,
					    dd->bounce_len, DMA_FROM_DEVICE);
	if (dma_mapping_error(&pdev->dev, dd->bounce_out_dma)) {
		err = -ENOMEM;
		goto err_unmap_in;
	}

	err = devm_request_irq(&pdev->dev, dd->irq, ak_enc_irq, 0,
			       dev_name(&pdev->dev), dd);
	if (err)
		goto err_unmap_out;

	platform_set_drvdata(pdev, dd);
	ak_enc_singleton = dd;

	err = ak_enc_register_algs(dd);
	if (err)
		goto err_singleton;

	dd->debug = debugfs_create_dir("ak-encrypt", NULL);
	if (dd->debug)
		debugfs_create_file("stats", 0400, dd->debug, dd,
				    &ak_enc_stats_fops);

	dev_info(&pdev->dev,
		 "AES offload ready, priority %u, staging %u B, transfer %u B%s\n",
		 priority, dd->bounce_len, dd->max_xfer,
		 dd->force_pio ? ", register path" : "");
	return 0;

err_singleton:
	ak_enc_singleton = NULL;
err_unmap_out:
	dma_unmap_single(&pdev->dev, dd->bounce_out_dma, dd->bounce_len,
			 DMA_FROM_DEVICE);
err_unmap_in:
	dma_unmap_single(&pdev->dev, dd->bounce_in_dma, dd->bounce_len,
			 DMA_TO_DEVICE);
err_bounce:
	kfree(dd->bounce_out);
	kfree(dd->bounce_in);
err_clk:
	clk_disable_unprepare(dd->clk);
	return err;
}

static int ak_enc_remove(struct platform_device *pdev)
{
	struct ak_enc_dev *dd = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ak_enc_algs); i++)
		crypto_unregister_alg(&ak_enc_algs[i].alg);

	del_timer_sync(&dd->watchdog);
	tasklet_kill(&dd->done_task);

	debugfs_remove_recursive(dd->debug);

	for (i = 1; i <= AES_MAX_KEY_SIZE / 4; i++)
		ak_enc_write(dd, 0, AKENC_KEY_INPUT(i));
	ak_enc_write(dd, 0, AKENC_CONTROL);

	dma_unmap_single(&pdev->dev, dd->bounce_out_dma, dd->bounce_len,
			 DMA_FROM_DEVICE);
	dma_unmap_single(&pdev->dev, dd->bounce_in_dma, dd->bounce_len,
			 DMA_TO_DEVICE);
	kfree(dd->bounce_out);
	kfree(dd->bounce_in);
	clk_disable_unprepare(dd->clk);
	ak_enc_singleton = NULL;
	return 0;
}

static const struct of_device_id ak_enc_of_match[] = {
	{ .compatible = "anyka,ak39ev330-encrypt" },
	{ .compatible = "anyka,ak3918ev200-encrypt" },
	{}
};
MODULE_DEVICE_TABLE(of, ak_enc_of_match);

static struct platform_driver ak_enc_driver = {
	.probe	= ak_enc_probe,
	.remove	= ak_enc_remove,
	.driver	= {
		.name		= "ak-encrypt",
		.of_match_table	= ak_enc_of_match,
	},
};

module_platform_driver(ak_enc_driver);

MODULE_DESCRIPTION("Anyka AK39 encrypt block AES offload");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ak-encrypt");
