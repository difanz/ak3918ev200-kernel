#include "echos_speech_detect.h"

static uint32_t mul_mtp(uint32_t a, uint32_t b)
{
	return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 14);
}

static uint32_t muladddiv(uint32_t a, int16_t b, uint32_t c, uint32_t shift)
{
	uint32_t b_q14 = (uint32_t)((int32_t)b << 14);
	uint64_t acc = (uint64_t)c * (uint64_t)a + (uint64_t)b_q14;
	return (uint32_t)(acc >> shift);
}

int echos_speechDetect(echos_speech_state_t *st,
                       const int16_t *pcm,
                       int nsamp,
                       const echos_speech_cfg_t *cfg)
{
	int16_t peak;
	int i;
	uint32_t thr;

	if (pcm == 0 || nsamp < 1)
		return st->result;

	peak = 0;
	for (i = 0; i < nsamp; i++) {
		int16_t s = pcm[i];
		if (s < 0)
			s = (int16_t)(-s);
		if (peak < s)
			peak = s;
	}
	st->peak_q14 = (uint32_t)((int32_t)peak << 14);

	switch (st->fsm) {
	case 0:
		thr = mul_mtp(st->noise_q14, cfg->enter_thr_q14);
		if (st->peak_q14 > thr && st->peak_q14 > cfg->abs_min_q14) {
			st->fsm = 1;
			st->hangover = cfg->enter_hangover;
			st->active_cnt = 0;
		} else {
			st->noise_q14 = muladddiv(st->noise_q14, peak, 255u, 8u);
		}
		break;

	case 1:
		thr = mul_mtp(st->noise_q14, cfg->exit_thr_q14);
		if (st->peak_q14 < thr) {
			if (st->active_cnt > 9) {
				st->fsm = 3;
				st->speech_hold = st->active_cnt;
				st->trail_cnt = 0;
			} else {
				st->fsm = 0;
				st->speech_hold = 0;
			}
		} else {
			st->speech_lvl_q14 = muladddiv(st->speech_lvl_q14, peak, 3u, 2u);

			if (st->hangover > 0)
				st->hangover -= 1;
			else if (st->hangover == 0)
				st->noise_q14 = muladddiv(st->noise_q14, peak, 255u, 8u);

			if ((uint32_t)st->active_cnt < cfg->max_active)
				st->active_cnt += 1;
		}
		break;

	case 3:
		st->trail_cnt += 1;
		if (st->trail_cnt > cfg->leave_hangover)
			st->fsm = 2;
		/* FALLTHROUGH */
	case 2:
		thr = mul_mtp(st->noise_q14, cfg->enter_thr_q14);
		if (st->peak_q14 > thr && st->peak_q14 > cfg->abs_min_q14) {
			st->fsm = 1;
			st->hangover = cfg->enter_hangover;
		} else {
			st->noise_q14 = muladddiv(st->noise_q14, peak, 255u, 8u);

			if (st->active_cnt > 0)
				st->active_cnt -= 1;

			if (st->speech_hold < 1)
				st->fsm = 0;
			else
				st->speech_hold -= 1;
		}
		break;

	default:
		break;
	}

	if (st->fsm == 3)
		st->result = 1;
	else
		st->result = st->fsm;

	return st->result;
}
