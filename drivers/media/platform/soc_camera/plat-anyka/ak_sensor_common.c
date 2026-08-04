#include <plat-anyka/ak_sensor.h>
#include <plat-anyka/ak_sensor_i2c.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include "ak_sensor_common.h"

int ak_sensor_read_register(struct ak_sensor_i2c_data *p_i2c_data)
{
	int ret;

	ret = sensor_read_register((T_SENSOR_I2C_DATA_S *)p_i2c_data);
	/*
	 * sensor_read_register returns the datum as s32 (negative on error).
	 * Stash into u32Data for callers that read the struct field; the
	 * return value remains authoritative.
	 */
	if (ret >= 0)
		p_i2c_data->u32Data = (unsigned int)ret;
	return ret;
}
EXPORT_SYMBOL_GPL(ak_sensor_read_register);

int ak_sensor_write_register(const struct ak_sensor_i2c_data *p_i2c_data)
{
	return sensor_write_register((T_SENSOR_I2C_DATA_S *)p_i2c_data);
}
EXPORT_SYMBOL_GPL(ak_sensor_write_register);

int ak_sensor_set_pin_as_gpio(const int pin)
{
	return gpio_request_one(pin, GPIOF_OUT_INIT_LOW, "ak_sensor");
}
EXPORT_SYMBOL_GPL(ak_sensor_set_pin_as_gpio);

int ak_sensor_set_pin_dir(const int pin, const int is_output)
{
	if (is_output)
		return gpio_direction_output(pin, 0);
	else
		return gpio_direction_input(pin);
}
EXPORT_SYMBOL_GPL(ak_sensor_set_pin_dir);

int ak_sensor_set_pin_level(const int pin, const int level)
{
	gpio_set_value(pin, level ? 1 : 0);
	return 0;
}
EXPORT_SYMBOL_GPL(ak_sensor_set_pin_level);

int ak_sensor_mdelay(const int msec)
{
	mdelay(msec);
	return 0;
}
EXPORT_SYMBOL_GPL(ak_sensor_mdelay);
