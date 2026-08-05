/*
  * @file ak camera.c
  * @camera host driver for ak
  * @Copyright (C) 2010 Anyka (Guangzhou) Microelectronics Technology Co
  * @author wu_daochao
  * @date 2011-04
  * @version
  * @for more information , please refer to AK980x Programmer's Guide Mannul
  */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/sched.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>

#include <asm/io.h>
#include <asm/cacheflush.h>

#include <media/soc_camera.h>
#include <media/videobuf-core.h>
#include <media/videobuf-dma-contig.h>
#include <media/soc_mediabus.h>

#include <mach/gpio.h>
#include <mach/clock.h>
#include <mach/reset.h>
#include <plat-anyka/ak_camera.h>
#include <plat-anyka/ak_sensor.h>
#include <mach-anyka/ispdrv_interface.h>

//#define CAMIF_DEBUG
#ifdef CAMIF_DEBUG
#define isp_dbg(fmt...)			printk(KERN_INFO " ISP: " fmt)
#define CAMDBG(fmt...)	printk(KERN_INFO " ISP: " fmt)//do{}while(0)
#else
#define CAMDBG(fmt...)	do{}while(0)
#define isp_dbg(fmt, args...)	do{}while(0)
#endif

enum buffer_list_state {
	LIST_ZERO = 1,
	LIST_ONE,
	LIST_TWO,
	LIST_THREE,
	LIST_FOUR,
	LIST_FIVE,
};

struct ak_buffer {
	struct videobuf_buffer vb;
	enum v4l2_mbus_pixelcode	code;
	int				inwork;
};

struct ak_camera_dev {
	struct soc_camera_host soc_host;
	struct soc_camera_device *icd;
	struct ak_camera_pdata		*pdata;

	struct clk	*clk;		// camera controller clk. it's parent is vclk defined in clock.c
	struct clk	*cis_sclk;		// cis_sclk clock for sensor
	unsigned long	mclk;
	unsigned int	irq;
	struct list_head capture;
	/* members to manage the dma and buffer*/
	spinlock_t		lock;  /* for videobuf_queue , passed in init_videobuf */

	enum isp_working_mode def_mode;
	enum isp_working_mode cur_mode;
	enum v4l2_mbus_pixelcode cur_mode_class;

	struct videobuf_queue  *vq;
	enum buffer_list_state list_state;
	enum buffer_list_state free_list;

	struct delayed_work awb_work;
	struct delayed_work ae_work;
	struct work_struct resume_work;
	struct mutex stream_lock;

	int stream_ctrl_off;
	int stream_active;
	int cur_buf_id;


};

struct ak_camera_cam {
	/* Client output, as seen by the CEU */
	unsigned int width;
	unsigned int height;
};

#define	ISP_TIMEOUT				(1)	//unit: s
#define EMPTY_FRAME_NUM			(2)


static const char *ak_cam_driver_description = "AK_Camera";

static int _tdnr_flag = 0;
static int _tdnr_set = 0;
static int video_frame_interval;

static unsigned long in_irq_jf = 0;
static unsigned long start_set_td_jf;

AK_ISP_SENSOR_CB *ak_sensor_get_sensor_cb(void);

static void ak_camera_resume_work(struct work_struct *work)
{
	struct ak_camera_dev *pcdev = container_of(work,
					struct ak_camera_dev, resume_work);
	unsigned long flags;

	mutex_lock(&pcdev->stream_lock);
	if (pcdev->stream_ctrl_off && pcdev->icd &&
	    pcdev->list_state >= LIST_FOUR) {
		ispdrv_set_isp_resume();
		pcdev->stream_ctrl_off = 0;
		spin_lock_irqsave(&pcdev->lock, flags);
		pcdev->stream_active = 1;
		spin_unlock_irqrestore(&pcdev->lock, flags);
	}
	mutex_unlock(&pcdev->stream_lock);
}

static void ak_camera_stop_streaming(struct ak_camera_dev *pcdev)
{
	unsigned long flags;
	int stream_active;

	/* A queued restart must not race this teardown and re-enable the ISP. */
	cancel_work_sync(&pcdev->resume_work);

	mutex_lock(&pcdev->stream_lock);
	spin_lock_irqsave(&pcdev->lock, flags);
	stream_active = pcdev->stream_active;
	spin_unlock_irqrestore(&pcdev->lock, flags);
	if (stream_active) {
		ispdrv_set_isp_pause();
		ispdrv_vi_stop_capturing();
		spin_lock_irqsave(&pcdev->lock, flags);
		pcdev->stream_active = 0;
		spin_unlock_irqrestore(&pcdev->lock, flags);
		pcdev->stream_ctrl_off = 1;
	}
	ispdrv_vo_clear_irq_status(0xffff);
	synchronize_irq(pcdev->irq);
	cancel_delayed_work_sync(&pcdev->awb_work);
	cancel_delayed_work_sync(&pcdev->ae_work);
	spin_lock_irqsave(&pcdev->lock, flags);
	pcdev->cur_buf_id = -1;
	spin_unlock_irqrestore(&pcdev->lock, flags);
	mutex_unlock(&pcdev->stream_lock);
}

/**
 * @brief:  for ak_videobuf_release, free buffer if camera stopped.
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *vq: V4L2 buffer queue information structure
 * @param [in] *buf: ak camera drivers structure, include struct videobuf_buffer
 */
static void free_buffer(struct videobuf_queue *vq, struct ak_buffer *buf)
{
	unsigned long flags;
	struct soc_camera_device *icd = vq->priv_data;
	struct videobuf_buffer *vb = &buf->vb;
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct ak_camera_dev *pcdev = ici->priv;
	int stopping = vq->streaming == 0 && vq->reading == 0;

	isp_dbg("%s (vb=0x%p) buf[%d] 0x%08lx %d\n",
			__func__, vb, vb->i, vb->baddr, vb->bsize);

	BUG_ON(in_interrupt());

	/* Stop IRQ producers and deferred 3A/MD consumers before DMA is freed. */
	if (stopping)
		ak_camera_stop_streaming(pcdev);

	spin_lock_irqsave(&pcdev->lock, flags);
	if (!list_empty(&vb->queue))
		list_del_init(&vb->queue);
	vb->state = VIDEOBUF_DONE;
	if (stopping) {
		pcdev->list_state = LIST_ZERO;
		pcdev->free_list = LIST_ZERO;
	}
	spin_unlock_irqrestore(&pcdev->lock, flags);

	/* The state transition above makes videobuf_waiton non-blocking. */
	videobuf_waiton(vq, vb, 0, 0);

	videobuf_dma_contig_free(vq, vb);

	vb->state = VIDEOBUF_NEEDS_INIT;
}

