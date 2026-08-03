/*
 *  arch/arm/mach-ak39/gpio.c
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */
#include <linux/module.h>
#include <asm/irq.h>
#include <mach/gpio.h>

#ifdef CONFIG_CPU_AK3918EV200

/*
 * Peripheral pad mux for AK3918EV200, recovered from this unit's stock kernel.
 * The four mask/value pairs are SHAREPIN_CON1..CON4.
 *
 * Every module assignment differs from the two-bank parts, and the registers
 * differ too: SD/MMC lives in CON4 here, not CON3.  ePIN_AS_UART1 is the UART0
 * console pair, CON1[2:1], which this sets rather than clears - the same value
 * the decompressor and the console reservation in g_ak39_setpin_as_gpio()
 * write.
 */
struct gpio_sharepin_cfg share_cfg_module[] = {
	{ePIN_AS_OPCLK,     SHARE_CFG1,  0x00000600, 0x00000200, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_JTAG,      SHARE_CFG12, 0x00000061, 0x00000061, 0x000003f0, 0x000003f0, 0, 0, 0, 0},
	{ePIN_AS_RTCK,      SHARE_CFG1,  0x00000060, 0x00000060, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_I2S,       SHARE_CFG1,  0x0103c000, 0x0003c000, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_PWM1,      SHARE_CFG1,  0x00000800, 0x00000800, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_PWM2,      SHARE_CFG1,  0x00001000, 0x00001000, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_PWM3,      SHARE_CFG1,  0x00002000, 0x00002000, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_PWM4,      SHARE_CFG1,  0x00080000, 0x00080000, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_PWM5,      SHARE_CFG1,  0x00100000, 0x00100000, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_SPI1,      SHARE_CFG14, 0x02000000, 0x02000000, 0, 0, 0, 0, 0x00003f03, 0x00003103},
	{ePIN_AS_SPI2,      SHARE_CFG1,  0, 0, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_UART1,     SHARE_CFG1,  0x00000006, 0x00000006, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_UART2,     SHARE_CFG2,  0, 0, 0x000000f0, 0x000000a0, 0, 0, 0, 0},
	{ePIN_AS_CAMERA,    SHARE_CFG2,  0, 0, 0x000fffff, 0x00000000, 0, 0, 0, 0},
	{ePIN_AS_MCI,       SHARE_CFG4,  0, 0, 0, 0, 0, 0, 0x00003fc0, 0x000031c0},
	{ePIN_AS_SDIO,      SHARE_CFG14, 0x08000000, 0x00000000, 0, 0, 0, 0, 0x0ff00000, 0x05700000},
	{ePIN_AS_MCI_8LINE, SHARE_CFG4,  0, 0, 0, 0, 0, 0, 0x0003ffc0, 0x00001780},
	{ePIN_AS_MAC,       SHARE_CFG13, 0x00000600, 0x00000200, 0, 0, 0x0fffffff, 0x0575f5a5, 0, 0},
	{ePIN_AS_RMAC,      SHARE_CFG13, 0x00000600, 0x00000200, 0, 0, 0x0fffffff, 0x01450525, 0, 0},
	{ePIN_AS_I2C,       SHARE_CFG1,  0x00000180, 0x00000180, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_IRDA,      SHARE_CFG3,  0, 0, 0, 0, 0, 0, 0, 0},
	{ePIN_AS_DUMMY,     EXIT_CFG,    0, 0, 0, 0, 0, 0, 0, 0},
};

#else	/* two-bank parts: vendor table, unchanged */

//share pin config fore module in AK39xx
struct gpio_sharepin_cfg share_cfg_module[] = {
	{ePIN_AS_OPCLK,    SHARE_CFG1,	(0x3<<2), (1<<2), 0, 0, 0, 0},
    {ePIN_AS_JTAG,     SHARE_CFG1,	(0x3f<<16)|(1<<1), (0x15<<16)|(1<<1), 0, 0, 0, 0},
    {ePIN_AS_RTCK,	   SHARE_CFG1,	(0x3<<22), (1<<22), 0, 0, 0, 0},
    {ePIN_AS_I2S,      SHARE_CFG13,	((0xf<<8)|(1<<13)), ((0xf<<8)|(1<<13)), 0, 0, (0x3<<4), (0x3<<4)},
    {ePIN_AS_PWM1,     SHARE_CFG1,	((0x3<<16)|(1<<4)), ((0x3<<16)|(1<<4)), 0, 0, 0, 0},
    {ePIN_AS_PWM2,     SHARE_CFG1,	(0x3<<18), (0x3<<18), 0, 0, 0, 0},
    {ePIN_AS_PWM3,     SHARE_CFG1,	((0x3<<20)|(1<<6)), ((0x3<<20)|(1<<6)), 0, 0, 0, 0},
    {ePIN_AS_PWM4,     SHARE_CFG1,	((0x3<<22)|(1<<7)), ((0x3<<22)|(1<<7)), 0, 0, 0, 0},
#if defined(CONFIG_CPU_AK3910)
	{ePIN_AS_PWM5,     SHARE_CFG13,	(0x3<<2), (0x3<<2), 0, 0, (0x3<<4), (0x1<<4)},
    {ePIN_AS_SPI1,     SHARE_CFG3,	0, 0, 0, 0, ((0x3<<23)|(1<<21)|(1<<19)|(0x3)),((0x1<<23)|(1<<21)|(1<<19)|(0x3))},
    {ePIN_AS_SPI2,     SHARE_CFG3,	0, 0, 0, 0, ((0x3<<23)|(1<<21)|(1<<19)|(0xf<<2)),((0x3<<23)|(1<<21)|(1<<19)|(0xa<<2))},
#elif defined(CONFIG_CPU_AK3916) || defined(CONFIG_CPU_AK3918)
	{ePIN_AS_PWM5,	   SHARE_CFG13, ((1<<12)|(0x3<<2)), ((1<<12)|(0x3<<2)), 0, 0, (0x3<<4), (0x1<<4)},
	{ePIN_AS_SPI1,	   SHARE_CFG3,	0, 0, 0, 0, ((0x3<<23)|(1<<21)|(1<<19)|(0x3)),((0x1<<23)|(1<<21)|(1<<19)|(0x3))},
	{ePIN_AS_SPI2,	   SHARE_CFG3,	0, 0, 0, 0, ((0x3<<23)|(1<<21)|(1<<19)|(0xf<<2)),((0x3<<23)|(1<<21)|(1<<19)|(0xa<<2))},
#endif
	{ePIN_AS_UART1,    SHARE_CFG1,	(0x3<<14), (0x3<<14), 0, 0, 0, 0},
    {ePIN_AS_UART2,    SHARE_CFG1,	(0xff<<16), (0xaa<<16), 0, 0, 0, 0},
    {ePIN_AS_CAMERA,   SHARE_CFG2,	0, 0, (0xf), (0x0), 0, 0},
    {ePIN_AS_SDIO,     SHARE_CFG3,	0, 0, 0, 0, (0xff<<16), (0x57<<16)},
    {ePIN_AS_MCI,	   SHARE_CFG3,	0, 0, 0, 0, (0xf<<6), (0xf<<6)},
	{ePIN_AS_MCI_8LINE, SHARE_CFG3, 0, 0, 0, 0, (0x3ff<<6), (0x15f<<6)},
	{ePIN_AS_MAC,      SHARE_CFG12,	(0x3<<2), (1<<2), (0x1fff<<4), (0x1fff<<4), 0, 0},
	{ePIN_AS_I2C,      SHARE_CFG3,	0, 0, 0, 0, (0x3<<25), (0x3<<25)},
	{ePIN_AS_IRDA,     SHARE_CFG3,	0, 0, 0, 0, (0x3<<2), (0x1<<2)},
    {ePIN_AS_DUMMY,    EXIT_CFG,	0, 0, 0, 0, 0, 0}
};

