// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The rate controller: a leaky bucket, one QP per frame from a smoothed
 * log-domain complexity estimate held still by a dead zone, the core's
 * ten-checkpoint ladder and the MAD threshold loop.
 *
 * Fixed point throughout, in Q8 where a fraction is carried. The only division
 * is 32-bit; nothing here allocates, sleeps, touches a register or knows what a
 * V4L2 control is, so it builds for the host as well as for the kernel.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#include "ak-venc-rc.h"

#define RC_Q8			256
#define RC_QP_MAX		51
#define RC_BPP_ANCHOR_Q16	5832U		/* 0.089 bits per pixel */
#define RC_MAD_MIN		4U
#define RC_MAD_MAX		40U
#define RC_STEP_Q8		(3 * RC_Q8)	/* per-frame QP limit, P frames */
#define RC_CPLX_DIV		4		/* inter complexity EWMA, 1/4 */
#define RC_CPLX_DIV_I		2		/* intra: one sample per GOP */
/*
 * The dead zone. A QP change costs a frame of reconstruction burst, so the loop
 * banks the signed rate error and spends it a step at a time: three QP-frames
 * of evidence buys a step up, six buys the dearer step down, and no single
 * frame contributes more than four so one outlier cannot move it on its own.
 */
#define RC_SKIP_MB_DIV		2		/* half a bit per macroblock */
#define RC_EVID_STEP_Q8		(4 * RC_Q8)
#define RC_EVID_UP_Q8		(3 * RC_Q8)
#define RC_EVID_DOWN_Q8		(6 * RC_Q8)
#define RC_EVID_CAP_Q8		(12 * RC_Q8)
#define RC_DOWN_MAX		1		/* QP a single frame may cut */
#define RC_HOLD_FRAMES		2		/* a QP cut's reconstruction burst */
#define RC_CREDIT_FRAMES	2		/* underspend the bucket may bank */

