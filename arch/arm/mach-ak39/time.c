/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>

#include <asm/io.h>
#include <asm/sched_clock.h>
#include <asm/mach/time.h>

#include <mach/map.h>


#define AK39_TIMER1_CTRL1		(AK_VA_SYSCTRL + 0xB4)
#define AK39_TIMER1_CTRL2		(AK_VA_SYSCTRL + 0xB8)
#define AK39_TIMER5_CTRL1		(AK_VA_SYSCTRL + 0xD4)
#define AK39_TIMER5_CTRL2		(AK_VA_SYSCTRL + 0xD8)

//define timer register bits
#define TIMER_CLEAR_BIT			(1<<30)
#define TIMER_FEED_BIT			(1<<29)
#define TIMER_ENABLE_BIT		(1<<28)
#define TIMER_STATUS_BIT		(1<<27)
#define TIMER_READ_SEL_BIT		(1<<26)

//define timer work modes (bits 25:24)
#define MODE_AUTO_RELOAD_TIMER	0x0
#define MODE_ONE_SHOT_TIMER		0x1

#define TIMER_CTRL2_PERIODIC	(TIMER_ENABLE_BIT | TIMER_FEED_BIT | \
					(MODE_AUTO_RELOAD_TIMER << 24))	/* 0x30000000 */
#define TIMER_CTRL2_ONESHOT		(TIMER_ENABLE_BIT | TIMER_FEED_BIT | \
					(MODE_ONE_SHOT_TIMER << 24))	/* 0x31000000 */

#define TIMER_CLK_RATE			12000000
#define TIMER_PERIODIC_LOAD		((TIMER_CLK_RATE / HZ) - 1)


/*
 * TIMER5 free-running clocksource read.
 * The hardware is a down-counter, so invert the latched count to present an
 * up-counting value to the timekeeping core.
 */
static cycle_t ak_timer5_read(struct clocksource *cs)
{
	unsigned long flags;
	u32 ctrl2, count;

	local_irq_save(flags);

	ctrl2 = __raw_readl(AK39_TIMER5_CTRL2);
	__raw_writel(ctrl2 | TIMER_READ_SEL_BIT, AK39_TIMER5_CTRL2);

	count = __raw_readl(AK39_TIMER5_CTRL1);

	ctrl2 = __raw_readl(AK39_TIMER5_CTRL2);
	__raw_writel(ctrl2 & ~TIMER_READ_SEL_BIT, AK39_TIMER5_CTRL2);

	local_irq_restore(flags);

	return (cycle_t)(~count);
}

/* Same MMIO path as the clocksource, exported to the scheduler. */
static u32 notrace ak_read_sched_clock(void)
{
	unsigned long flags;
	u32 ctrl2, count;

	local_irq_save(flags);

	ctrl2 = __raw_readl(AK39_TIMER5_CTRL2);
	__raw_writel(ctrl2 | TIMER_READ_SEL_BIT, AK39_TIMER5_CTRL2);

	count = __raw_readl(AK39_TIMER5_CTRL1);

	ctrl2 = __raw_readl(AK39_TIMER5_CTRL2);
	__raw_writel(ctrl2 & ~TIMER_READ_SEL_BIT, AK39_TIMER5_CTRL2);

	local_irq_restore(flags);

	return ~count;
}

static struct clocksource ak_timer5_cs = {
	.name	= "ak_timer5 cs",
	.rating	= 100,
	.read	= ak_timer5_read,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static void ak_timer1_set_mode(enum clock_event_mode mode,
			       struct clock_event_device *dev)
{
	u32 ctrl2;

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		__raw_writel(TIMER_PERIODIC_LOAD, AK39_TIMER1_CTRL1);
		__raw_writel(TIMER_CTRL2_PERIODIC, AK39_TIMER1_CTRL2);
		break;
	case CLOCK_EVT_MODE_ONESHOT:
		__raw_writel(0xffffffff, AK39_TIMER1_CTRL1);
		__raw_writel(TIMER_CTRL2_ONESHOT, AK39_TIMER1_CTRL2);
		break;
	case CLOCK_EVT_MODE_SHUTDOWN:
	case CLOCK_EVT_MODE_UNUSED:
		ctrl2 = __raw_readl(AK39_TIMER1_CTRL2);
		__raw_writel(ctrl2 & ~TIMER_ENABLE_BIT, AK39_TIMER1_CTRL2);
		break;
	case CLOCK_EVT_MODE_RESUME:
	default:
		break;
	}
}

static int ak_timer1_set_next_event(unsigned long evt,
				    struct clock_event_device *dev)
{
	__raw_writel(evt, AK39_TIMER1_CTRL1);
	__raw_writel(TIMER_CTRL2_ONESHOT, AK39_TIMER1_CTRL2);
	return 0;
}

static struct clock_event_device ak_timer1_ce = {
	.name		= "ak_timer1 ce",
	.features	= CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
	.rating		= 100,
	.irq		= IRQ_TIMER1,
	.set_next_event	= ak_timer1_set_next_event,
	.set_mode	= ak_timer1_set_mode,
};

static irqreturn_t ak39_timer1_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ce = dev_id;
	u32 ctrl2 = __raw_readl(AK39_TIMER1_CTRL2);

	if (!(ctrl2 & TIMER_STATUS_BIT))
		return IRQ_NONE;

	ce->event_handler(ce);

	__raw_writel(ctrl2 | TIMER_CLEAR_BIT, AK39_TIMER1_CTRL2);
	return IRQ_HANDLED;
}

static struct irqaction ak_timer1_irq = {
	.name		= "ak_timer1 irq",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL |
			  IRQF_NO_SUSPEND | IRQF_NO_THREAD,
	.handler	= ak39_timer1_interrupt,
	.dev_id		= &ak_timer1_ce,
};

static void __init ak39_sys_timer_init(void)
{
	/* TIMER5 free-running clocksource hardware */
	__raw_writel(0xffffffff, AK39_TIMER5_CTRL1);
	__raw_writel(TIMER_CTRL2_PERIODIC, AK39_TIMER5_CTRL2);

	if (__clocksource_register_scale(&ak_timer5_cs, 1, TIMER_CLK_RATE))
		pr_err("%s: clocksource register failed for %s\n",
		       __func__, ak_timer5_cs.name);

	clockevents_config_and_register(&ak_timer1_ce, TIMER_CLK_RATE,
					15, 0xffffffff);

	if (setup_irq(IRQ_TIMER1, &ak_timer1_irq))
		pr_err("%s: irq register failed for %s\n",
		       __func__, ak_timer1_irq.name);

	setup_sched_clock(ak_read_sched_clock, 32, TIMER_CLK_RATE);
}

struct sys_timer ak39_timer = {
	.init	= ak39_sys_timer_init,
};
