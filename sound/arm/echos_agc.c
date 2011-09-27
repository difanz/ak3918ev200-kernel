#include "echos_agc.h"
#include "ak_aec.h"

#include <linux/stddef.h>
#include <linux/types.h>

extern int echos_speechDetect(void *sd_state, const int16_t *pcm, int n, int *params);
extern int __aeabi_idiv(int num, int den);

#define AGC_DEFAULT_TARGET   0x6000
#define AGC_TARGET_MIN       0x2AAA
#define AGC_TARGET_MIN_THR   0x2AA9
#define AGC_DEFAULT_GAIN     0x0200
#define AGC_DEFAULT_MAX_GAIN 0x0500
#define AGC_DEFAULT_MIN_GAIN 0x0080
#define AGC_ATTACK_Q14       0x4666
#define AGC_RELEASE_Q14      0x3CCC
#define AGC_FAST_REL_Q14     0x3999
#define NOTCH_R_LE_12K       0x7333
#define NOTCH_R_LE_24K       0x7F5C
#define NOTCH_R_GT_24K       0x7EF9
#define NOTCH_B_SCALE_Q15    0x5999

static EchoS_AgcState *agc_of(void *handle)
{
	return (EchoS_AgcState *)((uint8_t *)handle + ECHOS_AGC_OFFSET_IN_HANDLE);
}

static int32_t sx16(uint32_t v)
{
	return (int32_t)(int16_t)(v & 0xffffu);
}

static int32_t sat_s16_full(int32_t x)
{
	if (x > 0x7fff)
		return 0x7fff;
	if (x < -0x8000)
		return -0x8000;
	return x;
}

static int32_t mul_q15_split(int32_t c, int32_t acc)
{
	int32_t hi = acc >> 15;
	int32_t lo = (int32_t)((uint32_t)acc << 17 >> 17);
	return c * hi + ((c * lo) >> 15);
}

static int32_t agc_muladddiv(uint32_t acc, int16_t x, int32_t mul, uint32_t shift)
{
	int64_t prod = (int64_t)(uint32_t)acc * (int64_t)mul;
	int64_t addend = (int64_t)(uint32_t)((int32_t)x << 14);
	int64_t sum = prod + addend;
	uint32_t sh = shift;

	if (sh >= 32u)
		return (int32_t)(sum >> 32 >> (sh - 32u));
	return (int32_t)(sum >> sh);
}

static void agc_cal_sxx(EchoS_AgcState *agc, const int16_t *in, int n)
{
	uint32_t sxx = agc->sxx;
	int i;

	for (i = 0; i < n; i++) {
		int16_t x = in[i];
		if (x < 0)
			x = 0;
		sxx = (uint32_t)agc_muladddiv(sxx, x, 0xff, 8);
	}
	agc->sxx_prev = agc->sxx;
	agc->sxx = sxx;
}

void echos_agc_init(void *handle, const void *config)
{
	EchoS_AgcState *agc = agc_of(handle);
	const T_AEC_INPUT *in = (const T_AEC_INPUT *)config;
	const T_AEC_PARAMS *p = in ? &in->m_info.m_Private.m_aec : NULL;
	uint16_t target;
	uint16_t max_g_cfg;
	uint16_t min_g_cfg;
	uint32_t sample_rate;

	target = p ? p->AGClevel : 0;
	if (target == 0)
		target = AGC_DEFAULT_TARGET;
	agc->target = target;

	if (sx16(agc->target) < 0)
		agc->target = 0x7fff;
	else if (agc->target <= AGC_TARGET_MIN_THR)
		agc->target = AGC_TARGET_MIN;

	agc->gain = AGC_DEFAULT_GAIN;
	agc->gain_hold = sx16(agc->gain);

	max_g_cfg = p ? p->maxGain : 0;
	if (max_g_cfg == 0)
		agc->max_gain = AGC_DEFAULT_MAX_GAIN;
	else
		agc->max_gain = sx16((uint32_t)max_g_cfg << 8);

	min_g_cfg = p ? p->minGain : 0;
	if (min_g_cfg == 0)
		agc->min_gain = AGC_DEFAULT_MIN_GAIN;
	else
		agc->min_gain = sx16((uint32_t)min_g_cfg << 8);

	agc->min_gain_seen = agc->max_gain;
	agc->attack = (int16_t)AGC_ATTACK_Q14;
	agc->release = (int16_t)AGC_RELEASE_Q14;
	agc->fast_release = (int16_t)AGC_FAST_REL_Q14;

	sample_rate = in ? in->m_info.m_SampleRate : 8000u;
	if (sample_rate <= 0x2edfu)
		agc->notch_r = NOTCH_R_LE_12K;
	else if (sample_rate <= 0x5dbfu)
		agc->notch_r = NOTCH_R_LE_24K;
	else
		agc->notch_r = NOTCH_R_GT_24K;
}

