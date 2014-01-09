/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Board data is specific to this unit.  Do not replace these GPIO assignments
 * with values from another AK3918 camera or EV board.
 */

#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <asm/sizes.h>
#include <linux/irq.h>
#include <linux/i2c.h>
#include <linux/i2c-gpio.h>

#include <plat-anyka/wifi.h>
#include <plat-anyka/otg-hshcd.h>
#include <plat-anyka/ak_camera.h>
#include <plat-anyka/aksensor.h>

#include <asm/mach/arch.h>
#include <asm/mach-types.h>
#include <asm/irq.h>

#include <plat/l2.h>
#include <mach/devices.h>
#include <mach/gpio.h>
#include <mach/ak_codec.h>

#include <mach/spi.h>
#include <linux/spi/flash.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <plat-anyka/akmci.h>
#include <plat-anyka/adkey.h>

#include <linux/input.h>
#include <plat-anyka/gpio_keys.h>
#include <plat-anyka/bat.h>

#include <plat-anyka/ak_motor.h>
#include "cpu.h"
#include "irq.h"
#include <mach/adc.h>
#include <plat-anyka/user_gpio.h>

/**
 * @brief: spi device info
 * 
 * @author: lixinhai
 * @date: 2014-01-09
 */
#define SPI_ONCHIP_CS 	(0)		/*means not need gpio*/
static unsigned long ak39_spidev_cs[AKSPI_CS_NUM] = {
	[AKSPI_ONCHIP_CS] = SPI_ONCHIP_CS,	/*gpio 25, spidev0: ak-spiflash*/
};

struct ak_spi_info ak39_spi1_info = {
	.pin_cs = ak39_spidev_cs,
	.num_cs = ARRAY_SIZE(ak39_spidev_cs),
	.bus_num = AKSPI_BUS_NUM1,
	.clk_name = "spi1",
	.mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH,
	.xfer_mode = AKSPI_XFER_MODE_DMA,
};

/**
 * @brief: spiflash device info
 * 
 * @author: lixinhai
 * @date: 2014-01-09
 */
static struct flash_platform_data ak39_spiflash_info= {
	.bus_width = FLASH_BUS_WIDTH_4WIRE | FLASH_BUS_WIDTH_2WIRE | FLASH_BUS_WIDTH_1WIRE,
	.type 	   = NULL,
};

/**
 * @brief: spi bus device info
 * 
 * @author: lixinhai
 * @date: 2014-01-09
 */
static struct spi_board_info ak39_spi_board_dev[] = {
	{ 		
		.modalias = "ak-spiflash",	
		.bus_num = AKSPI_BUS_NUM1,		
		.chip_select = AKSPI_ONCHIP_CS,
		.mode = SPI_MODE_0,
		.max_speed_hz = 20*1000*1000,
		.platform_data = &ak39_spiflash_info,
	},
};


/**
 * @brief: motor0 device info
 * 
 * @author: lixinhai
 * @date: 2014-01-09
 */
