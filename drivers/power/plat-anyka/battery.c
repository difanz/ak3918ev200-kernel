/*
 * @file battery.c
 * @battery driver for ak chip
 * @Copyright (C) 2010 Anyka (Guangzhou) Microelectronics Technology Co
 * @author gao_wangsheng
 * @date 2011-04
 * @version 2.0
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Battery driver for the AK39 platform. Registers a
 * power_supply device backed by ADC readings and GPIO-driven charge
 * detection.
 *
 *
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#include <mach/ak_adc1.h>

#define UPDATE_DISCHARGE_DELAY	(HZ * 6)
#define UPDATE_CHARGE_DELAY	(HZ * 60)

struct ak_bat {
	struct power_supply *bat_psy;
	struct power_supply *ac_psy;
	struct power_supply *usb_psy;

	struct gpio_desc *ac_gpiod;
	struct gpio_desc *usb_gpiod;
	struct gpio_desc *full_gpiod;
	int ac_irq;
	int usb_irq;

	struct delayed_work work;

	/* bat_adc: ADC voltage-divider correction, mV domain */
	int up_resistance;
	int dw_resistance;
	int voltage_correct;

	/* bat_mach_info: capacity curve endpoints, mV */
	int max_voltage;
	int min_voltage;

	int voltage;		/* last-read battery voltage, mV */
	int capacity;		/* last-computed capacity, % */
	int status;
};

static int ak_bat_read_voltage(struct ak_bat *battery)
{
	int voltage = adc1_read_bat();

	if (battery->dw_resistance)
		voltage = voltage * (battery->up_resistance + battery->dw_resistance)
				/ battery->dw_resistance;
	voltage += battery->voltage_correct;

	return voltage;
}

static int ak_bat_capacity(struct ak_bat *battery, int voltage)
{
	int range = battery->max_voltage - battery->min_voltage;

	if (range <= 0)
		return 0;
	if (voltage <= battery->min_voltage)
		return 0;
	if (voltage >= battery->max_voltage)
		return 100;

	return (voltage - battery->min_voltage) * 100 / range;
}

static bool ak_bat_charging(struct ak_bat *battery)
{
	bool ac = battery->ac_gpiod && gpiod_get_value_cansleep(battery->ac_gpiod);
	bool usb = battery->usb_gpiod && gpiod_get_value_cansleep(battery->usb_gpiod);

	return ac || usb;
}

static void ak_bat_update_work(struct work_struct *work)
{
	struct ak_bat *battery = container_of(work, struct ak_bat, work.work);
	bool charging = ak_bat_charging(battery);
	bool full = battery->full_gpiod && gpiod_get_value_cansleep(battery->full_gpiod);

	battery->voltage = ak_bat_read_voltage(battery);
	battery->capacity = ak_bat_capacity(battery, battery->voltage);

	if (full)
		battery->status = POWER_SUPPLY_STATUS_FULL;
	else if (charging)
		battery->status = POWER_SUPPLY_STATUS_CHARGING;
	else
		battery->status = POWER_SUPPLY_STATUS_DISCHARGING;

	power_supply_changed(battery->bat_psy);

	schedule_delayed_work(&battery->work,
			      charging ? UPDATE_CHARGE_DELAY : UPDATE_DISCHARGE_DELAY);
}

static irqreturn_t ak_bat_charge_irq(int irq, void *data)
{
	struct ak_bat *battery = data;

	mod_delayed_work(system_freezable_wq, &battery->work, msecs_to_jiffies(50));
	return IRQ_HANDLED;
}

static int ak_bat_get_property(struct power_supply *psy, enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct ak_bat *battery = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = battery->status;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = battery->voltage * 1000; /* mV -> uV */
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = battery->capacity;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
#if 0
	case POWER_SUPPLY_PROP_POWEROFF_CAP:
	case POWER_SUPPLY_PROP_LOW_CAP:
	case POWER_SUPPLY_PROP_RECOVER_CAP:
	case POWER_SUPPLY_PROP_POWER_ON_VOLTAGE:
	case POWER_SUPPLY_PROP_CPOWER_ON_VOLTAGE:
		break;
#endif
	default:
		return -EINVAL;
	}

	return 0;
}

static enum power_supply_property ak_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_PRESENT,
};

static const struct power_supply_desc ak_bat_desc = {
	.name		= "battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= ak_bat_props,
	.num_properties	= ARRAY_SIZE(ak_bat_props),
	.get_property	= ak_bat_get_property,
};

static int ak_ac_get_property(struct power_supply *psy, enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct ak_bat *battery = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	val->intval = battery->ac_gpiod && gpiod_get_value_cansleep(battery->ac_gpiod);
	return 0;
}

