// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Anyka AK3918EV200 V4L2 capture bridge.
 *
 * The ISP2 core is a register library owned by /dev/isp_char and reached
 * through the ispdrv_* shim; this driver owns the DVP capture interface, the
 * four ISP video-output address slots, the frame interrupt and the deferred 3A
 * work, and presents them as plain V4L2 capture nodes over videobuf2.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_graph.h>
#include <linux/of_reserved_mem.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/videodev2.h>

#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include <mach/map.h>
#include <mach-anyka/ispdrv_interface.h>
#include <plat-anyka/ak_sensor.h>

#include "ak-cam.h"

static const char *ak_cam_card = "AK_Camera";

static int _tdnr_flag;
static int _tdnr_set;
static int video_frame_interval;

static unsigned long in_irq_jf;
static unsigned long start_set_td_jf;

static struct ak_cam_buffer *to_ak_cam_buffer(struct vb2_buffer *vb)
{
	return ak_cam_vb_to_buf(vb, struct ak_cam_buffer);
}

static struct ak_cam_node *ak_cam_file_node(struct file *file)
{
	return video_drvdata(file);
}

static unsigned int ak_cam_frame_bytes(unsigned int width, unsigned int height)
{
	return width * height * 3 / 2;
}

/* -------------------------------------------------------------------------
 * ISP slot scheduling
 * ------------------------------------------------------------------------- */

static void ak_cam_resume_work(struct work_struct *work)
{
	struct ak_cam_dev *dev = container_of(work, struct ak_cam_dev,
					      resume_work);
	unsigned long flags;

	mutex_lock(&dev->stream_lock);
	if (dev->stream_ctrl_off && dev->users &&
	    dev->list_state >= LIST_FOUR) {
		ispdrv_set_isp_resume();
		dev->stream_ctrl_off = 0;
		spin_lock_irqsave(&dev->lock, flags);
		dev->stream_active = 1;
		spin_unlock_irqrestore(&dev->lock, flags);
	}
	mutex_unlock(&dev->stream_lock);
}

static void ak_cam_stop_capture(struct ak_cam_dev *dev)
{
	unsigned long flags;
	int stream_active;

	cancel_work_sync(&dev->resume_work);

	mutex_lock(&dev->stream_lock);
	spin_lock_irqsave(&dev->lock, flags);
	stream_active = dev->stream_active;
	spin_unlock_irqrestore(&dev->lock, flags);
	if (stream_active) {
		ispdrv_set_isp_pause();
		ispdrv_vi_stop_capturing();
		spin_lock_irqsave(&dev->lock, flags);
		dev->stream_active = 0;
		spin_unlock_irqrestore(&dev->lock, flags);
		dev->stream_ctrl_off = 1;
	}
	ispdrv_vo_clear_irq_status(0xffff);
	synchronize_irq(dev->irq);
	cancel_delayed_work_sync(&dev->awb_work);
	cancel_delayed_work_sync(&dev->ae_work);
	spin_lock_irqsave(&dev->lock, flags);
	dev->cur_buf_id = -1;
	spin_unlock_irqrestore(&dev->lock, flags);
	mutex_unlock(&dev->stream_lock);
}

/*
 * Slot n is armed when the main node has buffer n and, if the sub node is
 * streaming, the sub node has buffer n too. A sub channel that is not streaming
 * is pointed at one driver-owned scratch frame, which the ISP overwrites and
 * nobody dequeues.
 */
static void ak_cam_arm_slot(struct ak_cam_dev *dev, unsigned int slot)
{
	struct ak_cam_node *main_node = &dev->node[AK_CAM_CHAN_MAIN];
	struct ak_cam_node *sub_node = &dev->node[AK_CAM_CHAN_SUB];
	u32 yaddr_chl1, yaddr_chl2;
	int i;

	if (dev->slot_armed & (1ul << slot))
		return;
	if (!main_node->streaming || !main_node->armed[slot])
		return;
	if (sub_node->streaming && !sub_node->armed[slot])
		return;

	yaddr_chl1 = vb2_dma_contig_plane_dma_addr(ak_cam_buf_vb(main_node->armed[slot]), 0);
	if (sub_node->armed[slot]) {
		yaddr_chl2 = vb2_dma_contig_plane_dma_addr(ak_cam_buf_vb(sub_node->armed[slot]), 0);
		dev->slot_sub |= 1ul << slot;
	} else {
		yaddr_chl2 = dev->scratch_dma;
		dev->slot_sub &= ~(1ul << slot);
	}

	if (dev->list_state == LIST_ZERO) {
		for (i = 0; i < AK_CAM_SLOTS; i++)
			ispdrv_vo_disable_buffer(BUFFER_ONE + i);
	}

	ispdrv_vo_set_buffer_addr(BUFFER_ONE + slot, yaddr_chl1, yaddr_chl2);
	ispdrv_vo_enable_buffer(BUFFER_ONE + slot);
	dev->slot_armed |= 1ul << slot;

	if (dev->list_state < LIST_FIVE)
		dev->list_state++;

	if (dev->list_state == LIST_FOUR) {
		ispdrv_vi_apply_mode(dev->cur_mode);
		ispdrv_vo_enable_irq_status(0x1);

		if (dev->stream_ctrl_off == 0) {
			ispdrv_vi_start_capturing();
			dev->stream_active = 1;
		} else {
			schedule_work(&dev->resume_work);
		}
	}
}

static void ak_cam_arm_all(struct ak_cam_dev *dev)
{
	struct ak_cam_node *main_node = &dev->node[AK_CAM_CHAN_MAIN];
	struct ak_cam_buffer *buf;

	list_for_each_entry(buf, &main_node->capture, list)
		ak_cam_arm_slot(dev, ak_cam_vb_index(ak_cam_buf_vb(buf)));
}

/* -------------------------------------------------------------------------
 * interrupt
 * ------------------------------------------------------------------------- */