/**
 * @brief:  Called when application apply buffers, camera buffer initial.
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *vq: V4L2 buffer queue information structure
 * @param [in] *count: buffer's number
 * @param [in] *size: buffer total size
 */
static int ak_videobuf_setup(struct videobuf_queue *vq, unsigned int *count,
								unsigned int *size)
{
	struct soc_camera_device *icd = vq->priv_data;
	int bytes_per_line = soc_mbus_bytes_per_line(icd->user_width,
						icd->current_fmt->host_fmt);

	bytes_per_line = icd->user_width * 3 /2;
	if (bytes_per_line < 0)
		return bytes_per_line;

	*size = bytes_per_line * icd->user_height;
	//printk(KERN_ERR "%s size:%u, bytes_per_line:%d, icd->user_height:%d\n", __func__, *size, bytes_per_line, icd->user_height);

	/* ISP2 exposes four VO buffer slots and starts only after all are set. */
	if (*size > CONFIG_VIDEO_RESERVED_MEM_SIZE / 4)
		return -ENOMEM;
	*count = 4;

	isp_dbg("%s count=%d, size=%d, bytes_per_line=%d\n",
			__func__, *count, *size, bytes_per_line);

	return 0;
}

/**
 * @brief: Called when application apply buffers, camera buffer initial.
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *vq: V4L2  buffer queue information structure
 * @param [in] *vb: V4L2  buffer information structure
 * @param [in] field: V4L2_FIELD_ANY
 */
static int ak_videobuf_prepare(struct videobuf_queue *vq,
			struct videobuf_buffer *vb, enum v4l2_field field)
{
	struct soc_camera_device *icd = vq->priv_data;
	struct ak_buffer *buf = container_of(vb, struct ak_buffer, vb);
	int ret;
	int bytes_per_line = soc_mbus_bytes_per_line(icd->user_width,
						icd->current_fmt->host_fmt);

	isp_dbg("%s (vb=0x%p) buf[%d] vb->baddr=0x%08lx vb->bsize=%d bytes_per_line=%d\n",
			__func__, vb, vb->i, vb->baddr, vb->bsize, bytes_per_line);

	bytes_per_line = icd->user_width * 3 /2;

	if (bytes_per_line < 0)
		return bytes_per_line;

	/* Added list head initialization on alloc */
	WARN_ON(!list_empty(&vb->queue));

	BUG_ON(NULL == icd->current_fmt);

	/* I think, in buf_prepare you only have to protect global data,
	 * the actual buffer is yours */
	buf->inwork = 1;

	if (buf->code	!= icd->current_fmt->code ||
	    vb->width	!= icd->user_width ||
	    vb->height	!= icd->user_height ||
	    vb->field	!= field) {
		buf->code	= icd->current_fmt->code;
		vb->width	= icd->user_width;
		vb->height	= icd->user_height;
		vb->field	= field;
		vb->state	= VIDEOBUF_NEEDS_INIT;
	}

	//vb->size = bytes_per_line * vb->height;
	vb->size = bytes_per_line * icd->user_height;
	if (0 != vb->baddr && vb->bsize < vb->size) {
		ret = -EINVAL;
		goto out;
	}

	if (vb->state == VIDEOBUF_NEEDS_INIT) {
		ret = videobuf_iolock(vq, vb, NULL);
		if (ret)
			goto fail;

		vb->state = VIDEOBUF_PREPARED;
	}

	buf->inwork = 0;

	return 0;

fail:
	free_buffer(vq, buf);
out:
	buf->inwork = 0;
	return ret;
}


static int queue_single_mode(struct videobuf_buffer *vb, struct ak_camera_dev *pcdev)
{
	unsigned long flags;
	u32 yaddr_chl1, yaddr_chl2, size;
	struct soc_camera_device *icd = pcdev->icd;

	//size = vb->width * vb->height;
	size = icd->user_width * icd->user_height;
	yaddr_chl1 = videobuf_to_dma_contig(vb); /* for mater channel */
	yaddr_chl2 = yaddr_chl1 + size * 3 / 2; /* for secondary channel */

	spin_lock_irqsave(&pcdev->lock, flags);
	vb->state = VIDEOBUF_ACTIVE;
	list_add_tail(&vb->queue, &pcdev->capture);
	spin_unlock_irqrestore(&pcdev->lock, flags);

	switch (pcdev->list_state) {
	case LIST_ZERO:
		ispdrv_vo_set_buffer_addr(BUFFER_ONE, yaddr_chl1, yaddr_chl2);

		ispdrv_vo_enable_buffer(BUFFER_ONE);

		ispdrv_vi_apply_mode(pcdev->cur_mode);
		ispdrv_vi_start_capturing();
		pcdev->stream_active = 1;
		pcdev->stream_ctrl_off = 0;

		pcdev->list_state++;
		break;
	case LIST_ONE:
	case LIST_TWO:
	case LIST_THREE:
	//case LIST_FOUR:
		break;
	default:
		printk("Not defined list stat [single mode].\n");
		break;
	}

	return 0;
}

static int queue_continous_mode(struct videobuf_buffer *vb, struct ak_camera_dev *pcdev)
{
	int i;
	unsigned long flags;
	u32 yaddr_chl1, yaddr_chl2, size;
	struct soc_camera_device *icd = pcdev->icd;

	//size = vb->width * vb->height;
	size = icd->user_width * icd->user_height;
	yaddr_chl1 = videobuf_to_dma_contig(vb); /* for mater channel */
	yaddr_chl2 = yaddr_chl1 + size * 3 / 2; /* for secondary channel */

	isp_dbg("%s vb->i=%d, phyaddr=%x, user_width:%d, user_height:%d\n", __func__, vb->i, yaddr_chl1, icd->user_width, icd->user_height);
	switch (pcdev->list_state) {
	case LIST_FIVE:
		break;
	case LIST_ZERO:
		for (i = 0; i < 4; i++) {
			ispdrv_vo_disable_buffer(BUFFER_ONE + i);
		}

	case LIST_ONE:
	case LIST_TWO:
	case LIST_THREE:
		ispdrv_vo_set_buffer_addr(BUFFER_ONE + vb->i, yaddr_chl1, yaddr_chl2);
	case LIST_FOUR:
		pcdev->list_state++;
		break;
	default:
		printk("Not defined list stat [continous mode].\n");
		break;
	}


	spin_lock_irqsave(&pcdev->lock, flags);
	vb->state = VIDEOBUF_ACTIVE;
	list_add_tail(&vb->queue, &pcdev->capture);

	ispdrv_vo_enable_buffer(BUFFER_ONE + vb->i);
	pcdev->free_list++;
	spin_unlock_irqrestore(&pcdev->lock, flags);

	if (pcdev->list_state == LIST_FOUR) {
		ispdrv_vi_apply_mode(pcdev->cur_mode);
		ispdrv_vo_enable_irq_status(0x1);

		if (pcdev->stream_ctrl_off == 0) {
			//printk(KERN_ERR "%s start capture\n", __func__);
			ispdrv_vi_start_capturing();
			pcdev->stream_active = 1;
		} else {
			/* ispdrv_set_isp_resume() sleeps; buf_queue holds irqlock. */
			schedule_work(&pcdev->resume_work);
		}
	}



	return 0;
}

