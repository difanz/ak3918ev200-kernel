// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The 8290's register layer: field addressing, the H.264 configuration, the
 * frame stores and the input/output addresses.
 *
 * Stage-one port of lib/video/akcam_h8290.c from the akcam userspace tree,
 * which reproduces the vendor register capture field by field and encoded a
 * decodable 720p IDR on this hardware. Task 4 replaces this copy with the
 * shared sources and their host test.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#include <linux/kernel.h>
#include <linux/types.h>

#include "ak-venc.h"

static const struct akcam_h8290_field *field_of(unsigned field)
{
	const struct akcam_h8290_field *f;

	if (field >= ARRAY_SIZE(akcam_h8290_fields))
		return NULL;
	f = &akcam_h8290_fields[field];
	/* A zero mask is a gap in the table, not a field that happens to be off. */
	if (!f->mask || f->reg >= AKCAM_H8290_REGISTERS)
		return NULL;
	return f;
}

void akcam_h8290_set(struct akcam_h8290 *h, unsigned field, u32 value)
{
	const struct akcam_h8290_field *f = field_of(field);

	if (!h || !f)
		return;
	h->reg[f->reg] = (h->reg[f->reg] & ~f->mask) |
			 ((value << f->shift) & f->mask);
}

u32 akcam_h8290_get(const struct akcam_h8290 *h, unsigned field)
{
	const struct akcam_h8290_field *f = field_of(field);

	if (!h || !f)
		return 0;
	return (h->reg[f->reg] & f->mask) >> f->shift;
}

int akcam_h8290_reg_of(unsigned field)
{
	const struct akcam_h8290_field *f = field_of(field);

	return f ? (int)f->reg : -1;
}

u32 akcam_h8290_extract(unsigned field, u32 value)
{
	const struct akcam_h8290_field *f = field_of(field);

	if (!f)
		return 0;
	return (value & f->mask) >> f->shift;
}

static void write_quant_bank(void (*write)(void *, unsigned, u32),
			     u32 (*read)(void *, unsigned), void *context,
			     const u32 *reg, unsigned first, unsigned last)
{
	unsigned n;

	for (n = first; n <= last; n++) {
		write(context, n * 4, reg[n]);
		if (read)
			(void)read(context, n * 4);
	}
}

void akcam_h8290_write_frame(const struct akcam_h8290 *h,
			     void (*write)(void *, unsigned, u32),
			     u32 (*read)(void *, unsigned),
			     void *context)
{
	unsigned n;

	if (!h || !write)
		return;

	/*
	 * The enable register is written twice: here with its enable bit clear,
	 * and again at the end with it set. It carries the encoding mode as
	 * well as the start bit, and the core has to be in JPEG mode on the bus
	 * before the quantiser bank is written or the bank is not the quantiser
	 * that frame is coded with - it holds the values and the DCT stage
	 * never samples them.
	 */
	for (n = AKCAM_H8290_FIRST_WRITTEN; n <= AKCAM_H8290_LAST_WRITTEN; n++)
		write(context, n * 4,
		      n == AKCAM_H8290_ENABLE_REG ? h->reg[n] & ~1u : h->reg[n]);

	/*
	 * Luma as one pass and chroma as another, each write retired by a read
	 * of the same address. Writes to this bank are posted, and a burst of
	 * them all lands at whichever address was latched last. The registers
	 * below 0x100 do not need it.
	 */
	if (akcam_h8290_get(h, H8290_ENCODING_MODE) == AKCAM_H8290_MODE_JPEG) {
		write_quant_bank(write, read, context, h->reg,
				 AKCAM_H8290_QUANT_FIRST_REG,
				 AKCAM_H8290_QUANT_LUMA_LAST_REG);
		write_quant_bank(write, read, context, h->reg,
				 AKCAM_H8290_QUANT_CHROMA_FIRST_REG,
				 AKCAM_H8290_QUANT_LAST_REG);
	}

	write(context, AKCAM_H8290_ENABLE_REG * 4,
	      h->reg[AKCAM_H8290_ENABLE_REG] | 1u);
}

enum akcam_h8290_status akcam_h8290_status(u32 swreg1)
{
	const struct akcam_h8290_field *f;

	/* The library's order, which is not the bit order: a frame that
	 * completed and faulted reports the fault. */
	f = field_of(H8290_IRQ_BUS_ERROR_OR_TIMEOUT_STATUS_BIT);
	if (f && (swreg1 & f->mask))
		return AKCAM_H8290_BUS_ERROR;

