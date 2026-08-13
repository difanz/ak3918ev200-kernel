/*
 * akmci-of.c - Anyka AK39 MCI host, Linux 4.4 device-tree glue
 *
 * Copyright (C) 2010 Anyka, Ltd, All Rights Reserved.
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/timer.h>

#include <mach/anyka_types.h>
#include <mach/ak_l2.h>
#include <mach/map.h>

#include "akmci.h"

#define DRIVER_NAME		"akmci"

/*
 * Module soft reset. 3.4 reached this through ak_soft_reset(); the 4.4 port
 * has no reset controller, and the only in-tree path that pulses the bit is
 * the gate clock's enable op, which sleeps and so cannot run from the data
 * interrupt. The register is the same one 3.4 wrote, with the same bit
 * numbering, and ak_top_wdt.c already reaches it this way.
 */
#define MODULE_RESET_CON1	(AK_VA_SYSCTRL + 0x20)
#define AK_SRESET_MMC0		1
#define AK_SRESET_MMC1		2

struct akmci_of {
	struct akmci_host	*host;
	struct platform_device	*pdev;
	struct resource		*mem;
	struct clk		*clk;
	int			mci_mode;
	int			irq_mci;
	int			irq_cd;
	int			irq_cd_type;
	int			has_cd_irq;
	struct timer_list	detect_timer;
};

static inline struct akmci_of *to_of(struct akmci_host *host)
{
	return host->glue;
}

static inline l2_device_t akmci_of_l2_dev(struct akmci_host *host)
{
	return to_of(host)->mci_mode == AKMCI_DEV_MMC ? ADDR_MMC0 : ADDR_MMC1;
}

/* ------------------------------------------------------------ L2 backend -- */

static int akmci_of_buf_get(struct akmci_host *host, u8 *buf_id)
{
	u8 id = l2_alloc_nowait(akmci_of_l2_dev(host));

	if (id == BUF_NULL)
		return -EBUSY;

	*buf_id = id;
	return 0;
}

static void akmci_of_buf_put(struct akmci_host *host)
{
	l2_free(akmci_of_l2_dev(host));
}

static int akmci_of_xfer_dma(struct akmci_host *host, dma_addr_t phys,
			     unsigned int len, int to_card)
{
	if (host->l2buf_id == AKMCI_L2BUF_NONE)
		return -EIO;

	return l2_combuf_dma((unsigned long)phys, host->l2buf_id, len,
			     to_card ? MEM2BUF : BUF2MEM, AK_FALSE);
}

static int akmci_of_xfer_cpu(struct akmci_host *host, void *virt,
			     unsigned int len, int to_card)
{
	if (host->l2buf_id == AKMCI_L2BUF_NONE)
		return -EIO;

	return l2_combuf_cpu((unsigned long)virt, host->l2buf_id, len,
			     to_card ? MEM2BUF : BUF2MEM);
}

static int akmci_of_wait(struct akmci_host *host)
{
	if (host->l2buf_id == AKMCI_L2BUF_NONE)
		return -EIO;

	return l2_combuf_wait_dma_finish(host->l2buf_id) == AK_FALSE ?
			-ETIMEDOUT : 0;
}

static void akmci_of_clr_status(struct akmci_host *host)
{
	if (host->l2buf_id != AKMCI_L2BUF_NONE)
		l2_clr_status(host->l2buf_id);
}

static u8 akmci_of_buf_status(struct akmci_host *host)
{
	return l2_get_status(host->l2buf_id);
}

static void akmci_of_reset(struct akmci_host *host)
{
	unsigned int bit = to_of(host)->mci_mode == AKMCI_DEV_MMC ?
			AK_SRESET_MMC0 : AK_SRESET_MMC1;
	unsigned long flags;
	u32 val;

	local_irq_save(flags);
	val = __raw_readl(MODULE_RESET_CON1);
	__raw_writel(val | (1u << bit), MODULE_RESET_CON1);
	local_irq_restore(flags);

	mdelay(5);

	local_irq_save(flags);
	val = __raw_readl(MODULE_RESET_CON1);
	__raw_writel(val & ~(1u << bit), MODULE_RESET_CON1);
	local_irq_restore(flags);
}

static const struct akmci_l2_ops akmci_of_l2_ops = {
	.buf_get	= akmci_of_buf_get,
	.buf_put	= akmci_of_buf_put,
	.xfer_dma	= akmci_of_xfer_dma,
	.xfer_cpu	= akmci_of_xfer_cpu,
	.wait		= akmci_of_wait,
	.clr_status	= akmci_of_clr_status,
	.reset		= akmci_of_reset,
	.buf_status	= akmci_of_buf_status,
};

static ssize_t probe_stats_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct akmci_host *host = dev_get_drvdata(dev);
	unsigned long flags;
	int n;

	spin_lock_irqsave(&host->lock, flags);
	n = akmci_probe_format(host, buf, PAGE_SIZE);
	spin_unlock_irqrestore(&host->lock, flags);

	return n;
}
static DEVICE_ATTR(probe_stats, 0444, probe_stats_show, NULL);