/**
 * @brief: Called when application apply buffers, camera start data collection
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *vq: V4L2  buffer queue information structure
 * @param [in] *vb: V4L2  buffer information structure
 */
static void ak_videobuf_queue(struct videobuf_queue *vq,
								struct videobuf_buffer *vb)
{
	struct soc_camera_device *icd = vq->priv_data;
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct ak_camera_dev *pcdev = ici->priv;

	isp_dbg("%s (vb=0x%p) buf[%d] baddr = 0x%08lx, bsize = %d\n",
			__func__,  vb, vb->i, vb->baddr, vb->bsize);

	switch(pcdev->cur_mode) {
	case ISP_YUV_OUT:
	case ISP_RGB_OUT:
		/* for single mode */
		queue_single_mode(vb, pcdev);
		break;

	case ISP_YUV_VIDEO_OUT:
	case ISP_RGB_VIDEO_OUT:
		/* for continous mode */
		queue_continous_mode(vb, pcdev);
		pcdev->vq = vq;
		break;

	default:
		printk("The working mode of ISP hasn't been initialized.\n");
		break;
	}
}

/**
 * @brief:  for ak_videobuf_release, free buffer if camera stopped.
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *vq: V4L2 buffer queue information structure
 * @param [in] *vb: V4L2  buffer information structure
 */
static void ak_videobuf_release(struct videobuf_queue *vq,
					struct videobuf_buffer *vb)
{
	struct ak_buffer *buf = container_of(vb, struct ak_buffer, vb);

	isp_dbg("%s (vb=0x%p) buf[%d] 0x%08lx %d\n",
			__func__, vb, vb->i, vb->baddr, vb->bsize);

	switch (vb->state) {
	case VIDEOBUF_ACTIVE:
		CAMDBG("vb status: ACTIVE\n");
		break;
	case VIDEOBUF_QUEUED:
		CAMDBG("vb status: QUEUED\n");
		break;
	case VIDEOBUF_PREPARED:
		CAMDBG("vb status: PREPARED\n");
		break;
	default:
		CAMDBG("vb status: unknown\n");
		break;
	}

	free_buffer(vq, buf);
}

static struct videobuf_queue_ops ak_videobuf_ops = {
	.buf_setup      = ak_videobuf_setup,
	.buf_prepare    = ak_videobuf_prepare,
	.buf_queue      = ak_videobuf_queue,
	.buf_release    = ak_videobuf_release,
};

static inline int list_how_many_entries(struct list_head *head)
{
	int i;
	struct list_head *list = head;

	for (i = 0; list->next != head; i++)
		list = list->next;

	return i;
}

static int delay_works(struct ak_camera_dev *pcdev)
{
	if (pcdev->cur_mode_class >= V4L2_MBUS_FMT_SBGGR8_1X8 &&
			pcdev->cur_mode_class <= V4L2_MBUS_FMT_SRGGB12_1X12) {
		if (1){//(pcdev->rfled_ison == 0) {
			schedule_delayed_work(&pcdev->awb_work, 0);
		}
		schedule_delayed_work(&pcdev->ae_work, 0);
	}

	return 0;
}

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

static int irq_handle_single_mode(struct videobuf_buffer *vb, struct ak_camera_dev *pcdev)
{
	int entries;
	u32 yaddr_chl1, yaddr_chl2, size;
	struct videobuf_buffer *vb_active;
	struct ak_buffer *ak_active;
	unsigned long timestamp_ms;


	timestamp_ms = get_timestamp();
	vb->ts.tv_sec = timestamp_ms / 1000;
	vb->ts.tv_usec = (timestamp_ms % 1000) * 1000;

	list_del_init(&vb->queue);
	vb->state = VIDEOBUF_DONE;
	vb->field_count++;

	entries = list_how_many_entries(&pcdev->capture);
	if (entries >= 1) {
		ak_active = list_entry(pcdev->capture.next,
				   struct ak_buffer, vb.queue);
		vb_active = &ak_active->vb;

		//size = vb_active->width * vb_active->height;
		size = pcdev->icd->user_width * pcdev->icd->user_height;
		yaddr_chl1 = videobuf_to_dma_contig(vb_active); /* for mater channel */
		yaddr_chl2 = yaddr_chl1 + size * 3 / 2; /* for secondary channel */

		ispdrv_vo_set_buffer_addr(BUFFER_ONE, yaddr_chl1, yaddr_chl2);
		ispdrv_vi_start_capturing();
	} else {
		ispdrv_vo_disable_buffer(BUFFER_ONE);
		pcdev->list_state = LIST_ZERO;
	}

	return 0;
}

