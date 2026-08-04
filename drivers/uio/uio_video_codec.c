/**
 * drivers/uio/uio_video_codec.c
 *
 * Userspace I/O driver for anyka soc video hardware codec.
 * Based on uio_pdrv.c by Uwe Kleine-Koenig,
 *
 * Jacky Lau
 * 2011-07-05
 *
 * Copyright (C) 2011 by Anyka Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * AKUIO_SYSREG_WRITE / AKUIO_WAIT_IRQ / AKUIO_INVALIDATE_L2CACHE /
 * AKUIO_INVALIDATE_L1CACHE (include/linux/akuio_driver.h) are
 * ABI-critical for the userspace H.264 encoder library.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uio_driver.h>
#include <asm/cacheflush.h>
#include <asm/uaccess.h>

#include <linux/akuio_driver.h>
#include <mach/map.h>

#define DRIVER_NAME "uio_vcodec"

#define init_MUTEX(sem)		sema_init(sem, 1)
#define init_MUTEX_LOCKED(sem)	sema_init(sem, 0)

/* platform data of this driver */
struct uio_platdata {
	struct uio_info uioinfo;
	struct clk *clk;
	int irq;
	struct semaphore vcodec_sem;
	unsigned int open_count;
};

static DEFINE_SPINLOCK(sys_ctrl_reg_lock);

/*
 * Anyka system control register read-modify-write, bounded to the
 * SYSCTRL block.
 */
static void sys_ctrl_reg_set(unsigned long reg_phy_addr, unsigned long reg_mask,
			      unsigned long reg_val)
{
	unsigned long flags;
	unsigned long val;
	void __iomem *reg_virt_addr;

	if (reg_phy_addr < AK_PA_SYSCTRL || reg_phy_addr > (AK_PA_SYSCTRL + AK_SZ_SYSCTRL))
		return;

	spin_lock_irqsave(&sys_ctrl_reg_lock, flags);

	reg_virt_addr = (void __iomem *)((unsigned long)AK_VA_SYSCTRL + reg_phy_addr - AK_PA_SYSCTRL);
	val = __raw_readl(reg_virt_addr);
	val = (val & ~reg_mask) | (reg_val & reg_mask);
	__raw_writel(val, reg_virt_addr);

	spin_unlock_irqrestore(&sys_ctrl_reg_lock, flags);
}

static irqreturn_t uio_vcodec_irq_handler(int irq, void *dev_id)
{
	struct uio_platdata *pdata = dev_id;

	disable_irq_nosync(irq);
	up(&pdata->vcodec_sem);

	return IRQ_HANDLED;
}

static int uio_vcodec_ioctl(struct uio_info *uioinfo, unsigned int cmd, unsigned long arg)
{
	struct uio_platdata *pdata = uioinfo->priv;
	int err;

	switch (cmd) {
	case AKUIO_SYSREG_WRITE:
	{
		struct akuio_sysreg_write_t reg_write;

		if (copy_from_user(&reg_write, (void __user *)arg, sizeof(reg_write)))
			return -EFAULT;

		sys_ctrl_reg_set(reg_write.paddr, reg_write.mask, reg_write.val);

		err = 0;
	}
	break;

	case AKUIO_WAIT_IRQ:
		enable_irq(pdata->irq);
		down(&pdata->vcodec_sem);
		err = 0;
		break;

	case AKUIO_INVALIDATE_L2CACHE:
		flush_cache_all();
		err = 0;
		break;

	case AKUIO_INVALIDATE_L1CACHE:
		err = 0;
		break;

	default:
		err = -EINVAL;
		break;
	}

	return err;
}

static int uio_vcodec_open(struct uio_info *uioinfo, struct inode *inode)
{
	struct uio_platdata *pdata = uioinfo->priv;

	if (pdata->open_count++ > 0)
		return 0;

	init_MUTEX_LOCKED(&pdata->vcodec_sem);

	if (request_irq(pdata->irq, uio_vcodec_irq_handler, 0, "VIDEO HW CODEC", pdata))
		return -EBUSY;
	disable_irq_nosync(pdata->irq);

	return 0;
}

static int uio_vcodec_release(struct uio_info *uioinfo, struct inode *inode)
{
	struct uio_platdata *pdata = uioinfo->priv;

	if (--pdata->open_count != 0)
		return 0;

	free_irq(pdata->irq, pdata);

	return 0;
}

static int uio_vcodec_probe(struct platform_device *pdev)
{
	struct uio_platdata *pdata;
	struct resource *res;
	void __iomem *base;
	int ret;

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	pdata->irq = platform_get_irq(pdev, 0);
	if (pdata->irq < 0)
		return pdata->irq;

	pdata->clk = devm_clk_get(&pdev->dev, NULL);
	if (!IS_ERR(pdata->clk))
		clk_prepare_enable(pdata->clk);

	pdata->uioinfo.name = "video_codec";
	pdata->uioinfo.version = "0.1.0";
	pdata->uioinfo.irq = UIO_IRQ_CUSTOM;
	pdata->uioinfo.mem[0].memtype = UIO_MEM_PHYS;
	pdata->uioinfo.mem[0].addr = res->start;
	pdata->uioinfo.mem[0].size = resource_size(res);
	pdata->uioinfo.open = uio_vcodec_open;
	pdata->uioinfo.release = uio_vcodec_release;
	pdata->uioinfo.ioctl = uio_vcodec_ioctl;
	pdata->uioinfo.priv = pdata;

	pdata->open_count = 0;

	ret = uio_register_device(&pdev->dev, &pdata->uioinfo);
	if (ret) {
		if (!IS_ERR(pdata->clk))
			clk_disable_unprepare(pdata->clk);
		return ret;
	}

	platform_set_drvdata(pdev, pdata);

	return 0;
}

static int uio_vcodec_remove(struct platform_device *pdev)
{
	struct uio_platdata *pdata = platform_get_drvdata(pdev);

	uio_unregister_device(&pdata->uioinfo);
	if (!IS_ERR(pdata->clk))
		clk_disable_unprepare(pdata->clk);

	return 0;
}

static const struct of_device_id uio_vcodec_of_match[] = {
	{ .compatible = "anyka,ak39ev330-uio-vencoder" },
	{ }
};
MODULE_DEVICE_TABLE(of, uio_vcodec_of_match);

static struct platform_driver uio_vcodec = {
	.probe	= uio_vcodec_probe,
	.remove	= uio_vcodec_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= uio_vcodec_of_match,
	},
};
module_platform_driver(uio_vcodec);

MODULE_AUTHOR("Jacky Lau");
MODULE_DESCRIPTION("Userspace driver for anyka video hw codec");
MODULE_LICENSE("GPL v2");
