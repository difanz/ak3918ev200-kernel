#ifndef __AK_MOTOR_H__
#define __AK_MOTOR_H__

#ifdef __KERNEL__
enum ak_motor_phase {
	AK_MOTOR_PHASE_A = 0,
	AK_MOTOR_PHASE_B,
	AK_MOTOR_PHASE_C,
	AK_MOTOR_PHASE_D,
	AK_MOTOR_PHASE_NUM,
};

enum ak_motor_hit {
	AK_MOTOR_HIT_LEFT = 0,
	AK_MOTOR_HIT_RIGHT,
	AK_MOTOR_HIT_NUM,
};


struct ak_motor_plat_data 
{
    struct gpio_info gpio_phase[AK_MOTOR_PHASE_NUM];
	struct gpio_info gpio_hit[AK_MOTOR_HIT_NUM];
	unsigned int irq_hit_type[AK_MOTOR_HIT_NUM];
	void (* gpio_init) (const struct gpio_info *);

	unsigned int angular_speed;
};
#endif

#define AK_MOTOR_IOC_MAGIC 		'm'
#define AK_MOTOR_SET_ANG_SPEED 		_IOW(AK_MOTOR_IOC_MAGIC, 11, int)
#define AK_MOTOR_GET_ANG_SPEED 		_IOR(AK_MOTOR_IOC_MAGIC, 12, int)
#define AK_MOTOR_TURN_CLKWISE 		_IOW(AK_MOTOR_IOC_MAGIC, 13, int)
#define AK_MOTOR_TURN_ANTICLKWISE 	_IOW(AK_MOTOR_IOC_MAGIC, 14, int)
#define AK_MOTOR_GET_HIT_STATUS 	_IOW(AK_MOTOR_IOC_MAGIC, 15, int)
#define AK_MOTOR_TURN_STOP 			_IOW(AK_MOTOR_IOC_MAGIC, 16, int)

#define AK_MOTOR_EVENT_HIT 		(1)
#define AK_MOTOR_EVENT_UNHIT 	(2)
#define AK_MOTOR_EVENT_STOP 	(3)

#define AK_MOTOR_HITTING_LEFT 	(1<<0)
#define AK_MOTOR_HITTING_RIGHT 	(1<<1)

/*
 * get_delay_by_speed() turns these into a step period of 200/speed ms, so 16
 * is 12 ms and a 350 degree pan takes 2048 steps - about twenty-five seconds,
 * far slower than the mechanism needs. The board asked for 200, which is 1 ms
 * and too quick for the geared steppers to follow: they buzz on the stop
 * instead of turning. 50 is 4 ms, three times quicker than 16 and well clear
 * of the rate that buzzes.
 */
#define AK_MOTOR_MIN_SPEED 		(1)
#define AK_MOTOR_MAX_SPEED 		(50)
struct notify_data
{
	int hit_num;
	int event;
	int remain_angle;
};

#endif
