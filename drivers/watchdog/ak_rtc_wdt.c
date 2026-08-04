/*
 * drivers/watchdog/ak_rtc_wdt.c
 *
 * RTC-backed watchdog driver for the Anyka AK39 SoC.
 * driver). Registers a /dev/watchdog miscdevice whose timeout is driven by
 * the SoC's RTC alarm.
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>

#include <mach/ak_rtc_regs.h>

#define SELECT_WTC	1
#define SELECT_RTC	0

static int nowayout = WATCHDOG_NOWAYOUT;
static unsigned int def_heartbeat = 0x1FFF;	/* ~8s, vendor default */
static unsigned int now_heartbeat = 0x1FFF;

static unsigned long in_use;
static atomic_t in_write = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(ak_rtc_wdt_lock);

module_param(def_heartbeat, int, 0);
MODULE_PARM_DESC(def_heartbeat, "Watchdog heartbeat, RTC alarm ticks (default 0x1FFF, ~8s)");

module_param(nowayout, int, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started");

static void select_wdt_rtc(int which)
{
	unsigned long val = ak_rtc_read(AK_RTC_SETTING);

	if (which == SELECT_RTC)
		val &= ~(1 << 10);
	else
		val |= (1 << 10);
	ak_rtc_write(AK_RTC_SETTING, val);
}

static void ak_rtc_wdt_enable(void)
{
	unsigned long val;

	spin_lock(&ak_rtc_wdt_lock);
	select_wdt_rtc(SELECT_WTC);

	val = ak_rtc_read(AK_WDT_RTC_TIMER_CONF);
	val |= (1 << 13);
	ak_rtc_write(AK_WDT_RTC_TIMER_CONF, val);

	val = ak_rtc_read(AK_WDT_RTC_TIMER_CONF);
	val &= (1 << 13);
	val |= (def_heartbeat & 0x1FFF);
	ak_rtc_write(AK_WDT_RTC_TIMER_CONF, val);

	val = ak_rtc_read(AK_RTC_SETTING);
	val |= ((1 << 5) | (1 << 2));
	val &= ~(1 << 11);
	ak_rtc_write(AK_RTC_SETTING, val);

	select_wdt_rtc(SELECT_RTC);
	spin_unlock(&ak_rtc_wdt_lock);
}

static void ak_rtc_wdt_disable(void)
{
	unsigned long val;

	spin_lock(&ak_rtc_wdt_lock);
	select_wdt_rtc(SELECT_WTC);

	val = ak_rtc_read(AK_RTC_SETTING);
	val |= (1 << 6);
	ak_rtc_write(AK_RTC_SETTING, val);

	val = ak_rtc_read(AK_WDT_RTC_TIMER_CONF);
	val &= ~(1 << 13);
	ak_rtc_write(AK_WDT_RTC_TIMER_CONF, val);

	val = ak_rtc_read(AK_RTC_SETTING);
	val &= ~((1 << 2) | (1 << 5));
	ak_rtc_write(AK_RTC_SETTING, val);

	select_wdt_rtc(SELECT_RTC);
	spin_unlock(&ak_rtc_wdt_lock);
}

static void ak_rtc_wdt_keepalive(unsigned int heartbeat)
{
	unsigned long val;

	spin_lock(&ak_rtc_wdt_lock);
	select_wdt_rtc(SELECT_WTC);

	val = ak_rtc_read(AK_RTC_SETTING);
	val |= (1 << 6);
	ak_rtc_write(AK_RTC_SETTING, val);

	val = ak_rtc_read(AK_WDT_RTC_TIMER_CONF);
	val &= (1 << 13);
	val |= (heartbeat & 0x1FFF);
	ak_rtc_write(AK_WDT_RTC_TIMER_CONF, val);

	select_wdt_rtc(SELECT_RTC);
	spin_unlock(&ak_rtc_wdt_lock);
}

static int ak_rtc_wdt_disable_nb(struct notifier_block *n, unsigned long state, void *cmd)
{
	ak_rtc_wdt_disable();
	return NOTIFY_DONE;
}

static struct notifier_block ak_rtc_wdt_nb = {
	.notifier_call = ak_rtc_wdt_disable_nb,
};

static struct watchdog_info ak_rtc_wdt_ident = {
	.options	= WDIOF_MAGICCLOSE | WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING,
	.identity	= "ANYKA AK39 RTC Watchdog",
};

static long ak_rtc_wdt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int __user *p = argp;
	int time;

	switch (cmd) {
	case WDIOC_GETSUPPORT:
		return copy_to_user(argp, &ak_rtc_wdt_ident, sizeof(ak_rtc_wdt_ident)) ?
			-EFAULT : 0;
	case WDIOC_GETSTATUS:
	case WDIOC_GETBOOTSTATUS:
		return put_user(0, p);
	case WDIOC_KEEPALIVE:
		ak_rtc_wdt_keepalive(def_heartbeat);
		now_heartbeat = def_heartbeat;
		return 0;
	case WDIOC_SETTIMEOUT:
		if (get_user(time, p))
			return -EFAULT;
		if (time <= 0 || time > 8)
			return -EINVAL;
		now_heartbeat = time * 1024 - 1;
		ak_rtc_wdt_keepalive(now_heartbeat);
		return 0;
	case WDIOC_GETTIMEOUT:
		return put_user((now_heartbeat + 1) / 1024, p);
	default:
		return -ENOTTY;
	}
}

