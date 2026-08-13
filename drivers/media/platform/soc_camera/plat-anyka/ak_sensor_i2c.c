/*
 * ak sensor Driver
 *
 * Copyright (C) 2012 Anyka
 *
 * Based on anykaplatform driver,
 *
 */

#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/i2c.h>
#include <linux/spinlock.h>
#include <plat-anyka/ak_sensor_i2c.h>

static struct i2c_client *g_client = NULL;

enum ak_sensor_i2c_event {
	AK_SENSOR_I2C_READ_RETRY,
	AK_SENSOR_I2C_READ_FAIL,
	AK_SENSOR_I2C_WRITE_RETRY,
	AK_SENSOR_I2C_WRITE_FAIL,
	AK_SENSOR_I2C_NO_CLIENT,
	AK_SENSOR_I2C_EVENTS,
};

static const char * const ak_sensor_i2c_event_name[AK_SENSOR_I2C_EVENTS] = {
	[AK_SENSOR_I2C_READ_RETRY]	= "read_retry",
	[AK_SENSOR_I2C_READ_FAIL]	= "read_fail",
	[AK_SENSOR_I2C_WRITE_RETRY]	= "write_retry",
	[AK_SENSOR_I2C_WRITE_FAIL]	= "write_fail",
	[AK_SENSOR_I2C_NO_CLIENT]	= "no_client",
};

static DEFINE_SPINLOCK(ak_sensor_i2c_stats_lock);
static u32 ak_sensor_i2c_count[AK_SENSOR_I2C_EVENTS];
static unsigned long ak_sensor_i2c_reported;

static bool ak_sensor_i2c_note(enum ak_sensor_i2c_event ev, unsigned int n)
{
	unsigned long flags;
	bool report;

	spin_lock_irqsave(&ak_sensor_i2c_stats_lock, flags);
	ak_sensor_i2c_count[ev] += n;
	report = !(ak_sensor_i2c_reported & BIT(ev));
	ak_sensor_i2c_reported |= BIT(ev);
	spin_unlock_irqrestore(&ak_sensor_i2c_stats_lock, flags);

	return report;
}

static ssize_t i2c_stats_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	u32 stats[AK_SENSOR_I2C_EVENTS];
	unsigned long flags;
	ssize_t len = 0;
	int i;

	spin_lock_irqsave(&ak_sensor_i2c_stats_lock, flags);
	memcpy(stats, ak_sensor_i2c_count, sizeof(stats));
	spin_unlock_irqrestore(&ak_sensor_i2c_stats_lock, flags);

	for (i = 0; i < AK_SENSOR_I2C_EVENTS; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s %u\n",
				 ak_sensor_i2c_event_name[i], stats[i]);

	return len;
}
static DEVICE_ATTR_RO(i2c_stats);

/**
 * @brief: write sensor register by i2c bus
 * 
 * @author: caolianming
 * @date: 2014-01-09
 * @param [in] raddr: sensor device register(cmd)
 * @param [in] *data: pointer to data, the data writed to sensor register
 * @param [in] size: data number
 */
static s32 aksensor_i2c_write_byte_short(u16 raddr, u8 data)
{
	unsigned char msg[3];

	msg[0] = raddr >> 8;
	msg[1] = raddr & 0xff;
	msg[2] = data;
	
//	printk("msg=0x%02x%02x, 0x%02x(write)\n", msg[0], msg[1], msg[2]);
	return i2c_master_send(g_client, msg, 3);
}

/**
 * @brief: read sensor register value by i2c bus
 * 
 * @author: caolianming
 * @date: 2014-01-09
 * @param [in] raddr: sensor device register(cmd)
 */
static s32 aksensor_i2c_read_byte_short(u16 raddr)
{
	int ret;
	unsigned char msg[2];
	unsigned char data;

	msg[0] = raddr >> 8;
	msg[1] = raddr & 0xff;

	ret = i2c_master_send(g_client, msg, 2);
	if (ret < 0)
		return ret;

	ret = i2c_master_recv(g_client, &data, 1);
	if (ret < 0)
		return ret;

	//	printk("msg=0x%02x%02x, 0x%02x(read)\n", msg[0], msg[1], data);

	return data;
}

/**
 * @brief: write sensor register by i2c bus
 * 
 * @author: caolianming
 * @date: 2014-01-09
 * @param [in] raddr: sensor device register(cmd)
 * @param [in] *data: pointer to data, the data writed to sensor register
 * @param [in] size: data number
 */
static s32 aksensor_i2c_write_word_data(u16 raddr, u16 data)
{
	unsigned char msg[4];

	msg[0] = raddr >> 8;	//high 8bit first send
	msg[1] = raddr & 0xff;	//low 8bit second send
	msg[2] = data >> 8;	//high 8bit firt send
	msg[3] = data & 0xff;	//low 8bit second send
	
	//printk("(cmd): 0x%02x %02x, (data): %02x %02x\n", msg[0], msg[1], msg[2], msg[3]);
	return i2c_master_send(g_client, msg, 4);
}

/**
 * @brief: read sensor register value by i2c bus
 * 
 * @author: caolianming
 * @date: 2014-01-09
 * @param [in] raddr: sensor device register(cmd)
 */
static s32 aksensor_i2c_read_word_data(u16 raddr)
{
	int ret;
	unsigned char msg[4];
	unsigned char buf[2];
	
	msg[0] = raddr >> 8;	//high 8bit first send
	msg[1] = raddr & 0xff;	//low 8bit second send
	
	ret = i2c_master_send(g_client, msg, 2);
	if (ret < 0)
		return ret;
	
	ret = i2c_master_recv(g_client, buf, 2);
	if (ret < 0)
		return ret;

	return (buf[0] << 8) | buf[1];
}