static enum power_supply_property ak_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc ak_ac_desc = {
	.name		= "ac",
	.type		= POWER_SUPPLY_TYPE_MAINS,
	.properties	= ak_ac_props,
	.num_properties	= ARRAY_SIZE(ak_ac_props),
	.get_property	= ak_ac_get_property,
};

static int ak_usb_get_property(struct power_supply *psy, enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct ak_bat *battery = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	val->intval = battery->usb_gpiod && gpiod_get_value_cansleep(battery->usb_gpiod);
	return 0;
}

static enum power_supply_property ak_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc ak_usb_desc = {
	.name		= "usb",
	.type		= POWER_SUPPLY_TYPE_USB,
	.properties	= ak_usb_props,
	.num_properties	= ARRAY_SIZE(ak_usb_props),
	.get_property	= ak_usb_get_property,
};

static int ak_battery_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ak_bat *battery;
	struct power_supply_config cfg = { };
	u32 val;

	battery = devm_kzalloc(dev, sizeof(*battery), GFP_KERNEL);
	if (!battery)
		return -ENOMEM;

	if (!of_property_read_u32(dev->of_node, "up-resistance", &val))
		battery->up_resistance = val;
	if (!of_property_read_u32(dev->of_node, "down-resistance", &val))
		battery->dw_resistance = val;
	of_property_read_u32(dev->of_node, "voltage-correct", &battery->voltage_correct);

	if (of_property_read_u32(dev->of_node, "max-voltage-mv", &val)) {
		dev_err(dev, "missing max-voltage-mv\n");
		return -EINVAL;
	}
	battery->max_voltage = val;

	if (of_property_read_u32(dev->of_node, "min-voltage-mv", &val)) {
		dev_err(dev, "missing min-voltage-mv\n");
		return -EINVAL;
	}
	battery->min_voltage = val;

	battery->ac_gpiod = devm_gpiod_get_optional(dev, "ac", GPIOD_IN);
	battery->usb_gpiod = devm_gpiod_get_optional(dev, "usb", GPIOD_IN);
	battery->full_gpiod = devm_gpiod_get_optional(dev, "full", GPIOD_IN);
	if (IS_ERR(battery->ac_gpiod) || IS_ERR(battery->usb_gpiod) ||
	    IS_ERR(battery->full_gpiod))
		return -EPROBE_DEFER;

	adc1_init();

	platform_set_drvdata(pdev, battery);

	cfg.drv_data = battery;
	cfg.of_node = dev->of_node;

	battery->bat_psy = devm_power_supply_register(dev, &ak_bat_desc, &cfg);
	if (IS_ERR(battery->bat_psy))
		return PTR_ERR(battery->bat_psy);

	battery->ac_psy = devm_power_supply_register(dev, &ak_ac_desc, &cfg);
	if (IS_ERR(battery->ac_psy))
		return PTR_ERR(battery->ac_psy);

	battery->usb_psy = devm_power_supply_register(dev, &ak_usb_desc, &cfg);
	if (IS_ERR(battery->usb_psy))
		return PTR_ERR(battery->usb_psy);

	INIT_DELAYED_WORK(&battery->work, ak_bat_update_work);

	if (battery->ac_gpiod) {
		battery->ac_irq = gpiod_to_irq(battery->ac_gpiod);
		if (battery->ac_irq >= 0)
			devm_request_irq(dev, battery->ac_irq, ak_bat_charge_irq,
					  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
					  "ac_charge", battery);
	}
	if (battery->usb_gpiod) {
		battery->usb_irq = gpiod_to_irq(battery->usb_gpiod);
		if (battery->usb_irq >= 0)
			devm_request_irq(dev, battery->usb_irq, ak_bat_charge_irq,
					  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
					  "usb_charge", battery);
	}

	schedule_delayed_work(&battery->work, 0);

	return 0;
}

static int ak_battery_remove(struct platform_device *pdev)
{
	struct ak_bat *battery = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&battery->work);

	return 0;
}

static const struct of_device_id ak_battery_of_match[] = {
	{ .compatible = "anyka,ak39-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, ak_battery_of_match);

static struct platform_driver ak_battery_driver = {
	.probe	= ak_battery_probe,
	.remove	= ak_battery_remove,
	.driver	= {
		.name		= "battery",
		.owner		= THIS_MODULE,
		.of_match_table	= ak_battery_of_match,
	},
};
module_platform_driver(ak_battery_driver);

MODULE_DESCRIPTION("AK39 battery driver");
MODULE_AUTHOR("Gao Wangsheng <gao_wangsheng@anyka.oa>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:battery");
