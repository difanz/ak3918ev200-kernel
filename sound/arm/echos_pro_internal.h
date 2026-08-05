#ifndef ECHOS_PRO_INTERNAL_H
#define ECHOS_PRO_INTERNAL_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include "ak_aec.h"
#include "echos_speech_detect.h"
#include "echos_agc.h"
#include "echos_printk.h"

#define ECHOS_HANDLE_SIZE       0x1260
#define ECHOS_RING_DATA_SIZE    0x800
#define ECHOS_FRAME_BYTES       0x40
#define ECHOS_FRAME_SAMPLES     0x20
#define ECHOS_RING_MAX_IDX      0x7ff
#define ECHOS_RING_STRIDE       0x850

#define ECHOS_DIV1000_MAGIC     0x10624dd3u

#define ECHOS_DEF_WARMUP_MS     100
#define ECHOS_DEF_ABS_THR       0x200
#define ECHOS_DEF_MAX_ACTIVE_MS 0x398
#define ECHOS_DEF_ENTER_THR     0x7333
#define ECHOS_DEF_HANGOVER_MS   0x1388
#define ECHOS_Q14_ONE           0x4000
#define ECHOS_GAIN_UNITY        0x7fff
#define ECHOS_NEAR_STEP_UP      0x28
#define ECHOS_NEAR_STEP_DOWN    0x100
#define ECHOS_FAR_STEP_UP       0x28
#define ECHOS_FAR_STEP_DOWN     0x100

#define H_MEM_ALLOC       0x000
#define H_MEM_FREE        0x004
#define H_SAMPLE_RATE     0x014
#define H_CHANNELS        0x018
#define H_SAMPLE_BITS     0x01a
#define H_DAC_SPEECH      0x01c
#define H_ADC_SPEECH      0x044
#define H_FAR_PRE_SPEECH  0x094
#define H_NEAR_PRE_SPEECH 0x0bc
#define H_ADC_RING        0x0e4
#define H_DAC_RING        0x934

#define RING_RD           0x800
#define RING_WR           0x804
#define RING_TMP          0x808
#define RING_TMP_LEN      0x848
#define RING_OVERFLOW     0x84c

#define H_ADC_RD          (H_ADC_RING + RING_RD)
#define H_ADC_WR          (H_ADC_RING + RING_WR)
#define H_ADC_OVERFLOW    (H_ADC_RING + RING_OVERFLOW)
#define H_DAC_RD          (H_DAC_RING + RING_RD)
#define H_DAC_WR          (H_DAC_RING + RING_WR)
#define H_DAC_OVERFLOW    (H_DAC_RING + RING_OVERFLOW)

#define H_MUFFLER_FLAGS   0x1184
#define H_ADC_MODE        0x11a4
#define H_DAC_MODE        0x11a8
#define H_ADC_LOCK        0x11ac
#define H_DAC_LOCK        0x11b0
#define H_AGC_HEAP        0x11b4
#define H_NEAR_GAIN       0x11c0
#define H_NEAR_STEP_DN    0x11c2
#define H_NEAR_STEP_UP    0x11c4
#define H_FAR_GAIN        0x11c8
#define H_FAR_STEP_DN     0x11ca
#define H_FAR_STEP_UP     0x11cc
#define H_FAR_GAIN_FLOOR  0x11ce
#define H_AES_DISABLE     0x11d0
#define H_NEAR_ABS_Q14    0x11d4
#define H_FAR_ABS_Q14     0x11d8
#define H_WARMUP_SAMPLES  0x11dc
#define H_WARMUP_COUNT    0x11e0
#define H_NEAR_MAX_ACT    0x11e4
#define H_FAR_MAX_ACT     0x11e8
#define H_NEAR_ENTER      0x11ec
#define H_FAR_ENTER       0x11f0
#define H_NEAR_HANG       0x11f4
#define H_FAR_HANG        0x11f8
#define H_DELAY_FRAMES    0x11fc
#define H_NEAR_MODE       0x1200
#define H_AGC_ENABLE      0x1204
#define H_OUT_ACTIVE      0x1208
#define H_AGC_STATE       0x120c
#define H_FAR_PRE_ENABLE  0x1244
#define H_NEAR_PRE_ENABLE 0x1248
#define H_DC_NOTCH        0x124c
#define H_THR_COPY        0x1254
#define H_DA_VOLUME       0x1256
#define H_VOL_ENABLE      0x1258
#define H_DEBUG_TICK      0x125c