s32 ak_sensor_i2c_set_client(struct i2c_client *client)
{
	unsigned long flags;

	if (client != g_client) {
		spin_lock_irqsave(&ak_sensor_i2c_stats_lock, flags);
		memset(ak_sensor_i2c_count, 0, sizeof(ak_sensor_i2c_count));
		ak_sensor_i2c_reported = 0;
		spin_unlock_irqrestore(&ak_sensor_i2c_stats_lock, flags);

		if (client &&
		    device_create_file(&client->dev, &dev_attr_i2c_stats))
			dev_warn(&client->dev, "no i2c_stats attribute\n");
	}

	g_client = client;
	return 0;
}
EXPORT_SYMBOL(ak_sensor_i2c_set_client);

s32 sensor_read_register(T_SENSOR_I2C_DATA_S *pI2cData)
{
	int retry = 0, retry_max = 5;
	s32 ret = 0;

	if (!g_client) {
		if (ak_sensor_i2c_note(AK_SENSOR_I2C_NO_CLIENT, 1))
			printk(KERN_ERR "%s g_client is NULL; counted in i2c_stats from here on\n",
			       __func__);
		return 0;
	}

	g_client->addr = pI2cData->u8DevAddr >> 1;

__retry:
	switch (pI2cData->u32RegAddrByteNum) {
	case 1:
		if (pI2cData->u32DataByteNum == 1)
			ret = i2c_smbus_read_byte_data(g_client, pI2cData->u32RegAddr);
		else if (pI2cData->u32RegAddrByteNum == 2)
			ret = i2c_smbus_read_word_data(g_client, pI2cData->u32RegAddr);
		break;
	case 2:
		if (pI2cData->u32DataByteNum == 1)
			ret = aksensor_i2c_read_byte_short(pI2cData->u32RegAddr);
		else if (pI2cData->u32RegAddrByteNum == 2)
			ret = aksensor_i2c_read_word_data(pI2cData->u32RegAddr);
		break;
	default:
		break;
	}

	if ((ret < 0) && (retry++ < retry_max)) {
		goto __retry;
	}

	if (retry > 0) {
		if (ak_sensor_i2c_note(AK_SENSOR_I2C_READ_RETRY, retry))
			printk(KERN_ERR "i2c read dev:0x%x reg[0x%x] retry:%d; further retries are counted in i2c_stats only\n",
			       (pI2cData->u8DevAddr >> 1) << 1, pI2cData->u32RegAddr, retry);
	}

	if (ret < 0) {
		if (ak_sensor_i2c_note(AK_SENSOR_I2C_READ_FAIL, 1))
			printk(KERN_ERR "i2c read dev:0x%x reg[0x%x] fail; counted in i2c_stats from here on\n",
			       (pI2cData->u8DevAddr >> 1) << 1, pI2cData->u32RegAddr);
	}

	return ret;
}
EXPORT_SYMBOL(sensor_read_register);

s32 sensor_write_register(T_SENSOR_I2C_DATA_S *pI2cData)
{
	int retry = 0, retry_max = 5;
	s32 ret = 0;

	if (!g_client) {
		if (ak_sensor_i2c_note(AK_SENSOR_I2C_NO_CLIENT, 1))
			printk(KERN_ERR "%s g_client is NULL; counted in i2c_stats from here on\n",
			       __func__);
		return -1;
	}

	g_client->addr = pI2cData->u8DevAddr >> 1;

__retry:
	switch (pI2cData->u32RegAddrByteNum) {
	case 1:
		if (pI2cData->u32DataByteNum == 1)
			ret = i2c_smbus_write_byte_data(g_client, pI2cData->u32RegAddr, pI2cData->u32Data);
		else if (pI2cData->u32RegAddrByteNum == 2)
			ret = i2c_smbus_write_word_data(g_client, pI2cData->u32RegAddr, pI2cData->u32Data);
		break;
	case 2:
		if (pI2cData->u32DataByteNum == 1)
			ret = aksensor_i2c_write_byte_short(pI2cData->u32RegAddr, pI2cData->u32Data);
		else if (pI2cData->u32RegAddrByteNum == 2)
			ret = aksensor_i2c_write_word_data(pI2cData->u32RegAddr, pI2cData->u32Data);
		break;
	default:
		break;
	}

	if ((ret < 0) && (retry++ < retry_max)) {
		goto __retry;
	}

	if (retry > 0) {
		if (ak_sensor_i2c_note(AK_SENSOR_I2C_WRITE_RETRY, retry))
			printk(KERN_ERR "i2c write dev:0x%x reg[0x%x] data[0x%x] retry:%d; further retries are counted in i2c_stats only\n",
			       (pI2cData->u8DevAddr >> 1) << 1, pI2cData->u32RegAddr, pI2cData->u32Data, retry);
	}

	if (ret < 0) {
		if (ak_sensor_i2c_note(AK_SENSOR_I2C_WRITE_FAIL, 1))
			printk(KERN_ERR "i2c write dev:0x%x reg[0x%x] data[0x%x] fail; counted in i2c_stats from here on\n",
			       (pI2cData->u8DevAddr >> 1) << 1, pI2cData->u32RegAddr, pI2cData->u32Data);
	}

	return ret;
}
EXPORT_SYMBOL(sensor_write_register);