void echos_filter_dc_notch16(const int16_t *in, int16_t coef, int16_t *out,
                             int n, int32_t *state, int stride)
{
	int32_t r = coef;
	int32_t t;
	uint32_t b2_u, a_u;
	int16_t b;
	int i;

	b2_u = (uint32_t)((r * r) >> 15) & 0xffffu;
	t = 0x7fff - r;
	a_u = (uint32_t)((t * t) >> 15);
	a_u = (uint32_t)(((int32_t)NOTCH_B_SCALE_Q15 * (int32_t)a_u) >> 15) & 0xffffu;
	b = (int16_t)((b2_u + a_u) & 0xffffu);

	for (i = 0; i < n; i++) {
		int16_t x = in[stride * i];
		int32_t acc = state[0] + ((int32_t)x << 15);
		int32_t mr = mul_q15_split(r, acc);
		int32_t y;

		state[0] = state[1] + (((-(int32_t)x << 15) + mr) << 1);
		state[1] = ((int32_t)x << 15) - mul_q15_split((int32_t)b, acc);

		y = (mul_q15_split(r, acc) + 0x4000) >> 15;
		if (y > 0x7fff)
			y = 0x7fff;
		else if (y < -32767)
			y = -32767;
		out[i] = (int16_t)y;
	}
}

int echos_agc_proc(void *handle, const int16_t *in, int16_t *out, int nsamples)
{
	EchoS_AgcState *agc = agc_of(handle);
	uint8_t *h = (uint8_t *)handle;
	int speech_params[6];
	int speech_flag;
	int32_t mode;
	int32_t new_gain;
	int32_t g;
	int32_t level;
	int i;

	speech_params[0] = 0;
	speech_params[1] = 0;
	speech_params[2] = 0;
	speech_params[3] = 0;
	speech_params[4] = 0;
	speech_params[5] = 0;

	agc->sample_acc += nsamples;

	speech_params[0] = 0;
	speech_params[1] = *(int32_t *)(h + 0x11ec);
	speech_params[2] = 0x4000;
	speech_params[3] = *(int32_t *)(h + 0x11d4);
	speech_params[4] = *(int32_t *)(h + 0x11f4);
	speech_params[5] = 12;

	speech_flag = echos_speechDetect(h + 0x6c, in, nsamples, speech_params);

	mode = *(int32_t *)(h + 0x90);
	switch (mode) {
	case 1:
		speech_flag = 1;
		break;
	case 0:
	case 2:
	case 3:
	default:
		speech_flag = 0;
		break;
	}

	if (speech_flag != 0) {
		if (agc->speech_prev != speech_flag)
			agc->gain = (uint16_t)(agc->gain_hold & 0xffff);

		agc_cal_sxx(agc, in, nsamples);

		{
			uint16_t level_u = (uint16_t)((*(uint32_t *)(h + 0x84)) >> 14);
			if (level_u == 0)
				level_u = 1;
			level = (int32_t)level_u;
		}

		new_gain = __aeabi_idiv((int)((uint32_t)agc->target << 8), (int)level);
		g = sx16(agc->gain);

		if ((g * (int32_t)agc->attack >> 14) < new_gain)
			new_gain = g * (int32_t)agc->attack >> 14;
		else if ((g * (int32_t)agc->release >> 14) > new_gain &&
		         agc->sxx > agc->sxx_prev + (agc->sxx_prev >> 3))
			new_gain = g * (int32_t)agc->fast_release >> 14;
		else if ((g * (int32_t)agc->release >> 14) > new_gain &&
		         agc->sxx < agc->sxx_prev * 10u)
			new_gain = g * (int32_t)agc->release >> 14;

		if (agc->max_gain < new_gain)
			new_gain = agc->max_gain;
		if (new_gain < agc->min_gain)
			new_gain = agc->min_gain;

		agc->gain = (uint16_t)(new_gain & 0xffff);
		agc->sample_acc = 0;

		if (sx16(agc->gain) < agc->min_gain_seen)
			agc->min_gain_seen = sx16(agc->gain);
	} else {
		if (agc->speech_prev != speech_flag) {
			int32_t t = agc->min_gain_seen * 10;
			if (t < 0)
				t += 7;
			agc->gain_hold = t >> 3;
			agc->min_gain_seen = agc->max_gain;
		}

		if (sx16(agc->gain) > agc->min_gain) {
			new_gain = sx16(agc->gain) * (int32_t)agc->release >> 14;
			new_gain = sx16((uint32_t)new_gain);
			if (agc->min_gain > new_gain)
				new_gain = agc->min_gain;
			agc->gain = (uint16_t)(new_gain & 0xffff);
		}
	}

	agc->speech_prev = speech_flag;

	g = sx16(agc->gain);
	for (i = 0; i < nsamples; i++) {
		int32_t y = (g * (int32_t)in[i]) >> 8;
		out[i] = (int16_t)sat_s16_full(y);
	}

	return nsamples << 1;
}
