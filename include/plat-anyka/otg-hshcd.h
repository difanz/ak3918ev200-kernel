/*
 * AKOTG HS register declarations and HCD data structures
 */

#ifndef __ANYKA_OTG_HS_H_
#define __ANYKA_OTG_HS_H_


#include <linux/delay.h>

#define AKOTG_HC_HCD

#define USB_OP_MOD_REG			(AK_VA_SYSCTRL + 0x58)
#define USB_MODULE_RESET_REG	(AK_VA_SYSCTRL + 0x20)


#define USB_HC_BASE_ADDR	(AK_VA_USB)
#define H_MAXPACKET			512	   /* bytes in fifo */

#endif /* __ANYKA_OTG_HS_H_ */