#endif	/* CONFIG_CPU_AK3918EV200 */

#ifdef CONFIG_CPU_AK3918EV200

/*
 * Pull configuration for AK3918EV200, recovered from this unit's stock kernel.
 * PUPD_CFG1..4 are SYSCTRL +0x80, +0x84, +0x88 and +0xE0.  Pads 3, 49, 56 and
 * 79 have no entry on this part.
 */
struct gpio_pupd_cfg pupd_cfg_info[] = {
	//pin, index, register, up/down
	{AK_GPIO_0,  0,  PUPD_CFG1, PULLUP},
	{AK_GPIO_1,  1,  PUPD_CFG1, PULLUP},
	{AK_GPIO_2,  2,  PUPD_CFG1, PULLUP},
	{AK_GPIO_4,  4,  PUPD_CFG1, PULLUP},
	{AK_GPIO_5,  5,  PUPD_CFG1, PULLUP},
	{AK_GPIO_6,  4,  PUPD_CFG2, PULLDOWN},
	{AK_GPIO_7,  5,  PUPD_CFG2, PULLDOWN},
	{AK_GPIO_8,  6,  PUPD_CFG2, PULLDOWN},
	{AK_GPIO_9,  7,  PUPD_CFG2, PULLDOWN},
	{AK_GPIO_10, 0,  PUPD_CFG3, PULLDOWN},
	{AK_GPIO_11, 1,  PUPD_CFG3, PULLDOWN},
	{AK_GPIO_12, 2,  PUPD_CFG3, PULLDOWN},
	{AK_GPIO_13, 3,  PUPD_CFG3, PULLDOWN},
	{AK_GPIO_14, 5,  PUPD_CFG3, PULLDOWN},
	{AK_GPIO_15, 6,  PUPD_CFG3, PULLDOWN},
	{AK_GPIO_16, 7,  PUPD_CFG3, PULLDOWN},
	{AK_GPIO_17, 8,  PUPD_CFG3, PULLDOWN},
	{AK_GPIO_18, 9,  PUPD_CFG3, PULLDOWN},
	{AK_GPIO_19, 11, PUPD_CFG3, PULLDOWN},
	{AK_GPIO_20, 12, PUPD_CFG3, PULLDOWN},
	{AK_GPIO_21, 13, PUPD_CFG3, PULLDOWN},
	{AK_GPIO_22, 14, PUPD_CFG3, PULLDOWN},
	{AK_GPIO_23, 15, PUPD_CFG3, PULLDOWN},
	{AK_GPIO_24, 16, PUPD_CFG3, PULLDOWN},
	{AK_GPIO_25, 0,  PUPD_CFG4, PULLUP},
	{AK_GPIO_26, 1,  PUPD_CFG4, PULLUP},
	{AK_GPIO_27, 6,  PUPD_CFG1, PULLUP},
	{AK_GPIO_28, 7,  PUPD_CFG1, PULLUP},
	{AK_GPIO_29, 2,  PUPD_CFG4, PULLUP},
	{AK_GPIO_30, 3,  PUPD_CFG4, PULLUP},
	{AK_GPIO_31, 4,  PUPD_CFG4, PULLUP},
	{AK_GPIO_32, 5,  PUPD_CFG4, PULLUP},
	{AK_GPIO_33, 6,  PUPD_CFG4, PULLUP},
	{AK_GPIO_34, 7,  PUPD_CFG4, PULLUP},
	{AK_GPIO_35, 8,  PUPD_CFG4, PULLUP},
	{AK_GPIO_36, 9,  PUPD_CFG4, PULLUP},
	{AK_GPIO_37, 10, PUPD_CFG4, PULLUP},
	{AK_GPIO_38, 11, PUPD_CFG4, PULLUP},
	{AK_GPIO_39, 12, PUPD_CFG4, PULLUP},
	{AK_GPIO_40, 13, PUPD_CFG4, PULLUP},
	{AK_GPIO_41, 14, PUPD_CFG4, PULLUP},
	{AK_GPIO_42, 15, PUPD_CFG4, PULLUP},
	{AK_GPIO_43, 16, PUPD_CFG4, PULLUP},
	{AK_GPIO_44, 17, PUPD_CFG4, PULLUP},
	{AK_GPIO_45, 18, PUPD_CFG4, PULLUP},
	{AK_GPIO_46, 19, PUPD_CFG4, PULLUP},
	{AK_GPIO_47, 8,  PUPD_CFG1, PULLDOWN},
	{AK_GPIO_48, 9,  PUPD_CFG1, PULLDOWN},
	{AK_GPIO_50, 10, PUPD_CFG1, PULLUP},
	{AK_GPIO_51, 11, PUPD_CFG1, PULLUP},
	{AK_GPIO_52, 12, PUPD_CFG1, PULLUP},
	{AK_GPIO_53, 13, PUPD_CFG1, PULLUP},
	{AK_GPIO_54, 14, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_55, 15, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_57, 17, PUPD_CFG1, PULLUP},
	{AK_GPIO_58, 18, PUPD_CFG1, PULLUP},
	{AK_GPIO_59, 19, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_60, 20, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_61, 21, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_62, 22, PUPD_CFG1, PULLUP},
	{AK_GPIO_63, 23, PUPD_CFG1, PULLUP},
	{AK_GPIO_64, 0,  PUPD_CFG2, PULLDOWN},
	{AK_GPIO_65, 1,  PUPD_CFG2, PULLDOWN},
	{AK_GPIO_66, 2,  PUPD_CFG2, PULLDOWN},
	{AK_GPIO_67, 3,  PUPD_CFG2, PULLDOWN},
	{AK_GPIO_68, 8,  PUPD_CFG2, PULLDOWN},
	{AK_GPIO_69, 9,  PUPD_CFG2, PULLDOWN},
	{AK_GPIO_70, 10, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_71, 11, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_72, 12, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_73, 13, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_74, 14, PUPD_CFG2, PULLUP},
	{AK_GPIO_75, 15, PUPD_CFG2, PULLUP},
	{AK_GPIO_76, 4,  PUPD_CFG3, PULLDOWN},
	{AK_GPIO_77, 10, PUPD_CFG3, PULLDOWN},
	{AK_GPIO_78, 17, PUPD_CFG3, PULLDOWN},
};