static unsigned long sjf = 0;
static int irq_handle_continous_mode(struct ak_camera_dev *pcdev)
{
	int video_data_err = 0;
	int id;
	unsigned long timestamp_ms;
	struct videobuf_buffer *vb;
	struct ak_buffer *ak_buf;
	struct list_head *next;
	static struct list_head *save_list;
	unsigned long ul , ul2;
	int fps = 10;
	AK_ISP_SENSOR_CB *sensor_cb;

	sensor_cb = ak_sensor_get_sensor_cb();
	if (sensor_cb && sensor_cb->sensor_get_fps_func)
		fps = sensor_cb->sensor_get_fps_func();
	if (fps <= 0)
		fps = 10;
	//printk(KERN_ERR "fps:%d\n", fps);
	video_frame_interval = 1000 / fps + 10;

	ul = ul2 = jiffies;
	if (sjf == 0)
		sjf = jiffies;

	if (ul >= sjf)
		ul -= sjf;
	else
		ul = (~(unsigned long)0) - sjf+ ul;
	if (jiffies_to_msecs(ul) > video_frame_interval) {
		printk(KERN_ERR ">%d, sjf:%lu, ul:%lu\n", video_frame_interval, sjf, ul);
		_tdnr_flag = 1;
	}
	sjf = ul2;

	id = ispdrv_vo_get_using_frame_buf_id();
	//	printk("HWFrameId=%d\n", id);
//	isp2_print_reg_table();

	next = pcdev->capture.next;
	if (next == &pcdev->capture) {
		return 0;
	}

	ak_buf = list_entry(next, struct ak_buffer, vb.queue);
	vb = &ak_buf->vb;
	if (id == -1)
	{
		if ((vb->field_count > 1) && (save_list != next )) {
			printk("%s %d: vb->i=%d, but id %d\n", __func__,__LINE__, vb->i, id);
			save_list = next;
		}
		return 0;
	} else if ((id & 0x7F) != vb->i) {
		printk("vb->i=%d, but id %d\n", vb->i, id);
		return 0;
	} else if (id >= 0x80) {
		if ((id & 0x7F) != vb->i) {
			printk("~~%s %d: vb->i=%d, but id %d\n", __func__,__LINE__, vb->i, id);
			return 0;
		}

		video_data_err = 1;
	}
	save_list = next;
	next = next->next;

	if (next != &pcdev->capture) {
		ispdrv_vo_disable_buffer(BUFFER_ONE + vb->i);

		if (!video_data_err) {
			timestamp_ms = get_timestamp();
			vb->ts.tv_sec = timestamp_ms / 1000;
			vb->ts.tv_usec = (timestamp_ms % 1000) * 1000;
		} else {
			vb->ts.tv_sec = 0;
			vb->ts.tv_usec = 0;
		}

		list_del_init(&vb->queue);
		vb->state = VIDEOBUF_DONE;
		vb->field_count++;
//		printk("%s vb->i=%d DONE\n", __func__, vb->i);

		pcdev->cur_buf_id = vb->i;
		// here,  current frame commit to video_buffer layer
	 	wake_up(&vb->done);


	} else {
		pcdev->cur_buf_id = -1;
		printk("Warnning, lost frame at %ld\n", jiffies);
	}

	return 0;
}

static irqreturn_t ak_camera_dma_irq(int channel, void *data)
{
	unsigned long stat;
	struct ak_camera_dev *pcdev = data;
	struct ak_buffer *ak_active;
	struct videobuf_buffer *vb;
	unsigned long flags;

	in_irq_jf = jiffies;
	//printk(KERN_ERR "%lu\n",in_irq_jf);

	spin_lock_irqsave(&pcdev->lock, flags);

	if (!((stat = ispdrv_vo_check_irq_status()) & 0x01)) {
		ispdrv_vo_clear_irq_status(0xfffe);
		spin_unlock_irqrestore(&pcdev->lock, flags);
		printk("%s %d stat:0x%lx\n", __func__, __LINE__, stat);
		return IRQ_HANDLED;
//		goto out;
	}

	ispdrv_irq_work();
	if (pcdev->list_state == LIST_ZERO)
	{
		printk("%s: state not handled\n", __func__);
		goto out;
	}

	if (!ispdrv_is_continuous()) { //?? 库没提供函数
		ak_active = list_entry(pcdev->capture.next,
						   struct ak_buffer, vb.queue);
		vb = &ak_active->vb;
		WARN_ON(ak_active->inwork || list_empty(&vb->queue));
		irq_handle_single_mode(vb, pcdev);
	} else {
		irq_handle_continous_mode(pcdev);
	}

	delay_works(pcdev);

out:
	ispdrv_vo_clear_irq_status(0xffff);
	spin_unlock_irqrestore(&pcdev->lock, flags);
//	printk("%s stat:%x\n", __func__, stat);
	return IRQ_HANDLED;
}

static void isp_awb_work(struct work_struct *work)
{
	ispdrv_awb_work();
}

static void isp_ae_work(struct work_struct *work)
{
	int aec_delay_ms = 10;
	unsigned long flags;
	unsigned long cur_jf = jiffies;
	struct ak_camera_dev *pcdev = container_of(work,
					struct ak_camera_dev, ae_work.work);
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

	if (pcdev->cur_buf_id != -1) {
		void *yuv_paddr, *mdinfo;
		void *yuv_vaddr;

		ispdrv_get_yuvaddr_and_mdinfo(pcdev->cur_buf_id,
						&yuv_paddr, &mdinfo);
		yuv_vaddr = ioremap_nocache((unsigned long)yuv_paddr, 1024);
		if (yuv_vaddr) {
			memcpy(yuv_vaddr, mdinfo, 1024);
			iounmap(yuv_vaddr);
		}
		pcdev->cur_buf_id = -1;
	}

	if (_tdnr_flag && !_tdnr_set) {
		ispdrv_set_td();
		spin_lock_irqsave(&pcdev->lock, flags);
		_tdnr_set = 1;
		_tdnr_flag = 0;
		start_set_td_jf = jiffies;
		spin_unlock_irqrestore(&pcdev->lock, flags);
	} else if (_tdnr_set &&
		   jiffies_to_msecs(jiffies - start_set_td_jf) >
		   2 * video_frame_interval) {
		ispdrv_reload_td();
		spin_lock_irqsave(&pcdev->lock, flags);
		_tdnr_set = 0;
		_tdnr_flag = 0;
		spin_unlock_irqrestore(&pcdev->lock, flags);
	}
}


static void set_sensor_cis_sclk(unsigned int cis_sclk)
{
	unsigned long regval;
	unsigned int cis_sclk_div;

	unsigned int peri_pll = ak_get_peri_pll_clk()/1000000;

	cis_sclk_div = peri_pll/cis_sclk - 1;

	regval = REG32(CLOCK_PERI_PLL_CTRL2);
	regval &= ~(0x3f << 10);
	regval |= (cis_sclk_div << 10);
	REG32(CLOCK_PERI_PLL_CTRL2) = (1 << 19)|regval;

	isp_dbg("%s() cis_sclk=%dMHz peri_pll=%dMHz cis_sclk_div=%d\n",
			__func__, cis_sclk, peri_pll, cis_sclk_div);
}

static int set_sensor_interface(struct device *dev, int sensor_interface)
{
	switch (sensor_interface) {
	case DVP_INTERFACE:
		return 0;
	case MIPI_INTERFACE:
		/*
		 * The 1.0.05 platform layer has no MIPI register map, reset or
		 * clock API. Keep the interface decision explicit so adding that
		 * platform support does not disturb DVP sensor selection.
		 */
		dev_err(dev, "MIPI sensor selected but platform support is absent\n");
		return -EOPNOTSUPP;
	default:
		dev_err(dev, "unsupported sensor interface %d\n",
			sensor_interface);
		return -EINVAL;
	}
}

/**
 * @brief: Called when the /dev/videox is opened. initial ISP and sensor device.
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *icd: soc_camera_device information structure,
 * akcamera depends on the soc driver.
 */