static ssize_t ak_rtc_wdt_write(struct file *file, const char *data, size_t len, loff_t *ppos)
{
	size_t i;

	if (!len)
		return 0;

	atomic_set(&in_write, 1);
	for (i = 0; i != len; i++) {
		char c;

		if (get_user(c, data + i))
			return -EFAULT;
		if (c == 'V')
			atomic_set(&in_write, 0);
	}
	ak_rtc_wdt_keepalive(def_heartbeat);

	return len;
}

static int ak_rtc_wdt_open(struct inode *inode, struct file *file)
{
	if (test_and_set_bit(0, &in_use))
		return -EBUSY;

	if (nowayout)
		__module_get(THIS_MODULE);

	ak_rtc_wdt_enable();

	return nonseekable_open(inode, file);
}

static int ak_rtc_wdt_release(struct inode *inode, struct file *file)
{
	if (nowayout)
		pr_info("WATCHDOG: nowayout set - no way to disable watchdog\n");
	else if (atomic_read(&in_write))
		pr_crit("WATCHDOG: Device closed unexpectedly - timer will not stop\n");
	else
		ak_rtc_wdt_disable();

	clear_bit(0, &in_use);
	return 0;
}

static const struct file_operations ak_rtc_wdt_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.write		= ak_rtc_wdt_write,
	.unlocked_ioctl	= ak_rtc_wdt_ioctl,
	.open		= ak_rtc_wdt_open,
	.release	= ak_rtc_wdt_release,
};

static struct miscdevice ak_rtc_wdt_miscdev = {
	.minor	= WATCHDOG_MINOR,
	.name	= "watchdog",
	.fops	= &ak_rtc_wdt_fops,
};

static int ak_rtc_wdt_probe(struct platform_device *pdev)
{
	u32 heartbeat;

	if (!of_property_read_u32(pdev->dev.of_node, "def_heartbeat", &heartbeat) && heartbeat) {
		def_heartbeat = (heartbeat * 1024) - 1;
		now_heartbeat = def_heartbeat;
	}

	ak_rtc_power(RTC_ON);
	register_reboot_notifier(&ak_rtc_wdt_nb);

	return misc_register(&ak_rtc_wdt_miscdev);
}

static int ak_rtc_wdt_remove(struct platform_device *pdev)
{
	unregister_reboot_notifier(&ak_rtc_wdt_nb);
	ak_rtc_power(RTC_OFF);
	misc_deregister(&ak_rtc_wdt_miscdev);

	return 0;
}

static const struct of_device_id ak_rtc_wdt_of_match[] = {
	{ .compatible = "anyka,ak39ev330-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, ak_rtc_wdt_of_match);

static struct platform_driver ak_rtc_wdt_driver = {
	.probe	= ak_rtc_wdt_probe,
	.remove	= ak_rtc_wdt_remove,
	.driver	= {
		.name		= "ak-rtc-watchdog",
		.owner		= THIS_MODULE,
		.of_match_table	= ak_rtc_wdt_of_match,
	},
};
module_platform_driver(ak_rtc_wdt_driver);

MODULE_DESCRIPTION("ANYKA AK39 RTC Watchdog");
MODULE_LICENSE("GPL");
MODULE_ALIAS_MISCDEV(WATCHDOG_MINOR);