#else	/* two-bank parts: vendor table, unchanged */

struct gpio_pupd_cfg pupd_cfg_info[] = {
	//pin, index, register, up/down
	{AK_GPIO_0, 0, PUPD_CFG1, PULLUP},
	{AK_GPIO_1, 19, PUPD_CFG1, PULLUP},
	{AK_GPIO_2, 20, PUPD_CFG1, PULLUP},
	{AK_GPIO_3, 1, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_4, 21, PUPD_CFG1, PULLUP},
	{AK_GPIO_5, 22, PUPD_CFG1, PULLUP},
	{AK_GPIO_6, 23, PUPD_CFG1, PULLUP},
	{AK_GPIO_7, 24, PUPD_CFG1, PULLUP},
	{AK_GPIO_8, 4, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_9, 5, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_10, 7, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_11, 8, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_12, 10, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_13, 11, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_14, 14, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_15, 15, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_16, 16, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_17, 17, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_18, 18, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_19, 20, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_20, 21, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_21, 22, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_22, 23, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_23, 25, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_24, 26, PUPD_CFG2, PULLDOWN},
	{AK_GPIO_25, 0, PUPD_CFG3, PULLUP},
	{AK_GPIO_26, 1, PUPD_CFG3, PULLUP},
	{AK_GPIO_27, 20, PUPD_CFG3, PULLUP},
	{AK_GPIO_28, 21, PUPD_CFG3, PULLUP},
	{AK_GPIO_29, 2, PUPD_CFG3, PULLUP},
	{AK_GPIO_30, 3, PUPD_CFG3, PULLUP},
	{AK_GPIO_31, 4, PUPD_CFG3, PULLUP},
	{AK_GPIO_32, 5, PUPD_CFG3, PULLUP},
	{AK_GPIO_33, 6, PUPD_CFG3, PULLUP},
	{AK_GPIO_34, 7, PUPD_CFG3, PULLUP},
	{AK_GPIO_35, 8, PUPD_CFG3, PULLUP},
	{AK_GPIO_36, 9, PUPD_CFG3, PULLUP},
	{AK_GPIO_37, 10, PUPD_CFG3, PULLUP},
	{AK_GPIO_38, 11, PUPD_CFG3, PULLUP},
	{AK_GPIO_39, 12, PUPD_CFG3, PULLUP},
	{AK_GPIO_40, 13, PUPD_CFG3, PULLUP},
	{AK_GPIO_41, 14, PUPD_CFG3, PULLUP},
	{AK_GPIO_42, 15, PUPD_CFG3, PULLUP},
	{AK_GPIO_43, 16, PUPD_CFG3, PULLUP},
	{AK_GPIO_44, 17, PUPD_CFG3, PULLUP},
	{AK_GPIO_45, 18, PUPD_CFG3, PULLUP},
	{AK_GPIO_46, 19, PUPD_CFG3, PULLUP},
	{AK_GPIO_47, 2, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_48, 3, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_50, 5, PUPD_CFG1, PULLUP},
	{AK_GPIO_51, 6, PUPD_CFG1, PULLUP},
	{AK_GPIO_52, 7, PUPD_CFG1, PULLUP},
	{AK_GPIO_53, 8, PUPD_CFG1, PULLUP},
	{AK_GPIO_54, 9, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_55, 10, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_56, 11, PUPD_CFG1, PULLUP},
	{AK_GPIO_57, 12, PUPD_CFG1, PULLUP},
	{AK_GPIO_58, 13, PUPD_CFG1, PULLUP},
	{AK_GPIO_59, 14, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_60, 15, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_61, 16, PUPD_CFG1, PULLDOWN},
	{AK_GPIO_62, 17, PUPD_CFG1, PULLUP},
	{AK_GPIO_63, 18, PUPD_CFG1, PULLUP},
};

#endif	/* CONFIG_CPU_AK3918EV200 */

#ifdef CONFIG_CPU_AK3918EV200

/*
 * Share-pin (pad mux) tables for AK3918EV200.
 *
 * These are pure silicon description, and the assignments are not the ones the
 * two-bank parts use.  Four tables cover four registers, together describing
 * pads 0..78.  A pad may appear more than once when selecting GPIO takes more
 * than one non-adjacent mux bit: pad 5 needs both CON1 bit 5 and bit 6, since
 * it carries PWM2, UART1_TXD, JTAG_RTCK, AIN1 and TWI2_SCK.  setpin_walk()
 * applies every matching row, not just the first.
 *
 * The difference is not inert.  On this part CON1 bit 2 is the mux for pad
 * GPIO2, one of the two UART0 console pads, while the two-bank layout puts
 * pad 47 there - so requesting GPIO 47 on an EV200 with the wrong table takes
 * the console away mid-boot and leaves pad 47 in its alternate function.
 */

//this used to clr in gpio chare pin cfg1
struct sharepin_as_gpio sharepin_cfg_gpio1[] = {
    {0,		0,		0,	AS_GPIO_CFG_BIT1},
    {1,		1,		1,	AS_GPIO_CFG_BIT1},	/* UART0 console pad */
    {2,		2,		2,	AS_GPIO_CFG_BIT1},	/* UART0 console pad */
    {3,		3,		3,	AS_GPIO_CFG_BIT1},
    {4,		4,		4,	AS_GPIO_CFG_BIT1},
    {5,		5,		5,	AS_GPIO_CFG_BIT1},
    {5,		5,		6,	AS_GPIO_CFG_BIT1},
    {27,	27,		7,	AS_GPIO_CFG_BIT1},
    {28,	28,		8,	AS_GPIO_CFG_BIT1},
    {47,	47,		9,	AS_GPIO_CFG_BIT2},	/* shared with OPCLK */
    {48,	48,		11,	AS_GPIO_CFG_BIT1},
    {50,	50,		12,	AS_GPIO_CFG_BIT1},
    {51,	51,		13,	AS_GPIO_CFG_BIT1},
    {52,	52,		14,	AS_GPIO_CFG_BIT1},
    {53,	53,		15,	AS_GPIO_CFG_BIT1},
    {54,	54,		16,	AS_GPIO_CFG_BIT1},
    {55,	55,		17,	AS_GPIO_CFG_BIT1},
    {56,	56,		18,	AS_GPIO_CFG_BIT1},
    {57,	57,		19,	AS_GPIO_CFG_BIT1},
    {58,	58,		20,	AS_GPIO_CFG_BIT1},
};