static int ak_camera_add_device(struct soc_camera_device *icd)
{
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct ak_camera_dev *pcdev = ici->priv;
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct ak_camera_cam *cam;
	int sensor_interface = DVP_INTERFACE;
	int sensor_io_level = SENSOR_IO_LEVEL_1V8;
	enum sensor_bus_type sensor_bus_type = BUS_TYPE_RAW;
	unsigned long sensor_mclk = pcdev->mclk;
	AK_ISP_SENSOR_CB *sensor_cb;
	int cam_allocated = 0;
	int ret;

	CAMDBG("entry %s\n", __func__);

	/* The host supports one active soc-camera device at a time. */
	if (pcdev->icd)
		return -EBUSY;

	/* A previous close must have drained every driver-owned buffer. */
	if (!list_empty(&pcdev->capture)) {
		dev_err(icd->parent, "capture list is not empty at open\n");
		return -EBUSY;
	}

	cam = icd->host_priv;
	if (!cam) {
		cam = kzalloc(sizeof(*cam), GFP_KERNEL);
		if (!cam)
			return -ENOMEM;
		cam_allocated = 1;
	}

	/********** config sensor module **********/
	v4l2_subdev_call(sd, core, init, 0);

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
	dev_dbg(icd->parent,
		"sensor bus %d, interface %d, IO level %d, mclk %luMHz\n",
		sensor_bus_type, sensor_interface, sensor_io_level, sensor_mclk);

	/* Platform data is the fallback for sensors without a clock callback. */
	clk_enable(pcdev->cis_sclk);
	set_sensor_cis_sclk(sensor_mclk);

	// load the default setting for sensor
//	v4l2_subdev_call(sd, core, load_fw);

	/********** config isp module **********/
	ak_soft_reset(AK_SRESET_CAMERA);

	// enable isp clock
	clk_enable(pcdev->clk);
//	printk("ISP CLOCK ENABLE \n");
	REG32(CLOCK_PERI_PLL_CTRL1) &=~(0x01<<25);

	ret = set_sensor_interface(icd->parent, sensor_interface);
	if (ret) {
		clk_disable(pcdev->clk);
		clk_disable(pcdev->cis_sclk);
		v4l2_subdev_call(sd, core, reset, 0);
		if (cam_allocated)
			kfree(cam);
		return ret;
	}

	if (cam_allocated)
		icd->host_priv = cam;
	pcdev->icd = icd;
	dev_info(icd->parent, "AK Camera driver attached to camera %d\n",
		 icd->devnum);

	return 0;
}

/**
 * @brief: Called when the /dev/videox is close. close ISP and sensor device.
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *icd: soc_camera_device information structure,
 * akcamera depends on the soc driver.
 */
static void ak_camera_remove_device(struct soc_camera_device *icd)
{
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct ak_camera_dev *pcdev = ici->priv;
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	unsigned long flags;

	CAMDBG("entry %s\n", __func__);

	_tdnr_set = 0;
	_tdnr_flag = 0;

	BUG_ON(icd != pcdev->icd);

	ak_camera_stop_streaming(pcdev);
	v4l2_subdev_call(sd, core, reset, 0);

	mdelay(500);

	/* disable the clock of isp module */
	clk_disable(pcdev->clk);

	/* disable sensor clk */
	clk_disable(pcdev->cis_sclk);
	//ak_soft_reset(AK_SRESET_CAMERA);

	dev_info(icd->parent, "AK Camera driver detached from camera %d\n",
		 icd->devnum);

	/*
	 * A process that dies mid-stream leaves its buffers queued here, and
	 * refusing to clean up after it made the next open fail with -EBUSY
	 * for good: one crash cost a reboot. videobuf frees the buffers
	 * themselves; what has to go is this driver's list of them.
	 */
	spin_lock_irqsave(&pcdev->lock, flags);
	while (!list_empty(&pcdev->capture)) {
		struct ak_buffer *buf = list_entry(pcdev->capture.next,
						   struct ak_buffer, vb.queue);

		list_del_init(&buf->vb.queue);
		buf->vb.state = VIDEOBUF_ERROR;
		wake_up(&buf->vb.done);
	}
	pcdev->list_state = LIST_ZERO;
	pcdev->free_list = LIST_ZERO;
	pcdev->icd = NULL;
	spin_unlock_irqrestore(&pcdev->lock, flags);

	mutex_lock(&pcdev->stream_lock);
	pcdev->stream_ctrl_off = 0;
	mutex_unlock(&pcdev->stream_lock);

	CAMDBG("Leave %s\n", __func__);
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

static int ak_camera_cropcap(struct soc_camera_device *icd,
					struct v4l2_cropcap *crop)
{
	int pclk_polar;
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);

	isp_dbg("enter %s\n", __func__);

	pclk_polar = ispdrv_get_pclk_polar();
	switch (pclk_polar) {
	case POLAR_RISING:
		set_pclk_polar(1);
		break;
	case POLAR_FALLING:
		set_pclk_polar(0);
		break;
	default:
		printk("pclk polar wrong: %d\n", pclk_polar);
		break;
	}

	if (crop->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	// isp support crop, need complete.
	return v4l2_subdev_call(sd, video, cropcap, crop);
}

static int ak_camera_get_crop(struct soc_camera_device *icd,
		struct v4l2_crop *crop)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);

	isp_dbg("entry %s\n", __func__);

	return v4l2_subdev_call(sd, video, g_crop, crop);
}

static int ak_camera_set_crop(struct soc_camera_device *icd,
		                   struct v4l2_crop *crop)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct v4l2_cropcap cropcap;
	struct v4l2_crop local_crop;
	struct v4l2_rect off_rect;
	int ret;
	int total_left, total_top;
	AK_ISP_SENSOR_CB *sensor_cb;

	isp_dbg("entry %s\n", __func__);
	sensor_cb = ak_sensor_get_sensor_cb();
	if (NULL == sensor_cb) {
		printk(KERN_ERR "%s get sensor_cb failed!!!!!\n", __func__);
		return -ENODEV;
	} else {
		sensor_cb->sensor_get_valid_coordinate_func(&off_rect.left, &off_rect.top);
	}

	if (v4l2_subdev_call(sd, video, cropcap, &cropcap)) {
		v4l2_err(&ici->v4l2_dev,
				"%s %d cropcap err\n", __func__, __LINE__);
		return -EINVAL;
	}

	if ((cropcap.bounds.width - crop->c.left < crop->c.width) ||
			(cropcap.bounds.height - crop->c.top < crop->c.height)) {
		v4l2_err(&ici->v4l2_dev,
				"%s %d error crop values\n", __func__, __LINE__);
		return -EINVAL;
	}

	total_left = off_rect.left + crop->c.left;
	total_top = off_rect.top + crop->c.top;

	ret = ispdrv_vi_set_crop(total_left, total_top, crop->c.width, crop->c.height);
	if (ret) {
		v4l2_err(&ici->v4l2_dev,
				"%s %d error set isp crop\n", __func__, __LINE__);
		return -EINVAL;
	}


	local_crop.c.left = total_left;
	local_crop.c.top = total_top;
	local_crop.c.width = crop->c.width;
	local_crop.c.height = crop->c.height;
	return v4l2_subdev_call(sd, video, s_crop, &local_crop);
}

