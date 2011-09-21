# sensor 
obj-$(CONFIG_SENSOR_GC0308)   += camera_gc0308.o 
obj-$(CONFIG_SENSOR_OV2643)   += camera_ov2643.o
obj-$(CONFIG_SENSOR_OV7725)   += camera_ov7725.o
obj-$(CONFIG_SENSOR_HM1375)   += camera_hm1375.o
obj-$(CONFIG_SENSOR_NT99141)+= camera_nt99141.o

# wrap sensor
obj-$(CONFIG_LINUX_AKSENSOR)   += wrap_sensor.o aksensor.o

# ISP2 sensor framework.  This must follow the stack-B sensor drivers above;
# i2c_add_driver() can synchronously probe an already registered client.
obj-$(CONFIG_AK_ISP_CHAR)     += ak_sensor.o ak_sensor_common.o ak_sensor_i2c.o