//this used to clr in gpio share pin cfg2
struct sharepin_as_gpio sharepin_cfg_gpio2[] = {
    {64,	64,		0,	AS_GPIO_CFG_BIT1},
    {65,	65,		1,	AS_GPIO_CFG_BIT1},
    /*
     * Verbatim from stock, inverted range and all: gpio_start 67 is greater
     * than gpio_end 66, so this entry can never match and pads 66/67 have no
     * mux entry.  Kept as-is rather than "corrected" to a guess, because we
     * have no evidence for what the intended range was.
     */
    {67,	66,		2,	AS_GPIO_CFG_BIT1},
    {6,		6,		4,	AS_GPIO_CFG_BIT2_01},
    {7,		7,		6,	AS_GPIO_CFG_BIT2_01},
    {8,		8,		8,	AS_GPIO_CFG_BIT2_01},
    {9,		9,		10,	AS_GPIO_CFG_BIT2_01},
    {68,	68,		12,	AS_GPIO_CFG_BIT1},
    {69,	69,		13,	AS_GPIO_CFG_BIT1},
    {70,	70,		14,	AS_GPIO_CFG_BIT1},
    {71,	71,		15,	AS_GPIO_CFG_BIT1},
    {72,	72,		16,	AS_GPIO_CFG_BIT1},
    {73,	73,		17,	AS_GPIO_CFG_BIT1},
    {74,	74,		18,	AS_GPIO_CFG_BIT1},
    {75,	75,		19,	AS_GPIO_CFG_BIT1},
};

//this used to clr in gpio share pin cfg3
struct sharepin_as_gpio sharepin_cfg_gpio3[] = {
    {10,	10,		0,	AS_GPIO_CFG_BIT2},
    {11,	11,		2,	AS_GPIO_CFG_BIT2},
    {12,	12,		4,	AS_GPIO_CFG_BIT1},
    {13,	13,		5,	AS_GPIO_CFG_BIT2},
    {76,	76,		7,	AS_GPIO_CFG_BIT1},
    {14,	14,		8,	AS_GPIO_CFG_BIT2},
    {15,	15,		10,	AS_GPIO_CFG_BIT2},
    {16,	16,		12,	AS_GPIO_CFG_BIT1},
    {17,	17,		13,	AS_GPIO_CFG_BIT1},
    {18,	18,		14,	AS_GPIO_CFG_BIT1},
    {19,	19,		16,	AS_GPIO_CFG_BIT2},
    {20,	20,		18,	AS_GPIO_CFG_BIT2},
    {21,	21,		20,	AS_GPIO_CFG_BIT1},
    {22,	22,		21,	AS_GPIO_CFG_BIT1},
    {23,	23,		22,	AS_GPIO_CFG_BIT2},
    {24,	24,		24,	AS_GPIO_CFG_BIT2},
    {78,	78,		26,	AS_GPIO_CFG_BIT1},
};

//this used to clr in gpio share pin cfg4 (EV200 only, SYSCTRL + 0xDC)
struct sharepin_as_gpio sharepin_cfg_gpio4[] = {
    {25,	25,		0,	AS_GPIO_CFG_BIT1},
    {26,	26,		1,	AS_GPIO_CFG_BIT1},
    {29,	29,		2,	AS_GPIO_CFG_BIT2},
    {30,	30,		4,	AS_GPIO_CFG_BIT2},
    {31,	31,		6,	AS_GPIO_CFG_BIT1},
    {32,	32,		7,	AS_GPIO_CFG_BIT1},
    {33,	33,		8,	AS_GPIO_CFG_BIT2},
    {34,	34,		10,	AS_GPIO_CFG_BIT2},
    {35,	36,		12,	AS_GPIO_CFG_BIT2},
    {37,	38,		14,	AS_GPIO_CFG_BIT2},
    {39,	39,		16,	AS_GPIO_CFG_BIT2},
    {40,	40,		18,	AS_GPIO_CFG_BIT2},
    {41,	41,		20,	AS_GPIO_CFG_BIT1},	/* ircut_b */
    {42,	42,		21,	AS_GPIO_CFG_BIT1},
    {43,	43,		22,	AS_GPIO_CFG_BIT2},
    {44,	44,		24,	AS_GPIO_CFG_BIT2},
    {45,	46,		26,	AS_GPIO_CFG_BIT2},
};

#else	/* two-bank parts: vendor tables, unchanged */

//this used to clr in gpio chare pin cfg1
struct sharepin_as_gpio sharepin_cfg_gpio1[] = {
    {0,		0,		0,	AS_GPIO_CFG_BIT1},
    {3,		3,		1,	AS_GPIO_CFG_BIT1},
    {47,	47,		2,	AS_GPIO_CFG_BIT2},
    {48,	48,		4,	AS_GPIO_CFG_BIT1},
    {50,	50,		6,	AS_GPIO_CFG_BIT1},
    {51,	51,		7,	AS_GPIO_CFG_BIT1},
    {52,	52,		8,	AS_GPIO_CFG_BIT1},
    {53,	53,		9,	AS_GPIO_CFG_BIT1},
    {54,	54,		10,	AS_GPIO_CFG_BIT1},
    {55,	55,		11,	AS_GPIO_CFG_BIT1},
    {56,	56,		12,	AS_GPIO_CFG_BIT1},
    {57,	57,		13,	AS_GPIO_CFG_BIT1},
    {1,		1,		14,	AS_GPIO_CFG_BIT1},
    {2,		2,		15,	AS_GPIO_CFG_BIT1},
    {4,		4,		16,	AS_GPIO_CFG_BIT2},
    {5,		5,		18,	AS_GPIO_CFG_BIT2},
    {6,		6,		20,	AS_GPIO_CFG_BIT2},
    {7,		7,		22,	AS_GPIO_CFG_BIT2},
};

//this used to clr in gpio chare pin cfg2
struct sharepin_as_gpio sharepin_cfg_gpio3[] = {
    {25,	25,		0,	AS_GPIO_CFG_BIT1},
    {26,	26,		1,	AS_GPIO_CFG_BIT1},
    {29,	29,		2,	AS_GPIO_CFG_BIT2},
    {30,	30,		4,	AS_GPIO_CFG_BIT2},
    {31,	31,		6,	AS_GPIO_CFG_BIT1},
    {32,	32,		7,	AS_GPIO_CFG_BIT1},
    {33,	33,		8,	AS_GPIO_CFG_BIT1},
    {34,	36,		9,	AS_GPIO_CFG_BIT1},
    {37,	38,		10,	AS_GPIO_CFG_BIT2},
    {39,	39,		12,	AS_GPIO_CFG_BIT2},
    {40,	40,		14,	AS_GPIO_CFG_BIT2},
    {41,	41,		16, AS_GPIO_CFG_BIT1},
    {42,	42,		17, AS_GPIO_CFG_BIT1},
    {43,	43,		18, AS_GPIO_CFG_BIT2},
    {44,	44,		20, AS_GPIO_CFG_BIT2},
    {45,	46,		22, AS_GPIO_CFG_BIT2},
    {27,	27,		25, AS_GPIO_CFG_BIT1},
    {28,	28,		26, AS_GPIO_CFG_BIT1},
};