static inline unsigned long get_timestamp(void)
{
	unsigned long ul;

	ul = jiffies;
	if (ul >= INITIAL_JIFFIES)
		ul -= INITIAL_JIFFIES;
	else
		ul = (~(unsigned long)0) - INITIAL_JIFFIES + ul;
	ul = jiffies_to_msecs(ul);

	return ul;
}

static void ak_cam_delay_works(struct ak_cam_dev *dev)
{
	if (dev->cur_mode_class >= AK_CAM_MBUS_FMT_SBGGR8_1X8 &&
	    dev->cur_mode_class <= AK_CAM_MBUS_FMT_SRGGB12_1X12) {
		schedule_delayed_work(&dev->awb_work, 0);
		schedule_delayed_work(&dev->ae_work, 0);
	}
}

static void ak_cam_finish_buffer(struct ak_cam_node *node,
				 struct ak_cam_buffer *buf, int data_err)
{
	unsigned long timestamp_ms;

	if (!data_err) {
		timestamp_ms = get_timestamp();
		ak_cam_vb_set_timestamp(ak_cam_buf_vb(buf), timestamp_ms / 1000,
					(timestamp_ms % 1000) * 1000);
	} else {
		ak_cam_vb_set_timestamp(ak_cam_buf_vb(buf), 0, 0);
	}

	ak_cam_vb_set_sequence(ak_cam_buf_vb(buf), node->sequence++);
	list_del_init(&buf->list);
	node->armed[ak_cam_vb_index(ak_cam_buf_vb(buf))] = NULL;
	vb2_buffer_done(ak_cam_buf_vb(buf), VB2_BUF_STATE_DONE);
}

static void ak_cam_finish_sub(struct ak_cam_dev *dev, unsigned int slot,
			      int data_err)
{
	struct ak_cam_node *node = &dev->node[AK_CAM_CHAN_SUB];
	struct ak_cam_buffer *buf = node->armed[slot];

	if (!(dev->slot_sub & (1ul << slot)) || !buf)
		return;

	dev->slot_sub &= ~(1ul << slot);
	ak_cam_finish_buffer(node, buf, data_err);
}

static unsigned long sjf;
static int ak_cam_irq_continuous(struct ak_cam_dev *dev)
{
	struct ak_cam_node *node = &dev->node[AK_CAM_CHAN_MAIN];
	int video_data_err = 0;
	int id;
	struct ak_cam_buffer *buf;
	struct list_head *next;
	unsigned long ul, ul2;
	int fps = 10;
	unsigned int slot;
	AK_ISP_SENSOR_CB *sensor_cb;

	sensor_cb = ak_sensor_get_sensor_cb();
	if (sensor_cb && sensor_cb->sensor_get_fps_func)
		fps = sensor_cb->sensor_get_fps_func();
	if (fps <= 0)
		fps = 10;
	video_frame_interval = 1000 / fps + 10;

	ul = ul2 = jiffies;
	if (sjf == 0)
		sjf = jiffies;

	if (ul >= sjf)
		ul -= sjf;
	else
		ul = (~(unsigned long)0) - sjf + ul;
	if (jiffies_to_msecs(ul) > video_frame_interval)
		_tdnr_flag = 1;
	sjf = ul2;

	id = ispdrv_vo_get_using_frame_buf_id();

	next = node->capture.next;
	if (next == &node->capture)
		return 0;

	buf = list_entry(next, struct ak_cam_buffer, list);
	slot = ak_cam_vb_index(ak_cam_buf_vb(buf));

	if (id == -1)
		return 0;
	else if ((id & 0x7F) != slot)
		return 0;
	else if (id >= 0x80)
		video_data_err = 1;

	next = next->next;

	if (next != &node->capture) {
		ispdrv_vo_disable_buffer(BUFFER_ONE + slot);
		dev->slot_armed &= ~(1ul << slot);

		ak_cam_finish_sub(dev, slot, video_data_err);
		ak_cam_finish_buffer(node, buf, video_data_err);

		dev->cur_buf_id = slot;
	} else {
		dev->cur_buf_id = -1;
		v4l2_warn(&dev->v4l2_dev, "lost frame at %ld\n", jiffies);
	}

	return 0;
}

static irqreturn_t ak_cam_irq(int irq, void *data)
{
	struct ak_cam_dev *dev = data;
	unsigned long stat;
	unsigned long flags;

	in_irq_jf = jiffies;

	spin_lock_irqsave(&dev->lock, flags);

	if (!((stat = ispdrv_vo_check_irq_status()) & 0x01)) {
		ispdrv_vo_clear_irq_status(0xfffe);
		spin_unlock_irqrestore(&dev->lock, flags);
		return IRQ_HANDLED;
	}

	ispdrv_irq_work();
	if (dev->list_state == LIST_ZERO)
		goto out;

	if (!ispdrv_is_continuous()) {
		v4l2_err(&dev->v4l2_dev, "ISP is not in a video mode\n");
		goto out;
	}

	ak_cam_irq_continuous(dev);
	ak_cam_delay_works(dev);

out:
	ispdrv_vo_clear_irq_status(0xffff);
	spin_unlock_irqrestore(&dev->lock, flags);
	return IRQ_HANDLED;
}

static void ak_cam_awb_work(struct work_struct *work)
{
	ispdrv_awb_work();
}

