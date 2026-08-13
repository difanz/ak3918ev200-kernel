#ifndef __AK_MOTOR_H__
#define __AK_MOTOR_H__

#define AK_MOTOR_IOC_MAGIC		'm'
#define AK_MOTOR_SET_ANG_SPEED		_IOW(AK_MOTOR_IOC_MAGIC, 11, int)
#define AK_MOTOR_GET_ANG_SPEED		_IOR(AK_MOTOR_IOC_MAGIC, 12, int)
#define AK_MOTOR_TURN_CLKWISE		_IOW(AK_MOTOR_IOC_MAGIC, 13, int)
#define AK_MOTOR_TURN_ANTICLKWISE	_IOW(AK_MOTOR_IOC_MAGIC, 14, int)
#define AK_MOTOR_GET_HIT_STATUS		_IOW(AK_MOTOR_IOC_MAGIC, 15, int)
#define AK_MOTOR_TURN_STOP		_IOW(AK_MOTOR_IOC_MAGIC, 16, int)

#define AK_MOTOR_EVENT_HIT		(1u << 0)
#define AK_MOTOR_EVENT_UNHIT		(1u << 1)
#define AK_MOTOR_EVENT_STOP		(1u << 2)

#define AK_MOTOR_HITTING_LEFT		(1 << 0)
#define AK_MOTOR_HITTING_RIGHT		(1 << 1)

#define AK_MOTOR_MIN_SPEED		(1)
#define AK_MOTOR_MAX_SPEED		(200)

#define AK_MOTOR_MAX_ANGLE		(360)

struct notify_data {
	int hit_num;
	int event;
	int remain_steps;
};

#endif
