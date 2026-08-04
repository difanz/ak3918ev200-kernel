/*
 * arch/arm/mach-anycloud/include/mach/ak_rtc_regs.h
 *
 * AK39 RTC/RTC-watchdog raw register interface.
 */
#ifndef __MACH_AK_RTC_REGS_H
#define __MACH_AK_RTC_REGS_H

#include <mach/map.h>

#define EPOCH_START_YEAR	(1900)
#define RTC_START_YEAR		(1980)
#define RTC_YEAR_COUNT		(127)

#define AK_RTC_CONF		(AK_VA_SYSCTRL + 0x50)
#define AK_RTC_DATA		(AK_VA_SYSCTRL + 0x54)
#define OTHER_WAKEUP_CTRL	(AK_VA_SYSCTRL + 0x34)
#define OTHER_WAKEUP_STAT	(AK_VA_SYSCTRL + 0x38)

#define RTC_RDY_INT_CTRL	(AK_VA_SYSCTRL + 0x2C)
#define RTC_RDY_INT_STAT	(AK_VA_SYSCTRL + 0x30)
#define RTC_RDY_CTRL_BIT	(1 << 7)
#define RTC_RDY_STAT_BIT	(1 << 7)

#define RTC_WAKEUP_EN		(1 << 12)
#define RTC_CONF_RTC_WR_EN	(1 << 25)
#define RTC_CONF_RTC_EN		(1 << 24)
#define RTC_CONF_RTC_READ	((1 << 21) | (2 << 18) | (1 << 17))
#define RTC_CONF_RTC_WRITE	((1 << 21) | (2 << 18) | (0 << 17))

#define AK_RTC_REAL_TIME1	(0x0)
#define AK_RTC_REAL_TIME2	(0x1)
#define AK_RTC_REAL_TIME3	(0x2)
#define AK_RTC_ALARM_TIME1	(0x3)
#define AK_RTC_ALARM_TIME2	(0x4)
#define AK_RTC_ALARM_TIME3	(0x5)
#define AK_WDT_RTC_TIMER_CONF	(0x6)
#define AK_RTC_SETTING		(0x7)
#define AK_RTC_REG_MAX		AK_RTC_SETTING

#define RTC_ON			1
#define RTC_OFF			0
#define RTC_SETTING_REAL_TIME_RE	(1 << 4)
#define RTC_SETTING_REAL_TIME_WR	(1 << 3)
#define RTC_WAIT_TIME_OUT	2000

void ak_rtc_power(int op);
unsigned int ak_rtc_read(unsigned int addr);
unsigned int ak_rtc_write(unsigned int addr, unsigned int value);
int test_rtc_inter_reg(unsigned int addr);

#endif /* __MACH_AK_RTC_REGS_H */