typedef struct echos_pcm_ring {
	uint8_t  data[ECHOS_RING_DATA_SIZE];
	uint32_t rd;
	uint32_t wr;
	uint8_t  tmp[ECHOS_FRAME_BYTES];
	uint32_t tmp_len;
	uint32_t overflow;
} echos_pcm_ring_t;

struct echos_handle {
	uint8_t raw[ECHOS_HANDLE_SIZE];
#if UINTPTR_MAX > 0xffffffffu
	AEC_CALLBACK_FUN_MALLOC host_alloc;
	AEC_CALLBACK_FUN_FREE   host_free;
#endif
};

static inline uint8_t *HB(echos_handle_t *h)
{
	return h->raw;
}

static inline int32_t *H32(echos_handle_t *h, unsigned off)
{
	return (int32_t *)(h->raw + off);
}

static inline uint32_t *HU32(echos_handle_t *h, unsigned off)
{
	return (uint32_t *)(h->raw + off);
}

static inline int16_t *H16(echos_handle_t *h, unsigned off)
{
	return (int16_t *)(h->raw + off);
}

static inline uint16_t *HU16(echos_handle_t *h, unsigned off)
{
	return (uint16_t *)(h->raw + off);
}

static inline echos_speech_state_t *H_speech(echos_handle_t *h, unsigned off)
{
	return (echos_speech_state_t *)(h->raw + off);
}

static inline echos_pcm_ring_t *H_ring(echos_handle_t *h, unsigned base)
{
	return (echos_pcm_ring_t *)(h->raw + base);
}

static inline void H_store_fn(echos_handle_t *h, unsigned off, void *fn)
{
	uint32_t lo = (uint32_t)(uintptr_t)fn;
	memcpy(h->raw + off, &lo, 4);
#if UINTPTR_MAX > 0xffffffffu
	if (off == H_MEM_ALLOC)
		h->host_alloc = (AEC_CALLBACK_FUN_MALLOC)fn;
	else if (off == H_MEM_FREE)
		h->host_free = (AEC_CALLBACK_FUN_FREE)fn;
#else
	(void)fn;
#endif
}

static inline AEC_CALLBACK_FUN_MALLOC H_alloc(echos_handle_t *h)
{
#if UINTPTR_MAX > 0xffffffffu
	return h->host_alloc;
#else
	AEC_CALLBACK_FUN_MALLOC fn;
	memcpy(&fn, h->raw + H_MEM_ALLOC, sizeof(fn));
	return fn;
#endif
}

static inline AEC_CALLBACK_FUN_FREE H_free_fn(echos_handle_t *h)
{
#if UINTPTR_MAX > 0xffffffffu
	return h->host_free;
#else
	AEC_CALLBACK_FUN_FREE fn;
	memcpy(&fn, h->raw + H_MEM_FREE, sizeof(fn));
	return fn;
#endif
}

/*
 * checkBufDataLen(), debug_output_state(), echos_muffler(), echos_speech(),
 * echos_memcpy(), echos_save_pcmtotmp(), echos_save_pcm(),
 * echos_state_change_adc(), echos_state_change_dac(), echos_AdcDetec(),
 * echos_DacDetec(), echos_fadeOut(), echos_fadeIn(), echos_volume() and
 * echos_get_noise_point() are internal to echos_pro.c only (kernel-build
 * staticisation - see echos_pro.c). Each is defined before its first use
 * within that file, so no forward declaration is needed here.
 */

#endif
