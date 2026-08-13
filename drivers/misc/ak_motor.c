/*
 *  AK on-chip 4-phase stepper motor driver
 *  Copyright (C) 2013 Anyka CO.,LTD
 *  Author: lixinhai
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <plat-anyka/ak_motor.h>

#define AK_MOTOR_DEVNAME	"ak-motor"

#define AK_MOTOR_PHASE_NUM	(4)

#define MOTOR_TURN_CLKWISE	(0)
#define MOTOR_TURN_ANTICLKWISE	(1)

#define MOTOR_STEP_PERIOD	(64)
#define MOTOR_STEP_ANGLE	(360 / MOTOR_STEP_PERIOD)
#define MOTOR_STEP_REMAIN	(360 % MOTOR_STEP_PERIOD)
#define MOTOR_DEFAULT_DELAY_MS	(2)

#define MOTOR_STATUS_RUNNING	(1)
#define MOTOR_STATUS_STOPING	(2)
#define MOTOR_STATUS_STOPED	(3)

static const u8 phase_seq[2][AK_MOTOR_PHASE_NUM] = {
	[MOTOR_TURN_CLKWISE]	 = { 0x03, 0x09, 0x0c, 0x06 },
	[MOTOR_TURN_ANTICLKWISE] = { 0x03, 0x06, 0x0c, 0x09 },
};

struct ak_motor {
	struct miscdevice	miscdev;
	struct device		*dev;
	struct gpio_desc	*phase[AK_MOTOR_PHASE_NUM];

	spinlock_t		lock;
	struct timer_list	work_timer;
	wait_queue_head_t	event;
	atomic_t		opened;

	unsigned int		angular_speed;
	unsigned int		delay_ms;
	unsigned long		delay_jiffies;

	int			dir;
	int			angle;
	int			total;
	int			count;
	int			remain_angle;
	int			running;

	int			rd_flags;
	struct notify_data	data;
};

static inline int motor_step_count(int angle)
{
	int count;

	count = angle * MOTOR_STEP_ANGLE;
	count += (angle * MOTOR_STEP_REMAIN) / MOTOR_STEP_PERIOD;

	return count;
}

static inline unsigned int get_delay_by_speed(unsigned int speed)
{
	unsigned int time;

	time = 1000 / speed;
	time /= MOTOR_STEP_ANGLE;
	if (time == 0)
		time = MOTOR_DEFAULT_DELAY_MS;

	return time;
}

static void ak_motor_set_speed(struct ak_motor *motor, unsigned int speed)
{
	motor->angular_speed = speed;
	motor->delay_ms = get_delay_by_speed(speed);
	motor->delay_jiffies = msecs_to_jiffies(motor->delay_ms);
}

static void ak_motor_phase_write(struct ak_motor *motor, u8 pattern)
{
	int i;

	for (i = 0; i < AK_MOTOR_PHASE_NUM; i++)
		gpiod_set_value(motor->phase[i], !!(pattern & (1 << i)));
}

static void ak_motor_notify(struct ak_motor *motor, int num, int event)
{
	motor->data.hit_num = num;
	motor->data.event = event;
	motor->data.remain_angle = motor->remain_angle;
	motor->rd_flags = 1;

	wake_up_interruptible(&motor->event);
}

static void ak_motor_timer_handler(unsigned long data)
{
	struct ak_motor *motor = (struct ak_motor *)data;
	unsigned long flags;
	int step;

	spin_lock_irqsave(&motor->lock, flags);

	if (motor->running != MOTOR_STATUS_RUNNING) {
		motor->running = MOTOR_STATUS_STOPED;
		ak_motor_phase_write(motor, 0);
		ak_motor_notify(motor, 0, AK_MOTOR_EVENT_STOP);
		goto out;
	}

	step = motor->total - motor->count;
	ak_motor_phase_write(motor,
			     phase_seq[motor->dir][step % AK_MOTOR_PHASE_NUM]);

	motor->remain_angle = motor->angle -
			      (step * MOTOR_STEP_PERIOD) / 360;

	if (--motor->count > 0) {
		mod_timer(&motor->work_timer, jiffies + motor->delay_jiffies);
	} else {
		motor->running = MOTOR_STATUS_STOPED;
		motor->remain_angle = 0;
		ak_motor_phase_write(motor, 0);
		ak_motor_notify(motor, 0, AK_MOTOR_EVENT_STOP);
	}

out:
	spin_unlock_irqrestore(&motor->lock, flags);
}

static int ak_motor_turn(struct ak_motor *motor, int dir, int angle)
{
	unsigned long flags;

	spin_lock_irqsave(&motor->lock, flags);

	motor->dir = dir;
	motor->angle = angle;
	motor->remain_angle = angle;
	motor->total = motor_step_count(angle);
	motor->count = motor->total;
	motor->running = MOTOR_STATUS_RUNNING;

	if (motor->count <= 0) {
		motor->running = MOTOR_STATUS_STOPED;
		motor->remain_angle = 0;
		ak_motor_notify(motor, 0, AK_MOTOR_EVENT_STOP);
		spin_unlock_irqrestore(&motor->lock, flags);
		return 0;
	}

	mod_timer(&motor->work_timer, jiffies);

	spin_unlock_irqrestore(&motor->lock, flags);

	return 0;
}

static void ak_motor_stop(struct ak_motor *motor)
{
	unsigned long flags;

	spin_lock_irqsave(&motor->lock, flags);
	if (motor->running == MOTOR_STATUS_RUNNING)
		motor->running = MOTOR_STATUS_STOPING;
	spin_unlock_irqrestore(&motor->lock, flags);
}

static int ak_motor_open(struct inode *inode, struct file *file)
{
	struct ak_motor *motor = container_of(file->private_data,
					      struct ak_motor, miscdev);

	if (atomic_cmpxchg(&motor->opened, 0, 1) != 0)
		return -EBUSY;

	motor->rd_flags = 0;
	motor->data.event = AK_MOTOR_EVENT_UNHIT;
	motor->data.hit_num = 0;
	motor->data.remain_angle = 0;

	file->private_data = motor;

	return 0;
}

static int ak_motor_release(struct inode *inode, struct file *file)
{
	struct ak_motor *motor = file->private_data;

	ak_motor_stop(motor);
	del_timer_sync(&motor->work_timer);

	spin_lock_irq(&motor->lock);
	motor->running = MOTOR_STATUS_STOPED;
	ak_motor_phase_write(motor, 0);
	spin_unlock_irq(&motor->lock);

	atomic_set(&motor->opened, 0);

	return 0;
}

static ssize_t ak_motor_read(struct file *file, char __user *buf, size_t len,
			     loff_t *ofs)
{
	struct ak_motor *motor = file->private_data;
	struct notify_data data;
	unsigned long flags;
	int ret;

	if (len < sizeof(data))
		return -EINVAL;

	if (file->f_flags & O_NONBLOCK) {
		if (!motor->rd_flags)
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(motor->event,
					       motor->rd_flags != 0);
		if (ret)
			return ret;
	}

	spin_lock_irqsave(&motor->lock, flags);
	data = motor->data;
	motor->rd_flags = 0;
	spin_unlock_irqrestore(&motor->lock, flags);

	if (copy_to_user(buf, &data, sizeof(data)))
		return -EFAULT;

	return sizeof(data);
}

static unsigned int ak_motor_poll(struct file *file, poll_table *wait)
{
	struct ak_motor *motor = file->private_data;
	unsigned int mask = 0;

	poll_wait(file, &motor->event, wait);

	if (motor->rd_flags)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static long ak_motor_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct ak_motor *motor = file->private_data;
	int __user *argp = (int __user *)arg;
	int val;

	if (_IOC_TYPE(cmd) != AK_MOTOR_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case AK_MOTOR_SET_ANG_SPEED:
		if (get_user(val, argp))
			return -EFAULT;
		if (val < AK_MOTOR_MIN_SPEED || val > AK_MOTOR_MAX_SPEED)
			return -EINVAL;
		ak_motor_set_speed(motor, val);
		return 0;

	case AK_MOTOR_GET_ANG_SPEED:
		if (put_user((int)motor->angular_speed, argp))
			return -EFAULT;
		return 0;

	case AK_MOTOR_TURN_CLKWISE:
	case AK_MOTOR_TURN_ANTICLKWISE:
		if (get_user(val, argp))
			return -EFAULT;
		if (val < 0 || val > AK_MOTOR_MAX_ANGLE)
			return -EINVAL;
		if (motor->running == MOTOR_STATUS_RUNNING)
			return -EBUSY;
		return ak_motor_turn(motor,
				     cmd == AK_MOTOR_TURN_CLKWISE ?
				     MOTOR_TURN_CLKWISE : MOTOR_TURN_ANTICLKWISE,
				     val);

	case AK_MOTOR_GET_HIT_STATUS:
		if (put_user(0, argp))
			return -EFAULT;
		return 0;

	case AK_MOTOR_TURN_STOP:
		ak_motor_stop(motor);
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations ak_motor_fops = {
	.owner		= THIS_MODULE,
	.open		= ak_motor_open,
	.release	= ak_motor_release,
	.read		= ak_motor_read,
	.poll		= ak_motor_poll,
	.unlocked_ioctl	= ak_motor_ioctl,
	.llseek		= no_llseek,
};

static int ak_motor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ak_motor *motor;
	u32 speed = AK_MOTOR_MAX_SPEED;
	int i, id, ret;

	motor = devm_kzalloc(dev, sizeof(*motor), GFP_KERNEL);
	if (!motor)
		return -ENOMEM;

	id = of_alias_get_id(dev->of_node, "motor");
	if (id < 0)
		id = pdev->id < 0 ? 0 : pdev->id;

	for (i = 0; i < AK_MOTOR_PHASE_NUM; i++) {
		motor->phase[i] = devm_gpiod_get_index(dev, "phase", i,
						       GPIOD_OUT_LOW);
		if (IS_ERR(motor->phase[i])) {
			ret = PTR_ERR(motor->phase[i]);
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "phase %d gpio unavailable: %d\n",
					i, ret);
			return ret;
		}
	}

	of_property_read_u32(dev->of_node, "angular-speed", &speed);
	if (speed < AK_MOTOR_MIN_SPEED || speed > AK_MOTOR_MAX_SPEED) {
		dev_warn(dev, "angular speed %u out of range, using %d\n",
			 speed, AK_MOTOR_MAX_SPEED);
		speed = AK_MOTOR_MAX_SPEED;
	}

	motor->dev = dev;
	spin_lock_init(&motor->lock);
	init_waitqueue_head(&motor->event);
	atomic_set(&motor->opened, 0);
	motor->running = MOTOR_STATUS_STOPED;
	ak_motor_set_speed(motor, speed);
	setup_timer(&motor->work_timer, ak_motor_timer_handler,
		    (unsigned long)motor);

	motor->miscdev.minor = MISC_DYNAMIC_MINOR;
	motor->miscdev.name = devm_kasprintf(dev, GFP_KERNEL, "%s%d",
					     AK_MOTOR_DEVNAME, id);
	if (!motor->miscdev.name)
		return -ENOMEM;
	motor->miscdev.fops = &ak_motor_fops;
	motor->miscdev.mode = 0600;

	ret = misc_register(&motor->miscdev);
	if (ret) {
		dev_err(dev, "register misc device fail: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, motor);
	dev_info(dev, "%s: %u steps per turn, speed %u, %u ms per step\n",
		 motor->miscdev.name, motor_step_count(360),
		 motor->angular_speed, jiffies_to_msecs(motor->delay_jiffies));

	return 0;
}

static int ak_motor_remove(struct platform_device *pdev)
{
	struct ak_motor *motor = platform_get_drvdata(pdev);

	misc_deregister(&motor->miscdev);
	del_timer_sync(&motor->work_timer);
	ak_motor_phase_write(motor, 0);

	return 0;
}

static const struct of_device_id ak_motor_of_match[] = {
	{ .compatible = "anyka,ak39ev330-motor0" },
	{ .compatible = "anyka,ak39ev330-motor1" },
	{ }
};
MODULE_DEVICE_TABLE(of, ak_motor_of_match);

static struct platform_driver ak_motor_driver = {
	.probe	= ak_motor_probe,
	.remove	= ak_motor_remove,
	.driver	= {
		.name		= AK_MOTOR_DEVNAME,
		.owner		= THIS_MODULE,
		.of_match_table	= ak_motor_of_match,
	},
};

module_platform_driver(ak_motor_driver);

MODULE_AUTHOR("Anyka");
MODULE_DESCRIPTION("Anyka 4-phase stepper motor driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ak-motor");
