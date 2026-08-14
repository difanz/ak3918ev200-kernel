/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Which frames the Anyka video encoder codes intra.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#ifndef AK_VENC_GOP_H
#define AK_VENC_GOP_H

struct ak_venc_gop {
	unsigned gop;		/* frames between intra frames, >= 1 */
	unsigned since_idr;	/* 0 forces the next frame intra */
};

static inline int ak_venc_gop_is_intra(const struct ak_venc_gop *g)
{
	return !(g->since_idr && g->since_idr < g->gop);
}

static inline void ak_venc_gop_advance(struct ak_venc_gop *g, int intra)
{
	if (intra)
		g->since_idr = 1;
	else
		g->since_idr++;
}

#endif