static int ak_camera_set_livecrop(struct soc_camera_device *icd, struct v4l2_crop *crop)
{

	isp_dbg("entry %s\n", __func__);

	return ak_camera_set_crop(icd, crop);
}

/**
 * @brief: setting image format information, Called before ak_camera_set_fmt.
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *icd: soc_camera_device information structure,
 * akcamera depends on the soc driver.
 * @param [in] *f: image format
 */
static int ak_camera_try_fmt(struct soc_camera_device *icd,
			      struct v4l2_format *f)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	const struct soc_camera_format_xlate *xlate;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct v4l2_mbus_framefmt mf;
	int ret;
	/* TODO: limit to ak hardware capabilities */
	CAMDBG("entry %s\n", __func__);

	xlate = soc_camera_xlate_by_fourcc(icd, pix->pixelformat);
	if (!xlate) {
		dev_warn(icd->parent, "Format %x not found\n",
			 pix->pixelformat);
		return -EINVAL;
	}

	mf.width	= pix->width;
	mf.height	= pix->height;
	mf.field	= pix->field;
	mf.colorspace	= pix->colorspace;
	mf.code		= xlate->code;

	/* limit to sensor capabilities */
	ret = v4l2_subdev_call(sd, video, try_mbus_fmt, &mf);
	if (ret < 0) {
		return ret;
	}

	pix->width	= mf.width;
	pix->height = mf.height;
	pix->field	= mf.field;
	pix->colorspace = mf.colorspace;

	return 0;
}

/**
 * @brief: setting image format information
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *icd: soc_camera_device information structure,
 * akcamera depends on the soc driver.
 * @param [in] *f: image format
 */
static int ak_camera_set_fmt(struct soc_camera_device *icd,
			      struct v4l2_format *f)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
//	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
//	struct ak_camera_dev *pcdev = ici->priv;
	const struct soc_camera_format_xlate *xlate;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct v4l2_mbus_framefmt mf;
	struct v4l2_cropcap cropcap;
	int ret = 0, buswidth;

	isp_dbg("entry %s\n", __func__);

	/*
	 * This programs the ISP, and the ISP core only exists once
	 * /dev/isp_char has been opened. soc_camera_open calls straight in
	 * here, so opening the two devices in the wrong order is a NULL
	 * dereference in ak_isp_vo_set_main_channel_scale - and it happens
	 * with the soc-camera mutex held, so every later open then hangs in
	 * v4l2_open until reboot. Probe does not come this way, so the sensor
	 * is still probed at boot.
	 */
	if (!ispdrv_is_ready()) {
		dev_err(icd->parent, "ISP core is down - open /dev/isp_char first\n");
		return -ENODEV;
	}

	xlate = soc_camera_xlate_by_fourcc(icd, pix->pixelformat);
	if (!xlate) {
		dev_warn(icd->parent, "Format %x not found\n",
			 pix->pixelformat);
		return -EINVAL;
	}

	buswidth = xlate->host_fmt->bits_per_sample;
	if (buswidth > 10) {
		dev_warn(icd->parent,
			 "bits-per-sample %d for format %x unsupported\n",
			 buswidth, pix->pixelformat);
		return -EINVAL;
	}

	mf.width	= pix->width;
	mf.height	= pix->height;
	mf.field	= pix->field;
	mf.colorspace	= pix->colorspace;
	mf.code		= xlate->code;
	icd->current_fmt = xlate;

	v4l2_subdev_call(sd, video, cropcap, &cropcap);
	if (mf.width > cropcap.bounds.width)
		mf.width = cropcap.bounds.width;

	if (mf.height > cropcap.bounds.height)
		mf.height = cropcap.bounds.height;

	isp_dbg("%s. mf.width = %d, mf.height = %d\n",
			__func__, mf.width, mf.height);

	if (mf.code != xlate->code)
		return -EINVAL;

	ispdrv_vo_set_main_channel_scale(pix->width, pix->height);

	return ret;
}

/**
 * @brief: getting image format information
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *icd: soc_camera_device information structure,
 * akcamera depends on the soc driver.
 * @param [in] *f: image format
 */
static const struct soc_mbus_pixelfmt ak_camera_nv12 = {
	.fourcc			= V4L2_PIX_FMT_NV12,
	.name			= "YUV 4:2:0 semi-planar",
	.bits_per_sample	= 8,
	.packing		= SOC_MBUS_PACKING_1_5X8,
	.order			= SOC_MBUS_ORDER_LE,
};

static int ak_camera_get_formats(struct soc_camera_device *icd, unsigned int idx,
				     struct soc_camera_format_xlate *xlate)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct device *dev = icd->parent;
	struct soc_camera_host *ici = to_soc_camera_host(dev);
	struct ak_camera_dev *pcdev = ici->priv;
	int ret, formats = 0;
	enum v4l2_mbus_pixelcode code;
	const struct soc_mbus_pixelfmt *fmt;

	CAMDBG("entry %s\n", __func__);
	ret = v4l2_subdev_call(sd, video, enum_mbus_fmt, idx, &code);
	if (ret < 0)
		/* No more formats */
		return 0;

	/*
	 * What the ISP writes is NV12: a full-size Y plane followed by
	 * interleaved chroma at half resolution, 1.5 bytes per pixel. This
	 * driver only offered YUYV - packed 4:2:2, two bytes per pixel - so
	 * every caller sized its buffers a third too large and expected a
	 * layout the hardware never produces.
	 *
	 * YUYV stays second because the vendor library asks for it by name and
	 * has always lived with the mismatch; offering only NV12 makes its
	 * S_FMT fail. jpeg out is not offered either way.
	 */
	fmt = &ak_camera_nv12;
	CAMDBG("get format %s code=%d from sensor\n", fmt->name, code);

	/* Generic pass-through */
	formats++;
	if (xlate) {
		xlate->host_fmt	= fmt;
		xlate->code	= code;
		xlate++;

		/*
		  * @decide the default working mode of isp
		  * @prefer RGB mode
		  */
		if (code >= V4L2_MBUS_FMT_SBGGR8_1X8 && code <= V4L2_MBUS_FMT_SRGGB12_1X12) {
			pcdev->def_mode = ISP_RGB_VIDEO_OUT;
			//pcdev->def_mode = ISP_RGB_OUT;
		} else if (code >= V4L2_MBUS_FMT_Y8_1X8 &&
				code <= V4L2_MBUS_FMT_YVYU10_1X20) {
			pcdev->def_mode = ISP_YUV_VIDEO_OUT;
			//pcdev->def_mode = ISP_YUV_OUT;
			printk(KERN_ERR "set yuv video out\n");
		} else {
			pcdev->def_mode = ISP_RGB_VIDEO_OUT;
		}

		pcdev->cur_mode = pcdev->def_mode;
		pcdev->cur_mode_class = code;

		dev_dbg(dev, "Providing format %s in pass-through mode\n",
			fmt->name);
	}

	fmt = soc_mbus_get_fmtdesc(V4L2_MBUS_FMT_YUYV8_2X8);
	if (fmt) {
		formats++;
		if (xlate) {
			xlate->host_fmt	= fmt;
			xlate->code	= code;
			xlate++;
		}
	}

	return formats;
}

