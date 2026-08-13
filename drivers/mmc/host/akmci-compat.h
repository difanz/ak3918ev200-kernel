/*
 * akmci-compat.h - kernel-version shims for the Anyka MCI host driver
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This is the only file in the driver permitted to test LINUX_VERSION_CODE.
 * akmci-core.c and akmci.h are byte-identical on every tree; everything that
 * differs between kernels is either named here or lives in the per-tree probe
 * glue (resource claiming, clocks, GPIO, power management, L2 backend).
 */
#ifndef __DRIVERS_MMC_HOST_AKMCI_COMPAT_H__
#define __DRIVERS_MMC_HOST_AKMCI_COMPAT_H__

#include <linux/device.h>
#include <linux/mmc/host.h>
#include <linux/ratelimit.h>
#include <linux/timer.h>
#include <linux/version.h>

/*
 * Host claim callbacks. struct mmc_host_ops carried .enable/.disable until
 * v3.11 removed them. The core's ops table is one piece of text on every tree,
 * so the two slots are contributed from here and are empty everywhere: the
 * driver has never had per-claim work to do, and the 3.4 vendor bodies were
 * a debug print each.
 */
#define AKMCI_MMC_OPS_CLAIM_HOOKS	/* nothing on any supported tree */

/*
 * Timer setup. 3.4 and 4.4 both take a callback of void(*)(unsigned long) and
 * stash the argument in the timer; v4.15 replaced that with timer_setup() and
 * container_of on the timer itself.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
typedef unsigned long akmci_timer_arg_t;
#define akmci_timer_setup(t, fn, host)					\
	setup_timer((t), (fn), (unsigned long)(host))
#define akmci_timer_host(arg, member)					\
	((struct akmci_host *)(arg))
#else
typedef struct timer_list *akmci_timer_arg_t;
#define akmci_timer_setup(t, fn, host)					\
	timer_setup((t), (fn), 0)
#define akmci_timer_host(arg, member)					\
	container_of((arg), struct akmci_host, member)
#endif

/*
 * Rate-limited device errors. v3.6 added the dev_*_ratelimited family; 3.4 has
 * only the unlimited dev_err(). Every error the driver can emit from the I/O
 * path goes through this, because a flood would land in the log on the very
 * card the driver is failing to write.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 6, 0)
#define akmci_err_ratelimited(dev, fmt, ...)				\
({									\
	static DEFINE_RATELIMIT_STATE(_akmci_rs,			\
				      DEFAULT_RATELIMIT_INTERVAL,	\
				      DEFAULT_RATELIMIT_BURST);		\
									\
	if (__ratelimit(&_akmci_rs))					\
		dev_err(dev, fmt, ##__VA_ARGS__);			\
})
#else
#define akmci_err_ratelimited(dev, fmt, ...)				\
	dev_err_ratelimited(dev, fmt, ##__VA_ARGS__)
#endif

/* Settle time before the core is told the slot occupancy changed. */
#define AKMCI_DETECT_DELAY		msecs_to_jiffies(50)

#endif /* __DRIVERS_MMC_HOST_AKMCI_COMPAT_H__ */
