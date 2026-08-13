// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The half of the bitstream the 8290 does not write: the bit writer, the
 * sequence and picture parameter sets, and their Annex B framing.
 *
 * Stage-one port of lib/video/akcam_bits.c and lib/video/akcam_h264_hdr.c,
 * which have host tests against the standard's own tables and whose output
 * ffmpeg decoded from this core with zero errors. Task 4 replaces this copy
 * with the shared sources.
 *
 * Two values here are forced by the core rather than chosen:
 * log2_max_frame_num_minus4 = 12, because the core emits frame_num in sixteen
 * bits, and pic_order_cnt_type = 2, because the core writes no picture order
 * syntax at all.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>

#include "ak-venc.h"

void akcam_bits_init(struct akcam_bits *b, u8 *buf, size_t capacity)
{
	b->buf = buf;
	b->capacity = capacity;
	b->bit = 0;
	b->overflow = 0;
	if (buf && capacity)
		memset(buf, 0, capacity);
}

void akcam_bits_put(struct akcam_bits *b, unsigned n, u32 value)
{
	unsigned i;

	if (b->overflow)
		return;
	if (!n || n > 32) {
		b->overflow = 1;
		return;
	}
	if (b->bit + n > b->capacity * 8) {
		b->overflow = 1;
		return;
	}

	for (i = 0; i < n; i++) {
		unsigned src = n - 1 - i;

		if ((value >> src) & 1u)
			b->buf[b->bit >> 3] |= (u8)(0x80u >> (b->bit & 7));
		b->bit++;
	}
}

void akcam_bits_put1(struct akcam_bits *b, int flag)
{
	akcam_bits_put(b, 1, flag ? 1u : 0u);
}

void akcam_bits_ue(struct akcam_bits *b, u32 value)
{
	unsigned leading = 0;
	u32 v;

	if (b->overflow)
		return;
	if (value == 0xffffffffu) {
		b->overflow = 1;
		return;
	}

	v = value + 1u;
	/* Bounded at 31 because v >> 32 is undefined and on ARM returns v, so
	 * the largest encodable value would spin here for ever. */
	while (leading < 31 && (v >> (leading + 1)) != 0)
		leading++;

	if (leading)
		akcam_bits_put(b, leading, 0);
	akcam_bits_put(b, leading + 1, v);
}

void akcam_bits_se(struct akcam_bits *b, s32 value)
{
	u32 code;

	if (value > 0)
		code = ((u32)value << 1) - 1u;
	else
		code = (u32)(-(s64)value) << 1;

	akcam_bits_ue(b, code);
}

void akcam_bits_rbsp_trailing(struct akcam_bits *b)
{
	akcam_bits_put1(b, 1);
	while (b->bit & 7)
		akcam_bits_put1(b, 0);
}

size_t akcam_bits_length(const struct akcam_bits *b)
{
	if (b->overflow)
		return 0;
	return (b->bit + 7) >> 3;
}

size_t akcam_bits_escape(const u8 *rbsp, size_t len, u8 *out, size_t out_cap)
{
	size_t i, n = 0;
	unsigned zeroes = 0;

	if (!rbsp || !out)
		return 0;

	for (i = 0; i < len; i++) {
		/* The counter resets after an escape, which is why 00 00 00 00
		 * needs two escapes and not one. */
		if (zeroes >= 2 && rbsp[i] <= 3) {
			if (n >= out_cap)
				return 0;
			out[n++] = 0x03;
			zeroes = 0;
		}
		if (n >= out_cap)
			return 0;
		out[n++] = rbsp[i];
		zeroes = rbsp[i] == 0 ? zeroes + 1 : 0;
	}
	return n;
}

void akcam_h264_sps_defaults(struct akcam_h264_sps *s)
{
	memset(s, 0, sizeof(*s));
	s->profile_idc = 66;			/* Baseline */
	s->level_idc = 30;			/* 3.0 */
	s->constraint_set0 = 1;
	s->constraint_set1 = 1;
	s->constraint_set2 = 1;
	s->log2_max_frame_num_minus4 = 12;
	s->pic_order_cnt_type = 2;
	s->num_ref_frames = 1;
	s->frame_mbs_only = 1;
	s->direct_8x8_inference = 1;
	s->width_mbs_minus1 = 10;
	s->height_map_units_minus1 = 8;
	s->vui.bitstream_restriction = 1;
	s->vui.max_dec_frame_buffering = s->num_ref_frames;
}

void akcam_h264_pps_defaults(struct akcam_h264_pps *p)
{
	memset(p, 0, sizeof(*p));
	p->chroma_qp_index_offset = 2;
	p->deblocking_filter_control_present = 1;
}

