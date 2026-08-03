# sensor 
obj-$(CONFIG_SENSOR_GC0308)   += camera_gc0308.o 
obj-$(CONFIG_SENSOR_OV2643)   += camera_ov2643.o
obj-$(CONFIG_SENSOR_OV7725)   += camera_ov7725.o
obj-$(CONFIG_SENSOR_HM1375)   += camera_hm1375.o
obj-$(CONFIG_SENSOR_OV9712)   += camera_ov9712.o
obj-$(CONFIG_SENSOR_OV2710)   += camera_ov2710.o
obj-$(CONFIG_SENSOR_AR0130)   += camera_ar0130.o
obj-$(CONFIG_SENSOR_H22)	  += camera_h22.o
obj-$(CONFIG_SENSOR_NT99141)+= camera_nt99141.o

# wrap sensor
obj-$(CONFIG_LINUX_AKSENSOR)   += wrap_sensor.o aksensor.o

# Stack-B sensor drivers register themselves before the aksensor I2C driver is
# added.  Keep every sensor_*.o in this block so probe_sensors() sees the whole
# runtime-detection registry when an existing board-info client probes.
obj-$(CONFIG_SENSOR_GC1084)   += sensor_gc1084.o

# ISP2 sensor framework.  This must follow the stack-B sensor drivers above;
# i2c_add_driver() can synchronously probe an already registered client.
obj-$(CONFIG_AK_ISP_CHAR)     += ak_sensor.o ak_sensor_common.o ak_sensor_i2c.o

# ISP core. Anyka ships this as a prebuilt libispdrv.a; we build it from the
# SDK sources instead. See notes/isp-port.md.
obj-$(CONFIG_AK_ISP_CORE)     += ak39_isp2.o ak39_isp2_3a.o