/* -------------------------------------------------------------- board io -- */

static int akmci_of_gpio_get(int gpio)
{
	return gpio_get_value(gpio);
}

static void akmci_of_detect_change(unsigned long data)
{
	struct akmci_of *ofd = (struct akmci_of *)data;

	akmci_card_event(ofd->host);

	if (ofd->irq_cd_type == IRQ_TYPE_LEVEL_LOW)
		ofd->irq_cd_type = IRQ_TYPE_LEVEL_HIGH;
	else
		ofd->irq_cd_type = IRQ_TYPE_LEVEL_LOW;

	irq_set_irq_type(ofd->irq_cd, ofd->irq_cd_type);
	enable_irq(ofd->irq_cd);
}

static irqreturn_t akmci_of_cd_irq(int irq, void *dev)
{
	struct akmci_of *ofd = dev;

	disable_irq_nosync(irq);
	mod_timer(&ofd->detect_timer, jiffies + msecs_to_jiffies(400));

	return IRQ_HANDLED;
}

/* ----------------------------------------------------------------- probe -- */

static const struct of_device_id akmci_of_match[] = {
	{ .compatible = "anyka,ak3918ev200-mmc0", .data = (void *)AKMCI_DEV_MMC },
	{ .compatible = "anyka,ak3918ev200-mmc1", .data = (void *)AKMCI_DEV_SDIO },
	{ .compatible = "anyka,ak39ev330-mmc0",   .data = (void *)AKMCI_DEV_MMC },
	{ .compatible = "anyka,ak39ev330-mmc1",   .data = (void *)AKMCI_DEV_SDIO },
	{ },
};
MODULE_DEVICE_TABLE(of, akmci_of_match);

static void akmci_of_read_cfg(struct akmci_host *host, struct device_node *np,
			      int dev_kind)
{
	struct akmci_board_cfg *cfg = &host->cfg;
	u32 val;

	cfg->dev_kind = dev_kind;
	cfg->xfer_kind = AKMCI_XFER_DMA;
	cfg->gpio_get = akmci_of_gpio_get;

	cfg->data_lines = 4;
	if (!of_property_read_u32(np, "bus-width", &val))
		cfg->data_lines = val;

	cfg->max_speed_hz = 25 * 1000 * 1000;
	if (!of_property_read_u32(np, "max-frequency", &val))
		cfg->max_speed_hz = val;

	cfg->cap_highspeed = of_property_read_bool(np, "cap-sd-highspeed") ||
			     of_property_read_bool(np, "cap-mmc-highspeed");

	if (of_property_read_bool(np, "non-removable"))
		cfg->extra_caps |= MMC_CAP_NONREMOVABLE;

	if (of_find_property(np, "cd-gpios", NULL) &&
	    !of_property_read_bool(np, "broken-cd"))
		cfg->cd_kind = AKMCI_CD_GPIO;
	else
		cfg->cd_kind = AKMCI_CD_ALWAYS;
}

