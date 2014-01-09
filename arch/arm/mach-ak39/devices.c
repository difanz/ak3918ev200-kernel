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


/**
 * @brief: I2C device info
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
#if defined(CONFIG_I2C_AK39_HW)
struct gpio_info i2c_gpios[] = {
	{
		.pin 		= AK_GPIO_27,
		.pulldown 	= -1,
		.pullup 	= AK_PULLUP_DISABLE,
		.dir		= AK_GPIO_DIR_OUTPUT,
		.value 		= AK_GPIO_OUT_HIGH,
		.int_pol	= -1,
	},
	{
		.pin 		= AK_GPIO_28,
		.pulldown 	= -1,
		.pullup 	= AK_PULLUP_DISABLE,
		.dir		= AK_GPIO_DIR_OUTPUT,
		.value 		= AK_GPIO_OUT_HIGH,
		.int_pol	= -1,

	},
};

static struct ak39_platform_i2c ak39_default_i2c_data = {
	.flags		= 0,
	.bus_num	= 0,
	.slave_addr	= 0x10,
	.frequency	= 100*1000,
	.sda_delay	= 100,
	.gpios		= i2c_gpios,
	.npins		= ARRAY_SIZE(i2c_gpios),
};

static struct resource ak39_i2c_resource[] = {
	[0] = {
		.start = 0x20150000,
		.end   = 0x20150000+SZ_256,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_I2C,
		.end   = IRQ_I2C,
		.flags = IORESOURCE_IRQ,
	},
};

struct platform_device ak39_i2c_device = {
	.name	= "i2c-ak39",
	.id		= -1,
	.dev	= {
		.platform_data = &ak39_default_i2c_data,
	},
	.num_resources	= ARRAY_SIZE(ak39_i2c_resource),
	.resource		= ak39_i2c_resource,
};
EXPORT_SYMBOL(ak39_i2c_device);

#elif defined(CONFIG_I2C_GPIO_SOFT)
struct i2c_gpio_platform_data ak39_i2c_data={
	.sda_pin = INVALID_GPIO,
	.scl_pin = INVALID_GPIO,
	.udelay = 10,
	.timeout = 200
};

struct platform_device ak39_i2c_device = {
	.name	= "i2c-gpio",
	.id		= -1,
	.dev	= {
		.platform_data = &ak39_i2c_data,
	},
};
EXPORT_SYMBOL(ak39_i2c_device);
#else
struct platform_device ak39_i2c_device = {
	.name   = "i2c",
	.id     = -1,
};
EXPORT_SYMBOL(ak39_i2c_device);
#endif


/**
 * @brief: USB otg host device info
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static struct resource usb_otg_hcd_resource[] = {
	[0] = {
		.start	= 0x20200000,
		.end	= 0x202003ff,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.name	= "usb mcu irq",
		.start	= IRQ_USBOTG_MCU,
		.flags	= IORESOURCE_IRQ,
	},
	[2] = {
		.name	= "usb dma irq",
		.start	= IRQ_USBOTG_DMA,
		.flags	= IORESOURCE_IRQ,
	},
};

struct platform_device ak39_usb_otg_hcd_device = {
	.name = "usb-host",
	.id = -1,
	.num_resources = ARRAY_SIZE(usb_otg_hcd_resource),
	.resource = usb_otg_hcd_resource,
};
EXPORT_SYMBOL(ak39_usb_otg_hcd_device);

/**
 * @brief: MAC device info
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static struct resource ak39_mac_resource[] = {
	[0] = {
	   .start = 0x20300000,
	   .end = 0x20301fff,
	   .flags = IORESOURCE_MEM,
	},
	[1] = {
	   .name = "mac irq",
	   .start = IRQ_MAC,
	   .flags = IORESOURCE_IRQ,
	},
};

struct platform_device ak39_mac_device = {
	.name = "ak_ethernet",
	.id = 0,
	.num_resources = ARRAY_SIZE(ak39_mac_resource),
	.resource = ak39_mac_resource,
};
EXPORT_SYMBOL(ak39_mac_device);

/**
 * @brief: SPI device info
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static struct resource ak39_spi1_resource[] = {
	[0] = {
		.start = 0x20120000,
		.end = 0x20120027,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_SPI1,
		.end = IRQ_SPI1,
		.flags = IORESOURCE_IRQ,
	}
};

struct platform_device ak39_spi1_device = {
	.name = "ak-spi",
	.id = -1,
	.num_resources = ARRAY_SIZE(ak39_spi1_resource),
	.resource = ak39_spi1_resource,
};
EXPORT_SYMBOL(ak39_spi1_device);


/**
 * @brief: Camera interface resource info
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static struct resource ak39_camera_resource[] = {
	[0] = {
		.name = "camera if irq",
		.start = IRQ_CAMERA,
		.flags = IORESOURCE_IRQ,
	},
	[1] = {
		.start = 0x20000000,
		.end = 0x20000000 + 0x30,
		.flags = IORESOURCE_MEM,
	},
};

/* camera interface */
struct platform_device ak39_camera_interface = {
	.name = "ak_camera",
	.id   = 39,
	.num_resources	= ARRAY_SIZE(ak39_camera_resource),	
	.resource = ak39_camera_resource,	
};

EXPORT_SYMBOL(ak39_camera_interface);