#endif	/* CONFIG_CPU_AK3918EV200 */

#define INVALID_WK_BIT 0xff
struct t_gpio_wakeup_cfg gpio_wakeup_cfg[] = {
	//gpio_start        gpio_end      start_bit
	{AK_GPIO_0,		AK_GPIO_7,   0},
	{AK_GPIO_12,	AK_GPIO_14,  8},
	{AK_GPIO_22,	AK_GPIO_30,  11},
	{AK_GPIO_39,	AK_GPIO_44,  16},
	{AK_GPIO_47,	AK_GPIO_55,  22},
	{AK_GPIO_57,	AK_GPIO_57,  31},
};


unsigned int ak3910_invalid_gpio[] = {
	AK_GPIO_37, AK_GPIO_38, AK_GPIO_39, AK_GPIO_40, AK_GPIO_56,
	AK_GPIO_58, AK_GPIO_59, AK_GPIO_60, AK_GPIO_61, AK_GPIO_62,
	AK_GPIO_63,
};

unsigned int ak3916_invalid_gpio[] = {
};

static unsigned char get_bit_by_pin_wk(unsigned char pin)
{
    int i, n;
    n = ARRAY_SIZE(gpio_wakeup_cfg);

    for (i=0; i<n; i++) {
        if (pin >= gpio_wakeup_cfg[i].gpio_start && pin <= gpio_wakeup_cfg[i].gpio_end)
            return gpio_wakeup_cfg[i].start_bit + (pin - gpio_wakeup_cfg[i].gpio_start);
    }

    return INVALID_WK_BIT;
}

/* when the specific bit is set to 0, the wake-up GPIO is rising triggered
 * when the specific bit is set to 1, the wake-up GPIO is falling triggered
 */
void ak_gpio_wakeup_pol(unsigned int pin, unsigned char pol)
{
    unsigned char bit = get_bit_by_pin_wk(pin);
    unsigned int val;

    if (bit == INVALID_WK_BIT) {
        panic("this pin %u doesn't support wakeup function\n", pin);
        return;
    }
    
    val = REG32(AK_WGPIO_POLARITY);
    val &= ~(1 << bit);
    val |= (pol << bit);
    REG32(AK_WGPIO_POLARITY) = val;
}
EXPORT_SYMBOL(ak_gpio_wakeup_pol);

int ak_gpio_wakeup(unsigned int pin, unsigned char enable)
{
    unsigned char bit = get_bit_by_pin_wk(pin);
    unsigned int val;

    if (bit == INVALID_WK_BIT) {
        panic("this pin %d doesn't support wakeup function\n", pin);
        return -1;
    }
    //clear wake gpio status
    val = REG32(AK_WGPIO_CLEAR);
    val |= (1 << bit);
    REG32(AK_WGPIO_CLEAR) = val;
    val &= ~(1 << bit);
    REG32(AK_WGPIO_CLEAR) = val;
    
    val = REG32(AK_WGPIO_ENABLE);
    if (enable == AK_WAKEUP_ENABLE) {
        val |= (1 << bit);
    } else if (enable == AK_WAKEUP_DISABLE) {
        val &= ~(1 << bit);
    } else
        panic("wrong enable value in ak_gpio_wakeup\n");
    REG32(AK_WGPIO_ENABLE) = val;
    
    return 0;
}
EXPORT_SYMBOL(ak_gpio_wakeup);

/*
 * @brief set gpio pin group as specified module used
 * @param[in] PinCfg enum data. the specified module
 */
