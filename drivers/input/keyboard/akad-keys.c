/*
 * drivers/input/keyboard/akad-keys.c
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/errno.h>
#include <linux/pm.h>
#include <linux/workqueue.h>

#include <mach/ak_adc1.h>

#define DRV_NAME	"ad-keys"

struct adgpio_key {
	int code;	/* input event code */
	int min;	/* min mV */
	int max;	/* max mV */
};

struct adgpio_key_data {
	struct input_dev *input;
	struct adgpio_key *keys;
	struct timer_list timer;
	struct delayed_work work;

	int poll_interval;
	int debounce_interval;
	int keycode;
	int nkey;

	unsigned char press;
	unsigned char sync_me;
};

static void adkeys_poll(struct adgpio_key_data *adkey);

static void adkey_input_queue_work(struct adgpio_key_data *adkey)
{
	unsigned long delay = msecs_to_jiffies(adkey->poll_interval);

	if (delay >= HZ)
		delay = round_jiffies_relative(delay);

	queue_delayed_work(system_freezable_wq, &adkey->work, delay);
}

static void adkey_input_device_work(struct work_struct *work)
{
	struct adgpio_key_data *adkey =
		container_of(work, struct adgpio_key_data, work.work);

	adkeys_poll(adkey);
	adkey_input_queue_work(adkey);
}

static void adkeys_timer(unsigned long _data)
{
	struct adgpio_key_data *adkey = (struct adgpio_key_data *)_data;
	struct input_dev *input_dev = adkey->input;

	if (!adkey->press)
		adkey->sync_me = 0;

	input_event(input_dev, EV_KEY, adkey->keycode, adkey->press);
	input_sync(input_dev);
}

static void adkeys_poll(struct adgpio_key_data *adkey)
{
	int advol, i;

	advol = adc1_read_ad5();

	i = 0;
	adkey->press = 0;
	while (i < adkey->nkey) {
		if (advol > adkey->keys[i].min && advol < adkey->keys[i].max) {
			adkey->press = 1;
			adkey->sync_me = 1;
			adkey->keycode = adkey->keys[i].code;
			break;
		}
		i++;
	}

	if (adkey->sync_me)
		mod_timer(&adkey->timer,
			  jiffies + msecs_to_jiffies(adkey->debounce_interval));
}

static int akad_keys_parse_dt(struct device *dev, struct adgpio_key_data *adkey)
{
	struct device_node *np = dev->of_node;
	struct device_node *child;
	struct adgpio_key *keys;
	int i = 0;
	u32 val;

	if (!np)
		return -ENODEV;

	adkey->nkey = of_get_child_count(np);
	if (adkey->nkey == 0)
		return -ENODEV;

	keys = devm_kzalloc(dev, adkey->nkey * sizeof(*keys), GFP_KERNEL);
	if (!keys)
		return -ENOMEM;

	for_each_child_of_node(np, child) {
		if (of_property_read_u32(child, "linux,code", &val)) {
			dev_err(dev, "%s: missing linux,code\n", child->name);
			return -EINVAL;
		}
		keys[i].code = val;

		if (of_property_read_u32(child, "adc-min", &val)) {
			dev_err(dev, "%s: missing adc-min\n", child->name);
			return -EINVAL;
		}
		keys[i].min = val;

		if (of_property_read_u32(child, "adc-max", &val)) {
			dev_err(dev, "%s: missing adc-max\n", child->name);
			return -EINVAL;
		}
		keys[i].max = val;

		i++;
	}

	adkey->keys = keys;

	if (of_property_read_u32(np, "poll-interval", &val) == 0)
		adkey->poll_interval = val;
	else
		adkey->poll_interval = 500;

	if (of_property_read_u32(np, "debounce-interval", &val) == 0)
		adkey->debounce_interval = val;
	else
		adkey->debounce_interval = 20;

	return 0;
}

static int analog_gpio_probe(struct platform_device *pdev)
{
	struct adgpio_key_data *adkey;
	struct input_dev *input_dev;
	int i, err;

	adkey = devm_kzalloc(&pdev->dev, sizeof(*adkey), GFP_KERNEL);
	if (!adkey)
		return -ENOMEM;

	err = akad_keys_parse_dt(&pdev->dev, adkey);
	if (err)
		return err;

	input_dev = input_allocate_device();
	if (!input_dev)
		return -ENOMEM;

	adc1_init();

	platform_set_drvdata(pdev, adkey);
	adkey->input = input_dev;
	adkey->press = 0;
	adkey->sync_me = 0;

	input_dev->name = pdev->name;
	input_dev->phys = DRV_NAME"/input0";
	input_dev->id.bustype = BUS_HOST;
	input_dev->id.vendor = 0x0001;
	input_dev->id.product = 0x0010;
	input_dev->id.version = 0x0100;
	input_dev->dev.parent = &pdev->dev;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REP);
	for (i = 0; i < adkey->nkey; i++)
		set_bit(adkey->keys[i].code, input_dev->keybit);
	clear_bit(KEY_RESERVED, input_dev->keybit);

	input_set_capability(input_dev, EV_MSC, MSC_SCAN);

	INIT_DELAYED_WORK(&adkey->work, adkey_input_device_work);
	adkey_input_queue_work(adkey);

	setup_timer(&adkey->timer, adkeys_timer, (unsigned long)adkey);

	err = input_register_device(adkey->input);
	if (err) {
		dev_err(&pdev->dev, "failed to register input device: %d\n", err);
		cancel_delayed_work_sync(&adkey->work);
		input_free_device(input_dev);
		return err;
	}

	return 0;
}

static int analog_gpio_remove(struct platform_device *pdev)
{
	struct adgpio_key_data *adkey = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&adkey->work);
	input_unregister_device(adkey->input);
	input_free_device(adkey->input);
	del_timer_sync(&adkey->timer);

	return 0;
}

static const struct of_device_id akad_keys_of_match[] = {
	{ .compatible = "anyka,ak39ev330-adckeys" },
	{ }
};
MODULE_DEVICE_TABLE(of, akad_keys_of_match);

static struct platform_driver analog_ops = {
	.probe	= analog_gpio_probe,
	.remove	= analog_gpio_remove,
	.driver	= {
		.owner		= THIS_MODULE,
		.name		= DRV_NAME,
		.of_match_table	= akad_keys_of_match,
	},
};
module_platform_driver(analog_ops);

MODULE_AUTHOR("Anyka Microelectronic Ltd.");
MODULE_DESCRIPTION("Anyka ADC simulate gpio keys driver");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_LICENSE("GPL");