	f = field_of(H8290_IRQ_SW_RESET_STATUS_BIT);
	if (f && (swreg1 & f->mask))
		return AKCAM_H8290_SW_RESET;

	f = field_of(H8290_IRQ_FRAME_READY_STATUS_BIT);
	if (f && (swreg1 & f->mask))
		return AKCAM_H8290_FRAME_READY;

	f = field_of(H8290_IRQ_BUFFER_FULL_STATUS_BIT);
	if (f && (swreg1 & f->mask))
		return AKCAM_H8290_BUFFER_FULL;

	return AKCAM_H8290_RUNNING;
}

void akcam_h8290_h264_defaults(struct akcam_h8290_h264 *c)
{
	if (!c)
		return;
	c->width = AK_VENC_DEF_WIDTH;
	c->height = AK_VENC_DEF_HEIGHT;
	c->input_row_length = 0;
	c->intra = 1;
	c->initial_qp = AK_VENC_DEF_QP;
	c->qp_min = AK_VENC_DEF_QP_MIN;
	c->qp_max = AK_VENC_DEF_QP_MAX;
	c->pps_qp = AK_VENC_DEF_QP;
	c->frame_num = 0;
	c->idr_pic_id = 0;
	c->chroma_qp_index_offset = 2;
	c->deblocking_mode = 0;
	c->constrained_intra_pred = 0;
	c->filter_alpha_offset_div2 = 0;
	c->filter_beta_offset_div2 = 0;
	c->mad_qp_delta = 0;
	c->mad_threshold = 8;
	c->checkpoint_distance = 0;
	c->checkpoint_target = NULL;
	c->checkpoint_error = NULL;
	c->checkpoint_delta_qp = NULL;
}

static void config_checkpoints(struct akcam_h8290 *h,
			       const struct akcam_h8290_h264 *c)
{
	static const unsigned target_field[AK_VENC_RC_CHECKPOINTS] = {
		H8290_CHECKPOINT_1_WORD_TARGET_USAGE_DIV32_0,
		H8290_CHECKPOINT_2_WORD_TARGET_USAGE_DIV32_0,
		H8290_CHECKPOINT_3_WORD_TARGET_USAGE_DIV32_0,
		H8290_CHECKPOINT_4_WORD_TARGET_USAGE_DIV32_0,
		H8290_CHECKPOINT_5_WORD_TARGET_USAGE_DIV32_0,
		H8290_CHECKPOINT_6_WORD_TARGET_USAGE_DIV32_0,
		H8290_CHECKPOINT_7_WORD_TARGET_USAGE_DIV32_0,
		H8290_CHECKPOINT_8_WORD_TARGET_USAGE_DIV32_0,
		H8290_CHECKPOINT_9_WORD_TARGET_USAGE_DIV32_0,
		H8290_CHECKPOINT_10_WORD_TARGET_USAGE_DIV32_0
	};
	static const unsigned error_field[AK_VENC_RC_ERRORS] = {
		H8290_CHECKPOINT_WORD_ERROR_1_DIV4_32768,
		H8290_CHECKPOINT_WORD_ERROR_2_DIV4_32768,
		H8290_CHECKPOINT_WORD_ERROR_3_DIV4_32768,
		H8290_CHECKPOINT_WORD_ERROR_4_DIV4_32768,
		H8290_CHECKPOINT_WORD_ERROR_5_DIV4_32768,
		H8290_CHECKPOINT_WORD_ERROR_6_DIV4_32768
	};
	static const unsigned delta_field[AK_VENC_RC_DELTAS] = {
		H8290_CHECKPOINT_DELTA_QP_1_8,
		H8290_CHECKPOINT_DELTA_QP_2_8,
		H8290_CHECKPOINT_DELTA_QP_3_8,
		H8290_CHECKPOINT_DELTA_QP_4_8,
		H8290_CHECKPOINT_DELTA_QP_5_8,
		H8290_CHECKPOINT_DELTA_QP_6_8,
		H8290_CHECKPOINT_DELTA_QP_7_8
	};
	unsigned i;

	akcam_h8290_set(h, H8290_CHECKPOINT_DISTANCE_MB_0_DISABLED_0,
			c->checkpoint_distance);
	if (!c->checkpoint_distance)
		return;

	for (i = 0; i < AK_VENC_RC_CHECKPOINTS; i++)
		akcam_h8290_set(h, target_field[i], c->checkpoint_target[i]);
	for (i = 0; i < AK_VENC_RC_ERRORS; i++)
		akcam_h8290_set(h, error_field[i], (u32)c->checkpoint_error[i]);
	for (i = 0; i < AK_VENC_RC_DELTAS; i++)
		akcam_h8290_set(h, delta_field[i],
				(u32)c->checkpoint_delta_qp[i]);
}