void ak_group_config(T_GPIO_SHAREPIN_CFG mod_name)
{
    unsigned long i, flags, val = 0;
    
    if(ePIN_AS_GPIO == mod_name) {
        //set all pin as gpio except uart0
        local_irq_save(flags);
#ifdef CONFIG_CPU_AK3918EV200
        /*
         * The two-bank constants would clear CON1[2:1] here, which on this
         * part is exactly the UART0 console pair.  CON1 = 0x6 keeps pads 1
         * and 2 muxed as UART0.  CON2 = 0x550 follows from
         * sharepin_cfg_gpio2[]: pads 6..9 are two-bit fields whose GPIO
         * encoding is 0b01, at bits 4, 6, 8 and 10.
         *
         * Nothing in this tree calls ak_group_config(ePIN_AS_GPIO) today;
         * these are kept correct so the first caller is not surprised.
         */
        __raw_writel(0x6, AK_SHAREPIN_CON1);
		__raw_writel(0x550, AK_SHAREPIN_CON2);
		__raw_writel(0x0, AK_SHAREPIN_CON3);
		__raw_writel(0x0, AK_SHAREPIN_CON4);
#else
        __raw_writel(0xc000, AK_SHAREPIN_CON1);
		__raw_writel(0xf, AK_SHAREPIN_CON2);
		__raw_writel(0x0, AK_SHAREPIN_CON3);
#endif
        local_irq_restore(flags);
        return;
    }

    for(i = 0; ; i++) {
        if(ePIN_AS_DUMMY == share_cfg_module[i].func_module)
            break;

        if(mod_name == share_cfg_module[i].func_module) {    
            //set pull attribute for module
            g_ak39_setgroup_attribute(mod_name);
            
            local_irq_save(flags);
            switch(share_cfg_module[i].share_config) {
                case SHARE_CFG1: //set share pin cfg reg1
                    val = __raw_readl(AK_SHAREPIN_CON1);
                    val &= ~(share_cfg_module[i].reg1_bit_mask);
                    val |= (share_cfg_module[i].reg1_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON1);
                    break;
                    
                case SHARE_CFG2: //set share pin cfg reg2
                    val = __raw_readl(AK_SHAREPIN_CON2);
                    val &= ~(share_cfg_module[i].reg2_bit_mask);
                    val |= (share_cfg_module[i].reg2_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON2);
                    break;
					
                case SHARE_CFG3: //set share pin cfg reg3
                    val = __raw_readl(AK_SHAREPIN_CON3);
                    val &= ~(share_cfg_module[i].reg3_bit_mask);
                    val |= (share_cfg_module[i].reg3_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON3);
                    break;

#ifdef CONFIG_CPU_AK3918EV200
                case SHARE_CFG4: //set share pin cfg reg4
                    val = __raw_readl(AK_SHAREPIN_CON4);
                    val &= ~(share_cfg_module[i].reg4_bit_mask);
                    val |= (share_cfg_module[i].reg4_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON4);
                    break;

                case SHARE_CFG14:
                    val = __raw_readl(AK_SHAREPIN_CON1);
                    val &= ~(share_cfg_module[i].reg1_bit_mask);
                    val |= (share_cfg_module[i].reg1_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON1);

                    val = __raw_readl(AK_SHAREPIN_CON4);
                    val &= ~(share_cfg_module[i].reg4_bit_mask);
                    val |= (share_cfg_module[i].reg4_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON4);
                    break;

                case SHARE_CFG134:
                    val = __raw_readl(AK_SHAREPIN_CON1);
                    val &= ~(share_cfg_module[i].reg1_bit_mask);
                    val |= (share_cfg_module[i].reg1_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON1);

                    val = __raw_readl(AK_SHAREPIN_CON3);
                    val &= ~(share_cfg_module[i].reg3_bit_mask);
                    val |= (share_cfg_module[i].reg3_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON3);

                    val = __raw_readl(AK_SHAREPIN_CON4);
                    val &= ~(share_cfg_module[i].reg4_bit_mask);
                    val |= (share_cfg_module[i].reg4_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON4);
                    break;
#endif	/* CONFIG_CPU_AK3918EV200 */

                case SHARE_CFG12:
					val = __raw_readl(AK_SHAREPIN_CON1);
                    val &= ~(share_cfg_module[i].reg1_bit_mask);
                    val |= (share_cfg_module[i].reg1_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON1);
                    
                    val = __raw_readl(AK_SHAREPIN_CON2);
                    val &= ~(share_cfg_module[i].reg2_bit_mask);
                    val |= (share_cfg_module[i].reg2_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON2);
                    break;
				case SHARE_CFG13: 
                    val = __raw_readl(AK_SHAREPIN_CON1);
                    val &= ~(share_cfg_module[i].reg1_bit_mask);
                    val |= (share_cfg_module[i].reg1_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON1);
                    
                    val = __raw_readl(AK_SHAREPIN_CON3);
                    val &= ~(share_cfg_module[i].reg3_bit_mask);
                    val |= (share_cfg_module[i].reg3_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON3);
                    break;
					
				case SHARE_CFG23: 
                    val = __raw_readl(AK_SHAREPIN_CON2);
                    val &= ~(share_cfg_module[i].reg2_bit_mask);
                    val |= (share_cfg_module[i].reg2_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON2);
                    
                    val = __raw_readl(AK_SHAREPIN_CON3);
                    val &= ~(share_cfg_module[i].reg3_bit_mask);
                    val |= (share_cfg_module[i].reg3_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON3);
                    break;
					
				case SHARE_CFG123:
                    val = __raw_readl(AK_SHAREPIN_CON1);
                    val &= ~(share_cfg_module[i].reg1_bit_mask);
                    val |= (share_cfg_module[i].reg1_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON1);
                    
                    val = __raw_readl(AK_SHAREPIN_CON2);
                    val &= ~(share_cfg_module[i].reg2_bit_mask);
                    val |= (share_cfg_module[i].reg2_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON2);
					
					val = __raw_readl(AK_SHAREPIN_CON3);
                    val &= ~(share_cfg_module[i].reg3_bit_mask);
                    val |= (share_cfg_module[i].reg3_bit_value);
                    __raw_writel(val, AK_SHAREPIN_CON3);
                    break;
					
                default:
                    break;
            }
            local_irq_restore(flags);
            return ;
        }
    }
    return ;
}
EXPORT_SYMBOL(ak_group_config);

static unsigned char gpio_assert_legal(unsigned long pin)
{
    int i, len;
    unsigned int *gpio_legal;

    if ((pin < 0) || (pin > GPIO_UPLIMIT)) {
        return AK_FALSE;
    }
    
#if defined(CONFIG_CPU_AK3910)
    len = ARRAY_SIZE(ak3910_invalid_gpio);
    gpio_legal = ak3910_invalid_gpio;
#elif defined(CONFIG_CPU_AK3916) || defined(CONFIG_CPU_AK3918)
	len = ARRAY_SIZE(ak3916_invalid_gpio);
    gpio_legal = ak3916_invalid_gpio;
#endif

    for(i = 0; i < len; i++) {
        if(gpio_legal[i] == pin)
            return AK_FALSE;    
    }
    return AK_TRUE;
}


/**
 * @brief drive one share-pin table entry back to GPIO, if it covers this pad
 * @param tbl [in]  share-pin table
 * @param n   [in]  entries in tbl
 * @param reg [in]  share-pin register the table describes
 * @param pin [in]  gpio pin ID
 * @return 0 if an entry matched and was applied, -ENOENT otherwise
 */
static int setpin_walk(const struct sharepin_as_gpio *tbl, size_t n,
		       unsigned long reg, unsigned int pin)
{
	unsigned long flags;
	size_t i;
	int hit = 0;

	for (i = 0; i < n; i++) {
		if (pin < tbl[i].gpio_start || pin > tbl[i].gpio_end)
			continue;

		local_irq_save(flags);
		switch (tbl[i].flag) {
		case AS_GPIO_CFG_BIT1:
			REG32(reg) &= ~(1 << tbl[i].index);
			break;
		case AS_GPIO_CFG_BIT2:
			REG32(reg) &= ~(0x3 << tbl[i].index);
			break;
		case AS_GPIO_CFG_BIT2_01:
			REG32(reg) &= ~(0x3 << tbl[i].index);
			REG32(reg) |= (1 << tbl[i].index);
			break;
		}
		local_irq_restore(flags);
		hit = 1;
	}

	return hit ? 0 : -ENOENT;
}

/**
 * @brief set gpio share pin as gpio
 * @param pin [in]  gpio pin ID
 */
int g_ak39_setpin_as_gpio(unsigned int pin)
{
    //check param
    if(!gpio_assert_legal(pin)) {
        panic("Error, Invalid gpio %u configuration!\n", pin);
        return -1;
    }

#ifdef CONFIG_CPU_AK3918EV200

	/*
	 * Reserve the UART0 console pads.  On this part they are CON1[1] and
	 * CON1[2]; the two-bank layout has them at CON1[15:14].
	 */
	if (AK_GPIO_1 == pin || AK_GPIO_2 == pin) {
		REG32(AK_SHAREPIN_CON1) |= (0x3 << 1);
		return -1;
	}

	if (!setpin_walk(sharepin_cfg_gpio1, ARRAY_SIZE(sharepin_cfg_gpio1),
			 AK_SHAREPIN_CON1, pin))
		return 0;
	if (!setpin_walk(sharepin_cfg_gpio2, ARRAY_SIZE(sharepin_cfg_gpio2),
			 AK_SHAREPIN_CON2, pin))
		return 0;
	if (!setpin_walk(sharepin_cfg_gpio3, ARRAY_SIZE(sharepin_cfg_gpio3),
			 AK_SHAREPIN_CON3, pin))
		return 0;
	if (!setpin_walk(sharepin_cfg_gpio4, ARRAY_SIZE(sharepin_cfg_gpio4),
			 AK_SHAREPIN_CON4, pin))
		return 0;

	/* No entry: the pad has no mux and is always a GPIO. */
	return 0;

#else	/* two-bank parts */

	unsigned long flags;

	/* reserved uart0 confige, but provide the error info  */
    if(AK_GPIO_1 == pin || AK_GPIO_2 == pin) {
        REG32(AK_SHAREPIN_CON1) |= (0x3 << 14);
		return -1;
	}

	if (!setpin_walk(sharepin_cfg_gpio1, ARRAY_SIZE(sharepin_cfg_gpio1),
			 AK_SHAREPIN_CON1, pin))
		return 0;

    //find the correct bits to clr in share ping cfg2
    if ((pin >= AK_GPIO_8) && (pin <= AK_GPIO_24)) {
        local_irq_save(flags);
        if (pin <= AK_GPIO_11)
            REG32(AK_SHAREPIN_CON2) |= (1<<(pin-8));
        else
            REG32(AK_SHAREPIN_CON2) &= ~(1<<(pin-8));
        local_irq_restore(flags);
        return 0;
    }

	if (!setpin_walk(sharepin_cfg_gpio3, ARRAY_SIZE(sharepin_cfg_gpio3),
			 AK_SHAREPIN_CON3, pin))
		return 0;

    return 0;

#endif	/* CONFIG_CPU_AK3918EV200 */
}

