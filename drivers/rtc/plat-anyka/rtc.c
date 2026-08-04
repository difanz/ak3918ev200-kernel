/**
 *  @brief
 *   Copyright C 2011 Anyka CO.,LTD
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *  @author    zhou wenyong
 *  @date      2011-08-29
 *  @note
 *
 */
/*
 * drivers/rtc/rtc-ak.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * 2011-3-22 for AK zwy
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/seq_file.h>

#include <mach/ak_rtc_regs.h>
#include <mach/irqs.h>

static int ak_rtc_wakeup_enable(int en)
{
	unsigned int val;

	val = __raw_readl(OTHER_WAKEUP_CTRL);
	val &= ~RTC_WAKEUP_EN;
	__raw_writel(val, OTHER_WAKEUP_CTRL);

	if (en) {
		val |= RTC_WAKEUP_EN;
		__raw_writel(val, OTHER_WAKEUP_CTRL);
	}
	return 0;
}

static int ak_rtc_gettime(struct device *dev, struct rtc_time *tm)
{
	unsigned long rtcset, rtc_time1, rtc_time2, rtc_time3;

	rtcset = ak_rtc_read(AK_RTC_SETTING);
	rtcset |= RTC_SETTING_REAL_TIME_RE;
	ak_rtc_write(AK_RTC_SETTING, rtcset);

	rtc_time1 = ak_rtc_read(AK_RTC_REAL_TIME1);
	rtc_time2 = ak_rtc_read(AK_RTC_REAL_TIME2);
	rtc_time3 = ak_rtc_read(AK_RTC_REAL_TIME3);

	tm->tm_year  = ((rtc_time3 >> 4) & 0x7F) - EPOCH_START_YEAR + RTC_START_YEAR;
	tm->tm_mon   = (rtc_time3 & 0xF) - 1;
	tm->tm_mday  = (rtc_time2 >> 5) & 0x1F;
	tm->tm_hour  = rtc_time2 & 0x1F;
	tm->tm_min   = (rtc_time1 >> 6) & 0x3F;
	tm->tm_sec   = rtc_time1 & 0x3F;
	tm->tm_wday  = (rtc_time2 >> 10) & 0x7;
	tm->tm_isdst = -1;

	if (tm->tm_year < (RTC_START_YEAR - EPOCH_START_YEAR) ||
	    tm->tm_year > (RTC_START_YEAR - EPOCH_START_YEAR + RTC_YEAR_COUNT)) {
		dev_warn(dev, "RTC year out of range (tm_year=%d), resetting to 2011-01-01\n",
			 tm->tm_year);
		tm->tm_year  = 2011 - EPOCH_START_YEAR;
		tm->tm_mon   = 0;
		tm->tm_mday  = 1;
		tm->tm_hour  = 0;
		tm->tm_min   = 0;
		tm->tm_sec   = 0;
		tm->tm_wday  = 6;
	}

	return 0;
}

static int ak_rtc_settime(struct device *dev, struct rtc_time *tm)
{
	unsigned long regval;

	if (tm->tm_year < (RTC_START_YEAR - EPOCH_START_YEAR) ||
	    tm->tm_year > (RTC_START_YEAR - EPOCH_START_YEAR + RTC_YEAR_COUNT)) {
		dev_err(dev, "RTC year out of range (tm_year=%d)\n", tm->tm_year);
		return -EINVAL;
	}

	ak_rtc_write(AK_RTC_REAL_TIME1, (tm->tm_min << 6) | tm->tm_sec);
	ak_rtc_write(AK_RTC_REAL_TIME2, (tm->tm_wday << 10) | (tm->tm_mday << 5) | tm->tm_hour);
	ak_rtc_write(AK_RTC_REAL_TIME3,
		     ((tm->tm_year + EPOCH_START_YEAR - RTC_START_YEAR) << 4) | (tm->tm_mon + 1));

	regval = ak_rtc_read(AK_RTC_SETTING);
	regval |= RTC_SETTING_REAL_TIME_WR;
	ak_rtc_write(AK_RTC_SETTING, regval);

	while (ak_rtc_read(AK_RTC_SETTING) & RTC_SETTING_REAL_TIME_WR)
		;

	udelay(40);

	return 0;
}

static int ak_rtc_getalarm(struct device *dev, struct rtc_wkalrm *wkalrm)
{
	unsigned long rtc_alarm1, rtc_alarm2, rtc_alarm3;

	rtc_alarm1 = ak_rtc_read(AK_RTC_ALARM_TIME1);
	rtc_alarm2 = ak_rtc_read(AK_RTC_ALARM_TIME2);
	rtc_alarm3 = ak_rtc_read(AK_RTC_ALARM_TIME3);

	wkalrm->time.tm_sec  = rtc_alarm1 & 0x3F;
	wkalrm->time.tm_min  = (rtc_alarm1 >> 6) & 0x3F;
	wkalrm->time.tm_hour = rtc_alarm2 & 0x1F;
	wkalrm->time.tm_mday = (rtc_alarm2 >> 5) & 0x1F;
	wkalrm->time.tm_mon  = (rtc_alarm3 & 0xF) - 1;
	wkalrm->time.tm_year = ((rtc_alarm3 >> 4) & 0x7F) - EPOCH_START_YEAR + RTC_START_YEAR;
	wkalrm->time.tm_isdst = -1;

	return 0;
}

static void ak_rtc_alarm_enable(void)
{
	unsigned long rtc_alarm1, rtc_alarm2, rtc_alarm3;

	rtc_alarm1 = ak_rtc_read(AK_RTC_ALARM_TIME1);
	rtc_alarm2 = ak_rtc_read(AK_RTC_ALARM_TIME2);
	rtc_alarm3 = ak_rtc_read(AK_RTC_ALARM_TIME3);

	rtc_alarm1 |= (1 << 13);
	rtc_alarm2 |= (1 << 13);
	rtc_alarm3 |= (1 << 13);

	ak_rtc_write(AK_RTC_ALARM_TIME1, rtc_alarm1);
	ak_rtc_write(AK_RTC_ALARM_TIME2, rtc_alarm2);
	ak_rtc_write(AK_RTC_ALARM_TIME3, rtc_alarm3);
}

static void ak_rtc_alarm_disable(void)
{
	unsigned long rtc_alarm1, rtc_alarm2, rtc_alarm3;

	rtc_alarm1 = ak_rtc_read(AK_RTC_ALARM_TIME1);
	rtc_alarm2 = ak_rtc_read(AK_RTC_ALARM_TIME2);
	rtc_alarm3 = ak_rtc_read(AK_RTC_ALARM_TIME3);

	rtc_alarm1 &= ~(1 << 13);
	rtc_alarm2 &= ~(1 << 13);
	rtc_alarm3 &= ~(1 << 13);

	ak_rtc_write(AK_RTC_ALARM_TIME1, rtc_alarm1);
	ak_rtc_write(AK_RTC_ALARM_TIME2, rtc_alarm2);
	ak_rtc_write(AK_RTC_ALARM_TIME3, rtc_alarm3);
}

static int ak_rtc_setalarm(struct device *dev, struct rtc_wkalrm *wkalrm)
{
	unsigned long rtc_alarm1, rtc_alarm2, rtc_alarm3;

	rtc_alarm1 = ak_rtc_read(AK_RTC_ALARM_TIME1);
	rtc_alarm2 = ak_rtc_read(AK_RTC_ALARM_TIME2);
	rtc_alarm3 = ak_rtc_read(AK_RTC_ALARM_TIME3);

	rtc_alarm1 &= ~0xFFF;
	rtc_alarm2 &= ~0x3FF;
	rtc_alarm3 &= ~0x7FF;

	rtc_alarm1 |= (wkalrm->time.tm_min << 6) + wkalrm->time.tm_sec;
	rtc_alarm2 |= (wkalrm->time.tm_mday << 5) + wkalrm->time.tm_hour;
	rtc_alarm3 |= ((wkalrm->time.tm_year + EPOCH_START_YEAR - RTC_START_YEAR) << 4)
			+ (wkalrm->time.tm_mon + 1);

	ak_rtc_write(AK_RTC_ALARM_TIME1, rtc_alarm1);
	ak_rtc_write(AK_RTC_ALARM_TIME2, rtc_alarm2);
	ak_rtc_write(AK_RTC_ALARM_TIME3, rtc_alarm3);

	if (wkalrm->enabled) {
		ak_rtc_wakeup_enable(1);
		ak_rtc_alarm_enable();
	} else {
		ak_rtc_alarm_disable();
		ak_rtc_wakeup_enable(0);
	}

	return 0;
}

static int ak_rtc_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case RTC_AIE_OFF:
		ak_rtc_alarm_disable();
		ak_rtc_wakeup_enable(0);
		return 0;
	case RTC_AIE_ON:
		ak_rtc_wakeup_enable(1);
		ak_rtc_alarm_enable();
		return 0;
	case RTC_UIE_ON:
	case RTC_UIE_OFF:
		return -ENOTTY;
	default:
		return -ENOIOCTLCMD;
	}
}

static int ak_rtc_proc(struct device *dev, struct seq_file *seq)
{
	return 0;
}

static int ak_rtc_open(struct device *dev)
{
	return 0;
}

static void ak_rtc_release(struct device *dev)
{
}

static const struct rtc_class_ops ak_rtc_ops = {
	.open		= ak_rtc_open,
	.release	= ak_rtc_release,
	.ioctl		= ak_rtc_ioctl,
	.read_time	= ak_rtc_gettime,
	.set_time	= ak_rtc_settime,
	.read_alarm	= ak_rtc_getalarm,
	.set_alarm	= ak_rtc_setalarm,
	.proc		= ak_rtc_proc,
};

static irqreturn_t ak_rtc_alarmirq(int irq, void *id)
{
	struct rtc_device *rdev = id;
	unsigned int regval;

	regval = ak_rtc_read(AK_RTC_SETTING);
	regval |= (1 << 0);
	ak_rtc_write(AK_RTC_SETTING, regval);

	rtc_update_irq(rdev, 1, RTC_AF | RTC_IRQF);

	return IRQ_HANDLED;
}

static int ak_rtc_remove(struct platform_device *pdev)
{
	struct rtc_device *rtc = platform_get_drvdata(pdev);
	int irq = platform_get_irq(pdev, 0);

	free_irq(irq >= 0 ? irq : IRQ_RTC_ALARM, rtc);
	rtc_device_unregister(rtc);
	ak_rtc_power(RTC_OFF);

	return 0;
}

static int ak_rtc_probe(struct platform_device *pdev)
{
	struct rtc_device *rtc;
	int irq, ret;

	ak_rtc_power(RTC_ON);

	if (test_rtc_inter_reg(AK_RTC_REAL_TIME1) < 0) {
		dev_err(&pdev->dev, "no RTC hardware detected\n");
		ak_rtc_power(RTC_OFF);
		return -ENODEV;
	}

	rtc = rtc_device_register("ak-rtc", &pdev->dev, &ak_rtc_ops, THIS_MODULE);
	if (IS_ERR(rtc)) {
		dev_err(&pdev->dev, "cannot attach rtc\n");
		ak_rtc_power(RTC_OFF);
		return PTR_ERR(rtc);
	}

	rtc->max_user_freq = 128;
	platform_set_drvdata(pdev, rtc);

	ak_rtc_wakeup_enable(0);

	/* The "rtc" node has no "interrupts" property, so the alarm falls
	 * back to system-control-ic's fixed IRQ_RTC_ALARM line (mach/irqs.h). */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		irq = IRQ_RTC_ALARM;

	ret = request_irq(irq, ak_rtc_alarmirq, 0, "ak-rtc alarm", rtc);
	if (ret) {
		dev_err(&pdev->dev, "IRQ %d error %d\n", irq, ret);
		rtc_device_unregister(rtc);
		ak_rtc_power(RTC_OFF);
		return ret;
	}

	return 0;
}

#ifdef CONFIG_PM
static int ak_rtc_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct rtc_time tm;

	ak_rtc_gettime(&pdev->dev, &tm);
	return 0;
}

static int ak_rtc_resume(struct platform_device *pdev)
{
	struct rtc_time tm;

	ak_rtc_gettime(&pdev->dev, &tm);
	return 0;
}
#else
#define ak_rtc_suspend NULL
#define ak_rtc_resume  NULL
#endif

static const struct of_device_id ak_rtc_of_match[] = {
	{ .compatible = "anyka,ak39ev330-rtc" },
	{ }
};
MODULE_DEVICE_TABLE(of, ak_rtc_of_match);

static struct platform_driver ak_rtcdrv = {
	.probe		= ak_rtc_probe,
	.remove		= ak_rtc_remove,
	.suspend	= ak_rtc_suspend,
	.resume		= ak_rtc_resume,
	.driver		= {
		.name		= "ak-rtc",
		.owner		= THIS_MODULE,
		.of_match_table	= ak_rtc_of_match,
	},
};
module_platform_driver(ak_rtcdrv);

MODULE_DESCRIPTION("ANYKA AK RTC Driver");
MODULE_AUTHOR("anyka");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ak-rtc");