int akcam_h8290_config_h264(struct akcam_h8290 *h,
			    const struct akcam_h8290_h264 *c)
{
	unsigned mbw, mbh, row, penalty;

	if (!h || !c || !c->width || !c->height)
		return -1;
	if (c->width & 3)		/* the right-edge overfill counts by 4 */
		return -1;
	if (c->initial_qp > 51 || c->pps_qp > 51)
		return -1;
	if (c->mad_qp_delta < -8 || c->mad_qp_delta > 7)
		return -1;
	if (c->mad_threshold > 63)
		return -1;
	if (c->checkpoint_distance &&
	    (!c->checkpoint_target || !c->checkpoint_error ||
	     !c->checkpoint_delta_qp))
		return -1;

	mbw = (c->width + 15) / 16;
	mbh = (c->height + 15) / 16;
	row = c->input_row_length ? c->input_row_length : c->width;

	/* --- the core itself ---------------------------------------------- */

	akcam_h8290_set(h, H8290_BURST_LENGTH, 16);
	akcam_h8290_set(h, H8290_ENABLE_CLOCK_GATING, 1);
	/* A little-endian host reading a big-endian core without these gets a
	 * byte-reversed stream, which looks like corruption, not misconfig. */
	akcam_h8290_set(h, H8290_ENABLE_INPUT_SWAP_8_BITS, 1);
	akcam_h8290_set(h, H8290_ENABLE_INPUT_SWAP_16_BITS, 1);
	akcam_h8290_set(h, H8290_ENABLE_INPUT_SWAP_32_BITS, 1);
	akcam_h8290_set(h, H8290_ENABLE_OUTPUT_SWAP_8_BITS, 1);
	akcam_h8290_set(h, H8290_ENABLE_OUTPUT_SWAP_16_BITS, 1);
	akcam_h8290_set(h, H8290_ENABLE_OUTPUT_SWAP_32_BITS, 1);

	akcam_h8290_set(h, H8290_ENCODING_MODE, AKCAM_H8290_MODE_H264);
	akcam_h8290_set(h, H8290_ENCODED_PICTURE_TYPE, c->intra ? 1u : 0u);

	akcam_h8290_set(h, H8290_ENCODED_WIDTH, mbw);
	akcam_h8290_set(h, H8290_ENCODED_HEIGHT, mbh);

	akcam_h8290_set(h,
		H8290_ENABLE_WRITING_SIZE_OF_EACH_NAL_UNIT_TO_BASECONTROL_NALS, 1);
	akcam_h8290_set(h, H8290_ENABLE_INTERRUPT_FOR_TIMEOUT, 1);

	/* --- geometry ----------------------------------------------------- */

	akcam_h8290_set(h, H8290_INPUT_LUMINANCE_ROW_LENGTH, row);
	/* This has to agree with the SPS cropping window, which counts in
	 * chroma units: 360 rows overfill by 8 here and crop by 4 there. */
	akcam_h8290_set(h, H8290_OVERFILL_PIXELS_ON_RIGHT_EDGE_OF_IMAGE_DIV4_0,
			(mbw * 16 - c->width) / 4);
	akcam_h8290_set(h, H8290_OVERFILL_PIXELS_ON_BOTTOM_EDGE_OF_IMAGE,
			mbh * 16 - c->height);
	akcam_h8290_set(h, H8290_INPUT_IMAGE_FORMAT, 0);	/* I420 */
	akcam_h8290_set(h, H8290_INPUT_IMAGE_ROTATION, 0);

	/* --- picture and slice parameters ---------------------------------- */

	/* The PPS is emitted once a GOP, so this is not the frame's QP. */
	akcam_h8290_set(h, H8290_H_264_PIC_INIT_QP_IN_PPS_0_51, c->pps_qp);
	/* The signed fields are narrow two's complement and set() masks to the
	 * field, so a negative value carries its low bits across as it is. */
	akcam_h8290_set(h, H8290_H_264_CHROMA_QP_INDEX_OFFSET_12_12,
			(u32)c->chroma_qp_index_offset);
	akcam_h8290_set(h, H8290_H_264_SLICE_FILTER_ALPHA_C0_OFFSET_DIV2_6_6,
			(u32)c->filter_alpha_offset_div2);
	akcam_h8290_set(h, H8290_H_264_SLICE_FILTER_BETA_OFFSET_DIV2_6_6,
			(u32)c->filter_beta_offset_div2);
	akcam_h8290_set(h, H8290_H_264_DEBLOCKING_FILTER_MODE, c->deblocking_mode);
	akcam_h8290_set(h, H8290_H_264_CONSTRAINED_INTRA_PREDICTION_ENABLE,
			c->constrained_intra_pred);
	akcam_h8290_set(h, H8290_H_264_PIC_PARAMETER_SET_ID, 0);

	/* The core writes the slice header; this is how it learns what to put
	 * in it. */
	akcam_h8290_set(h, H8290_H_264_FRAME_NUM, c->frame_num);
	akcam_h8290_set(h, H8290_H_264_IDR_PICTURE_ID, c->idr_pic_id);

	akcam_h8290_set(h, H8290_H_264_SLICE_SIZE, 0);	/* one slice a picture */

	akcam_h8290_set(h, H8290_H_264_CABAC_ENABLE, 0);
	akcam_h8290_set(h, H8290_H_264_CABAC_INITIAL_IDC, 0);
	akcam_h8290_set(h, H8290_H_264_TRANSFORM_8X8_ENABLE, 0);

	akcam_h8290_set(h, H8290_H_264_STREAM_MODE, 0);	/* NAL unit stream */

	/* --- motion estimation --------------------------------------------- */

	penalty = ak_venc_dmv_penalty_1p(c->initial_qp);
	akcam_h8290_set(h, H8290_H_264_DISABLE_QUARTER_PIXEL_MVS, 1);
	akcam_h8290_set(h, H8290_H_264_INTER_4X4_MODE_RESTRICTION, 1);
	akcam_h8290_set(h, H8290_H_264_DIFFERENTIAL_MV_PENALTY_FOR_1P_QP_ME,
			penalty);
	akcam_h8290_set(h, H8290_H_264_DIFFERENTIAL_MV_PENALTY_FOR_4P_ME,
			ak_venc_dmv_penalty_4p(c->initial_qp));
	akcam_h8290_set(h, H8290_H_264_SKIP_MACROBLOCK_MODE_PENALTY,
			ak_venc_skip_penalty(c->initial_qp));
	akcam_h8290_set(h, H8290_H_264_INTER_MB_MODE_FAVOR_IN_INTRA_INTER, 335);
	akcam_h8290_set(h, H8290_H_264_INTRA_PREDICTION_PREVIOUS_4X4_MODE_FAVOR,
			ak_venc_intra4x4_favor(c->initial_qp));
	akcam_h8290_set(h, H8290_H_264_INTRA_PREDICTION_INTRA_16X16_MODE_FAVOR, 3335);

	/* --- rate control, as much of it as lives in the core -------------- */

	akcam_h8290_set(h, H8290_INITIAL_QP, c->initial_qp);
	akcam_h8290_set(h, H8290_MAXIMUM_QP, c->qp_max);
	akcam_h8290_set(h, H8290_MINIMUM_QP, c->qp_min);

	config_checkpoints(h, c);

	/* madQpChange is a signed 4-bit field: -3 is written as 13. */
	akcam_h8290_set(h, H8290_MAD_BASED_QP_ADJUSTMENT, (u32)c->mad_qp_delta);
	akcam_h8290_set(h, H8290_MAD_THRESHOLD_DIV256, c->mad_threshold);

	/* --- regions, all disabled ----------------------------------------- */

	/* Empty rather than flagged off: the right column and bottom row are
	 * exclusive bounds, so left == right encloses nothing. */
	akcam_h8290_set(h, H8290_INTRA_AREA_LEFT_MB_COLUMN_INSIDE_AREA_0, mbw);
	akcam_h8290_set(h, H8290_INTRA_AREA_RIGHT_MB_COLUMN_OUTSIDE_AREA_0, mbw);
	akcam_h8290_set(h, H8290_INTRA_AREA_TOP_MB_ROW_INSIDE_AREA_0, mbh);
	akcam_h8290_set(h, H8290_INTRA_AREA_BOTTOM_MB_ROW_OUTSIDE_AREA_0, mbh);

	akcam_h8290_set(h, H8290_2ND_ROI_AREA_LEFT_MB_COLUMN_INSIDE_AREA, mbw);
	akcam_h8290_set(h, H8290_2ND_ROI_AREA_RIGHT_MB_COLUMN_INSIDE_AREA, mbw);
	akcam_h8290_set(h, H8290_2ND_ROI_AREA_TOP_MB_ROW_INSIDE_AREA, mbh);
	akcam_h8290_set(h, H8290_2ND_ROI_AREA_BOTTOM_MB_ROW_INSIDE_AREA, mbh);

	akcam_h8290_set(h, H8290_1ST_ROI_AREA_DELTA_QP, 0);
	akcam_h8290_set(h, H8290_2ND_ROI_AREA_DELTA_QP, 0);

	akcam_h8290_set(h, H8290_MVC_VIEW_ID_0, 1);
	akcam_h8290_set(h, H8290_MVC_ANCHOR_PIC_FLAG, 1);

	return 0;
}