static void ak_cam_ae_work(struct work_struct *work)
{
	int aec_delay_ms = 10;
	unsigned long flags;
	unsigned long cur_jf = jiffies;
	struct ak_cam_dev *dev = container_of(work, struct ak_cam_dev,
					      ae_work.work);
	AK_ISP_SENSOR_CB *sensor_cb;

	if (cur_jf >= in_irq_jf) {
		int cur_fps = 25;
		int active_ms = 36;
		unsigned long diff_jf = cur_jf - in_irq_jf;
		int diff_ms;

		sensor_cb = ak_sensor_get_sensor_cb();
		if (sensor_cb && sensor_cb->sensor_get_parameter_func) {
			sensor_cb->sensor_get_parameter_func(GET_CUR_FPS,
							     &cur_fps);
			sensor_cb->sensor_get_parameter_func(GET_VSYNC_ACTIVE_MS,
							     &active_ms);
			if (cur_fps <= 0)
				cur_fps = 25;
			if (active_ms < 0)
				active_ms = 0;
			aec_delay_ms = MSEC_PER_SEC / cur_fps - active_ms + 1;
			if (aec_delay_ms < 0)
				aec_delay_ms = 0;
			if (!(aec_delay_ms % 10))
				aec_delay_ms = max(aec_delay_ms - 1, 0);
		}

		diff_ms = jiffies_to_msecs(diff_jf);
		if (aec_delay_ms > diff_ms)
			aec_delay_ms -= diff_ms;
		else
			aec_delay_ms = 0;

		usleep_range(aec_delay_ms * USEC_PER_MSEC,
			     (aec_delay_ms + 1) * USEC_PER_MSEC);
	}

	ispdrv_ae_work();

	if (dev->cur_buf_id != -1) {
		void *yuv_paddr, *mdinfo;
		void *yuv_vaddr;

		ispdrv_get_yuvaddr_and_mdinfo(dev->cur_buf_id,
					      &yuv_paddr, &mdinfo);
		yuv_vaddr = ioremap_nocache((unsigned long)yuv_paddr,
					    AK_CAM_MD_INFO_BYTES);
		if (yuv_vaddr) {
			memcpy(yuv_vaddr, mdinfo, AK_CAM_MD_INFO_BYTES);
			iounmap(yuv_vaddr);
		}
		dev->cur_buf_id = -1;
	}

	if (_tdnr_flag && !_tdnr_set) {
		ispdrv_set_td();
		spin_lock_irqsave(&dev->lock, flags);
		_tdnr_set = 1;
		_tdnr_flag = 0;
		start_set_td_jf = jiffies;
		spin_unlock_irqrestore(&dev->lock, flags);
	} else if (_tdnr_set &&
		   jiffies_to_msecs(jiffies - start_set_td_jf) >
		   2 * video_frame_interval) {
		ispdrv_reload_td();
		spin_lock_irqsave(&dev->lock, flags);
		_tdnr_set = 0;
		_tdnr_flag = 0;
		spin_unlock_irqrestore(&dev->lock, flags);
	}
}

/* -------------------------------------------------------------------------
 * clocks, reset and the sensor power sequence
 * ------------------------------------------------------------------------- */

#define REG32(_reg)		(*(volatile unsigned long *)(_reg))
#define CLOCK_PERI_PLL_CTRL1	(AK_VA_SYSCTRL + 0x14)
#define CLOCK_PERI_PLL_CTRL2	(AK_VA_SYSCTRL + 0x18)

static unsigned long ak_get_peri_pll_clk(void)
{
	unsigned long pll_m, pll_n, pll_od;
	unsigned long peri_pll_clk;
	unsigned long regval;

	regval = REG32(CLOCK_PERI_PLL_CTRL1);
	pll_od = (regval & (0x3 << 12)) >> 12;
	pll_n = (regval & (0xf << 8)) >> 8;
	pll_m = regval & 0xff;

	peri_pll_clk = (12 * pll_m) / (pll_n * (1 << pll_od));
	if ((pll_od >= 1) && (pll_n >= 2) && (pll_n <= 6) &&
	    (pll_m >= 84) && (pll_m <= 254))
		return peri_pll_clk * 1000000UL;

	panic("peri pll clk: %ld(Mhz) is unusable\n", peri_pll_clk);
}

static void set_sensor_cis_sclk(unsigned int cis_sclk)
{
	unsigned long regval;
	unsigned int cis_sclk_div;
	unsigned int peri_pll = ak_get_peri_pll_clk() / 1000000;

	cis_sclk_div = peri_pll / cis_sclk - 1;

	regval = REG32(CLOCK_PERI_PLL_CTRL2);
	regval &= ~(0x3f << 10);
	regval |= (cis_sclk_div << 10);
	REG32(CLOCK_PERI_PLL_CTRL2) = (1 << 19) | regval;
}

static void set_pclk_polar(int is_rising)
{
	unsigned long regval;

	regval = REG32(CLOCK_PERI_PLL_CTRL2);
	if (is_rising)
		regval |= (0x3 << 30);
	else
		regval &= ~(0x3 << 30);
	REG32(CLOCK_PERI_PLL_CTRL2) = regval;
}

static int set_sensor_interface(struct ak_cam_dev *dev, int sensor_interface)
{
	switch (sensor_interface) {
	case DVP_INTERFACE:
		return 0;
	case MIPI_INTERFACE:
		v4l2_err(&dev->v4l2_dev,
			 "MIPI sensor selected but platform support is absent\n");
		return -EOPNOTSUPP;
	default:
		v4l2_err(&dev->v4l2_dev, "unsupported sensor interface %d\n",
			 sensor_interface);
		return -EINVAL;
	}
}