static void ak_camera_put_formats(struct soc_camera_device *icd)
{
	CAMDBG("entry %s\n", __func__);
	kfree(icd->host_priv);
	icd->host_priv = NULL;
	CAMDBG("leave %s\n", __func__);
}

/* Maybe belong platform code fix me */
static int ak_camera_set_bus_param(struct soc_camera_device *icd)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct ak_camera_dev *pcdev = ici->priv;
	struct v4l2_mbus_config cfg = {.type = V4L2_MBUS_PARALLEL,};
	unsigned long common_flags;
	int ret;

	CAMDBG("entry %s\n", __func__);

	/* AK39 supports 8bit and 10bit buswidth */
	ret = v4l2_subdev_call(sd, video, g_mbus_config, &cfg);
	if (!ret) {
		common_flags = soc_mbus_config_compatible(&cfg, CSI_BUS_FLAGS);
		if (!common_flags) {
			dev_warn(icd->parent,
				 "Flags incompatible: camera 0x%x, host 0x%x\n",
				 cfg.flags, CSI_BUS_FLAGS);
			return -EINVAL;
		}
	} else if (ret != -ENOIOCTLCMD) {
		return ret;
	} else {
		common_flags = CSI_BUS_FLAGS;
	}

	/* Make choises, based on platform choice */
	if ((common_flags & V4L2_MBUS_VSYNC_ACTIVE_HIGH) &&
		(common_flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)) {
			if (!pcdev->pdata ||
			     pcdev->pdata->flags & AK_CAMERA_VSYNC_HIGH)
				common_flags &= ~V4L2_MBUS_VSYNC_ACTIVE_LOW;
			else
				common_flags &= ~V4L2_MBUS_VSYNC_ACTIVE_HIGH;
	}

	if ((common_flags & V4L2_MBUS_PCLK_SAMPLE_RISING) &&
		(common_flags & V4L2_MBUS_PCLK_SAMPLE_FALLING)) {
			if (!pcdev->pdata ||
			     pcdev->pdata->flags & AK_CAMERA_PCLK_RISING)
				common_flags &= ~V4L2_MBUS_PCLK_SAMPLE_FALLING;
			else
				common_flags &= ~V4L2_MBUS_PCLK_SAMPLE_RISING;
	}

	if ((common_flags & V4L2_MBUS_DATA_ACTIVE_HIGH) &&
		(common_flags & V4L2_MBUS_DATA_ACTIVE_LOW)) {
			if (!pcdev->pdata ||
			     pcdev->pdata->flags & AK_CAMERA_DATA_HIGH)
				common_flags &= ~V4L2_MBUS_DATA_ACTIVE_LOW;
			else
				common_flags &= ~V4L2_MBUS_DATA_ACTIVE_HIGH;
	}

	cfg.flags = common_flags;
	ret = v4l2_subdev_call(sd, video, s_mbus_config, &cfg);
	if (ret < 0 && ret != -ENOIOCTLCMD) {
		dev_dbg(icd->parent, "camera s_mbus_config(0x%lx) returned %d\n",
			common_flags, ret);
		return ret;
	}

	CAMDBG("leave %s\n", __func__);

	return 0;
}

/**
 * @brief: register video buffer by video sub-system
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *icd: soc_camera_device information structure,
 * akcamera depends on the soc driver.
 * @param [in] *q: V4L2  buffer queue information structure
 */
static void ak_camera_init_videobuf(struct videobuf_queue *q,
			struct soc_camera_device *icd)
{
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct ak_camera_dev *pcdev = ici->priv;

	CAMDBG("entry %s\n", __func__);

	videobuf_queue_dma_contig_init(q, &ak_videobuf_ops, icd->parent,
				&pcdev->lock, V4L2_BUF_TYPE_VIDEO_CAPTURE,
				V4L2_FIELD_NONE,
				sizeof(struct ak_buffer), icd, &icd->video_lock);

	CAMDBG("leave %s\n", __func__);
}

/**
 * @brief: request video buffer.
 *
 * @author: caolianming
 * @date: 2014-01-06
 * @param [in] *icd: soc_camera_device information structure,
 * akcamera depends on the soc driver.
 * @param [in] *q: V4L2  buffer queue information structure
 */
static int ak_camera_reqbufs(struct soc_camera_device *icd,
				struct v4l2_requestbuffers *p)
{
	int i;
	//struct soc_camera_host *ici = to_soc_camera_host(icd->parent);

	CAMDBG("entry %s\n", __func__);

	/* This is for locking debugging only. I removed spinlocks and now I
	 * check whether .prepare is ever called on a linked buffer, or whether
	 * a dma IRQ can occur for an in-work or unlinked buffer. Until now
	 * it hadn't triggered */
	for (i = 0; i < p->count; i++) {
		struct ak_buffer *buf = container_of(icd->vb_vidq.bufs[i],
						      struct ak_buffer, vb);
		buf->inwork = 0;
		INIT_LIST_HEAD(&buf->vb.queue);
	}

	CAMDBG("leave %s\n", __func__);

	return 0;
}

/* platform independent */
static unsigned int ak_camera_poll(struct file *file, poll_table *pt)
{
	struct soc_camera_device *icd = file->private_data;
	struct ak_buffer *buf;

	buf = list_entry(icd->vb_vidq.stream.next, struct ak_buffer,
				vb.stream);

	poll_wait(file, &buf->vb.done, pt);

	if (buf->vb.state == VIDEOBUF_DONE ||
			buf->vb.state == VIDEOBUF_ERROR) {
		return POLLIN | POLLRDNORM;
	}

	return 0;
}

