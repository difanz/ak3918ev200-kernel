/*
 *
 * Not present in Anyka 1.0.05. Required by:
 *   drivers/misc/ak_isp_char.c
 *
 * Platform structs live in aksensor.h (with pin_pwdn / crop left-top).
 * This header is the ISP glue: AK_ISP_SENSOR_CB publication + ispdrv enums.
 */
#ifndef __AK_SENSOR_H__
#define __AK_SENSOR_H__

#include <plat-anyka/aksensor.h>
#include <plat-anyka/ak_isp_drv.h>
#include <mach-anyka/ispdrv_interface.h>

void ak_sensor_set_sensor_cb(AK_ISP_SENSOR_CB *cb);
AK_ISP_SENSOR_CB *ak_sensor_get_sensor_cb(void);

int aksensor_get_sensor_id(void);
int aksensor_get_sensor_if(char *if_str);

#endif /* __AK_SENSOR_H__ */