static struct ak_motor_plat_data ak39_motor0_pdata = {
		.gpio_phase[0] = {
			.pin = AK_GPIO_19,
			.pulldown = -1,
			.pullup	= -1,
			.value	= AK_GPIO_OUT_LOW,
			.dir	= AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
		.gpio_phase[1] = {
			.pin = AK_GPIO_20,
			.pulldown = -1,
			.pullup	= -1,
			.value	= AK_GPIO_OUT_LOW,
			.dir	= AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
		.gpio_phase[2] = {
			.pin = AK_GPIO_10,
			.pulldown = -1,
			.pullup	= -1,
			.value	= AK_GPIO_OUT_LOW,
			.dir	= AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
		.gpio_phase[3] = {
			.pin = AK_GPIO_11,
			.pulldown = -1,
			.pullup	= -1,
			.value	= AK_GPIO_OUT_LOW,
			.dir	= AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},

		.gpio_hit[0] = {
			.pin = -1,
			.pulldown = -1,
			.pullup	= -1,
			.value	= -1,
			.dir	= AK_GPIO_DIR_INPUT,
			.int_pol = -1,
		},
		.gpio_hit[1] ={
			.pin = -1,
			.pulldown = -1,
			.pullup	= -1,
			.value	= -1,
			.dir	= AK_GPIO_DIR_INPUT,
			.int_pol = -1,
		},
	.irq_hit_type[0] = IRQ_TYPE_LEVEL_LOW,
	.irq_hit_type[1] = IRQ_TYPE_LEVEL_LOW,

	.angular_speed = 200,	/* exact stock platform data */
};


/**
 * @brief: motor1 device info
 * 
 * @author: lixinhai
 * @date: 2014-01-09
 */
static struct ak_motor_plat_data ak39_motor1_pdata = {
		.gpio_phase[0] = {
			.pin = AK_GPIO_15,
			.pulldown = -1,
			.pullup	= -1,
			.value	= AK_GPIO_OUT_LOW,
			.dir	= AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
		.gpio_phase[1] = {
			.pin = AK_GPIO_14,
			.pulldown = -1,
			.pullup	= -1,
			.value	= AK_GPIO_OUT_LOW,
			.dir	= AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
		.gpio_phase[2] = {
			.pin = AK_GPIO_13,
			.pulldown = -1,
			.pullup	= -1,
			.value	= AK_GPIO_OUT_LOW,
			.dir	= AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
		.gpio_phase[3] = {
			.pin = AK_GPIO_23,
			.pulldown = -1,
			.pullup	= -1,
			.value	= AK_GPIO_OUT_LOW,
			.dir	= AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},

		.gpio_hit[0] = {
			.pin = -1,
			.pulldown = -1,
			.pullup	= -1,
			.value	= -1,
			.dir	= AK_GPIO_DIR_INPUT,
			.int_pol = -1,
		},
		.gpio_hit[1] ={
			.pin = -1,
			.pulldown = -1,
			.pullup	= -1,
			.value	= -1,
			.dir	= AK_GPIO_DIR_INPUT,
			.int_pol = -1,
		},
	.irq_hit_type[0] = IRQ_TYPE_LEVEL_LOW,
	.irq_hit_type[1] = IRQ_TYPE_LEVEL_LOW,

	.angular_speed = 100,	/* exact stock platform data */
};


/**
 * @brief:  MMC/SD device platform data
 * 
 * @author: lixinhai
 * @date: 2014-01-09
 */
struct ak_mci_platform_data mmc_plat_data = {
	.irq_cd_type = IRQ_TYPE_LEVEL_LOW,
	/* Pad 34 is wifi_en on this board, so it is left out of the MCI mux
	 * group and DAT1 never reaches the controller. */
	.data_lines = 1,
	/* Card detect on AK_GPIO_5 reports an empty slot with a card inserted, and
	 * the pin is declared with neither pull configured. The slot is internal
	 * and never hot-swapped, so skip detection rather than guess the pull. */
	.detect_mode = AKMCI_PLUGIN_ALWAY,
	.xfer_mode	 = AKMCI_XFER_L2DMA,
	.mci_mode = MCI_MODE_MMC_SD,
	.max_speed_hz = 25*1000*1000,
    .gpio_init = ak_gpio_set,
    .gpio_cd = {
        .pin = AK_GPIO_5,
        .pulldown = -1, 
        .pullup = -1,
        .value = -1,
        .dir = AK_GPIO_DIR_INPUT,
        .int_pol = -1,
    },
    .gpio_wp = {
        .pin = -1,
        .pulldown = AK_PULLDOWN_DISABLE,
        .pullup = AK_PULLUP_ENABLE,
        .value = -1,
        .dir = AK_GPIO_DIR_INPUT,
        .int_pol = -1,
    }
};


/* akwifi platform data */
struct akwifi_platform_data akwifi_pdata = {
	.gpio_init = ak_gpio_set,
	.gpio_on = {
		.pin		= AK_GPIO_34,
		.pulldown	= -1,
		.pullup		= AK_PULLUP_DISABLE,
		.value		= AK_GPIO_OUT_HIGH,
		.dir		= AK_GPIO_DIR_OUTPUT,
		.int_pol	= -1,
	},
	.gpio_off = {
		.pin		= AK_GPIO_34,
		.pulldown	= -1,
		.pullup		= AK_PULLUP_DISABLE,
		.value		= AK_GPIO_OUT_LOW,
		.dir		= AK_GPIO_DIR_OUTPUT,
		.int_pol	= -1,
	},
	.power_on_delay   = 2000,
	.power_off_delay  = 200,
};

struct platform_device anyka_wifi_device = {
	.name = "anyka-wifi",
	.id = -1,
	.dev = {
		.platform_data = &akwifi_pdata,
	},
};


/**
 * @brief: usb bus device  platform data
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static struct akotghc_usb_platform_data akotghc_plat_data = {
	.gpio_init = ak_gpio_set,
	.gpio_pwr_on = {
		.pin = -1,
		.pulldown = AK_PULLDOWN_DISABLE,
		.pullup	= -1,
		.value	= AK_GPIO_OUT_HIGH,
		.dir	= AK_GPIO_DIR_OUTPUT,
		.int_pol = -1,
	},	
	.gpio_pwr_off = {
		.pin = -1,
		.pulldown = AK_PULLDOWN_DISABLE,
		.pullup	= -1,
		.value	= AK_GPIO_OUT_LOW,
		.dir	= AK_GPIO_DIR_OUTPUT,
		.int_pol = -1,
	},
	.switch_onboard = {
		.pin = -1,
	},
	.switch_extport = {
		.pin = -1,
	},
};


/**
* @brief		ak pcm device struct
			hp and spk can identify by GPIO or AD.
			wo can initialize it in this struct.
* @author	dengzhou 
* @date	  2012-07-19
*/
struct ak39_codec_platform_data ak39_codec_pdata =
{
	.hpdet_gpio =
	{
		.pin        = INVALID_GPIO, //AK_GPIO_7,
		.dir		= AK_GPIO_DIR_INPUT,
		.pullup		= AK_PULLUP_DISABLE,
		.pulldown	= -1,
		.value      = -1,
		.int_pol	= -1,
	},
	.spk_down_gpio =
	{
		.pin        = AK_GPIO_7,
		.dir		= AK_GPIO_DIR_OUTPUT,
		.pullup		= -1,
		.pulldown	= -1,
		.value      = AK_GPIO_OUT_LOW,
		.int_pol	= -1,
	},
	.hpmute_gpio =
	{
		.pin        = INVALID_GPIO, //AK_GPIO_29,
		.dir		= AK_GPIO_DIR_OUTPUT,
		.pullup		= -1,
		.pulldown	= -1,
		
		.value      = AK_GPIO_OUT_LOW,
		.int_pol	= -1,
	},

	.hp_on_value          = AK_GPIO_OUT_LOW,
	.hpdet_irq            = -1,
	.bIsHPmuteUsed        = 0,
	.hp_mute_enable_value = AK_GPIO_OUT_HIGH,
	.bIsMetalfixed        = 0,
	.boutput_only		  = 1,
};

struct resource ak39_codec_resources[] = {
	[0] = {
		.start = 0x08000000,
		.end = 0x0800FFFF,
		.flags = (int)IORESOURCE_MEM,
		.name = "akpcm_AnalogCtrlRegs",
	},
	[1] = {
		.start = 0x20110000,
		.end = 0x2011800F,
		.flags = (int)IORESOURCE_MEM,
		.name = "akpcm_ADC2ModeCfgRegs",
	},
};


struct platform_device ak39_codec_device = {
	.name	= "ak39-codec",
	.id	= -1,
	.resource = ak39_codec_resources,
	.num_resources = ARRAY_SIZE(ak39_codec_resources),
	.dev	= {
		.platform_data	= &ak39_codec_pdata,
	},
};


/**
 * @brief: sensor device  platform data, this info is fake.
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static struct i2c_board_info ak_camara_devices[] = {
	{
		I2C_BOARD_INFO("aksensor", 0x1),
	},
};

static struct aksensor_camera_info ak_soc_camera_info = {
	.buswidth = SOCAM_DATAWIDTH_8,
	.pin_avdd = INVALID_GPIO,
	.pin_power = INVALID_GPIO,
	.pin_pwdn = INVALID_GPIO,
	.pin_reset = AK_GPIO_0,
	.link = {
		.bus_id = 39,
		.power = NULL,
		.board_info = &ak_camara_devices[0],
		.i2c_adapter_id = 0,
		.priv = &ak_soc_camera_info,
	}	
};

/* fake device for soc_camera subsystem */
static struct platform_device soc_camera_interface = {
	.name = "soc-camera-pdrv",
	.id   = -1,
	.dev = {
		.platform_data = &ak_soc_camera_info.link,
	}
};


static struct user_gpio_info user_gpios[] = {
	{
		.name = "IR_LED",
		.info = {
			.pin = AK_GPIO_6,
			.pulldown = -1,
			.pullup = -1,
			.value = AK_GPIO_OUT_HIGH,
			.dir = AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
	},
	{
		.name = "wifi_en",
		.info = {
			.pin = AK_GPIO_34,
			.pulldown = -1,
			.pullup = -1,
			.value = AK_GPIO_OUT_HIGH,
			.dir = AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
	},
	{
		.name = "ircut_a",
		.info = {
			.pin = AK_GPIO_42,
			.pulldown = -1,
			.pullup = AK_PULLUP_DISABLE,
			.value = AK_GPIO_OUT_LOW,
			.dir = AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
	},
	{
		.name = "ircut_b",
		.info = {
			.pin = AK_GPIO_41,
			.pulldown = -1,
			.pullup = AK_PULLUP_DISABLE,
			.value = AK_GPIO_OUT_LOW,
			.dir = AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
	},
	{
		.name = "motor_switch",
		.info = {
			.pin = -1,
			.pulldown = -1,
			.pullup = -1,
			.value = AK_GPIO_OUT_LOW,
			.dir = AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
	},
	{
		.name = "WHITE_LED",
		.info = {
			.pin = AK_GPIO_24,
			.pulldown = -1,
			.pullup = -1,
			.value = AK_GPIO_OUT_LOW,
			.dir = AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
	},
	{
		.name = "SPK_PA",
		.info = {
			.pin = AK_GPIO_7,
			.pulldown = -1,
			.pullup = -1,
			.value = AK_GPIO_OUT_LOW,
			.dir = AK_GPIO_DIR_OUTPUT,
			.int_pol = -1,
		},
	},
};

static struct ak_user_gpio_pdata user_gpio_pdata = {
	.user_gpios = user_gpios,
	.nr_user_gpios = ARRAY_SIZE(user_gpios),
};

static struct platform_device user_gpio_device = {
	.name = "user_gpio",
	.id = -1,
	.dev = {
		.platform_data = &user_gpio_pdata,
	},
};


/**
 * @brief: camera platform data and initialis status
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static struct ak_camera_pdata ak39_camera_info = {
	.mclk = 27, /* GC1084 conf: base27M@pclk=36Mhz */
	.flags = 0,
	.gpio_get = ak_gpio_getpin,
	.gpio_set = ak_gpio_set,
	.rf_led = {
		.pin = -1,
		.pulldown = -1,
		.pullup = AK_PULLUP_DISABLE,
		.value = -1,
		.dir = AK_GPIO_DIR_INPUT,
		.int_pol = AK_GPIO_INT_LOWLEVEL,
	}
};


/**
 * @brief: GPIO buttons platform data and initialis status
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static struct gpio_keys_button gpio_keys_button[] = {
	{
		.code			= KEY_1,
		.type			= EV_KEY,
		.gpio			= AK_GPIO_47,
		.active_low		= 1,
		.wakeup			= 1,
		.debounce_interval	= 100, /* exact stock platform data */
		.desc			= "boot0",
		.pullup			= -1,
		.pulldown		= -1,
		.dir			= AK_GPIO_DIR_INPUT,
		.int_pol		= AK_GPIO_INT_LOWLEVEL,
	},
};

static struct akgpio_keys_platform_data gpio_keys_platform_data = {
	.buttons	= gpio_keys_button,
	.nbuttons	= ARRAY_SIZE(gpio_keys_button),
	.rep		= 0,
};


/**
 * @brief: ak39 platform devices table
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
 static struct platform_device *ak3918_platform_devices[] __initdata = {
	&ak39_uart0_device,
	&ak39_motor0_device,
	&ak39_motor1_device,
	&ak39_spi1_device,
	&ak39_mmc_device,
	&ak39_i2c_device,
	&user_gpio_device,
	&ak39_usb_otg_hcd_device,
	&anyka_wifi_device,
	&soc_camera_interface,
	&ak39_camera_interface,	
	&ak39_ion_device,
	&ak39_pcm_device,
	&ak39_codec_device,
	&ak39_mmx_device,
	&ak39_gpio_keys_device,
};

void wdt_enable(void);
void wdt_keepalive(unsigned int heartbeat);

/**
 * @brief: restart by "reboot" cmd 
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static void ak39_restart(char str, const char *cmd)
{
	//ak39_reboot_sys_by_soft();
#if defined CONFIG_AK39_WATCHDOG || defined CONFIG_AK39_WATCHDOG_TOP
	wdt_enable();
	wdt_keepalive(2);
#endif
}


/**
 * @brief: initial ak3918 machine 
 * 
 * @author: caolianming
 * @date: 2014-01-09
 */
static void __init ak3918_init_machine(void)
{
	adc1_init();

	spi_register_board_info(ak39_spi_board_dev, ARRAY_SIZE(ak39_spi_board_dev));	
		
	ak39_spi1_device.dev.platform_data = &ak39_spi1_info;

	ak39_motor0_device.dev.platform_data = &ak39_motor0_pdata;
	ak39_motor1_device.dev.platform_data = &ak39_motor1_pdata;
		
	ak39_mmc_device.dev.platform_data = &mmc_plat_data;

	ak39_usb_otg_hcd_device.dev.platform_data = &akotghc_plat_data;

	ak39_gpio_keys_device.dev.platform_data = &gpio_keys_platform_data;	

	ak39_camera_interface.dev.platform_data = &ak39_camera_info;
	
	platform_add_devices(ak3918_platform_devices, 
				ARRAY_SIZE(ak3918_platform_devices));
	
	l2_init();
	
	return;
}


/*
 * The ARM consistent window defaults to 2 MiB, and videobuf-dma-contig takes
 * capture buffers out of it: four 1280x720 YUYV frames are 7.4 MB, so
 * VIDIOC_REQBUFS succeeds and the mmap that follows returns ENOMEM.
 *
 * This costs virtual address space, not memory. init_consistent_dma_size()
 * only moves consistent_base; the pages behind it still come from the buddy
 * allocator on demand and go back on release. The whole price is one page
 * table page per 2 MiB of window, so 2 -> 8 MiB is 12 KiB - which matters on
 * a board with 36 MiB of usable RAM, and is why the window is 8 and not 16.
 */
static void __init ak39_init_early(void)
{
	init_consistent_dma_size(SZ_8M);
}

MACHINE_START(AK39XX, "Cloud39EV2_AK3918E80PIN_MNBD")
/* Maintainer: */
	.atag_offset = 0x100,
	.fixup = NULL,
	.map_io = ak39_map_io,
	.reserve = NULL,
	.init_irq = ak39_init_irq,
	.init_machine = ak3918_init_machine,
	.init_early = ak39_init_early,
	.timer = &ak39_timer, 
    .restart = ak39_restart,
    
MACHINE_END