static int ak_cam_power_on(struct ak_cam_dev *dev)
{
	int sensor_interface = DVP_INTERFACE;
	int sensor_io_level = SENSOR_IO_LEVEL_1V8;
	enum sensor_bus_type sensor_bus_type = BUS_TYPE_RAW;
	unsigned long sensor_mclk = dev->mclk;
	AK_ISP_SENSOR_CB *sensor_cb;
	int ret;

	v4l2_subdev_call(dev->sensor, core, s_power, 1);
	v4l2_subdev_call(dev->sensor, core, init, 0);

	sensor_cb = ak_sensor_get_sensor_cb();
	if (sensor_cb) {
		if (sensor_cb->sensor_get_mclk_func) {
			unsigned long callback_mclk;

			callback_mclk = sensor_cb->sensor_get_mclk_func();
			if (callback_mclk > 0)
				sensor_mclk = callback_mclk;
		}
		if (sensor_cb->sensor_get_bus_type_func)
			sensor_bus_type = sensor_cb->sensor_get_bus_type_func();
		if (sensor_cb->sensor_get_parameter_func) {
			sensor_cb->sensor_get_parameter_func(GET_INTERFACE,
							     &sensor_interface);
			sensor_cb->sensor_get_parameter_func(GET_SENSOR_IO_LEVEL,
							     &sensor_io_level);
		}
	}
	dev_dbg(dev->dev,
		"sensor bus %d, interface %d, IO level %d, mclk %luMHz\n",
		sensor_bus_type, sensor_interface, sensor_io_level, sensor_mclk);

	clk_prepare_enable(dev->cis_sclk);
	set_sensor_cis_sclk(sensor_mclk);

	clk_prepare_enable(dev->clk);
	REG32(CLOCK_PERI_PLL_CTRL1) &= ~(0x01 << 25);

	ret = set_sensor_interface(dev, sensor_interface);
	if (ret) {
		clk_disable_unprepare(dev->clk);
		clk_disable_unprepare(dev->cis_sclk);
		v4l2_subdev_call(dev->sensor, core, reset, 0);
		v4l2_subdev_call(dev->sensor, core, s_power, 0);
		return ret;
	}

	if (sensor_bus_type == BUS_TYPE_YUV) {
		dev->cur_mode_class = AK_CAM_MBUS_FMT_YUYV8_2X8;
		dev->cur_mode = ISP_YUV_VIDEO_OUT;
	} else {
		dev->cur_mode_class = AK_CAM_MBUS_FMT_SBGGR8_1X8;
		dev->cur_mode = ISP_RGB_VIDEO_OUT;
	}

	return 0;
}

static void ak_cam_power_off(struct ak_cam_dev *dev)
{
	_tdnr_set = 0;
	_tdnr_flag = 0;

	ak_cam_stop_capture(dev);
	v4l2_subdev_call(dev->sensor, core, reset, 0);
	v4l2_subdev_call(dev->sensor, core, s_power, 0);

	mdelay(500);

	clk_disable_unprepare(dev->clk);
	clk_disable_unprepare(dev->cis_sclk);

	mutex_lock(&dev->stream_lock);
	dev->stream_ctrl_off = 0;
	mutex_unlock(&dev->stream_lock);
}

/* -------------------------------------------------------------------------
 * videobuf2
 * ------------------------------------------------------------------------- */

static int ak_cam_queue_setup(struct vb2_queue *vq,
			      AK_CAM_QUEUE_SETUP_ARG *fmt,
			      unsigned int *nbuffers, unsigned int *nplanes,
			      unsigned int sizes[], void *alloc_ctxs[])
{
	struct ak_cam_node *node = vb2_get_drv_priv(vq);

	/* dma_alloc_from_coherent() rounds each buffer up to a whole order. */
	if (((size_t)AK_CAM_SLOTS << (get_order(node->sizeimage) + PAGE_SHIFT)) >
	    node->dev->pool_bytes)
		return -ENOMEM;

	*nbuffers = AK_CAM_SLOTS;
	*nplanes = 1;
	sizes[0] = node->sizeimage;
	alloc_ctxs[0] = node->dev->alloc_ctx;

	return 0;
}

static int ak_cam_buf_prepare(struct vb2_buffer *vb)
{
	struct ak_cam_node *node = vb2_get_drv_priv(vb->vb2_queue);

	if (ak_cam_vb_index(vb) >= AK_CAM_SLOTS)
		return -EINVAL;
	if (vb2_plane_size(vb, 0) < node->sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, node->sizeimage);
	ak_cam_vb_set_field(vb, V4L2_FIELD_NONE);

	return 0;
}

static void ak_cam_buf_queue(struct vb2_buffer *vb)
{
	struct ak_cam_node *node = vb2_get_drv_priv(vb->vb2_queue);
	struct ak_cam_dev *dev = node->dev;
	struct ak_cam_buffer *buf = to_ak_cam_buffer(vb);
	unsigned int slot = ak_cam_vb_index(vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	list_add_tail(&buf->list, &node->capture);
	node->armed[slot] = buf;
	ak_cam_arm_slot(dev, slot);
	spin_unlock_irqrestore(&dev->lock, flags);
}

static void ak_cam_return_buffers(struct ak_cam_node *node,
				  enum vb2_buffer_state state)
{
	struct ak_cam_dev *dev = node->dev;
	struct ak_cam_buffer *buf;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&dev->lock, flags);
	for (i = 0; i < AK_CAM_SLOTS; i++)
		node->armed[i] = NULL;
	while (!list_empty(&node->capture)) {
		buf = list_entry(node->capture.next, struct ak_cam_buffer, list);
		list_del_init(&buf->list);
		vb2_buffer_done(ak_cam_buf_vb(buf), state);
	}
	spin_unlock_irqrestore(&dev->lock, flags);
}

static int ak_cam_alloc_scratch(struct ak_cam_dev *dev)
{
	struct ak_cam_node *sub = &dev->node[AK_CAM_CHAN_SUB];
	size_t size = sub->sizeimage;

	if (dev->scratch_cpu && dev->scratch_size >= size)
		return 0;

	if (dev->scratch_cpu)
		dma_free_coherent(dev->dev, dev->scratch_size, dev->scratch_cpu,
				  dev->scratch_dma);

	dev->scratch_cpu = dma_alloc_coherent(dev->dev, size, &dev->scratch_dma,
					      GFP_KERNEL);
	if (!dev->scratch_cpu) {
		dev->scratch_size = 0;
		return -ENOMEM;
	}
	dev->scratch_size = size;

	return 0;
}

static void ak_cam_free_scratch(struct ak_cam_dev *dev)
{
	if (!dev->scratch_cpu)
		return;

	dma_free_coherent(dev->dev, dev->scratch_size, dev->scratch_cpu,
			  dev->scratch_dma);
	dev->scratch_cpu = NULL;
	dev->scratch_size = 0;
}

