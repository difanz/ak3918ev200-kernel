/*
 * arch/arm/mach-anycloud/ak_rtc.c
 *
 * AK39 RTC raw register access.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include <mach/ak_rtc_regs.h>

static unsigned int ak_rtc_refcnt;
static DEFINE_SPINLOCK(ak_rtc_refcnt_lock);

static inline void ak_rtc_wait_ready(void)
{
	unsigned long timeout = 0;

	while (!(__raw_readl(RTC_RDY_INT_STAT) & RTC_RDY_STAT_BIT)) {
		if (++timeout >= RTC_WAIT_TIME_OUT)
			break;
	}
}

static void rtc_ready_irq_enable(void)
{
	unsigned long regval;

	regval = __raw_readl(RTC_RDY_INT_CTRL);
	__raw_writel(regval | RTC_RDY_CTRL_BIT, RTC_RDY_INT_CTRL);

	ak_rtc_wait_ready();

	regval = __raw_readl(AK_RTC_CONF);
	regval |= RTC_CONF_RTC_WR_EN;
	__raw_writel(regval, AK_RTC_CONF);
}

static void rtc_ready_irq_disable(void)
{
	unsigned long regval;

	regval = __raw_readl(AK_RTC_CONF);
	regval &= ~RTC_CONF_RTC_WR_EN;
	__raw_writel(regval, AK_RTC_CONF);

	regval = __raw_readl(RTC_RDY_INT_CTRL);
	__raw_writel(regval & ~RTC_RDY_CTRL_BIT, RTC_RDY_INT_CTRL);
}

void ak_rtc_power(int op)
{
	unsigned long flags, rtcconf;

	spin_lock_irqsave(&ak_rtc_refcnt_lock, flags);
	if (op == RTC_ON) {
		if (++ak_rtc_refcnt == 1) {
			rtcconf = __raw_readl(AK_RTC_CONF);
			rtcconf |= RTC_CONF_RTC_EN;
			__raw_writel(rtcconf, AK_RTC_CONF);
		}
	} else {
		if (ak_rtc_refcnt && !--ak_rtc_refcnt) {
			rtcconf = __raw_readl(AK_RTC_CONF);
			rtcconf &= ~RTC_CONF_RTC_EN;
			__raw_writel(rtcconf, AK_RTC_CONF);
		}
	}
	spin_unlock_irqrestore(&ak_rtc_refcnt_lock, flags);
}
EXPORT_SYMBOL(ak_rtc_power);

/* Returns 0 if the RTC block acks a register access, -1 on timeout (no
 * RTC hardware present). */
int test_rtc_inter_reg(unsigned int addr)
{
	int timeout = 0, ret = 0;

	(void)addr;

	rtc_ready_irq_enable();
	while (!(__raw_readl(RTC_RDY_INT_STAT) & RTC_RDY_STAT_BIT)) {
		if (timeout++ > 1000) {
			ret = -1;
			break;
		}
		udelay(1);
	}
	rtc_ready_irq_disable();
	return ret;
}
EXPORT_SYMBOL(test_rtc_inter_reg);

unsigned int ak_rtc_read(unsigned int addr)
{
	unsigned int regval;

	if (addr > AK_RTC_REG_MAX)
		return -1;

	rtc_ready_irq_enable();

	regval = __raw_readl(AK_RTC_CONF);
	regval &= ~0x3FFFFF;
	regval |= (RTC_CONF_RTC_READ | (addr << 14));
	__raw_writel(regval, AK_RTC_CONF);

	udelay(100);
	ak_rtc_wait_ready();
	rtc_ready_irq_disable();

	/* Wait ~1/32K s for the RTC's own clock domain to settle before
	 * the data register is valid. */
	udelay(312);

	return __raw_readl(AK_RTC_DATA) & 0x3FFF;
}
EXPORT_SYMBOL(ak_rtc_read);

unsigned int ak_rtc_write(unsigned int addr, unsigned int value)
{
	unsigned int regval;

	if (addr > AK_RTC_REG_MAX)
		return -1;

	rtc_ready_irq_enable();

	regval = __raw_readl(AK_RTC_CONF);
	regval &= ~0x3FFFFF;
	regval |= (RTC_CONF_RTC_WRITE | (addr << 14) | value);
	__raw_writel(regval, AK_RTC_CONF);

	udelay(100);
	ak_rtc_wait_ready();
	rtc_ready_irq_disable();

	udelay(312);

	return 0;
}
EXPORT_SYMBOL(ak_rtc_write);
