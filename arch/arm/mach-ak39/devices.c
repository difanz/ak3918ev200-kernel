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


