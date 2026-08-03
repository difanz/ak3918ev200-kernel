/*
 * Named GPIO sysfs interface for the AK3918 camera board.
 *
 * The platform data is recovered from the unit's stock kernel. Attributes are
 * root-only because they control motors, illumination, IR-cut, Wi-Fi power,
 * and the speaker amplifier.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <mach/gpio.h>
#include <plat-anyka/user_gpio.h>

struct user_gpio_attribute {
	struct attribute attr;
	struct user_gpio_info *gpio;
};

struct user_gpio_driver_info {
	struct kobject kobj;
	struct attribute **attrs;
	struct user_gpio_attribute *entries;
};

static struct user_gpio_driver_info driver_info;

static void user_gpio_release(struct kobject *kobj)
{
}

static ssize_t user_gpio_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct user_gpio_attribute *entry;

	entry = container_of(attr, struct user_gpio_attribute, attr);
	if (entry->gpio->info.pin < 0)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 ak_gpio_getpin(entry->gpio->info.pin));
}

static ssize_t user_gpio_store(struct kobject *kobj, struct attribute *attr,
			       const char *buf, size_t count)
{
	struct user_gpio_attribute *entry;
	struct gpio_info info;
	long level;
	int ret;

	entry = container_of(attr, struct user_gpio_attribute, attr);
	if (entry->gpio->info.pin < 0)
		return -ENODEV;

	ret = strict_strtol(buf, 10, &level);
	if (ret)
		return ret;
	if (level != 0 && level != 1)
		return -EINVAL;

	info = entry->gpio->info;
	info.value = level;
	ak_gpio_set(&info);
	return count;
}

static const struct sysfs_ops user_gpio_sysfs_ops = {
	.show = user_gpio_show,
	.store = user_gpio_store,
};

static struct kobj_type user_gpio_ktype = {
	.release = user_gpio_release,
	.sysfs_ops = &user_gpio_sysfs_ops,
};

static int user_gpio_probe(struct platform_device *pdev)
{
	struct ak_user_gpio_pdata *pdata = pdev->dev.platform_data;
	int index;
	int visible = 0;
	int ret;

	if (!pdata || !pdata->user_gpios || pdata->nr_user_gpios <= 0)
		return -EINVAL;

	driver_info.attrs = kzalloc(sizeof(*driver_info.attrs) *
					   (pdata->nr_user_gpios + 1), GFP_KERNEL);
	if (!driver_info.attrs)
		return -ENOMEM;

	driver_info.entries = kzalloc(sizeof(*driver_info.entries) *
					     pdata->nr_user_gpios, GFP_KERNEL);
	if (!driver_info.entries) {
		ret = -ENOMEM;
		goto free_attrs;
	}

	for (index = 0; index < pdata->nr_user_gpios; index++) {
		struct user_gpio_info *gpio = &pdata->user_gpios[index];
		struct user_gpio_attribute *entry = &driver_info.entries[index];

		if (gpio->info.pin < 0)
			continue;

		entry->attr.name = gpio->name;
		entry->attr.mode = 0600;
		entry->gpio = gpio;
		driver_info.attrs[visible++] = &entry->attr;
		ak_gpio_set(&gpio->info);
	}

	ret = kobject_init_and_add(&driver_info.kobj, &user_gpio_ktype,
				   NULL, "user-gpio");
	if (ret)
		goto free_entries;

	ret = sysfs_create_files(&driver_info.kobj,
				 (const struct attribute **)driver_info.attrs);
	if (ret) {
		kobject_put(&driver_info.kobj);
		goto free_entries;
	}

	return 0;

free_entries:
	kfree(driver_info.entries);
	driver_info.entries = NULL;
free_attrs:
	kfree(driver_info.attrs);
	driver_info.attrs = NULL;
	return ret;
}

static int user_gpio_remove(struct platform_device *pdev)
{
	sysfs_remove_files(&driver_info.kobj,
			   (const struct attribute **)driver_info.attrs);
	kobject_put(&driver_info.kobj);
	kfree(driver_info.entries);
	kfree(driver_info.attrs);
	driver_info.entries = NULL;
	driver_info.attrs = NULL;
	return 0;
}

static struct platform_driver user_gpio_driver = {
	.probe = user_gpio_probe,
	.remove = __devexit_p(user_gpio_remove),
	.driver = {
		.name = "user_gpio",
		.owner = THIS_MODULE,
	},
};

static int __init user_gpio_init(void)
{
	return platform_driver_register(&user_gpio_driver);
}

static void __exit user_gpio_exit(void)
{
	platform_driver_unregister(&user_gpio_driver);
}

module_init(user_gpio_init);
module_exit(user_gpio_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AK3918 named board GPIO interface");
MODULE_ALIAS("platform:user_gpio");