static int ak_cam_start_main(struct ak_cam_dev *dev)
{
	struct ak_cam_node *sub = &dev->node[AK_CAM_CHAN_SUB];
	int pclk_polar;
	int ret;

	ret = ak_cam_alloc_scratch(dev);
	if (ret)
		return ret;

	if (!sub->streaming) {
		ret = ispdrv_vo_set_sub_channel_scale(sub->width, sub->height);
		if (ret)
			return ret;
	}

	pclk_polar = ispdrv_get_pclk_polar();
	switch (pclk_polar) {
	case POLAR_RISING:
		set_pclk_polar(1);
		break;
	case POLAR_FALLING:
		set_pclk_polar(0);
		break;
	default:
		v4l2_err(&dev->v4l2_dev, "pclk polar wrong: %d\n", pclk_polar);
		return -EINVAL;
	}

	return v4l2_subdev_call(dev->sensor, video, s_stream, 1);
}

static int ak_cam_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct ak_cam_node *node = vb2_get_drv_priv(vq);
	struct ak_cam_dev *dev = node->dev;
	unsigned long flags;
	int ret;

	if (!ispdrv_is_ready()) {
		v4l2_err(&dev->v4l2_dev,
			 "ISP core is down - open /dev/isp_char first\n");
		return -ENODEV;
	}

	if (count < AK_CAM_SLOTS) {
		v4l2_err(&dev->v4l2_dev, "%u buffers queued, %u needed\n",
			 count, AK_CAM_SLOTS);
		return -EINVAL;
	}

	if (node->chan == AK_CAM_CHAN_MAIN) {
		ret = ak_cam_start_main(dev);
		if (ret)
			return ret;
	} else if (!dev->node[AK_CAM_CHAN_MAIN].streaming) {
		ret = ispdrv_vo_set_sub_channel_scale(node->width, node->height);
		if (ret)
			return ret;
	}

	node->sequence = 0;

	spin_lock_irqsave(&dev->lock, flags);
	node->streaming = 1;
	ak_cam_arm_all(dev);
	spin_unlock_irqrestore(&dev->lock, flags);

	return 0;
}

static AK_CAM_STOP_STREAMING_RET ak_cam_stop_streaming(struct vb2_queue *vq)
{
	struct ak_cam_node *node = vb2_get_drv_priv(vq);
	struct ak_cam_dev *dev = node->dev;
	unsigned long flags;

	if (node->chan == AK_CAM_CHAN_MAIN) {
		ak_cam_stop_capture(dev);
		v4l2_subdev_call(dev->sensor, video, s_stream, 0);
	}

	spin_lock_irqsave(&dev->lock, flags);
	node->streaming = 0;
	if (node->chan == AK_CAM_CHAN_MAIN) {
		dev->list_state = LIST_ZERO;
		dev->slot_armed = 0;
	} else {
		dev->slot_sub = 0;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	ak_cam_return_buffers(node, VB2_BUF_STATE_ERROR);

	ak_cam_stop_streaming_done();
}

static void ak_cam_wait_prepare(struct vb2_queue *vq)
{
	struct ak_cam_node *node = vb2_get_drv_priv(vq);

	mutex_unlock(&node->dev->mutex);
}

static void ak_cam_wait_finish(struct vb2_queue *vq)
{
	struct ak_cam_node *node = vb2_get_drv_priv(vq);

	mutex_lock(&node->dev->mutex);
}

static struct vb2_ops ak_cam_qops = {
	.queue_setup	 = ak_cam_queue_setup,
	.buf_prepare	 = ak_cam_buf_prepare,
	.buf_queue	 = ak_cam_buf_queue,
	.start_streaming = ak_cam_start_streaming,
	.stop_streaming	 = ak_cam_stop_streaming,
	.wait_prepare	 = ak_cam_wait_prepare,
	.wait_finish	 = ak_cam_wait_finish,
};

/* -------------------------------------------------------------------------
 * ioctls
 * ------------------------------------------------------------------------- */

static int ak_cam_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct ak_cam_node *node = ak_cam_file_node(file);

	strlcpy(cap->driver, AK_CAM_NAME, sizeof(cap->driver));
	strlcpy(cap->card, ak_cam_card, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s.%u",
		 AK_CAM_NAME, node->chan);
	cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;

	return 0;
}

/*
 * The ISP writes I420: a full Y plane, then U at width*height and V at
 * width*height*5/4, three addresses per channel per slot.
 */
static int ak_cam_enum_fmt(struct file *file, void *priv,
			   struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_YUV420;
	strlcpy(f->description, "YUV 4:2:0 planar", sizeof(f->description));

	return 0;
}

static void ak_cam_fill_format(struct ak_cam_node *node,
			       struct v4l2_pix_format *pix)
{
	pix->width = node->width;
	pix->height = node->height;
	pix->pixelformat = V4L2_PIX_FMT_YUV420;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = node->bytesperline;
	pix->sizeimage = node->sizeimage;
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
}

static int ak_cam_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	ak_cam_fill_format(ak_cam_file_node(file), &f->fmt.pix);

	return 0;
}

static int ak_cam_bound_format(struct ak_cam_node *node,
			       struct v4l2_pix_format *pix)
{
	struct ak_cam_dev *dev = node->dev;
	struct v4l2_cropcap cropcap;
	int ret;

	ret = v4l2_subdev_call(dev->sensor, video, cropcap, &cropcap);
	if (ret)
		return ret;

	if (pix->width < AK_CAM_MIN_WIDTH)
		pix->width = AK_CAM_MIN_WIDTH;
	if (pix->height < AK_CAM_MIN_HEIGHT)
		pix->height = AK_CAM_MIN_HEIGHT;
	if (pix->width > cropcap.bounds.width)
		pix->width = cropcap.bounds.width;
	if (pix->height > cropcap.bounds.height)
		pix->height = cropcap.bounds.height;

	pix->width &= ~1u;
	pix->height &= ~1u;

	pix->pixelformat = V4L2_PIX_FMT_YUV420;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = pix->width;
	pix->sizeimage = ak_cam_frame_bytes(pix->width, pix->height);
	if (node->chan == AK_CAM_CHAN_SUB)
		pix->sizeimage += AK_CAM_MD_INFO_BYTES;
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int ak_cam_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	return ak_cam_bound_format(ak_cam_file_node(file), &f->fmt.pix);
}

