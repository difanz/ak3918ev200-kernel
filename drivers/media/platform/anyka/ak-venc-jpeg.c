// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The 8290's JPEG mode: the register configuration, the quantisation tables and
 * the JFIF header software has to write in front of the core's scan.
 *
 * Port of lib/video/akcam_jpeg_hdr.{c,h} and the JPEG half of
 * lib/video/akcam_h8290.c from the akcam userspace tree, whose openjpg tool is
 * what this is byte-compared against. The two copies are kept logically
 * identical for exactly that reason.
 *
 * JPEG on this core is stateless. The vendor's allocator, asked for JPEG,
 * returns without allocating a reference frame, a reconstruction frame, a NAL
 * length table or a CABAC block, and its frame setup then skips the four
 * frame-store addresses, the whole rate-control block and the region fields. So
 * a JPEG job carries nothing between frames and can be run between two frames
 * of a live H.264 session without disturbing it.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#include <linux/kernel.h>
#include <linux/types.h>

#include "ak-venc.h"

/* ITU-T T.81 Annex K, tables K.1 and K.2, in raster order. */
static const u8 BASE_LUMA[AKCAM_JPEG_QUANT_VALUES] = {
	16, 11, 10, 16,  24,  40,  51,  61,
	12, 12, 14, 19,  26,  58,  60,  55,
	14, 13, 16, 24,  40,  57,  69,  56,
	14, 17, 22, 29,  51,  87,  80,  62,
	18, 22, 37, 56,  68, 109, 103,  77,
	24, 35, 55, 64,  81, 104, 113,  92,
	49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103,  99
};

static const u8 BASE_CHROMA[AKCAM_JPEG_QUANT_VALUES] = {
	17, 18, 24, 47, 99, 99, 99, 99,
	18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99,
	47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99
};

/* T.81 figure A.6: raster index of the n'th coefficient in the zigzag scan. */
static const u8 ZIGZAG[AKCAM_JPEG_QUANT_VALUES] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
};

/*
 * The four Huffman tables of T.81 Annex K.
 *
 * Not a choice: the core has no Huffman registers, so the entropy coder is
 * wired to these and a DHT saying anything else describes a stream the hardware
 * did not produce. The vendor library carries exactly these constants.
 */
