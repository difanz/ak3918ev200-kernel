/* linux/arch/arm/mach-ak39/devices.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <plat-anyka/ak_camera.h>

#include <asm/irq.h>
#include <asm/gpio.h>
#include <mach/i2c.h>

/**
 * @brief: uart0 device info
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
struct platform_device ak39_uart0_device = {
    .name   = "ak39-uart",
    .id     = 0,
};
EXPORT_SYMBOL(ak39_uart0_device);

/**
 * @brief: uart1 device info
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
struct platform_device ak39_uart1_device = {
    .name   = "ak39-uart",
    .id     = 1,
};
EXPORT_SYMBOL(ak39_uart1_device);

/**
 * @brief: gpio uart device info
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
struct platform_device ak39_gpio_uart_device = {
    .name   = "gpio-uart",
    .id     = 0,
};
EXPORT_SYMBOL(ak39_gpio_uart_device);


/**
 * @brief: MCI device info
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static struct resource ak39_mmc_resource[] = {
	[0] = {
		.start = 0x20100000,
		.end = 0x20100000 + 0x43,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_MCI,
		.flags = IORESOURCE_IRQ,
	},
};

struct platform_device ak39_mmc_device = {
	.name = "ak_mci",
	.id = -1,
	.num_resources = ARRAY_SIZE(ak39_mmc_resource),
	.resource = ak39_mmc_resource,
};
EXPORT_SYMBOL(ak39_mmc_device);

/**
 * @brief: SDIO device info
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static struct resource ak39_sdio_resource[] = {
	[0] = {
		.start = 0x20108000,
		.end = 0x20108000 + 0x43,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_SDIO,
		.flags = IORESOURCE_IRQ,
	},
};

struct platform_device ak39_sdio_device = {
	.name = "ak_sdio",
	.id = -1,
	.num_resources = ARRAY_SIZE(ak39_sdio_resource),
	.resource = ak39_sdio_resource,
};
EXPORT_SYMBOL(ak39_sdio_device);