static int ak_cam_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ak_cam_node *node = ak_cam_file_node(file);
	struct ak_cam_dev *dev = node->dev;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	int ret;

	if (vb2_is_busy(&node->vq))
		return -EBUSY;

	if (node->chan == AK_CAM_CHAN_SUB && !node->streaming &&
	    dev->node[AK_CAM_CHAN_MAIN].streaming)
		return -EBUSY;

	if (!ispdrv_is_ready()) {
		v4l2_err(&dev->v4l2_dev,
			 "ISP core is down - open /dev/isp_char first\n");
		return -ENODEV;
	}

	ret = ak_cam_bound_format(node, pix);
	if (ret)
		return ret;

	if (node->chan == AK_CAM_CHAN_MAIN)
		ret = ispdrv_vo_set_main_channel_scale(pix->width, pix->height);
	else
		ret = ispdrv_vo_set_sub_channel_scale(pix->width, pix->height);
	if (ret)
		return ret;

	node->width = pix->width;
	node->height = pix->height;
	node->bytesperline = pix->bytesperline;
	node->sizeimage = pix->sizeimage;

	return 0;
}

static int ak_cam_cropcap(struct file *file, void *priv,
			  struct v4l2_cropcap *crop)
{
	struct ak_cam_node *node = ak_cam_file_node(file);

	if (crop->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return v4l2_subdev_call(node->dev->sensor, video, cropcap, crop);
}

static int ak_cam_g_crop(struct file *file, void *priv, struct v4l2_crop *crop)
{
	struct ak_cam_node *node = ak_cam_file_node(file);

	return v4l2_subdev_call(node->dev->sensor, video, g_crop, crop);
}

/*
 * The ISP input window. Without it the VI never completes a frame, so it is
 * driven from the main node only and the sub channel scales what it produces.
 */
static int ak_cam_s_crop(struct file *file, void *priv,
			 AK_CAM_S_CROP_ARG *crop)
{
	struct ak_cam_node *node = ak_cam_file_node(file);
	struct ak_cam_dev *dev = node->dev;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop local_crop;
	struct v4l2_rect off_rect;
	int total_left, total_top;
	int ret;
	AK_ISP_SENSOR_CB *sensor_cb;

	if (node->chan != AK_CAM_CHAN_MAIN)
		return -ENOTTY;

	sensor_cb = ak_sensor_get_sensor_cb();
	if (!sensor_cb)
		return -ENODEV;

	sensor_cb->sensor_get_valid_coordinate_func(&off_rect.left,
						    &off_rect.top);

	ret = v4l2_subdev_call(dev->sensor, video, cropcap, &cropcap);
	if (ret)
		return -EINVAL;

	if ((cropcap.bounds.width - crop->c.left < crop->c.width) ||
	    (cropcap.bounds.height - crop->c.top < crop->c.height)) {
		v4l2_err(&dev->v4l2_dev, "error crop values\n");
		return -EINVAL;
	}

	total_left = off_rect.left + crop->c.left;
	total_top = off_rect.top + crop->c.top;

	ret = ispdrv_vi_set_crop(total_left, total_top, crop->c.width,
				 crop->c.height);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "error set isp crop\n");
		return -EINVAL;
	}

	local_crop.c.left = total_left;
	local_crop.c.top = total_top;
	local_crop.c.width = crop->c.width;
	local_crop.c.height = crop->c.height;

	return v4l2_subdev_call(dev->sensor, video, s_crop, &local_crop);
}

static int ak_cam_g_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *parm)
{
	struct ak_cam_node *node = ak_cam_file_node(file);

	return v4l2_subdev_call(node->dev->sensor, video, g_parm, parm);
}

static int ak_cam_s_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *parm)
{
	struct ak_cam_node *node = ak_cam_file_node(file);

	if (node->chan != AK_CAM_CHAN_MAIN)
		return -ENOTTY;

	return v4l2_subdev_call(node->dev->sensor, video, s_parm, parm);
}

static int ak_cam_enum_input(struct file *file, void *priv,
			     struct v4l2_input *inp)
{
	if (inp->index)
		return -EINVAL;

	inp->type = V4L2_INPUT_TYPE_CAMERA;
	strlcpy(inp->name, "Camera", sizeof(inp->name));

	return 0;
}

static int ak_cam_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;

	return 0;
}

static int ak_cam_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static int ak_cam_reqbufs(struct file *file, void *priv,
			  struct v4l2_requestbuffers *p)
{
	return vb2_reqbufs(&ak_cam_file_node(file)->vq, p);
}

static int ak_cam_querybuf(struct file *file, void *priv,
			   struct v4l2_buffer *p)
{
	return vb2_querybuf(&ak_cam_file_node(file)->vq, p);
}

static int ak_cam_qbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	return vb2_qbuf(&ak_cam_file_node(file)->vq, p);
}

static int ak_cam_dqbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	return vb2_dqbuf(&ak_cam_file_node(file)->vq, p,
			 file->f_flags & O_NONBLOCK);
}

static int ak_cam_prepare_buf(struct file *file, void *priv,
			      struct v4l2_buffer *p)
{
	return vb2_prepare_buf(&ak_cam_file_node(file)->vq, p);
}

static int ak_cam_streamon(struct file *file, void *priv,
			   enum v4l2_buf_type type)
{
	return vb2_streamon(&ak_cam_file_node(file)->vq, type);
}

static int ak_cam_streamoff(struct file *file, void *priv,
			    enum v4l2_buf_type type)
{
	return vb2_streamoff(&ak_cam_file_node(file)->vq, type);
}