static int akmci_of_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	const struct of_device_id *match;
	struct akmci_host *host;
	struct akmci_of *ofd;
	struct resource *res;
	int gpio;
	int ret;

	match = of_match_device(akmci_of_match, &pdev->dev);
	if (!np || !match) {
		dev_err(&pdev->dev, "no device tree node\n");
		return -EINVAL;
	}

	ofd = devm_kzalloc(&pdev->dev, sizeof(*ofd), GFP_KERNEL);
	if (!ofd)
		return -ENOMEM;

	host = akmci_host_alloc(&pdev->dev);
	if (!host)
		return -ENOMEM;

	host->glue = ofd;
	host->l2 = &akmci_of_l2_ops;
	ofd->host = host;
	ofd->pdev = pdev;
	ofd->mci_mode = (int)(uintptr_t)match->data;
	ofd->irq_cd_type = IRQ_TYPE_LEVEL_LOW;

	akmci_of_read_cfg(host, np, ofd->mci_mode);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENOENT;
		goto err_free_host;
	}

	ofd->mem = request_mem_region(res->start, resource_size(res),
				      pdev->name);
	if (!ofd->mem) {
		dev_err(&pdev->dev, "failed to request io memory region\n");
		ret = -ENOENT;
		goto err_free_host;
	}

	host->base = ioremap(ofd->mem->start, resource_size(ofd->mem));
	if (!host->base) {
		dev_err(&pdev->dev, "failed to ioremap io memory region\n");
		ret = -EINVAL;
		goto err_release_mem;
	}

	ofd->irq_mci = platform_get_irq(pdev, 0);
	if (ofd->irq_mci <= 0) {
		dev_err(&pdev->dev, "failed to get interrupt resource\n");
		ret = -EINVAL;
		goto err_unmap;
	}

	ret = request_irq(ofd->irq_mci, akmci_irq, 0, pdev->name, host);
	if (ret) {
		dev_err(&pdev->dev, "failed to request mci interrupt\n");
		goto err_unmap;
	}

	ofd->clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(ofd->clk)) {
		dev_err(&pdev->dev, "failed to find clock source\n");
		ret = PTR_ERR(ofd->clk);
		ofd->clk = NULL;
		goto err_free_irq;
	}

	ret = clk_prepare_enable(ofd->clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable clock source\n");
		goto err_put_clk;
	}

	host->asic_clk = clk_get_rate(ofd->clk);

	akmci_of_reset(host);

	if (host->cfg.cd_kind == AKMCI_CD_GPIO) {
		gpio = of_get_named_gpio(np, "cd-gpios", 0);
		if (gpio_is_valid(gpio) &&
		    !devm_gpio_request_one(&pdev->dev, gpio, GPIOF_IN,
					   "mci-cd")) {
			host->cfg.gpio_cd = gpio;

			setup_timer(&ofd->detect_timer, akmci_of_detect_change,
				    (unsigned long)ofd);

			ofd->irq_cd = gpio_to_irq(gpio);
			ret = request_irq(ofd->irq_cd, akmci_of_cd_irq,
					  IRQF_TRIGGER_LOW, pdev->name, ofd);
			if (ret) {
				dev_err(&pdev->dev,
					"failed to request cd interrupt\n");
				goto err_disable_clk;
			}
			ofd->has_cd_irq = 1;
		} else {
			host->cfg.cd_kind = AKMCI_CD_ALWAYS;
		}
	}

	gpio = of_get_named_gpio(np, "wp-gpios", 0);
	if (gpio_is_valid(gpio) &&
	    !devm_gpio_request_one(&pdev->dev, gpio, GPIOF_IN, "mci-wp"))
		host->cfg.gpio_wp = gpio;

	platform_set_drvdata(pdev, host);

	ret = akmci_host_add(host);
	if (ret)
		goto err_free_cd;

	if (device_create_file(&pdev->dev, &dev_attr_probe_stats))
		dev_warn(&pdev->dev, "no probe_stats attribute\n");

	return 0;

err_free_cd:
	if (ofd->has_cd_irq) {
		free_irq(ofd->irq_cd, ofd);
		del_timer_sync(&ofd->detect_timer);
	}
err_disable_clk:
	clk_disable_unprepare(ofd->clk);
err_put_clk:
	clk_put(ofd->clk);
err_free_irq:
	free_irq(ofd->irq_mci, host);
err_unmap:
	iounmap(host->base);
err_release_mem:
	release_mem_region(ofd->mem->start, resource_size(ofd->mem));
err_free_host:
	akmci_host_free(host);
	return ret;
}

static int akmci_of_remove(struct platform_device *pdev)
{
	struct akmci_host *host = platform_get_drvdata(pdev);
	struct akmci_of *ofd = to_of(host);

	device_remove_file(&pdev->dev, &dev_attr_probe_stats);

	akmci_host_del(host);

	if (ofd->has_cd_irq) {
		free_irq(ofd->irq_cd, ofd);
		del_timer_sync(&ofd->detect_timer);
	}

	free_irq(ofd->irq_mci, host);

	clk_disable_unprepare(ofd->clk);
	clk_put(ofd->clk);

	iounmap(host->base);
	release_mem_region(ofd->mem->start, resource_size(ofd->mem));

	akmci_host_free(host);

	return 0;
}

#ifdef CONFIG_PM
static int akmci_of_suspend(struct device *dev)
{
	struct akmci_host *host = dev_get_drvdata(dev);

	if (host && !mmc_card_keep_power(host->mmc))
		clk_disable_unprepare(to_of(host)->clk);

	return 0;
}

static int akmci_of_resume(struct device *dev)
{
	struct akmci_host *host = dev_get_drvdata(dev);

	if (host && !mmc_card_keep_power(host->mmc))
		return clk_prepare_enable(to_of(host)->clk);

	return 0;
}

static struct dev_pm_ops akmci_of_pm = {
	.suspend	= akmci_of_suspend,
	.resume		= akmci_of_resume,
};

#define akmci_of_pm_ops		(&akmci_of_pm)
#else
#define akmci_of_pm_ops		NULL
#endif

static struct platform_driver akmci_of_driver = {
	.probe		= akmci_of_probe,
	.remove		= akmci_of_remove,
	.driver		= {
		.name		= DRIVER_NAME,
		.owner		= THIS_MODULE,
		.pm		= akmci_of_pm_ops,
		.of_match_table	= akmci_of_match,
	},
};

static int __init akmci_of_init(void)
{
	return platform_driver_register(&akmci_of_driver);
}

static void __exit akmci_of_exit(void)
{
	platform_driver_unregister(&akmci_of_driver);
}

module_init(akmci_of_init);
module_exit(akmci_of_exit);

MODULE_DESCRIPTION("Anyka MCI Interface driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anyka");