/*
    enable: 1:enable pullup 0:disable pullup function
      if the pin is attached pullup and pulldown resistor, then writing 1 to enable
        pullup, 0 to enable pulldown, if you want to disable pullup/pulldown, then 
        disable the PE parameter
*/
int g_ak39_gpio_pullup(unsigned int pin, unsigned char enable)
{
    void __iomem *base = AK_PPU_PPD_BASE(pin);
    unsigned long flags;
	int i;
	
   if(!gpio_assert_legal(pin)) {
        panic("Error, Invalid gpio %u configuration!\n", pin);
        return -1;
    }

	for (i = 0; i < ARRAY_SIZE(pupd_cfg_info); i++) {
		if (pin == pupd_cfg_info[i].pin) {

			if (pupd_cfg_info[i].pupd_type == PULLDOWN)
				panic("Invalid GPIO[%d] pullup config.\n", pin);
			
			switch(pupd_cfg_info[i].pupd_cfg) {
				case PUPD_CFG1:
					base = AK_PPU_PPD1;
					break;
				case PUPD_CFG2:
					base = AK_PPU_PPD2;
					break;
				case PUPD_CFG3:
					base = AK_PPU_PPD3;
					break;
#ifdef CONFIG_CPU_AK3918EV200
				case PUPD_CFG4:
					base = AK_PPU_PPD4;
					break;
#endif
			}

			local_irq_save(flags);
			if (enable == AK_PULLUP_ENABLE)
				REG32(base)	&= ~(1 << pupd_cfg_info[i].index);
			else if (enable == AK_PULLUP_DISABLE)
				REG32(base)	|= (1 << pupd_cfg_info[i].index);
			local_irq_restore(flags);
			break;
		}
	}

	return 0;
}

//1.enable pulldown 0.disable pulldown
 int g_ak39_gpio_pulldown(unsigned int pin, unsigned char enable)
{
    void __iomem *base = AK_PPU_PPD_BASE(pin);
    unsigned long flags;
	int i;
    
    if(!gpio_assert_legal(pin)) {
        panic("Error, Invalid gpio %u configuration!\n", pin);
        return -1;
    }
    
	for (i = 0; i < ARRAY_SIZE(pupd_cfg_info); i++) {
		if (pin == pupd_cfg_info[i].pin) {

			if (pupd_cfg_info[i].pupd_type == PULLUP)
				panic("Invalid GPIO[%d] pulldown config.\n", pin);
			
			switch(pupd_cfg_info[i].pupd_cfg) {
				case PUPD_CFG1:
					base = AK_PPU_PPD1;
					break;
				case PUPD_CFG2:
					base = AK_PPU_PPD2;
					break;
				case PUPD_CFG3:
					base = AK_PPU_PPD3;
					break;
#ifdef CONFIG_CPU_AK3918EV200
				case PUPD_CFG4:
					base = AK_PPU_PPD4;
					break;
#endif
			}

			local_irq_save(flags);
			if (enable == AK_PULLDOWN_ENABLE)
				REG32(base)	&= ~(1 << pupd_cfg_info[i].index);
			else if (enable == AK_PULLDOWN_DISABLE)
				REG32(base)	|= (1 << pupd_cfg_info[i].index);
			local_irq_restore(flags);
			break;
		}
	}

	return 0; 
}

void g_ak39_setgroup_attribute(T_GPIO_SHAREPIN_CFG mod_name)
{
    unsigned long pin, start_pin = 0, end_pin = 0;
    
    switch (mod_name) {
        case ePIN_AS_MCI:
            start_pin = 31, end_pin = 36;
            for (pin = start_pin; pin <= end_pin; pin++)
            {         
                g_ak39_gpio_pullup(pin, AK_TRUE);
            }
            break; 

		case ePIN_AS_MCI_8LINE:
            start_pin = 31, end_pin = 40;
            for (pin = start_pin; pin <= end_pin; pin++)
            {         
                g_ak39_gpio_pullup(pin, AK_TRUE);
            }
            break;
			
	    case ePIN_AS_SDIO:
            start_pin = 41, end_pin = 46;
            for (pin = start_pin; pin <= end_pin; pin++)
            {
                g_ak39_gpio_pullup(pin, AK_TRUE);
            }
            break;
			
        case ePIN_AS_SPI1:
			/* spi_clk  #spi_cs */
            start_pin = 25, end_pin = 26;
            for (pin = start_pin; pin <= end_pin; pin++)
            {
                g_ak39_gpio_pullup(pin, AK_TRUE);
            }

			start_pin = 43, end_pin = 46;
            for (pin = start_pin; pin <= end_pin; pin++)
            {
                g_ak39_gpio_pullup(pin, AK_TRUE);
            }
			
            break;
			
        case ePIN_AS_SPI2:
			/* spi_clk  #spi_cs */
            start_pin = 29, end_pin = 30;
            for (pin = start_pin; pin <= end_pin; pin++)
            {
                g_ak39_gpio_pullup(pin, AK_TRUE);
            }
			
            start_pin = 43, end_pin = 46;
            for (pin = start_pin; pin <= end_pin; pin++)
            {
                g_ak39_gpio_pullup(pin, AK_TRUE);
            }
            break;  
			
        case ePIN_AS_I2S:
            start_pin = 52, end_pin = 53;
            for (pin = start_pin; pin <= end_pin; pin++)
            {
                g_ak39_gpio_pullup(pin, AK_FALSE);   
            }
            g_ak39_gpio_pullup(AK_GPIO_57, AK_FALSE);
			g_ak39_gpio_pulldown(AK_GPIO_55, AK_FALSE);
			g_ak39_gpio_pullup(AK_GPIO_55, AK_FALSE);
            break;
            
        case ePIN_AS_UART1: 
            start_pin = 1, end_pin = 2;
            for (pin = start_pin; pin <= end_pin; pin++)
            {
                g_ak39_gpio_pullup(pin, AK_TRUE);
            }
            break;
            
        case ePIN_AS_UART2: 
            start_pin = 4, end_pin = 7;           
            for (pin = start_pin; pin <= end_pin; pin++)
            {
                g_ak39_gpio_pullup(pin, AK_TRUE);                
            }   
            break;
            
        case ePIN_AS_I2C:
            start_pin = 27, end_pin = 28;
            for (pin = start_pin; pin <= end_pin; pin++)
            {
                g_ak39_gpio_pullup(pin, AK_FALSE);
            }
            break;
        default:
            break;
    }
}

