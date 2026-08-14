/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Which frames the Anyka video encoder codes intra.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#ifndef AK_VENC_GOP_H
#define AK_VENC_GOP_H

#ifdef __KERNEL__
#include <linux/bitops.h>
#else
/*
 * The same schedule where nothing preempts anything, and the bit operations
 * need not be atomic.
 */
static inline void set_bit(unsigned nr, unsigned long *addr)
{
	*addr |= 1UL << nr;
}

static inline void clear_bit(unsigned nr, unsigned long *addr)
{
	*addr &= ~(1UL << nr);
}

static inline int test_bit(unsigned nr, const unsigned long *addr)
{
	return (*addr >> nr) & 1UL;
}
#endif

/*
 * The schedule is two separate things.
 *
 * `since_idr` is the phase: frames since the last intra picture, so 0 or gop
 * means the next picture is intra. It is advanced by the interrupt handler as
 * each picture completes, and read when the next one is programmed.
 *
 * `force` is one picture asked for out of turn, from process context. It is a
 * bit of its own rather than a phase of 0, so that a picture completing
 * between the request and the next program cannot count the request away, and
 * so that asking for a picture says nothing about when the next scheduled one
 * is due.
 */

#define AK_VENC_GOP_FORCE	0

struct ak_venc_gop {
	unsigned gop;		/* frames between intra frames, >= 1 */
	unsigned since_idr;	/* 0 forces the next frame intra */
	unsigned long force;	/* one intra picture asked for out of turn */
};

/* Whether the phase alone puts an intra picture next. */
static inline int ak_venc_gop_is_intra(const struct ak_venc_gop *g)
{
	return !(g->since_idr && g->since_idr < g->gop);
}

/* Ask for one intra picture, whatever the phase says. */
static inline void ak_venc_gop_request_intra(struct ak_venc_gop *g)
{
	set_bit(AK_VENC_GOP_FORCE, &g->force);
}

static inline int ak_venc_gop_forced(const struct ak_venc_gop *g)
{
	return test_bit(AK_VENC_GOP_FORCE, &g->force);
}

/*
 * The request is answered by an intra picture that has been programmed.
 * Called once that picture can no longer fail, so a request outlives a frame
 * the core was never given.
 */
static inline void ak_venc_gop_forced_done(struct ak_venc_gop *g)
{
	clear_bit(AK_VENC_GOP_FORCE, &g->force);
}

/* Account for a picture the core completed. `intra` is what it coded. */
static inline void ak_venc_gop_advance(struct ak_venc_gop *g, int intra)
{
	if (intra)
		g->since_idr = 1;
	else
		g->since_idr++;
}

/* Start the phase over, for when there is no usable reference left at all. */
static inline void ak_venc_gop_restart(struct ak_venc_gop *g)
{
	g->since_idr = 0;
}

#endif