static s32 rc_clamp(s32 v, s32 lo, s32 hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

/*
 * a * b / c without a 64-bit divide. The caller keeps b and c small enough that
 * (c - 1) * b does not wrap.
 */
static u32 rc_mul_div(u32 a, u32 b, u32 c)
{
	if (!c)
		return 0;
	return (a / c) * b + ((a % c) * b) / c;
}

static unsigned rc_fls32(u32 x)
{
	unsigned n = 0;

	while (x) {
		x >>= 1;
		n++;
	}
	return n;
}

/*
 * log2(x) in Q8 for x >= 1. The integer part is the bit position; the eight
 * fractional bits come from squaring the Q30 mantissa once per bit.
 */
s32 ak_venc_log2_q8(u32 x)
{
	unsigned n, i;
	s32 frac = 0;
	u64 m;

	if (!x)
		return 0;

	n = rc_fls32(x) - 1;
	m = (n >= 30) ? ((u64)x >> (n - 30)) : ((u64)x << (30 - n));

	for (i = 0; i < 8; i++) {
		m = (m * m) >> 30;
		frac <<= 1;
		if (m >= (2ull << 30)) {
			m >>= 1;
			frac |= 1;
		}
	}

	return (s32)(n << 8) + frac;
}

/*
 * DMVPenalty1p = floor(2^((qp + 6) / 6)), 64 at QP 30 and 287 at QP 43. The
 * sixth-root steps are 16.16 fixed point.
 */
u32 ak_venc_dmv_penalty_1p(unsigned qp)
{
	static const u32 frac[6] = {
		65536, 73562, 82570, 92682, 104032, 116772
	};
	unsigned n = qp + 6;

	return ((1u << (n / 6)) * frac[n % 6]) >> 16;
}

u32 ak_venc_dmv_penalty_4p(unsigned qp)
{
	return ak_venc_dmv_penalty_1p(qp) * 31 / 32;
}

/* Two captured points, QP 30 and QP 43, linear between and flat outside. */
static u32 rc_interp_30_43(unsigned qp, u32 at30, u32 at43)
{
	u32 span = at30 - at43;

	if (qp < 30)
		qp = 30;
	if (qp > 43)
		qp = 43;

	return (at30 * 13 - span * (qp - 30) + 6) / 13;
}

u32 ak_venc_skip_penalty(unsigned qp)
{
	return rc_interp_30_43(qp, 38, 11);
}

u32 ak_venc_intra4x4_favor(unsigned qp)
{
	return rc_interp_30_43(qp, 41, 32);
}

void ak_venc_rc_defaults(struct ak_venc_rc_cfg *cfg)
{
	cfg->width = 0;
	cfg->height = 0;
	cfg->bitrate = 2000000;
	cfg->fps_num = 25;
	cfg->fps_den = 1;
	cfg->gop_len = 50;
	cfg->bucket_size = 0;
	cfg->frame_rc = 0;
	cfg->cbr = 0;
	cfg->i_weight = 6;
	cfg->intra_qp_delta = -4;
	cfg->fixed_i_qp = 30;
	cfg->fixed_p_qp = 30;
	cfg->qp_min = 20;
	cfg->qp_max = RC_QP_MAX;
	cfg->mb_rc = AK_VENC_MB_RC_MAD;
	cfg->mad_qp_delta = -2;
	cfg->mad_threshold = 12;
	cfg->mad_adapt = 1;
}

static u32 rc_fps(const struct ak_venc_rc_cfg *cfg)
{
	u32 fps = (cfg->fps_num + cfg->fps_den / 2) / cfg->fps_den;

	if (!fps)
		fps = 1;
	if (fps > 240)
		fps = 240;
	return fps;
}

static int rc_apply_cfg(struct ak_venc_rc *rc, const struct ak_venc_rc_cfg *cfg)
{
	s32 seed;
	u32 pixels;

	if (!cfg->width || !cfg->height || !cfg->fps_num || !cfg->fps_den)
		return -1;
	if (!cfg->bitrate || !cfg->gop_len || !cfg->i_weight)
		return -1;
	if (cfg->qp_min > cfg->qp_max || cfg->qp_max > RC_QP_MAX)
		return -1;
	if (cfg->fixed_i_qp > RC_QP_MAX || cfg->fixed_p_qp > RC_QP_MAX)
		return -1;
	if (cfg->mad_qp_delta < -8 || cfg->mad_qp_delta > 7)
		return -1;
	if (cfg->mad_threshold > 63)
		return -1;

	rc->cfg = *cfg;
	rc->mb_rows = (cfg->height + 15) / 16;
	rc->mb_total = ((cfg->width + 15) / 16) * rc->mb_rows;
	rc->bits_per_pic = cfg->bitrate / rc_fps(cfg);
	if (!rc->bits_per_pic)
		return -1;

	rc->bucket_size = cfg->bucket_size ? cfg->bucket_size : cfg->bitrate;
	rc->credit = rc->bits_per_pic * RC_CREDIT_FRAMES;
	if (rc->credit > rc->bucket_size)
		rc->credit = rc->bucket_size;
	rc->skip_bits = rc->mb_total / RC_SKIP_MB_DIV;

	/* QP0 = 38 - 6 * log2(bpp / 0.089), with bpp in Q16. */
	pixels = cfg->width * cfg->height;
	seed = (38 * RC_Q8) -
	       6 * (ak_venc_log2_q8(rc->bits_per_pic) - ak_venc_log2_q8(pixels) -
		    ak_venc_log2_q8(RC_BPP_ANCHOR_Q16) + (16 * RC_Q8));
	rc->seed_qp_q8 = rc_clamp(seed, (s32)cfg->qp_min * RC_Q8,
				  (s32)cfg->qp_max * RC_Q8);
	return 0;
}

int ak_venc_rc_reset(struct ak_venc_rc *rc, const struct ak_venc_rc_cfg *cfg)
{
	if (rc_apply_cfg(rc, cfg))
		return -1;

	rc->bucket = 0;
	rc->bits_per_cw_q8 = 6 * RC_Q8;
	rc->mad_threshold = cfg->mad_threshold;
	rc->prev_bits[0] = 0;
	rc->prev_bits[1] = 0;
	rc->prev_qp_q8[0] = 0;
	rc->prev_qp_q8[1] = 0;
	rc->cplx_q8[0] = 0;
	rc->cplx_q8[1] = 0;
	rc->target_bits[0] = 0;
	rc->target_bits[1] = 0;
	rc->req_qp_q8[0] = 0;
	rc->req_qp_q8[1] = 0;
	rc->have[0] = 0;
	rc->have[1] = 0;
	rc->qp_hold_q8 = rc->seed_qp_q8;
	rc->last_qp_q8 = 0;
	rc->qp_bias_q8 = 0;
	rc->evidence_q8 = 0;
	rc->hold = 0;
	rc->quiet = 0;
	return 0;
}

/* A control changed mid-stream: keep the history, re-derive the budget. */
int ak_venc_rc_reconfig(struct ak_venc_rc *rc, const struct ak_venc_rc_cfg *cfg)
{
	if (rc_apply_cfg(rc, cfg))
		return -1;

	rc->bucket = rc_clamp(rc->bucket, -(s32)rc->credit,
			      (s32)rc->bucket_size);
	rc->qp_hold_q8 = rc_clamp(rc->qp_hold_q8, (s32)cfg->qp_min * RC_Q8,
				  (s32)cfg->qp_max * RC_Q8);
	if (rc->mad_threshold < RC_MAD_MIN)
		rc->mad_threshold = RC_MAD_MIN;
	if (rc->mad_threshold > RC_MAD_MAX)
		rc->mad_threshold = RC_MAD_MAX;
	return 0;
}

/* The GOP's budget split: an I frame is worth i_weight P frames. */
static u32 rc_frame_budget(const struct ak_venc_rc *rc, int intra)
{
	u32 gop = rc->cfg.gop_len;
	u32 den = rc->cfg.i_weight + gop - 1;
	u32 p;

	p = (rc->bits_per_pic / den) * gop + ((rc->bits_per_pic % den) * gop) / den;
	if (!p)
		p = 1;
	return intra ? p * rc->cfg.i_weight : p;
}

static u32 rc_target_bits(const struct ak_venc_rc *rc, int intra)
{
	u32 base = rc_frame_budget(rc, intra);
	u32 horizon = rc_fps(&rc->cfg) / 2;
	s32 target;
	s32 lo, hi;

	if (!horizon)
		horizon = 1;

	target = (s32)base - rc->bucket / (s32)horizon;

	if (rc->cfg.cbr) {
		lo = (s32)(base - base / 4);
		hi = (s32)(base + base / 4);
	} else {
		lo = (s32)(base / 4);
		hi = (s32)(base * 4);
	}

	return (u32)rc_clamp(target, lo, hi);
}

static void rc_checkpoints(const struct ak_venc_rc *rc, u32 target,
			   struct ak_venc_rc_frame *f)
{
	static const s8 ladder[AK_VENC_RC_DELTAS] = { -3, -2, -1, 0, 1, 2, 3 };
	u32 points = rc->mb_rows > 1 ? rc->mb_rows - 1 : 1;
	u32 dist, words, t;
	unsigned i;

	if (points > AK_VENC_RC_CHECKPOINTS)
		points = AK_VENC_RC_CHECKPOINTS;

	dist = rc->mb_total / (points + 1);
	if (!dist)
		return;

	words = rc_mul_div(target, RC_Q8, rc->bits_per_cw_q8);
	t = words / (points + 1) / 2;

	f->checkpoint_distance = dist;

	for (i = 0; i < AK_VENC_RC_CHECKPOINTS; i++) {
		u32 n = i < points ? i + 1 : points;
		u32 v = (words / (points + 1)) * n / 32;

		f->checkpoint_target[i] = v > 65535 ? 65535 : (u16)v;
	}

	for (i = 0; i < AK_VENC_RC_ERRORS; i++) {
		s32 steps = (s32)i - 3;	/* -3 -2 -1 then +1 +2 +3 */

		if (steps >= 0)
			steps++;
		f->checkpoint_error[i] =
			(s16)rc_clamp(steps * (s32)(t / 4), -32768, 32767);
	}

	for (i = 0; i < AK_VENC_RC_DELTAS; i++)
		f->checkpoint_delta_qp[i] = ladder[i];
}

/*
 * The inter QP, in the units the core reports having coded at rather than the
 * ones the register asks for: the complexity estimate is built from the coded
 * mean, so a loop that compared it against INITIAL_QP would chase whatever
 * constant offset the MB stage applies until it hit a rail.
 *
 * A change of QP costs a whole frame of reconstruction burst: the reference no
 * longer matches what the next picture quantises to, so identical input codes
 * expensively once. So the loop holds the QP it has and spends it only against
 * evidence that has been in one direction for long enough - fast upwards, where
 * the frames are real bits over the budget, one step at a time downwards, and
 * not at all while a burst it caused is still in flight or while the picture is
 * coding nothing at all.
 */
static s32 rc_inter_qp(struct ak_venc_rc *rc, s32 want_q8)
{
	s32 err, steps;

	if (rc->hold || rc->quiet)
		return rc->qp_hold_q8;

	err = rc_clamp(want_q8 - rc->qp_hold_q8,
		       -RC_EVID_STEP_Q8, RC_EVID_STEP_Q8);
	rc->evidence_q8 = rc_clamp(rc->evidence_q8 + err, -RC_EVID_CAP_Q8,
				   RC_EVID_CAP_Q8);

	if (rc->evidence_q8 >= RC_EVID_UP_Q8) {
		steps = rc->evidence_q8 / RC_EVID_UP_Q8;
		if (steps * RC_Q8 > RC_STEP_Q8)
			steps = RC_STEP_Q8 / RC_Q8;
		rc->evidence_q8 -= steps * RC_EVID_UP_Q8;
		rc->qp_hold_q8 += steps * RC_Q8;
	} else if (rc->evidence_q8 <= -RC_EVID_DOWN_Q8) {
		steps = -rc->evidence_q8 / RC_EVID_DOWN_Q8;
		if (steps > RC_DOWN_MAX)
			steps = RC_DOWN_MAX;
		rc->evidence_q8 += steps * RC_EVID_DOWN_Q8;
		rc->qp_hold_q8 -= steps * RC_Q8;
		rc->hold = RC_HOLD_FRAMES;
	}

	return rc->qp_hold_q8;
}

void ak_venc_rc_before(struct ak_venc_rc *rc, int intra,
		       struct ak_venc_rc_frame *f)
{
	unsigned idx = intra ? 1 : 0;
	s32 qp_q8;
	unsigned i;

	f->qp_min = rc->cfg.qp_min;
	f->qp_max = rc->cfg.qp_max;
	f->mad_qp_delta = 0;
	f->mad_threshold = rc->mad_threshold;
	f->checkpoint_distance = 0;
	for (i = 0; i < AK_VENC_RC_CHECKPOINTS; i++)
		f->checkpoint_target[i] = 0;
	for (i = 0; i < AK_VENC_RC_ERRORS; i++)
		f->checkpoint_error[i] = 0;
	for (i = 0; i < AK_VENC_RC_DELTAS; i++)
		f->checkpoint_delta_qp[i] = 0;

	f->target_bits = rc_target_bits(rc, intra);
	rc->target_bits[idx] = f->target_bits;

	if (!rc->cfg.frame_rc) {
		qp_q8 = (s32)(intra ? rc->cfg.fixed_i_qp : rc->cfg.fixed_p_qp) *
			RC_Q8;
	} else if (rc->have[idx]) {
		qp_q8 = 6 * (rc->cplx_q8[idx] -
			     ak_venc_log2_q8(f->target_bits));

		if (!intra)
			qp_q8 = rc_inter_qp(rc, qp_q8);
	} else if (intra) {
		qp_q8 = (rc->have[0] ? rc->qp_hold_q8 : rc->seed_qp_q8) +
			rc->cfg.intra_qp_delta * RC_Q8;
	} else {
		qp_q8 = rc->have[1] ?
			rc->prev_qp_q8[1] - rc->cfg.intra_qp_delta * RC_Q8 :
			rc->seed_qp_q8;
	}

	if (!intra && rc->cfg.frame_rc) {
		rc->qp_hold_q8 = rc_clamp(qp_q8, (s32)rc->cfg.qp_min * RC_Q8,
					  (s32)rc->cfg.qp_max * RC_Q8);
		qp_q8 = rc->qp_hold_q8 - rc->qp_bias_q8;

		/*
		 * The reference carries the quantisation of the frame that
		 * built it, and an intra sized to its own share of the GOP is
		 * often several QP coarser than the inter frames want. Asking
		 * for more than a step below what the reference holds is a
		 * whole picture of residual in one frame, so the walk down is
		 * paced by the step limit and the frames that pace it are burst
		 * frames, not measurements.
		 */
		if (qp_q8 < rc->last_qp_q8 - RC_STEP_Q8) {
			qp_q8 = rc->last_qp_q8 - RC_STEP_Q8;
			rc->hold = RC_HOLD_FRAMES;
		}
	}

	qp_q8 = rc_clamp(qp_q8, (s32)rc->cfg.qp_min * RC_Q8,
			 (s32)rc->cfg.qp_max * RC_Q8);
	rc->req_qp_q8[idx] = qp_q8;
	f->qp = (unsigned)(qp_q8 >> 8);

	switch (rc->cfg.mb_rc) {
	case AK_VENC_MB_RC_MAD:
		f->mad_qp_delta = rc->cfg.mad_qp_delta;
		break;
	case AK_VENC_MB_RC_CHECKPOINT:
		rc_checkpoints(rc, f->target_bits, f);
		break;
	case AK_VENC_MB_RC_OFF:
		break;
	}
}

void ak_venc_rc_after(struct ak_venc_rc *rc, int intra,
		      const struct ak_venc_rc_result *r)
{
	unsigned idx = intra ? 1 : 0;
	unsigned learn;
	s32 drift;

	if (!r->mb_count)
		return;

	rc->prev_bits[idx] = r->bits;
	rc->prev_qp_q8[idx] = (s32)((r->qp_sum_div2 * 2 * RC_Q8) / r->mb_count);
	rc->qp_bias_q8 += (rc->prev_qp_q8[idx] - rc->req_qp_q8[idx] -
			   rc->qp_bias_q8) / 4;
	rc->last_qp_q8 = rc->prev_qp_q8[idx];

	/*
	 * A picture at or below the skip floor put nothing but headers on the
	 * wire: the scene had no content left to code at this QP, which says
	 * nothing about how expensive content is, and a frame still carrying the
	 * burst of the loop's own last QP cut says only how big that cut was.
	 * Neither is a measurement of the source, so neither trains the estimate.
	 */
	learn = r->bits > (intra ? 0 : rc->skip_bits);
	if (!intra) {
		rc->quiet = !learn;
		if (rc->hold) {
			learn = 0;
			rc->hold--;
		}
	}

	if (learn) {
		s32 cplx = ak_venc_log2_q8(r->bits) + rc->prev_qp_q8[idx] / 6;

		if (rc->have[idx])
			rc->cplx_q8[idx] += (cplx - rc->cplx_q8[idx]) /
					    (intra ? RC_CPLX_DIV_I :
						     RC_CPLX_DIV);
		else
			rc->cplx_q8[idx] = cplx;
		rc->have[idx] = 1;
	}

	/* The picture the intra left behind is not the one the loop was holding
	 * against, so the inter frame that follows it is a burst too. */
	if (intra)
		rc->hold = 1;

	/*
	 * Underspend on a scene with nothing to code is not saved bitrate: there
	 * is no later frame that can spend it, and a bucket that banks it drags
	 * the target up and the QP down until the scene comes back and cannot be
	 * absorbed. So the credit side is bounded to a couple of frames while
	 * the debit side keeps the whole CPB.
	 */
	drift = rc->bucket + (s32)r->bits - (s32)rc->bits_per_pic;
	rc->bucket = rc_clamp(drift, -(s32)rc->credit, (s32)rc->bucket_size);

	/* Only inter frames carry the statistics the next inter frame will see. */
	if (!intra && r->rlc_div4) {
		u32 q8 = rc_mul_div(r->bits, RC_Q8, r->rlc_div4 * 4);

		if (q8)
			rc->bits_per_cw_q8 += ((s32)q8 -
					       (s32)rc->bits_per_cw_q8) / 4;
	}

	if (rc->cfg.mb_rc == AK_VENC_MB_RC_MAD && rc->cfg.mad_adapt) {
		if (r->mad_under * 4 < r->mb_count) {
			if (rc->mad_threshold < RC_MAD_MAX)
				rc->mad_threshold++;
		} else if (r->mad_under * 2 > r->mb_count) {
			if (rc->mad_threshold > RC_MAD_MIN)
				rc->mad_threshold--;
		}
	}
}