static const struct v4l2_ioctl_ops ak_cam_ioctl_ops = {
	.vidioc_querycap		= ak_cam_querycap,

	.vidioc_enum_fmt_vid_cap	= ak_cam_enum_fmt,
	.vidioc_g_fmt_vid_cap		= ak_cam_g_fmt,
	.vidioc_try_fmt_vid_cap		= ak_cam_try_fmt,
	.vidioc_s_fmt_vid_cap		= ak_cam_s_fmt,

	.vidioc_cropcap			= ak_cam_cropcap,
	.vidioc_g_crop			= ak_cam_g_crop,
	.vidioc_s_crop			= ak_cam_s_crop,

	.vidioc_g_parm			= ak_cam_g_parm,
	.vidioc_s_parm			= ak_cam_s_parm,

	.vidioc_enum_input		= ak_cam_enum_input,
	.vidioc_g_input			= ak_cam_g_input,
	.vidioc_s_input			= ak_cam_s_input,

	.vidioc_reqbufs			= ak_cam_reqbufs,
	.vidioc_querybuf		= ak_cam_querybuf,
	.vidioc_qbuf			= ak_cam_qbuf,
	.vidioc_dqbuf			= ak_cam_dqbuf,
	.vidioc_prepare_buf		= ak_cam_prepare_buf,

	.vidioc_streamon		= ak_cam_streamon,
	.vidioc_streamoff		= ak_cam_streamoff,
};

/* -------------------------------------------------------------------------
 * file operations
 * ------------------------------------------------------------------------- */

static int ak_cam_open(struct file *file)
{
	struct ak_cam_node *node = video_drvdata(file);
	struct ak_cam_dev *dev = node->dev;
	int ret = 0;

	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;

	if (node->users) {
		ret = -EBUSY;
		goto out;
	}

	if (dev->users == 0) {
		ret = ak_cam_power_on(dev);
		if (ret)
			goto out;
	}
	dev->users++;
	node->users++;

out:
	mutex_unlock(&dev->mutex);
	return ret;
}

static int ak_cam_release(struct file *file)
{
	struct ak_cam_node *node = video_drvdata(file);
	struct ak_cam_dev *dev = node->dev;

	mutex_lock(&dev->mutex);

	vb2_queue_release(&node->vq);
	node->users--;

	if (--dev->users == 0) {
		ak_cam_power_off(dev);
		ak_cam_free_scratch(dev);
	}

	mutex_unlock(&dev->mutex);

	return 0;
}

static unsigned int ak_cam_poll(struct file *file, poll_table *wait)
{
	struct ak_cam_node *node = video_drvdata(file);

	return vb2_poll(&node->vq, file, wait);
}

static int ak_cam_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ak_cam_node *node = video_drvdata(file);
	struct ak_cam_dev *dev = node->dev;
	int ret;

	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;
	ret = vb2_mmap(&node->vq, vma);
	mutex_unlock(&dev->mutex);

	return ret;
}

static const struct v4l2_file_operations ak_cam_fops = {
	.owner		= THIS_MODULE,
	.open		= ak_cam_open,
	.release	= ak_cam_release,
	.poll		= ak_cam_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= ak_cam_mmap,
};

static struct video_device ak_cam_videodev = {
	.name		= AK_CAM_NAME,
	.fops		= &ak_cam_fops,
	.ioctl_ops	= &ak_cam_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
};

/* -------------------------------------------------------------------------
 * platform driver
 * ------------------------------------------------------------------------- */

static int ak_cam_init_node(struct ak_cam_dev *dev, unsigned int chan)
{
	struct ak_cam_node *node = &dev->node[chan];
	struct video_device *vfd;
	struct vb2_queue *vq;
	int ret;

	node->dev = dev;
	node->chan = chan;
	INIT_LIST_HEAD(&node->capture);

	if (chan == AK_CAM_CHAN_MAIN) {
		node->width = AK_CAM_DEF_WIDTH;
		node->height = AK_CAM_DEF_HEIGHT;
		node->sizeimage = ak_cam_frame_bytes(node->width, node->height);
	} else {
		node->width = AK_CAM_SUB_DEF_WIDTH;
		node->height = AK_CAM_SUB_DEF_HEIGHT;
		node->sizeimage = ak_cam_frame_bytes(node->width, node->height) +
				  AK_CAM_MD_INFO_BYTES;
	}
	node->bytesperline = node->width;

	vq = &node->vq;
	vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vq->io_modes = VB2_MMAP | VB2_USERPTR;
	vq->drv_priv = node;
	vq->buf_struct_size = sizeof(struct ak_cam_buffer);
	vq->ops = &ak_cam_qops;
	vq->mem_ops = &vb2_dma_contig_memops;

	ret = vb2_queue_init(vq);
	if (ret)
		return ret;

	vfd = video_device_alloc();
	if (!vfd)
		return -ENOMEM;

	*vfd = ak_cam_videodev;
	snprintf(vfd->name, sizeof(vfd->name), "%s-%s", AK_CAM_NAME,
		 chan == AK_CAM_CHAN_MAIN ? "main" : "sub");
	vfd->v4l2_dev = &dev->v4l2_dev;
	vfd->lock = &dev->mutex;
	vfd->ctrl_handler = dev->sensor->ctrl_handler;
	node->vdev = vfd;
	video_set_drvdata(vfd, node);

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, -1);
	if (ret) {
		video_device_release(vfd);
		node->vdev = NULL;
		return ret;
	}

	return 0;
}

static void ak_cam_free_node(struct ak_cam_dev *dev, unsigned int chan)
{
	struct ak_cam_node *node = &dev->node[chan];

	if (!node->vdev)
		return;

	video_unregister_device(node->vdev);
	node->vdev = NULL;
}