/* The core codes whole macroblocks, so the frame stores are the padded size. */
static void frame_planes(unsigned width, unsigned height,
			 size_t *luma, size_t *chroma)
{
	size_t w = ((width + 15) / 16) * 16;
	size_t h = ((height + 15) / 16) * 16;

	*luma = w * h;
	*chroma = w * h / 2;
}

size_t akcam_h8290_frames_bytes(unsigned width, unsigned height)
{
	size_t luma, chroma;

	if (!width || !height)
		return 0;
	frame_planes(width, height, &luma, &chroma);
	return 2 * luma + 2 * chroma;
}

int akcam_h8290_frames_init(struct akcam_h8290_frames *f, u32 base,
			    unsigned width, unsigned height)
{
	if (!f || !width || !height)
		return -1;

	f->base = base;
	frame_planes(width, height, &f->luma, &f->chroma);
	f->recon = 1;
	return 0;
}

void akcam_h8290_frames_apply(const struct akcam_h8290_frames *f,
			      struct akcam_h8290 *h)
{
	u32 chroma_base;
	unsigned ref;

	if (!f || !h)
		return;

	ref = f->recon ^ 1u;
	chroma_base = f->base + (u32)(2 * f->luma);

	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_REFERENCE_LUMA,
			f->base + (u32)(f->luma * ref));
	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_RECONSTRUCTED_LUMA,
			f->base + (u32)(f->luma * f->recon));
	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_REFERENCE_CHROMA,
			chroma_base + (u32)(f->chroma * ref));
	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_RECONSTRUCTED_CHROMA,
			chroma_base + (u32)(f->chroma * f->recon));
}