int akcam_h264_sps_geometry(struct akcam_h264_sps *s,
			    unsigned width, unsigned height)
{
	unsigned mbw, mbh;

	/* The crop offsets count two luma samples for 4:2:0, so an odd size has
	 * no expressible cropping window. */
	if (!width || !height || (width & 1) || (height & 1))
		return -1;

	mbw = (width + 15) / 16;
	mbh = (height + 15) / 16;

	if (mbw > 1024 || mbh > 1024)
		return -1;

	s->width_mbs_minus1 = mbw - 1;
	s->height_map_units_minus1 = mbh - 1;

	s->crop_left = 0;
	s->crop_top = 0;
	s->crop_right = (mbw * 16 - width) / 2;
	s->crop_bottom = (mbh * 16 - height) / 2;
	s->crop = (s->crop_right || s->crop_bottom) ? 1 : 0;
	return 0;
}

static int vui_present(const struct akcam_h264_vui *v)
{
	return v->time_scale || v->bitstream_restriction || v->video_full_range ||
	       v->sar_width || v->pic_struct_present;
}

static void write_vui(struct akcam_bits *b, const struct akcam_h264_vui *v)
{
	if (v->sar_width && v->sar_height) {
		akcam_bits_put1(b, 1);
		if (v->sar_width == v->sar_height) {
			akcam_bits_put(b, 8, 1);	/* idc 1, square */
		} else {
			akcam_bits_put(b, 8, 255);	/* Extended_SAR */
			akcam_bits_put(b, 16, v->sar_width);
			akcam_bits_put(b, 16, v->sar_height);
		}
	} else {
		akcam_bits_put1(b, 0);
	}

	akcam_bits_put1(b, 0);			/* overscan_info_present_flag */

	if (v->video_full_range) {
		akcam_bits_put1(b, 1);		/* video_signal_type_present */
		akcam_bits_put(b, 3, 5);	/* video_format: unspecified */
		akcam_bits_put1(b, 1);		/* video_full_range_flag */
		akcam_bits_put1(b, 0);		/* colour_description_present */
	} else {
		akcam_bits_put1(b, 0);
	}

	akcam_bits_put1(b, 0);			/* chroma_loc_info_present_flag */

	if (v->time_scale) {
		akcam_bits_put1(b, 1);
		akcam_bits_put(b, 32, v->num_units_in_tick);
		akcam_bits_put(b, 32, v->time_scale);
		akcam_bits_put1(b, v->fixed_frame_rate);
	} else {
		akcam_bits_put1(b, 0);
	}

	/* low_delay_hrd_flag exists only when one HRD is present, so with both
	 * absent it is skipped; getting that wrong shifts every later bit. */
	akcam_bits_put1(b, 0);			/* nal_hrd_parameters_present */
	akcam_bits_put1(b, 0);			/* vcl_hrd_parameters_present */

	akcam_bits_put1(b, v->pic_struct_present);
	akcam_bits_put1(b, v->bitstream_restriction);
	if (v->bitstream_restriction) {
		akcam_bits_put1(b, 1);		/* motion vectors over bounds */
		akcam_bits_ue(b, 0);		/* max_bytes_per_pic_denom */
		akcam_bits_ue(b, 0);		/* max_bits_per_mb_denom */
		akcam_bits_ue(b, 9);		/* log2_max_mv_length_horizontal */
		akcam_bits_ue(b, 7);		/* log2_max_mv_length_vertical */
		akcam_bits_ue(b, 0);		/* num_reorder_frames */
		akcam_bits_ue(b, v->max_dec_frame_buffering);
	}
}

void akcam_h264_write_sps(struct akcam_bits *b, const struct akcam_h264_sps *s)
{
	akcam_bits_put(b, 8, s->profile_idc);
	akcam_bits_put1(b, s->constraint_set0);
	akcam_bits_put1(b, s->constraint_set1);
	akcam_bits_put1(b, s->constraint_set2);
	akcam_bits_put1(b, s->constraint_set3);
	akcam_bits_put(b, 4, 0);
	akcam_bits_put(b, 8, s->level_idc);
	akcam_bits_ue(b, s->sps_id);

	if (s->profile_idc > 99) {
		akcam_bits_ue(b, 1);		/* chroma_format_idc: 4:2:0 */
		akcam_bits_ue(b, 0);		/* bit_depth_luma_minus8 */
		akcam_bits_ue(b, 0);		/* bit_depth_chroma_minus8 */
		akcam_bits_put1(b, 0);		/* qpprime_y_zero_transform_bypass */
		akcam_bits_put1(b, 0);		/* seq_scaling_matrix_present */
	}

	akcam_bits_ue(b, s->log2_max_frame_num_minus4);
	akcam_bits_ue(b, s->pic_order_cnt_type);
	/* Types 0 and 1 write a picture-order block we never emit, so an
	 * unexpected type is refused rather than half-written. */
	if (s->pic_order_cnt_type != 2) {
		b->overflow = 1;
		return;
	}

	akcam_bits_ue(b, s->num_ref_frames);
	akcam_bits_put1(b, s->gaps_in_frame_num_allowed);
	akcam_bits_ue(b, s->width_mbs_minus1);
	akcam_bits_ue(b, s->height_map_units_minus1);
	akcam_bits_put1(b, s->frame_mbs_only);
	if (!s->frame_mbs_only) {
		b->overflow = 1;
		return;
	}

	akcam_bits_put1(b, s->direct_8x8_inference);
	akcam_bits_put1(b, s->crop);
	if (s->crop) {
		akcam_bits_ue(b, s->crop_left);
		akcam_bits_ue(b, s->crop_right);
		akcam_bits_ue(b, s->crop_top);
		akcam_bits_ue(b, s->crop_bottom);
	}

	akcam_bits_put1(b, vui_present(&s->vui));
	if (vui_present(&s->vui))
		write_vui(b, &s->vui);

	akcam_bits_rbsp_trailing(b);
}

