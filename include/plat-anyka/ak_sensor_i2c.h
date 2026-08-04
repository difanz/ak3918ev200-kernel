/*
 * Required by ak_sensor_i2c.c (T_SENSOR_I2C_DATA_S) and ak_sensor_common.c.
 * Layout must match struct ak_sensor_i2c_data in ak_sensor_common.h.
 *
 * Address form: u8DevAddr is the **8-bit write address**.
 *   sensor_read/write_register do: g_client->addr = u8DevAddr >> 1;
 * so GC1084 (7-bit 0x37) must pass 0x6E.
 *
 * Read form: sensor_read_register returns the datum as s32 (negative on
 * error). It does not fill u32Data; ak_sensor_common may stash it.
 */
#ifndef __AK_SENSOR_I2C_H__
#define __AK_SENSOR_I2C_H__

#include <linux/types.h>

struct i2c_client;

typedef struct {
	unsigned char	u8DevAddr;		/* 8-bit; kernel uses >> 1 */
	unsigned int	u32RegAddr;
	unsigned int	u32RegAddrByteNum;	/* 1 or 2 */
	unsigned int	u32Data;		/* write data; may be filled on read by common */
	unsigned int	u32DataByteNum;		/* 1 or 2 */
	unsigned int	reserved[2];
} T_SENSOR_I2C_DATA_S;

s32 ak_sensor_i2c_set_client(struct i2c_client *client);
s32 sensor_read_register(T_SENSOR_I2C_DATA_S *pI2cData);
s32 sensor_write_register(T_SENSOR_I2C_DATA_S *pI2cData);

#endif /* __AK_SENSOR_I2C_H__ */
