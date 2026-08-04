#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>

#define WIFI_DEV_ENABLE 1

struct akplat_wifi {
	struct gpio_desc *enable_gpio;
	unsigned int power_on_delay;
	unsigned int power_off_delay;
};

static int akplat_wifi_probe(struct platform_device *pdev)
{
	struct akplat_wifi *wifi;
	u32 val;

	wifi = devm_kzalloc(&pdev->dev, sizeof(*wifi), GFP_KERNEL);
	if (!wifi)
		return -ENOMEM;

	wifi->power_on_delay = 2000;
	wifi->power_off_delay = 200;
	if (!of_property_read_u32(pdev->dev.of_node, "power-on-delay-ms", &val))
		wifi->power_on_delay = val;
	if (!of_property_read_u32(pdev->dev.of_node, "power-off-delay-ms", &val))
		wifi->power_off_delay = val;

	wifi->enable_gpio = devm_gpiod_get_optional(&pdev->dev, "enable",
						     GPIOD_OUT_LOW);
	if (IS_ERR(wifi->enable_gpio))
		return PTR_ERR(wifi->enable_gpio);

	platform_set_drvdata(pdev, wifi);

	if (wifi->enable_gpio) {
		gpiod_set_value_cansleep(wifi->enable_gpio, WIFI_DEV_ENABLE);
		msleep(wifi->power_on_delay);
		dev_info(&pdev->dev, "wifi power on\n");
	}

	return 0;
}

static int akplat_wifi_remove(struct platform_device *pdev)
{
	struct akplat_wifi *wifi = platform_get_drvdata(pdev);

	if (wifi->enable_gpio) {
		gpiod_set_value_cansleep(wifi->enable_gpio, !WIFI_DEV_ENABLE);
		msleep(wifi->power_off_delay);
		dev_info(&pdev->dev, "wifi power off\n");
	}

	return 0;
}

static int akplat_wifi_suspend(struct platform_device *pdev, pm_message_t state)
{
	dev_dbg(&pdev->dev, "%s entered.\n", __func__);
	return 0;
}

static int akplat_wifi_resume(struct platform_device *pdev)
{
	dev_dbg(&pdev->dev, "%s entered.\n", __func__);
	return 0;
}

static const struct of_device_id akplat_wifi_of_match[] = {
	{ .compatible = "anyka,sdio-wifi-pwrseq" },
	{ },
};
MODULE_DEVICE_TABLE(of, akplat_wifi_of_match);

static struct platform_driver akplat_wifi_driver = {
	.driver = {
		.name = "anyka-wifi",
		.of_match_table = akplat_wifi_of_match,
	},
	.probe = akplat_wifi_probe,
	.remove = akplat_wifi_remove,
	.suspend = akplat_wifi_suspend,
	.resume = akplat_wifi_resume,
};


static int __init sdio_wifi_init(void)
{
	return platform_driver_register(&akplat_wifi_driver);
}

static void __exit sdio_wifi_exit(void)
{
	platform_driver_unregister(&akplat_wifi_driver);
}

module_init(sdio_wifi_init);
module_exit(sdio_wifi_exit);

MODULE_LICENSE("GPL");