static const u8 DC_LUMA_BITS[16] = {
	0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
static const u8 DC_CHROMA_BITS[16] = {
	0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};
static const u8 DC_VALS[12] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

static const u8 AC_LUMA_BITS[16] = {
	0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d
};
static const u8 AC_LUMA_VALS[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
	0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
	0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
	0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
	0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
	0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
	0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
	0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
	0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
	0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
	0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa
};

static const u8 AC_CHROMA_BITS[16] = {
	0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77
};
static const u8 AC_CHROMA_VALS[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
	0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
	0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
	0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
	0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
	0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
	0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
	0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
	0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
	0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa
};

unsigned akcam_jpeg_quant_round(unsigned value)
{
	if (value < 1)
		return 1;
	if (value > 255)
		value = 255;

	if (value > 128)
		return value & ~7u;
	if (value > 64)
		return value & ~3u;
	if (value > 32)
		return value & ~1u;
	return value;
}

void akcam_jpeg_quant_tables(u8 *luma, u8 *chroma, unsigned quality)
{
	unsigned scale, i;

	if (!luma || !chroma)
		return;

	/* The IJG scale, which is what V4L2_CID_JPEG_COMPRESSION_QUALITY
	 * carries: 50 is the Annex K table unscaled. */
	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;
	scale = quality < 50 ? 5000u / quality : 200u - 2u * quality;

	for (i = 0; i < AKCAM_JPEG_QUANT_VALUES; i++) {
		unsigned l = (BASE_LUMA[i] * scale + 50u) / 100u;
		unsigned c = (BASE_CHROMA[i] * scale + 50u) / 100u;

		luma[i] = (u8)akcam_jpeg_quant_round(l);
		chroma[i] = (u8)akcam_jpeg_quant_round(c);
	}
}

unsigned akcam_jpeg_restart_mcus(unsigned width, unsigned mb_rows)
{
	return mb_rows * ((width + 15u) / 16u);
}

/* Every segment is byte-aligned and byte-counted, so a cursor and a bounds
 * check is the whole of the writer's machinery. *n runs past cap on overflow
 * and the caller sees a refusal rather than a truncated header. */
static void put8(u8 *out, size_t cap, size_t *n, u8 v)
{
	if (*n < cap)
		out[*n] = v;
	(*n)++;
}

static void put16(u8 *out, size_t cap, size_t *n, unsigned v)
{
	put8(out, cap, n, (u8)(v >> 8));
	put8(out, cap, n, (u8)v);
}

static void put_bytes(u8 *out, size_t cap, size_t *n, const u8 *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		put8(out, cap, n, p[i]);
}

static void put_quant(u8 *out, size_t cap, size_t *n, unsigned id,
		      const u8 *table)
{
	unsigned i;

	/* Pq = 0 (8-bit precision) in the high nibble, Tq in the low one. */
	put8(out, cap, n, (u8)(id & 0xfu));
	for (i = 0; i < AKCAM_JPEG_QUANT_VALUES; i++)
		put8(out, cap, n, table[ZIGZAG[i]]);
}

static void put_huff(u8 *out, size_t cap, size_t *n, unsigned tc_th,
		     const u8 *bits, const u8 *vals, size_t nvals)
{
	put8(out, cap, n, (u8)tc_th);
	put_bytes(out, cap, n, bits, 16);
	put_bytes(out, cap, n, vals, nvals);
}

size_t akcam_jpeg_write_header(u8 *out, size_t cap,
			       const struct akcam_jpeg_hdr *c)
{
	static const u8 JFIF[5] = { 'J', 'F', 'I', 'F', 0 };
	size_t n = 0;

	if (!out || !c || !c->luma || !c->chroma)
		return 0;
	/* 4:2:0 halves both axes, so an odd dimension has no chroma sample. */
	if (!c->width || !c->height || (c->width & 1) || (c->height & 1))
		return 0;
	if (c->width > 65535 || c->height > 65535)
		return 0;

	put16(out, cap, &n, 0xffd8);			/* SOI */

	put16(out, cap, &n, 0xffe0);			/* APP0, JFIF */
	put16(out, cap, &n, 16);
	put_bytes(out, cap, &n, JFIF, sizeof(JFIF));
	put16(out, cap, &n, 0x0101);			/* version 1.1 */
	put8(out, cap, &n, 0);				/* density units: none */
	put16(out, cap, &n, 1);
	put16(out, cap, &n, 1);
	put8(out, cap, &n, 0);				/* no thumbnail */
	put8(out, cap, &n, 0);

	/* Both tables in one segment: 2 + 2 * (1 + 64). */
	put16(out, cap, &n, 0xffdb);			/* DQT */
	put16(out, cap, &n, 2 + 2 * (1 + AKCAM_JPEG_QUANT_VALUES));
	put_quant(out, cap, &n, 0, c->luma);
	put_quant(out, cap, &n, 1, c->chroma);

	put16(out, cap, &n, 0xffc0);			/* SOF0, baseline */
	put16(out, cap, &n, 8 + 3 * 3);
	put8(out, cap, &n, 8);				/* sample precision */
	put16(out, cap, &n, c->height);
	put16(out, cap, &n, c->width);
	put8(out, cap, &n, 3);
	/* Y sampled 2x2 against the chroma, which is what swreg20's JPEG mode 0
	 * means - "4 lum + 2 chr blocks/MCU" - and what the input planes are. */
	put8(out, cap, &n, 1); put8(out, cap, &n, 0x22); put8(out, cap, &n, 0);
	put8(out, cap, &n, 2); put8(out, cap, &n, 0x11); put8(out, cap, &n, 1);
	put8(out, cap, &n, 3); put8(out, cap, &n, 0x11); put8(out, cap, &n, 1);

	if (c->restart_interval) {
		put16(out, cap, &n, 0xffdd);		/* DRI */
		put16(out, cap, &n, 4);
		put16(out, cap, &n, c->restart_interval);
	}

	/* All four tables in one segment: 2 + 2 * 29 + 2 * 179. */
	put16(out, cap, &n, 0xffc4);			/* DHT */
	put16(out, cap, &n, 2 + 2 * (1 + 16 + 12) + 2 * (1 + 16 + 162));
	put_huff(out, cap, &n, 0x00, DC_LUMA_BITS, DC_VALS, sizeof(DC_VALS));
	put_huff(out, cap, &n, 0x10, AC_LUMA_BITS, AC_LUMA_VALS,
		 sizeof(AC_LUMA_VALS));
	put_huff(out, cap, &n, 0x01, DC_CHROMA_BITS, DC_VALS, sizeof(DC_VALS));
	put_huff(out, cap, &n, 0x11, AC_CHROMA_BITS, AC_CHROMA_VALS,
		 sizeof(AC_CHROMA_VALS));

	put16(out, cap, &n, 0xffda);			/* SOS */
	put16(out, cap, &n, 6 + 2 * 3);
	put8(out, cap, &n, 3);
	put8(out, cap, &n, 1); put8(out, cap, &n, 0x00);
	put8(out, cap, &n, 2); put8(out, cap, &n, 0x11);
	put8(out, cap, &n, 3); put8(out, cap, &n, 0x11);
	put8(out, cap, &n, 0);				/* Ss */
	put8(out, cap, &n, 63);				/* Se */
	put8(out, cap, &n, 0);				/* Ah, Al */

	return n > cap ? 0 : n;
}

/* -------------------------------------------------------------------------
 * The register side
 * ------------------------------------------------------------------------- */

int akcam_h8290_config_jpeg(struct akcam_h8290 *h,
			    const struct akcam_h8290_jpeg *c)
{
	unsigned row;

	if (!h || !c || !c->width || !c->height)
		return -1;
	if (c->width & 3)		/* the right-edge overfill counts by 4 */
		return -1;
	if (c->restart_mb_rows > 255)
		return -1;
	/* "Encoded width. lumWidth (macroblocks) JPEG:[6..511]", and the
	 * height's own range is [2..511]. */
	if ((c->width + 15) / 16 < 6 || (c->width + 15) / 16 > 511)
		return -1;
	if ((c->height + 15) / 16 < 2 || (c->height + 15) / 16 > 511)
		return -1;

	row = c->input_row_length ? c->input_row_length : c->width;

	akcam_h8290_set(h, H8290_BURST_LENGTH, 16);
	akcam_h8290_set(h, H8290_ENABLE_CLOCK_GATING, 1);
	akcam_h8290_set(h, H8290_ENABLE_INPUT_SWAP_8_BITS, 1);
	akcam_h8290_set(h, H8290_ENABLE_INPUT_SWAP_16_BITS, 1);
	akcam_h8290_set(h, H8290_ENABLE_INPUT_SWAP_32_BITS, 1);
	akcam_h8290_set(h, H8290_ENABLE_OUTPUT_SWAP_8_BITS, 1);
	akcam_h8290_set(h, H8290_ENABLE_OUTPUT_SWAP_16_BITS, 1);
	akcam_h8290_set(h, H8290_ENABLE_OUTPUT_SWAP_32_BITS, 1);

	akcam_h8290_set(h, H8290_ENCODING_MODE, AKCAM_H8290_MODE_JPEG);
	/* INTRA, which is what the vendor's JPEG instance sets once at init.
	 * A JPEG has nothing to predict from, so the field is arguably inert. */
	akcam_h8290_set(h, H8290_ENCODED_PICTURE_TYPE, 1);
	akcam_h8290_set(h, H8290_ENABLE_INTERRUPT_FOR_TIMEOUT, 1);
	/* The NAL length table is H.264's; there is nothing to find in a JPEG
	 * because the markers are in it. */
	akcam_h8290_set(h,
		H8290_ENABLE_WRITING_SIZE_OF_EACH_NAL_UNIT_TO_BASECONTROL_NALS, 0);

	akcam_h8290_set(h, H8290_ENCODED_WIDTH, (c->width + 15) / 16);
	akcam_h8290_set(h, H8290_ENCODED_HEIGHT, (c->height + 15) / 16);
	akcam_h8290_set(h, H8290_INPUT_LUMINANCE_ROW_LENGTH, row);
	/* The overfill has to agree with SOF0, which carries the real size. */
	akcam_h8290_set(h, H8290_OVERFILL_PIXELS_ON_RIGHT_EDGE_OF_IMAGE_DIV4_0,
			(ALIGN(c->width, 16) - c->width) / 4);
	akcam_h8290_set(h, H8290_OVERFILL_PIXELS_ON_BOTTOM_EDGE_OF_IMAGE,
			ALIGN(c->height, 16) - c->height);
	akcam_h8290_set(h, H8290_INPUT_IMAGE_FORMAT, 0);	/* I420 */
	akcam_h8290_set(h, H8290_INPUT_IMAGE_ROTATION, 0);

	/* "JPEG mode. 0=4:2:0 (4lum+2chr blocks/MCU). 1=4:2:2" - the input is
	 * I420 and SOF0 says 2x2 luma sampling, so 0 is what agrees with both. */
	akcam_h8290_set(h, H8290_JPEG_MODE, 0);
	/* "JPEG slice enable. 0=picture ends with EOI" - so the core writes the
	 * EOI itself and software must not append one. */
	akcam_h8290_set(h, H8290_JPEG_SLICE_ENABLE, 0);
	akcam_h8290_set(h,
		H8290_JPEG_RESTART_MARKER_INTERVAL_WHEN_SLICES_ARE_DISABLED_MB,
		c->restart_mb_rows);
	akcam_h8290_set(h, H8290_JPEG_RESTART_MARKER_FOR_FIRST_RST, 0);

	return 0;
}

void akcam_h8290_quant_apply(struct akcam_h8290 *h, const u8 *luma,
			     const u8 *chroma)
{
	u8 slot[128];
	unsigned r, c, n;

	if (!h || !luma || !chroma)
		return;

	/*
	 * The core reads the block in a scan of its own: four rows of one
	 * column at a time, the top half of a column pair before its bottom
	 * half, columns in pairs left to right. Neither raster nor zigzag - and
	 * the DQT segment wants zigzag, which is why the two orders are written
	 * out separately rather than shared.
	 */
	for (r = 0; r < 8; r++)
		for (c = 0; c < 8; c++) {
			unsigned i = 4u * (4u * (c >> 1) + 2u * (r >> 2) +
					   (c & 1u)) + (r & 3u);

			slot[i] = luma[r * 8 + c];
			slot[64 + i] = chroma[r * 8 + c];
		}

	/* Four 8-bit entries per register, most significant byte first. */
	for (n = 0; n < 32; n++)
		h->reg[AKCAM_H8290_QUANT_FIRST_REG + n] =
			((u32)slot[n * 4 + 0] << 24) |
			((u32)slot[n * 4 + 1] << 16) |
			((u32)slot[n * 4 + 2] << 8) |
			((u32)slot[n * 4 + 3]);
}

int akcam_h8290_jpeg_output_apply(struct akcam_h8290 *h,
				  u32 stream_base, size_t stream_bytes,
				  const u8 *header, size_t header_bytes)
{
	u32 msb = 0, lsb = 0;
	size_t whole, bits;
	unsigned i;

	if (!h || !header || !header_bytes)
		return -1;
	if (stream_base & 7)
		return -1;
	if (header_bytes + 8 > stream_bytes)
		return -1;

	whole = header_bytes & ~(size_t)7;
	bits = (header_bytes & 7) * 8;

	/*
	 * The core writes 64-bit words, so it cannot be pointed at the byte
	 * after the header. It is pointed at the last whole word the header
	 * occupies, told how many bits of that word are used, and handed those
	 * bytes back so it can rewrite the word whole.
	 */
	akcam_h8290_set(h, H8290_BASE_ADDRESS_FOR_OUTPUT_STREAM_DATA,
			stream_base + (u32)whole);
	akcam_h8290_set(h,
		H8290_STREAM_BUFFER_LIMIT_64BIT_ADDRESSES_OUTPUT_STREAM_SIZE_B,
		(u32)((stream_bytes - whole) / 8));
	akcam_h8290_set(h, H8290_STREAM_START_OFFSET_AMOUNT_OF_STRMHDRREM_BITS_0,
			(u32)bits);

	/* MSB aligned: header byte whole+0 occupies bits 31:24 of the MSB
	 * register. Bytes past the remainder are the core's to fill and are
	 * handed over as zero. */
	for (i = 0; i < 4 && whole + i < header_bytes; i++)
		msb |= (u32)header[whole + i] << (24 - 8 * i);
	for (i = 0; i < 4 && whole + 4 + i < header_bytes; i++)
		lsb |= (u32)header[whole + 4 + i] << (24 - 8 * i);

	akcam_h8290_set(h, H8290_STREAM_HEADER_REMAINDER_BITS_MSB_MSB_ALIGNED, msb);
	akcam_h8290_set(h, H8290_STREAM_HEADER_REMAINDER_BITS_LSB_MSB_ALIGNED, lsb);
	return 0;
}

size_t akcam_h8290_jpeg_stream_bytes(size_t header_bytes, u32 swreg24)
{
	return (header_bytes & ~(size_t)7) + akcam_h8290_output_bytes(swreg24);
}