void akcam_h8290_frames_advance(struct akcam_h8290_frames *f)
{
	if (f)
		f->recon ^= 1u;
}

size_t akcam_h8290_input_bytes(unsigned row_length, unsigned height)
{
	size_t luma;

	if (!row_length || !height || (row_length & 1) || (height & 1))
		return 0;

	luma = (size_t)row_length * height;
	return luma + luma / 2;
}

int akcam_h8290_input_apply(struct akcam_h8290 *h, u32 base,
			    unsigned row_length, unsigned height)
{
	size_t luma;

	if (!h || !akcam_h8290_input_bytes(row_length, height))
		return -1;

	/* The real height, not the padded one: this is what capture wrote. */
	luma = (size_t)row_length * height;

	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_INPUT_PICTURE_LUMA, base);
	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_INPUT_PICTURE_CB,
			base + (u32)luma);
	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_INPUT_PICTURE_CR,
			base + (u32)(luma + luma / 4));
	return 0;
}

int akcam_h8290_output_apply(struct akcam_h8290 *h,
			     u32 stream_base, size_t stream_bytes,
			     u32 control_base, u32 cabac_base)
{
	if (!h || !stream_bytes || (stream_bytes & 7))
		return -1;

	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_OUTPUT_STREAM_DATA, stream_base);
	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_OUTPUT_CONTROL_DATA, control_base);
	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_CABAC_CONTEXT_TABLES, cabac_base);

	/* The limit counts 64-bit words going in; read back after a frame the
	 * same register is the encoded size in bits. */
	akcam_h8290_set(h,
		H8290_STREAM_BUFFER_LIMIT_64BIT_ADDRESSES_OUTPUT_STREAM_SIZE_B,
		(u32)(stream_bytes / 8));
	return 0;
}

size_t akcam_h8290_output_bytes(u32 swreg24)
{
	return ((size_t)swreg24 + 7) / 8;
}
