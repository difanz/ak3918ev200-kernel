/*
 *
 * Copyright (c) 2012 anyka
 *
 * AK98- LEDs GPIO driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/leds.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/module.h>

struct ak_led_instance {
	struct led_classdev	cdev;
	struct gpio_desc	*gpiod;
};

struct ak_led_class {
	struct ak_led_instance *instance;
	int nr_instance;
};

static void ak_led_brightness_set(struct led_classdev *led_cdev, enum led_brightness value)
{
	struct ak_led_instance *instance =
		container_of(led_cdev, struct ak_led_instance, cdev);

	gpiod_set_value_cansleep(instance->gpiod, value ? 1 : 0);
}

static int ak_led_remove(struct platform_device *dev)
{
	int i;
	struct ak_led_class *class = platform_get_drvdata(dev);

	for (i = 0; i < class->nr_instance; i++) {
		led_classdev_unregister(&class->instance[i].cdev);
		gpiod_put(class->instance[i].gpiod);
	}

	return 0;
}

static int ak_led_probe(struct platform_device *dev)
{
	struct device *d = &dev->dev;
	struct device_node *np = d->of_node;
	struct device_node *child;
	struct ak_led_instance *instance;
	struct ak_led_class *class;
	int i, ret, count;

	if (!np)
		return -ENODEV;

	count = of_get_child_count(np);
	if (count == 0)
		return -ENODEV;

	class = devm_kzalloc(d, sizeof(*class), GFP_KERNEL);
	instance = devm_kzalloc(d, sizeof(*instance) * count, GFP_KERNEL);
	if (!class || !instance)
		return -ENOMEM;

	class->instance = instance;
	class->nr_instance = 0;
	platform_set_drvdata(dev, class);

	i = 0;
	for_each_child_of_node(np, child) {
		struct ak_led_instance *led = &instance[i];

		led->gpiod = fwnode_get_named_gpiod(&child->fwnode, "gpios");
		if (IS_ERR(led->gpiod)) {
			dev_err(d, "failed to get gpio for %s: %ld\n",
				child->name, PTR_ERR(led->gpiod));
			continue;
		}
		gpiod_direction_output(led->gpiod, 0);

		led->cdev.name = child->name;
		of_property_read_string(child, "label", &led->cdev.name);
		of_property_read_string(child, "linux,default-trigger",
					 &led->cdev.default_trigger);
		led->cdev.brightness_set = ak_led_brightness_set;
		led->cdev.flags |= LED_CORE_SUSPENDRESUME;

		ret = led_classdev_register(d, &led->cdev);
		if (ret < 0) {
			dev_err(d, "led_classdev_register failed for %s\n", child->name);
			gpiod_put(led->gpiod);
			of_node_put(child);
			goto fail;
		}

		class->nr_instance++;
		i++;
	}

	if (class->nr_instance == 0)
		return -ENODEV;

	return 0;

fail:
	for (i = 0; i < class->nr_instance; i++) {
		led_classdev_unregister(&class->instance[i].cdev);
		gpiod_put(class->instance[i].gpiod);
	}
	return ret;
}

static const struct of_device_id ak_led_of_match[] = {
	{ .compatible = "anyka,ak39ev330-leds" },
	{ }
};
MODULE_DEVICE_TABLE(of, ak_led_of_match);

static struct platform_driver ak_led_driver = {
	.probe		= ak_led_probe,
	.remove		= ak_led_remove,
	.driver		= {
		.name		= "ak_led",
		.owner		= THIS_MODULE,
		.of_match_table	= ak_led_of_match,
	},
};
module_platform_driver(ak_led_driver);

MODULE_AUTHOR("Hongguang Du <anyka>");
MODULE_DESCRIPTION("AK LED driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ak_led");
