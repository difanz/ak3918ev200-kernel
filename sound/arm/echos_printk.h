#ifndef ECHOS_PRINTK_H
#define ECHOS_PRINTK_H

#ifdef __KERNEL__
#include <linux/kernel.h>
#else
#include <stdio.h>

#define KERN_SOH	"\001"
#define KERN_ERR	KERN_SOH "3"
#define KERN_WARNING	KERN_SOH "4"
#define KERN_NOTICE	KERN_SOH "5"
#define KERN_INFO	KERN_SOH "6"
#define KERN_DEBUG	KERN_SOH "7"

#define printk(fmt, ...)	printf(fmt, ##__VA_ARGS__)
#endif

#endif
