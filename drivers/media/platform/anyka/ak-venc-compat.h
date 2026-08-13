/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 3.4 / 4.4 videobuf2 and mem2mem differences used by the Anyka encoder.
 *
 * Copyright (C) 2026 Alex Zhang <alex@osqdu.org>
 */

#ifndef AK_VENC_COMPAT_H
#define AK_VENC_COMPAT_H

#include <linux/version.h>
#include <media/v4l2-mem2mem.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)

#include <media/videobuf2-core.h>

#define AK_VENC_QUEUE_SETUP_ARG		const struct v4l2_format
#define AK_VENC_BUF_FINISH_RET		int
#define ak_venc_buf_finish_done()	return 0
#define AK_VENC_STOP_STREAMING_RET	int
#define ak_venc_stop_streaming_done()	return 0

#define ak_venc_vb_to_buf(vb, type)	container_of(vb, type, m2m.vb)

static inline void ak_venc_m2m_buf_done(struct vb2_buffer *vb,
					enum vb2_buffer_state state)
{
	v4l2_m2m_buf_done(vb, state);
}

static inline void ak_venc_m2m_buf_queue(struct v4l2_m2m_ctx *m2m_ctx,
					 struct vb2_buffer *vb)
{
	v4l2_m2m_buf_queue(m2m_ctx, vb);
}

static inline void ak_venc_vb_copy_timestamp(struct vb2_buffer *dst,
					     struct vb2_buffer *src)
{
	dst->v4l2_buf.timestamp = src->v4l2_buf.timestamp;
}

static inline void ak_venc_vb_set_frame_type(struct vb2_buffer *vb, bool intra)
{
	vb->v4l2_buf.flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME);
	vb->v4l2_buf.flags |= intra ? V4L2_BUF_FLAG_KEYFRAME :
				      V4L2_BUF_FLAG_PFRAME;
}

#else

#include <media/videobuf2-v4l2.h>

#define AK_VENC_QUEUE_SETUP_ARG		const void
#define AK_VENC_BUF_FINISH_RET		void
#define ak_venc_buf_finish_done()	do { } while (0)
#define AK_VENC_STOP_STREAMING_RET	void
#define ak_venc_stop_streaming_done()	do { } while (0)

#define ak_venc_vb_to_buf(vb, type)	container_of(to_vb2_v4l2_buffer(vb), \
						     type, m2m.vb)

static inline void ak_venc_m2m_buf_done(struct vb2_buffer *vb,
					enum vb2_buffer_state state)
{
	v4l2_m2m_buf_done(to_vb2_v4l2_buffer(vb), state);
}

static inline void ak_venc_m2m_buf_queue(struct v4l2_m2m_ctx *m2m_ctx,
					 struct vb2_buffer *vb)
{
	v4l2_m2m_buf_queue(m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static inline void ak_venc_vb_copy_timestamp(struct vb2_buffer *dst,
					     struct vb2_buffer *src)
{
	to_vb2_v4l2_buffer(dst)->timestamp = to_vb2_v4l2_buffer(src)->timestamp;
}

static inline void ak_venc_vb_set_frame_type(struct vb2_buffer *vb, bool intra)
{
	struct vb2_v4l2_buffer *v = to_vb2_v4l2_buffer(vb);

	v->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME);
	v->flags |= intra ? V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
}

#endif

#endif /* AK_VENC_COMPAT_H */
