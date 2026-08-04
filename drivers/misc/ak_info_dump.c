/*
 * Anyka ISP2 camera information exported to userspace.
 *
 * The ABI mirrors the stock ak_info_dump module: two read-only files live
 * directly under /sys/ak_info_dump and contain hexadecimal values.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#include <plat-anyka/ak_sensor.h>

static struct kobject *ak_info_dump_obj;

static ssize_t sensor_id_read(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%x\n",
			 (unsigned int)aksensor_get_sensor_id());
}

static ssize_t reserved_mem_size_read(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%x\n",
			 (unsigned int)CONFIG_VIDEO_RESERVED_MEM_SIZE);
}

static struct kobj_attribute sensor_id_obj =
	__ATTR(sensor_id, 0444, sensor_id_read, NULL);
static struct kobj_attribute reserved_mem_size_obj =
	__ATTR(reserved_mem_size, 0444, reserved_mem_size_read, NULL);

static struct attribute *ak_info_dump_attrs[] = {
	&sensor_id_obj.attr,
	&reserved_mem_size_obj.attr,
	NULL,
};

static const struct attribute_group ak_info_dump_group = {
	.attrs = ak_info_dump_attrs,
};

static int __init ak_info_dump_init(void)
{
	int ret;

	ak_info_dump_obj = kobject_create_and_add("ak_info_dump", NULL);
	if (!ak_info_dump_obj)
		return -ENOMEM;

	ret = sysfs_create_group(ak_info_dump_obj, &ak_info_dump_group);
	if (ret) {
		kobject_put(ak_info_dump_obj);
		ak_info_dump_obj = NULL;
	}

	return ret;
}

static void __exit ak_info_dump_exit(void)
{
	if (!ak_info_dump_obj)
		return;

	sysfs_remove_group(ak_info_dump_obj, &ak_info_dump_group);
	kobject_put(ak_info_dump_obj);
}

module_init(ak_info_dump_init);
module_exit(ak_info_dump_exit);

MODULE_AUTHOR("Anyka open firmware project");
MODULE_DESCRIPTION("Anyka ISP2 camera information sysfs interface");
MODULE_LICENSE("GPL");