static int ak_camera_querycap(struct soc_camera_host *ici,
		                   struct v4l2_capability *cap)
{
	isp_dbg("entry %s\n", __func__);

	/* cap->name is set by the friendly caller:-> */
	strlcpy(cap->card, ak_cam_driver_description, sizeof(cap->card));
	cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;

	return 0;
}

/*
 * Stock's only camera-host private parameter selects the ISP2 secondary
 * channel size. ISP attributes and sensor registers belong to /dev/isp_char.
 */
static int ak_camera_set_parm(struct soc_camera_device *icd,
			      struct v4l2_streamparm *parm)
{
	struct ak_camera_ch2_parm {
		int id;
		int width;
		int height;
	} *ch2 = (void *)parm->parm.raw_data;

	if (ch2->id != 1)
		return -EINVAL;

	return ispdrv_vo_set_sub_channel_scale(ch2->width, ch2->height);
}

static struct soc_camera_host_ops ak_soc_camera_host_ops = {
	.owner		= THIS_MODULE,
	.add			= ak_camera_add_device,
	.remove			= ak_camera_remove_device,
	.get_formats	= ak_camera_get_formats,
	.put_formats	= ak_camera_put_formats,
	.set_bus_param	= ak_camera_set_bus_param,
	.cropcap		= ak_camera_cropcap,
	.get_crop       = ak_camera_get_crop,
	.set_crop       = ak_camera_set_crop,
	.set_livecrop	= ak_camera_set_livecrop,
	.set_fmt		= ak_camera_set_fmt,
	.try_fmt		= ak_camera_try_fmt,
	.init_videobuf	= ak_camera_init_videobuf,
	.reqbufs		= ak_camera_reqbufs,
	.poll			= ak_camera_poll,
	.querycap		= ak_camera_querycap,
	.set_parm		= ak_camera_set_parm,
};

static int ak_camera_probe(struct platform_device *pdev)
{
	struct ak_camera_dev *pcdev;
	struct clk *clk, *cis_sclk;
	unsigned int irq;
	int err = 0;

	CAMDBG("entry %s\n", __func__);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		printk("platform_get_irq | platform_get_resource\n");
		err = -ENODEV;
		goto exit;
	}

	/*
	  * @get isp working clock
	  */
	clk = clk_get(&pdev->dev, "camera");
	if (IS_ERR(clk)) {
		err = PTR_ERR(clk);
		goto exit;
	}

	/*
	  * @get cis_sclk for sensor
	  */
	cis_sclk = clk_get(&pdev->dev, "sensor");
	if (IS_ERR(cis_sclk)) {
		err = PTR_ERR(cis_sclk);
		goto exit_put_clk;
	}

	/*
	** @allocate memory to struct ak_camera, including struct soc_camera_host
	** @and struct v4l2_device
	*/
	pcdev = kzalloc(sizeof(*pcdev), GFP_KERNEL);
	if (!pcdev) {
		err = -ENOMEM;
		goto exit_put_cisclk;
	}

	/* @initailization for struct pcdev */
	INIT_LIST_HEAD(&pcdev->capture);
	spin_lock_init(&pcdev->lock);
	mutex_init(&pcdev->stream_lock);
	INIT_DELAYED_WORK(&pcdev->awb_work, isp_awb_work);
	INIT_DELAYED_WORK(&pcdev->ae_work, isp_ae_work);
	INIT_WORK(&pcdev->resume_work, ak_camera_resume_work);
//	pcdev->res = res;
	pcdev->clk = clk;
	pcdev->cis_sclk = cis_sclk;

	pcdev->list_state = LIST_ZERO;
	pcdev->free_list = LIST_ZERO;

	pcdev->pdata = pdev->dev.platform_data;
	if (!pcdev->pdata) {
		err = -ENODEV;
		goto exit_isp_fini;
	}
	if (pcdev->pdata->mclk > 0)
		pcdev->mclk = pcdev->pdata->mclk;
	else {
		dev_warn(&pdev->dev,
			 "Platform mclk == 0; using default 24MHz\n");
		pcdev->mclk = 24;
	}

	/*
	  * request irq
	  */
	err = request_irq(irq, ak_camera_dma_irq, IRQF_DISABLED, "ak_camera", pcdev);
	if (err) {
		err = -EBUSY;
		goto exit_isp_fini;
	}
	pcdev->irq = irq;

	/*
	** @register soc_camera_host
	*/
	pcdev->soc_host.drv_name	= AK_CAM_DRV_NAME;
	pcdev->soc_host.ops		= &ak_soc_camera_host_ops;
	pcdev->soc_host.priv		= pcdev;
	pcdev->soc_host.v4l2_dev.dev	= &pdev->dev;
	pcdev->soc_host.nr		= pdev->id;

	err = soc_camera_host_register(&pcdev->soc_host);
	if (err) {
		goto exit_freeirq;
	}
	pcdev->cur_buf_id = -1;

	dev_info(&pdev->dev, "AK Camera driver loaded\n");

	return 0;

exit_freeirq:
	free_irq(irq, pcdev);
exit_isp_fini:
	kfree(pcdev);
exit_put_cisclk:
	clk_put(cis_sclk);
exit_put_clk:
	clk_put(clk);
exit:
	return err;
}

static int ak_camera_remove(struct platform_device *pdev)
{
	struct soc_camera_host *soc_host = to_soc_camera_host(&pdev->dev);
	struct ak_camera_dev *pcdev = container_of(soc_host,
					struct ak_camera_dev, soc_host);

	CAMDBG("entry %s\n", __func__);

	soc_camera_host_unregister(soc_host);

	cancel_work_sync(&pcdev->resume_work);
	cancel_delayed_work_sync(&pcdev->awb_work);
	cancel_delayed_work_sync(&pcdev->ae_work);
	free_irq(pcdev->irq, pcdev);

	/* free clk */
	clk_put(pcdev->cis_sclk);
	clk_put(pcdev->clk);

	kfree(pcdev);

	dev_info(&pdev->dev, "AK Camera driver unloaded\n");

	return 0;
}

static struct platform_driver ak_camera_driver = {
	.probe		= ak_camera_probe,
	.remove		= ak_camera_remove,
	.driver		= {
		.name = AK_CAM_DRV_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init ak_camera_init(void)
{
	CAMDBG("entry %s\n", __func__);
	return platform_driver_register(&ak_camera_driver);
}

static void __exit ak_camera_exit(void)
{
	CAMDBG("entry %s\n", __func__);

	platform_driver_unregister(&ak_camera_driver);
}

module_init(ak_camera_init);
module_exit(ak_camera_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wu_daochao <wu_daochao@anyka.oa>");
MODULE_DESCRIPTION("Driver for ak Camera Interface");
