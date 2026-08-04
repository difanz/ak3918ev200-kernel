#ifndef __PLAT_ANYKA_USER_GPIO_H
#define __PLAT_ANYKA_USER_GPIO_H

#include <mach/gpio.h>

struct user_gpio_info {
	const char *name;
	struct gpio_info info;
};

struct ak_user_gpio_pdata {
	struct user_gpio_info *user_gpios;
	int nr_user_gpios;
};

#endif
