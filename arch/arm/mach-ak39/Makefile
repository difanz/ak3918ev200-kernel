#linux/arch/arm/mach-ak39/Makefile
#
# Copyright 2013 Anyka Microelectronics
#
# Licensed under GPLv2

obj-y               :=
obj-m               :=
obj-n               :=
obj-                :=

# Core support for all Anyka SoCs
obj-y			+= clock.o cpu.o time.o l2cache.o irq.o reset.o reboot.o adc.o ak39-gpio.o gpio.o


# chip extra interface
obj-$(CONFIG_AK39_PWM_TIMER)		+= pwm_timer.o

# Power Management support
obj-$(CONFIG_AK39_PM)       += pm.o