void akcam_h264_write_pps(struct akcam_bits *b, const struct akcam_h264_pps *p)
{
	akcam_bits_ue(b, p->pps_id);
	akcam_bits_ue(b, p->sps_id);
	akcam_bits_put1(b, p->entropy_coding_mode);
	akcam_bits_put1(b, p->pic_order_present);
	akcam_bits_ue(b, p->num_slice_groups_minus1);
	/* More than one slice group brings a whole map into the syntax; a PPS
	 * without it parses as something else. */
	if (p->num_slice_groups_minus1) {
		b->overflow = 1;
		return;
	}

	akcam_bits_ue(b, p->num_ref_idx_l0_minus1);
	akcam_bits_ue(b, p->num_ref_idx_l1_minus1);
	akcam_bits_put1(b, p->weighted_pred);
	akcam_bits_put(b, 2, p->weighted_bipred_idc);
	akcam_bits_se(b, p->pic_init_qp_minus26);
	akcam_bits_se(b, p->pic_init_qs_minus26);
	akcam_bits_se(b, p->chroma_qp_index_offset);
	akcam_bits_put1(b, p->deblocking_filter_control_present);
	akcam_bits_put1(b, p->constrained_intra_pred);
	akcam_bits_put1(b, p->redundant_pic_cnt_present);

	if (p->transform_8x8) {
		akcam_bits_put1(b, 1);
		akcam_bits_put1(b, 0);		/* pic_scaling_matrix_present */
		akcam_bits_se(b, p->chroma_qp_index_offset);
	}

	akcam_bits_rbsp_trailing(b);
}

size_t akcam_h264_nal(u8 *out, size_t out_cap, unsigned ref_idc, unsigned type,
		      const u8 *rbsp, size_t rbsp_len)
{
	size_t n;

	/* A zero-length payload is what akcam_bits_length returns after an
	 * overflow, so it must not become a five-byte stub. */
	if (!out || !rbsp || !rbsp_len || out_cap < 5)
		return 0;

	out[0] = 0;
	out[1] = 0;
	out[2] = 0;
	out[3] = 1;
	out[4] = (u8)(((ref_idc & 3u) << 5) | (type & 0x1fu));

	n = akcam_bits_escape(rbsp, rbsp_len, out + 5, out_cap - 5);
	if (!n)
		return 0;
	return 5 + n;
}

#define AK_VENC_RBSP_MAX	64

size_t ak_venc_headers(u8 *out, size_t out_cap,
		       const struct akcam_h264_sps *sps,
		       const struct akcam_h264_pps *pps)
{
	u8 rbsp[AK_VENC_RBSP_MAX];
	struct akcam_bits b;
	size_t n, len;

	akcam_bits_init(&b, rbsp, sizeof(rbsp));
	akcam_h264_write_sps(&b, sps);
	len = akcam_bits_length(&b);
	if (!len)
		return 0;
	n = akcam_h264_nal(out, out_cap, 3, AKCAM_H264_NAL_SPS, rbsp, len);
	if (!n)
		return 0;

	akcam_bits_init(&b, rbsp, sizeof(rbsp));
	akcam_h264_write_pps(&b, pps);
	len = akcam_bits_length(&b);
	if (!len)
		return 0;
	len = akcam_h264_nal(out + n, out_cap - n, 3, AKCAM_H264_NAL_PPS,
			     rbsp, len);
	if (!len)
		return 0;
	return n + len;
}

unsigned ak_venc_hdr_remainder(const u8 *hdr, size_t len, u32 *msb, u32 *lsb)
{
	unsigned i, n;
	size_t off;
	u64 w = 0;

	*msb = 0;
	*lsb = 0;

	n = (unsigned)(len & 7) * 8;
	if (!n)
		return 0;

	off = len & ~(size_t)7;
	for (i = 0; i < 8; i++)
		w = (w << 8) | (off + i < len ? hdr[off + i] : 0);

	*msb = (u32)(w >> 32);
	*lsb = (u32)(w & 0xffffffffu);
	return n;
}
