/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register map of the Anyka AK39EV330/AK3918EV200 encrypt block.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#ifndef __AK_ENCRYPT_REGS_H__
#define __AK_ENCRYPT_REGS_H__

#define AKENC_CONTROL			0x000
#define AKENC_INT_STATUS		0x004
#define AKENC_TIMEOUT			0x008
/* n is 1-based in all four windows. */
#define AKENC_GRP_INPUT(n)		(0x00c + (((n) - 1) << 2))
#define AKENC_VEC_INPUT(n)		(0x01c + (((n) - 1) << 2))
#define AKENC_KEY_INPUT(n)		(0x02c + (((n) - 1) << 2))
#define AKENC_GRP_OUTPUT(n)		(0x04c + (((n) - 1) << 2))
#define AKENC_PLAINT_ADDR		0x05c
#define AKENC_DATALEN			0x060
#define AKENC_CIPHER_ADDR		0x064

#define AKENC_IO_SIZE			0x068

/* CONTROL */
#define AKENC_IV_BIT_SEQ		BIT(22)
#define AKENC_KEY_BIT_SEQ		BIT(21)
#define AKENC_CLK_EN			BIT(20)
#define AKENC_OUTPUT_BYTE_SEQ		BIT(19)
#define AKENC_OUTPUT_BIT_SEQ		BIT(18)
#define AKENC_INPUT_BYTE_SEQ		BIT(17)
#define AKENC_INPUT_BIT_SEQ		BIT(16)
#define AKENC_IV_MODE			BIT(14)
#define AKENC_MULT_GRP			BIT(13)
#define AKENC_ALG_SEL(s)		((s) << 10)
#define AKENC_WIDTH_SEL(s)		((s) << 8)
#define AKENC_OPT_MODE(s)		((s) << 5)
#define AKENC_TIMEOUT_INT_EN		BIT(4)
#define AKENC_INT_EN			BIT(3)
#define AKENC_OPT_STATUS		BIT(2)
#define AKENC_STOP			BIT(1)
#define AKENC_START			BIT(0)

#define AKENC_AES_BIT_SEQ		(AKENC_INPUT_BIT_SEQ | \
					 AKENC_OUTPUT_BIT_SEQ | \
					 AKENC_IV_BIT_SEQ)

#define AKENC_ALG_DES			0
#define AKENC_ALG_3DES_3KEY		1
#define AKENC_ALG_3DES_2KEY		2
#define AKENC_ALG_AES128		3
#define AKENC_ALG_AES192		4
#define AKENC_ALG_AES256		5

#define AKENC_WIDTH_FULL		0

#define AKENC_MODE_ECB			0
#define AKENC_MODE_CBC			1
#define AKENC_MODE_CFB			2
#define AKENC_MODE_OFB			3
#define AKENC_MODE_CTR			4

/* INT_STATUS */
#define AKENC_INT_TIMEOUT		BIT(1)
#define AKENC_INT_DONE			BIT(0)

#define AKENC_TIMEOUT_MAX		0xffffff

#endif /* __AK_ENCRYPT_REGS_H__ */
