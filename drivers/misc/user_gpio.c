/*
 * Named GPIO sysfs interface for the AK3918 camera board.
 *
 * The name table is recovered from the unit's stock kernel. Attributes are
 * root-only because they control motors, illumination, IR-cut, Wi-Fi power,
 * and the speaker amplifier.
 *
 * Every pad here is an output, and the attributes are write-only. Sampling one
 * as an input drops its drive; on the IR-cut pad that takes the hold off a
 * magnetic actuator with no current limit anywhere in the hardware. The level a
 * pad is holding is the level it was last given, and is not readable.
 */

#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

struct user_gpio_attribute {
	struct attribute	attr;
	struct gpio_desc	*desc;
};

struct user_gpio_driver_info {
	struct kobject			kobj;
	struct attribute		**attrs;
	struct user_gpio_attribute	*entries;
};

static struct user_gpio_driver_info driver_info;

static void user_gpio_release(struct kobject *kobj)
{
}

static ssize_t user_gpio_store(struct kobject *kobj, struct attribute *attr,
			       const char *buf, size_t count)
{
	struct user_gpio_attribute *entry;
	long level;
	int ret;

	entry = container_of(attr, struct user_gpio_attribute, attr);

	ret = kstrtol(buf, 10, &level);
	if (ret)
		return ret;
	if (level != 0 && level != 1)
		return -EINVAL;

	gpiod_set_value_cansleep(entry->desc, level);

	return count;
}

static const struct sysfs_ops user_gpio_sysfs_ops = {
	.store	= user_gpio_store,
};

static struct kobj_type user_gpio_ktype = {
	.release	= user_gpio_release,
	.sysfs_ops	= &user_gpio_sysfs_ops,
};

static int user_gpio_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int count, index, visible = 0;
	int ret;

	if (!np)
		return -ENODEV;

	count = of_property_count_strings(np, "gpio-names");
	if (count <= 0)
		return -EINVAL;

	driver_info.attrs = devm_kzalloc(&pdev->dev,
					 sizeof(*driver_info.attrs) *
					 (count + 1), GFP_KERNEL);
	if (!driver_info.attrs)
		return -ENOMEM;

	driver_info.entries = devm_kzalloc(&pdev->dev,
					   sizeof(*driver_info.entries) * count,
					   GFP_KERNEL);
	if (!driver_info.entries)
		return -ENOMEM;

	for (index = 0; index < count; index++) {
		struct user_gpio_attribute *entry = &driver_info.entries[index];
		enum gpiod_flags flags = GPIOD_OUT_LOW;
		struct gpio_desc *desc;
		const char *name;
		u32 value = 0;

		ret = of_property_read_string_index(np, "gpio-names", index,
						    &name);
		if (ret)
			continue;

		of_property_read_u32_index(np, "output-defaults", index,
					   &value);
		if (value)
			flags = GPIOD_OUT_HIGH;

		desc = devm_gpiod_get_index(&pdev->dev, NULL, index, flags);
		if (IS_ERR(desc)) {
			dev_warn(&pdev->dev, "%s unavailable: %ld\n", name,
				 PTR_ERR(desc));
			continue;
		}

		entry->attr.name = name;
		entry->attr.mode = 0200;
		entry->desc = desc;
		driver_info.attrs[visible++] = &entry->attr;
	}

	if (!visible)
		return -ENODEV;

	ret = kobject_init_and_add(&driver_info.kobj, &user_gpio_ktype, NULL,
				   "user-gpio");
	if (ret)
		return ret;

	ret = sysfs_create_files(&driver_info.kobj,
				 (const struct attribute **)driver_info.attrs);
	if (ret) {
		kobject_put(&driver_info.kobj);
		return ret;
	}

	return 0;
}

static int user_gpio_remove(struct platform_device *pdev)
{
	sysfs_remove_files(&driver_info.kobj,
			   (const struct attribute **)driver_info.attrs);
	kobject_put(&driver_info.kobj);

	return 0;
}

static const struct of_device_id user_gpio_of_match[] = {
	{ .compatible = "anyka,user-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, user_gpio_of_match);

static struct platform_driver user_gpio_driver = {
	.probe	= user_gpio_probe,
	.remove	= user_gpio_remove,
	.driver	= {
		.name		= "user_gpio",
		.owner		= THIS_MODULE,
		.of_match_table	= user_gpio_of_match,
	},
};

module_platform_driver(user_gpio_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AK3918 named board GPIO interface");
MODULE_ALIAS("platform:user_gpio");
