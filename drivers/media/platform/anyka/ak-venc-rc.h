/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Rate control for the 8290: one QP per frame, the core's checkpoint ladder and
 * the MAD threshold loop. Integer only, no register access, no V4L2.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#ifndef AK_VENC_RC_H
#define AK_VENC_RC_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#endif

#define AK_VENC_RC_CHECKPOINTS	10
#define AK_VENC_RC_ERRORS	6
#define AK_VENC_RC_DELTAS	7

/* Per-macroblock QP authority: the two stages are alternatives. */
enum ak_venc_mb_rc_mode {
	AK_VENC_MB_RC_OFF,
	AK_VENC_MB_RC_MAD,
	AK_VENC_MB_RC_CHECKPOINT
};

struct ak_venc_rc_cfg {
	u32 width, height;
	u32 bitrate;			/* bits per second */
	u32 fps_num, fps_den;		/* frames per second = num / den */
	u32 gop_len;
	u32 bucket_size;		/* bits; 0 means one second of bitrate */
	u32 peak_bitrate;		/* VBR ceiling; 0 means 4x the target */
	unsigned frame_rc;		/* 0 pins the QP to fixed_i_qp/fixed_p_qp */
	unsigned cbr;
	unsigned i_weight;
	int intra_qp_delta;
	unsigned fixed_i_qp, fixed_p_qp;
	unsigned qp_min, qp_max;
	enum ak_venc_mb_rc_mode mb_rc;
	int mad_qp_delta;		/* -8..7 */
	unsigned mad_threshold;		/* 0..63, the seed of the loop */
	unsigned mad_adapt;
};

struct ak_venc_rc {
	struct ak_venc_rc_cfg cfg;

	u32 mb_total, mb_rows;
	u32 bits_per_pic;
	u32 bucket_size;
	u32 credit;			/* how far the bucket may run negative */
	s32 bucket;
	s32 seed_qp_q8;

	u32 prev_bits[2];		/* [0] inter, [1] intra */
	s32 prev_qp_q8[2];
	s32 req_qp_q8[2];		/* what was asked for, before the MB stage */
	u32 target_bits[2];		/* what the last frame of each type asked for */
	s32 cplx_q8[2];			/* log2 bits at QP 0, smoothed, Q8 */
	unsigned have[2];

	s32 qp_hold_q8;			/* the inter QP the loop is holding */
	s32 last_qp_q8;			/* what the reference picture was coded at */
	s32 qp_bias_q8;			/* coded QP minus asked-for QP, smoothed */
	s32 evidence_q8;		/* sustained rate error, signed, QP units */
	u32 skip_bits;			/* at or below this a picture coded nothing */
	unsigned hold;			/* frames of reconstruction burst still owed */
	unsigned quiet;			/* the last inter frame coded nothing */

	u32 bits_per_cw_q8;
	unsigned mad_threshold;
};

/* What one frame needs written into the register file. */
struct ak_venc_rc_frame {
	unsigned qp;
	unsigned qp_min, qp_max;
	u32 target_bits;
	int mad_qp_delta;
	unsigned mad_threshold;
	unsigned checkpoint_distance;	/* 0 disables the stage */
	u16 checkpoint_target[AK_VENC_RC_CHECKPOINTS];
	s16 checkpoint_error[AK_VENC_RC_ERRORS];
	s8  checkpoint_delta_qp[AK_VENC_RC_DELTAS];
};

/* What the core reported for the frame just completed. */
struct ak_venc_rc_result {
	u32 bits;
	u32 qp_sum_div2;
	u32 mb_count;
	u32 rlc_div4;
	u32 mad_under;
};

void ak_venc_rc_defaults(struct ak_venc_rc_cfg *cfg);
int  ak_venc_rc_reset(struct ak_venc_rc *rc, const struct ak_venc_rc_cfg *cfg);
int  ak_venc_rc_reconfig(struct ak_venc_rc *rc, const struct ak_venc_rc_cfg *cfg);
void ak_venc_rc_before(struct ak_venc_rc *rc, int intra,
		       struct ak_venc_rc_frame *f);
void ak_venc_rc_after(struct ak_venc_rc *rc, int intra,
		      const struct ak_venc_rc_result *r);

/* Q8 base-2 logarithm, and the QP-dependent motion-estimation registers. */
s32 ak_venc_log2_q8(u32 x);
u32 ak_venc_dmv_penalty_1p(unsigned qp);
u32 ak_venc_dmv_penalty_4p(unsigned qp);
u32 ak_venc_skip_penalty(unsigned qp);
u32 ak_venc_intra4x4_favor(unsigned qp);

#endif