/* 
 * configuration gpio pin
 * 0: corresponding port is input mode
 * 1: corresponding port is output mode
 */
int g_ak39_gpio_cfgpin(unsigned int pin, unsigned int to)
{
    void __iomem *base = AK_GPIO_DIR_BASE(pin);
    unsigned int offset = ((pin) & 31);
    unsigned long flags;
    
     if(!gpio_assert_legal(pin)) {
        panic("Error, Invalid gpio %d configuration!\n", pin);
        return -1;
    }

	/* gpio[49] can't set output mode for ak39xx*/
	if ((to == AK_GPIO_DIR_OUTPUT)&&(pin == AK_GPIO_49)) {
		panic("Error, gpio %d isn't config output mode\n", pin);
		return -1;
	}

    local_irq_save(flags);
    if (AK_GPIO_DIR_INPUT == to)
        REG32(base) &= ~(1 << offset);
    else if (AK_GPIO_DIR_OUTPUT == to)
        REG32(base) |= (1 << offset);
    local_irq_restore(flags);
	
    return 0;
}

/* hold the real-time output value from GPIO[x] */ 
int g_ak39_gpio_setpin(unsigned int pin, unsigned int to)
{
    void __iomem *base = AK_GPIO_OUT_BASE(pin);
    unsigned int offset = ((pin) & 31);
    unsigned long flags;
        
    if(!gpio_assert_legal(pin)) {
        panic("Error, Invalid gpio %d configuration!\n", pin);
        return -1;
    }

	/* gpio[49] can't set output level for ak39xx*/
	if (pin == AK_GPIO_49) {
		panic("Error, gpio %d isn't config outpu level\n", pin);
		return -1;
	}

    local_irq_save(flags);
    if (AK_GPIO_OUT_LOW == to)
        REG32(base) &= ~(1 << offset);
    else if (AK_GPIO_OUT_HIGH == to)
        REG32(base) |= (1 << offset);
    local_irq_restore(flags);
	
    return 0;
}


/* hold the real-time input value of GPIO[x] */ 
 int g_ak39_gpio_getpin(unsigned int pin)
{
    void __iomem *base = AK_GPIO_IN_BASE(pin);
    unsigned int offset = ((pin) & 31);

    if(!gpio_assert_legal(pin)) {
        panic("Error, read invalid gpio %d status!\n", pin);
        return -1;
    }
    return ((__raw_readl(base) & (1 << offset)) == (1 << offset));
}


/* 
 * enalbe/disable the interrupt function of GPIO[X]
 * 1: interrupt function of corresponding port is enable
 * 0: interrupt function of corresponding port is disable
 */
int g_ak39_gpio_inten(unsigned int pin, unsigned int enable)
{
    void __iomem *base = AK_GPIO_INTEN_BASE(pin);
    unsigned int offset = ((pin) & 31);
    unsigned long flags;

    if(!gpio_assert_legal(pin)) {
        panic("Error, invalid gpio %d!\n", pin);
        return -1;
    }
    
    local_irq_save(flags);
    if (AK_GPIO_INT_ENABLE == enable)
        REG32(base) |= (1 << offset);
    else if (AK_GPIO_INT_DISABLE == enable)
        REG32(base) &= ~(1 << offset);
    local_irq_restore(flags);
	
    return 0;
}


/*
 * interrupt polarity selection
 * 0: the input interrupt polarity of GPIO[X] is active high
 * 1: the input interrupt polarity of GPIO[X] is active low
 */
int g_ak39_gpio_intpol(unsigned int pin, unsigned int level)
{
    void __iomem *base = AK_GPIO_INTPOL_BASE(pin);
    unsigned int offset = ((pin) & 31);
    unsigned long flags;

    if(!gpio_assert_legal(pin)) {
        panic("Error, invalid gpio %d!\n", pin);
        return -1;
    }
    
    local_irq_save(flags);
    if (AK_GPIO_INT_HIGHLEVEL == level)
        REG32(base) &= ~(1 << offset);
    else if (AK_GPIO_INT_LOWLEVEL == level)
        REG32(base) |= (1 << offset);
    local_irq_restore(flags);
	
    return 0;
}

int reg_set_mutli_bit(void __iomem *reg, unsigned int value, int bit, int index)
{
	unsigned long con, flags;

	if (bit <= 0)
		return -1;
	
	local_irq_save(flags);
	
	con = __raw_readl(reg);
	con &= ~(((1 << bit) - 1) << index);
	con |= (value << index);
	__raw_writel(con, reg);
	
	local_irq_restore(flags);
	return 0;
}
EXPORT_SYMBOL(reg_set_mutli_bit);

int g_ak39_gpio_to_irq(unsigned int pin)
{
    if(!gpio_assert_legal(pin)) {
        panic("Error, invalid gpio %d!\n", pin);
        return -1;
    }
    return (IRQ_GPIO_0 + (pin - AK_GPIO_0));
}

int g_ak39_irq_to_gpio(unsigned int irq)
{
    return (AK_GPIO_0 + (irq - IRQ_GPIO_0));
}


#if 0
static const char *ak39_gpio_list[AK_GPIO_MAX];

int ak_gpio_request(unsigned long gpio, const char *label)
{
    if (gpio > GPIO_UPLIMIT)
        return -EINVAL;
    
    if (ak39_gpio_list[gpio])
        return -EBUSY;
    
    if (label)
        ak39_gpio_list[gpio] = label;
    else
        ak39_gpio_list[gpio] = "busy";
    
    return 0;
}
EXPORT_SYMBOL(ak_gpio_request);

void ak_gpio_free(unsigned long gpio)
{
    BUG_ON(!ak39_gpio_list[gpio]);
    
    ak39_gpio_list[gpio] = NULL;
}
EXPORT_SYMBOL(ak_gpio_free);

#else

int ak_gpio_request(unsigned long gpio, const char *label)
{
    return 0;
}
EXPORT_SYMBOL(ak_gpio_request);

void ak_gpio_free(unsigned long gpio)
{
}
EXPORT_SYMBOL(ak_gpio_free);

#endif

