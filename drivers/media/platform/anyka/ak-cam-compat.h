/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 3.4 / 4.4 videobuf2 and V4L2 differences used by the Anyka capture bridge.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#ifndef AK_CAM_COMPAT_H
#define AK_CAM_COMPAT_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)

#include <media/videobuf2-core.h>

#define AK_CAM_QUEUE_SETUP_ARG		const struct v4l2_format
#define AK_CAM_STOP_STREAMING_RET	int
#define AK_CAM_S_CROP_ARG		struct v4l2_crop
#define ak_cam_stop_streaming_done()	return 0

#define AK_CAM_MBUS_CODE		enum v4l2_mbus_pixelcode
#define AK_CAM_MBUS_FMT_YUYV8_2X8	V4L2_MBUS_FMT_YUYV8_2X8
#define AK_CAM_MBUS_FMT_SBGGR8_1X8	V4L2_MBUS_FMT_SBGGR8_1X8
#define AK_CAM_MBUS_FMT_SRGGB12_1X12	V4L2_MBUS_FMT_SRGGB12_1X12

#define AK_CAM_VB_BASE			struct vb2_buffer
#define ak_cam_buf_vb(buf)		(&(buf)->vb)
#define ak_cam_vb_to_buf(vb, type)	container_of(vb, type, vb)

#define ak_cam_vb_index(vb)		((vb)->v4l2_buf.index)

static inline void ak_cam_vb_set_field(struct vb2_buffer *vb, unsigned field)
{
	vb->v4l2_buf.field = field;
}

static inline void ak_cam_vb_set_sequence(struct vb2_buffer *vb, unsigned seq)
{
	vb->v4l2_buf.sequence = seq;
}

static inline void ak_cam_vb_set_timestamp(struct vb2_buffer *vb,
					   unsigned long sec, unsigned long usec)
{
	vb->v4l2_buf.timestamp.tv_sec = sec;
	vb->v4l2_buf.timestamp.tv_usec = usec;
}

#else

#include <media/videobuf2-v4l2.h>

#define AK_CAM_QUEUE_SETUP_ARG		const void
#define AK_CAM_STOP_STREAMING_RET	void
#define AK_CAM_S_CROP_ARG		const struct v4l2_crop
#define ak_cam_stop_streaming_done()	do { } while (0)

#define AK_CAM_MBUS_CODE		u32
#define AK_CAM_MBUS_FMT_YUYV8_2X8	MEDIA_BUS_FMT_YUYV8_2X8
#define AK_CAM_MBUS_FMT_SBGGR8_1X8	MEDIA_BUS_FMT_SBGGR8_1X8
#define AK_CAM_MBUS_FMT_SRGGB12_1X12	MEDIA_BUS_FMT_SRGGB12_1X12

#define AK_CAM_VB_BASE			struct vb2_v4l2_buffer
#define ak_cam_buf_vb(buf)		(&(buf)->vb.vb2_buf)
#define ak_cam_vb_to_buf(vb, type)	container_of(to_vb2_v4l2_buffer(vb), \
						     type, vb)

#define ak_cam_vb_index(vb)		((vb)->index)

static inline void ak_cam_vb_set_field(struct vb2_buffer *vb, unsigned field)
{
	to_vb2_v4l2_buffer(vb)->field = field;
}

static inline void ak_cam_vb_set_sequence(struct vb2_buffer *vb, unsigned seq)
{
	to_vb2_v4l2_buffer(vb)->sequence = seq;
}

static inline void ak_cam_vb_set_timestamp(struct vb2_buffer *vb,
					   unsigned long sec, unsigned long usec)
{
	struct vb2_v4l2_buffer *v = to_vb2_v4l2_buffer(vb);

	v->timestamp.tv_sec = sec;
	v->timestamp.tv_usec = usec;
}

#endif

#endif /* AK_CAM_COMPAT_H */