static int ak_cam_init_sensor(struct ak_cam_dev *dev)
{
	struct device_node *ep, *remote;
	struct i2c_adapter *adapter;
	struct i2c_board_info info;
	u32 addr;

	ep = of_graph_get_next_endpoint(dev->dev->of_node, NULL);
	if (!ep)
		return -ENODEV;

	remote = of_graph_get_remote_port_parent(ep);
	of_node_put(ep);
	if (!remote)
		return -ENODEV;

	if (of_property_read_u32(remote, "reg", &addr)) {
		of_node_put(remote);
		return -ENODEV;
	}

	adapter = of_get_i2c_adapter_by_node(remote->parent);
	if (!adapter) {
		of_node_put(remote);
		return -EPROBE_DEFER;
	}

	memset(&info, 0, sizeof(info));
	strlcpy(info.type, AK_CAM_SENSOR_NAME, sizeof(info.type));
	info.addr = addr;
	info.of_node = remote;

	dev->sensor = v4l2_i2c_new_subdev_board(&dev->v4l2_dev, adapter,
						&info, NULL);
	i2c_put_adapter(adapter);
	of_node_put(remote);
	if (!dev->sensor)
		return -ENODEV;

	return 0;
}

static int ak_cam_claim_pool(struct platform_device *pdev, size_t *bytes)
{
	struct device_node *np;
	struct resource res;
	int ret;

	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!np) {
		dev_err(&pdev->dev, "no memory-region for the capture pool\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret) {
		dev_err(&pdev->dev, "capture pool has no usable reg\n");
		return ret;
	}

	ret = of_reserved_mem_device_init(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "cannot claim the capture pool: %d\n", ret);
		return ret;
	}

	*bytes = resource_size(&res);
	dev_info(&pdev->dev, "capture pool at %pa, %zu KiB\n",
		 &res.start, *bytes / 1024);

	return 0;
}

static int ak_cam_probe(struct platform_device *pdev)
{
	struct ak_cam_dev *dev;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;
	spin_lock_init(&dev->lock);
	mutex_init(&dev->mutex);
	mutex_init(&dev->stream_lock);
	INIT_DELAYED_WORK(&dev->awb_work, ak_cam_awb_work);
	INIT_DELAYED_WORK(&dev->ae_work, ak_cam_ae_work);
	INIT_WORK(&dev->resume_work, ak_cam_resume_work);
	dev->list_state = LIST_ZERO;
	dev->cur_buf_id = -1;
	dev->cur_mode = ISP_RGB_VIDEO_OUT;
	dev->cur_mode_class = AK_CAM_MBUS_FMT_SBGGR8_1X8;

	of_property_read_u32(pdev->dev.of_node, "mclk-mhz", (u32 *)&dev->mclk);
	if (dev->mclk == 0) {
		dev_warn(&pdev->dev, "mclk-mhz absent or 0; using 24MHz\n");
		dev->mclk = 24;
	}

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		goto err_free;
	dev->irq = ret;

	ret = ak_cam_claim_pool(pdev, &dev->pool_bytes);
	if (ret)
		goto err_free;

	dev->clk = clk_get(&pdev->dev, "camera");
	if (IS_ERR(dev->clk)) {
		ret = PTR_ERR(dev->clk);
		goto err_pool;
	}

	dev->cis_sclk = clk_get(&pdev->dev, "sensor");
	if (IS_ERR(dev->cis_sclk)) {
		ret = PTR_ERR(dev->cis_sclk);
		goto err_put_clk;
	}

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		goto err_put_cis_sclk;

	dev->alloc_ctx = vb2_dma_contig_init_ctx(&pdev->dev);
	if (IS_ERR(dev->alloc_ctx)) {
		ret = PTR_ERR(dev->alloc_ctx);
		goto err_v4l2;
	}

	ret = ak_cam_init_sensor(dev);
	if (ret)
		goto err_ctx;

	ret = request_irq(dev->irq, ak_cam_irq, 0, AK_CAM_NAME, dev);
	if (ret)
		goto err_ctx;

	ret = ak_cam_init_node(dev, AK_CAM_CHAN_MAIN);
	if (ret)
		goto err_irq;

	ret = ak_cam_init_node(dev, AK_CAM_CHAN_SUB);
	if (ret)
		goto err_main;

	platform_set_drvdata(pdev, dev);

	v4l2_info(&dev->v4l2_dev,
		  "capture bridge: main /dev/video%d, sub /dev/video%d, irq %d\n",
		  dev->node[AK_CAM_CHAN_MAIN].vdev->num,
		  dev->node[AK_CAM_CHAN_SUB].vdev->num, dev->irq);

	return 0;

err_main:
	ak_cam_free_node(dev, AK_CAM_CHAN_MAIN);
err_irq:
	free_irq(dev->irq, dev);
err_ctx:
	vb2_dma_contig_cleanup_ctx(dev->alloc_ctx);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
err_put_cis_sclk:
	clk_put(dev->cis_sclk);
err_put_clk:
	clk_put(dev->clk);
err_pool:
	of_reserved_mem_device_release(&pdev->dev);
err_free:
	kfree(dev);
	return ret;
}

static int ak_cam_remove(struct platform_device *pdev)
{
	struct ak_cam_dev *dev = platform_get_drvdata(pdev);

	ak_cam_free_node(dev, AK_CAM_CHAN_SUB);
	ak_cam_free_node(dev, AK_CAM_CHAN_MAIN);

	free_irq(dev->irq, dev);
	cancel_work_sync(&dev->resume_work);
	cancel_delayed_work_sync(&dev->awb_work);
	cancel_delayed_work_sync(&dev->ae_work);

	ak_cam_free_scratch(dev);
	vb2_dma_contig_cleanup_ctx(dev->alloc_ctx);
	v4l2_device_unregister(&dev->v4l2_dev);

	clk_put(dev->cis_sclk);
	clk_put(dev->clk);
	kfree(dev);

	of_reserved_mem_device_release(&pdev->dev);

	return 0;
}

static const struct of_device_id ak_cam_of_match[] = {
	{ .compatible = "anyka,ak39ev330-isp" },
	{ }
};
MODULE_DEVICE_TABLE(of, ak_cam_of_match);

static struct platform_driver ak_cam_driver = {
	.probe	= ak_cam_probe,
	.remove	= ak_cam_remove,
	.driver	= {
		.name		= AK_CAM_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= ak_cam_of_match,
	},
};

module_platform_driver(ak_cam_driver);

MODULE_DESCRIPTION("Anyka AK3918EV200 V4L2 capture bridge");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" AK_CAM_NAME);
