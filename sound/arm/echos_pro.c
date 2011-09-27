#include "echos_pro_internal.h"
#include "echos_pro_tables.h"

#include <linux/string.h>

static int echos_div1000(int x)
{
	long long p = (long long)x * (long long)(int)ECHOS_DIV1000_MAGIC;
	int hi = (int)(p >> 32);
	return (hi >> 6) - (x >> 31);
}

static int ms_to_frames(int ms, int sample_rate)
{
	int samples = echos_div1000(ms * sample_rate);
	if (samples < 0)
		samples += 31;
	return samples >> 5;
}

static int checkBufDataLen(echos_pcm_ring_t *ring)
{
	uint32_t rd = ring->rd;
	uint32_t wr = ring->wr;

	if (rd < wr)
		return (int)(wr - rd);
	if (rd > wr)
		return (int)(rd + (ECHOS_RING_DATA_SIZE - wr));
	return 0;
}

static void echos_memcpy(void *dst, const void *src, int n)
{
	int i;
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	for (i = 0; i < n; i++)
		d[i] = s[i];
}

static int echos_save_pcmtotmp(echos_pcm_ring_t *ring, const void *pcm, int len)
{
	int n = 0;

	if (ring->tmp_len < ECHOS_FRAME_BYTES) {
		n = (int)(ECHOS_FRAME_BYTES - ring->tmp_len);
		if (len < n)
			n = len;
		echos_memcpy(ring->tmp + ring->tmp_len, pcm, n);
		ring->tmp_len += (uint32_t)n;
	}
	return n;
}

static int echos_save_pcm(echos_pcm_ring_t *ring, const void *pcm, int len,
                   const void **frame_out, int *consumed)
{
	int remain = len;
	int free_space;

	*consumed = 0;
	*frame_out = 0;

	if (ring->tmp_len != 0 || len < ECHOS_FRAME_BYTES) {
		int n = echos_save_pcmtotmp(ring, pcm, len);
		*consumed = n;
		remain = len - n;
	}

	free_space = (int)ring->rd - (int)ring->wr;
	if (free_space < 1)
		free_space += ECHOS_RING_DATA_SIZE;
	if (free_space < 0x41)
		return -1;

	if (ring->tmp_len == ECHOS_FRAME_BYTES) {
		*frame_out = ring->tmp;
		ring->tmp_len = 0;
	} else {
		if (remain < ECHOS_FRAME_BYTES)
			return *consumed;
		*frame_out = (const uint8_t *)pcm + (len - remain);
		*consumed = ECHOS_FRAME_BYTES;
	}

	echos_memcpy(ring->data + ring->wr, *frame_out, ECHOS_FRAME_BYTES);
	ring->wr += ECHOS_FRAME_BYTES;
	if (ring->wr > ECHOS_RING_MAX_IDX)
		ring->wr = 0;
	return *consumed;
}

static int echos_get_noise_point(int32_t *cursor)
{
	int16_t *base = (int16_t *)(uintptr_t)(uint32_t)cursor[0];
	int16_t s = base[cursor[1]];

	cursor[1] += 1;
	if (cursor[1] >= cursor[2])
		cursor[1] = 0;
	return (int)s;
}

static int echos_muffler(echos_handle_t *h, const int16_t *in, int16_t *out, int nsamp)
{
	int i;
	int16_t *pgain = H16(h, H_NEAR_GAIN);
	int16_t step = *H16(h, H_NEAR_STEP_DN);

	if (*pgain < 1) {
		for (i = 0; i < nsamp; i++)
			out[i] = 0;
		*H32(h, H_OUT_ACTIVE) = 0;
		*H32(h, H_DAC_LOCK) = 0;
	} else {
		for (i = 0; i < nsamp; i++) {
			out[i] = (int16_t)(((int)in[i] * (int)*pgain) >> 15);
			*pgain = (int16_t)(*pgain - step);
			if (*pgain < 1)
				*pgain = 0;
		}
		*H32(h, H_OUT_ACTIVE) = 1;
		*H32(h, H_DAC_LOCK) = 1;
	}
	return i << 1;
}

static int echos_speech(echos_handle_t *h, const int16_t *in, int16_t *out, int nsamp)
{
	int i;
	int16_t *pgain = H16(h, H_NEAR_GAIN);
	int16_t step = *H16(h, H_NEAR_STEP_UP);

	if (*pgain == (int16_t)ECHOS_GAIN_UNITY) {
		for (i = 0; i < nsamp; i++)
			out[i] = in[i];
	} else {
		for (i = 0; i < nsamp; i++) {
			out[i] = (int16_t)(((int)in[i] * (int)*pgain) >> 15);
			*pgain = (int16_t)(*pgain + step);
			if (*pgain < 0)
				*pgain = (int16_t)ECHOS_GAIN_UNITY;
		}
	}
	*H32(h, H_DAC_LOCK) = 0;
	*H32(h, H_OUT_ACTIVE) = 1;
	return i << 1;
}

static int echos_fadeOut(echos_handle_t *h, int16_t *pcm, int nsamp)
{
	int i;
	int16_t *pgain = H16(h, H_FAR_GAIN);
	int16_t step = *H16(h, H_FAR_STEP_DN);
	int16_t floor = *H16(h, H_FAR_GAIN_FLOOR);

	if (floor < *pgain) {
		for (i = 0; i < nsamp; i++) {
			pcm[i] = (int16_t)(((int)pcm[i] * (int)*pgain) >> 15);
			*pgain = (int16_t)(*pgain - step);
			if (*pgain <= floor)
				*pgain = floor;
		}
		*H32(h, H_ADC_LOCK) = 1;
		*H32(h, H_VOL_ENABLE) = 1;
	} else {
		for (i = 0; i < nsamp; i++)
			pcm[i] = (int16_t)(((int)pcm[i] * (int)*pgain) >> 15);
		*H32(h, H_ADC_LOCK) = 0;
		*H32(h, H_VOL_ENABLE) = 0;
	}
	return i;
}

static int echos_fadeIn(echos_handle_t *h, int16_t *pcm, int nsamp)
{
	int i;
	int16_t *pgain = H16(h, H_FAR_GAIN);
	int16_t step = *H16(h, H_FAR_STEP_UP);

	if (*pgain == (int16_t)ECHOS_GAIN_UNITY) {
		for (i = 0; i < nsamp; i++)
			;
	} else {
		for (i = 0; i < nsamp; i++) {
			pcm[i] = (int16_t)(((int)pcm[i] * (int)*pgain) >> 15);
			*pgain = (int16_t)(*pgain + step);
			if (*pgain < 0)
				*pgain = (int16_t)ECHOS_GAIN_UNITY;
		}
	}
	*H32(h, H_ADC_LOCK) = 0;
	*H32(h, H_VOL_ENABLE) = 1;
	return i;
}

static int echos_volume(int16_t *pcm, int nsamp, uint16_t vol_q10)
{
	int i;

	for (i = 0; i < nsamp; i++) {
		int v = (int)((uint32_t)vol_q10 * (int)pcm[i]) >> 10;
		if (v > 0x7fff)
			v = 0x7fff;
		else if (v < -0x8000)
			v = -0x8000;
		pcm[i] = (int16_t)v;
	}
	return i;
}

static void echos_state_change_adc(echos_handle_t *h, int speech_result)
{
	if (*H32(h, H_ADC_LOCK) != 0)
		return;
	if (speech_result < 1)
		*H32(h, H_ADC_MODE) = 0;
	else if (speech_result == 1)
		*H32(h, H_ADC_MODE) = 1;
	else if (speech_result == 2)
		*H32(h, H_ADC_MODE) = 4;
}

static void echos_state_change_dac(echos_handle_t *h, int speech_result)
{
	if (*H32(h, H_DAC_LOCK) != 0)
		return;
	if (speech_result < 1)
		*H32(h, H_DAC_MODE) = 0;
	else if (speech_result == 1)
		*H32(h, H_DAC_MODE) = 2;
	else if (speech_result == 2)
		*H32(h, H_DAC_MODE) = 3;
}

static void echos_AdcDetec(echos_handle_t *h, const int16_t *pcm, int slot)
{
	echos_speech_cfg_t cfg;
	int result = 0;
	echos_speech_state_t *st = H_speech(h, H_ADC_SPEECH);

	memset(&cfg, 0, sizeof(cfg));

	if (*H32(h, H_DAC_MODE) == 2) {
		st->result = 0;
		st->fsm = 0;
		st->active_cnt = 0;
		result = 0;
	} else {
		cfg.max_active     = (uint32_t)*H32(h, H_NEAR_MAX_ACT);
		cfg.enter_thr_q14  = (uint32_t)*H32(h, H_NEAR_ENTER);
		cfg.exit_thr_q14   = ECHOS_Q14_ONE;
		cfg.abs_min_q14    = (uint32_t)*H32(h, H_NEAR_ABS_Q14);
		cfg.enter_hangover = *H32(h, H_NEAR_HANG);
		cfg.leave_hangover = 12;
		result = echos_speechDetect(st, pcm, ECHOS_FRAME_SAMPLES, &cfg);
		echos_state_change_adc(h, result);
	}

	{
		uint8_t *flags = HB(h) + H_MUFFLER_FLAGS;
		if (*H32(h, H_ADC_MODE) == 0 || *H32(h, H_DAC_MODE) == 2)
			flags[slot] = 1;
		else
			flags[slot] = 0;
	}
}

static void echos_DacDetec(echos_handle_t *h, const int16_t *pcm, int unused)
{
	echos_speech_cfg_t cfg;
	int result;
	echos_speech_state_t *st = H_speech(h, H_DAC_SPEECH);

	(void)unused;
	memset(&cfg, 0, sizeof(cfg));

	if (*H32(h, H_ADC_MODE) == 1) {
		st->result = 0;
		st->fsm = 0;
		st->active_cnt = 0;
	} else {
		cfg.max_active     = (uint32_t)*H32(h, H_FAR_MAX_ACT);
		cfg.enter_thr_q14  = (uint32_t)*H32(h, H_FAR_ENTER);
		cfg.exit_thr_q14   = ECHOS_Q14_ONE;
		cfg.abs_min_q14    = (uint32_t)*H32(h, H_FAR_ABS_Q14);
		cfg.enter_hangover = *H32(h, H_FAR_HANG);
		cfg.leave_hangover = *H32(h, H_DELAY_FRAMES);
		result = echos_speechDetect(st, pcm, ECHOS_FRAME_SAMPLES, &cfg);
		echos_state_change_dac(h, result);
	}
}

static const char *speech_fsm_sym(unsigned s)
{
	if (s == 0)
		return "idle";
	if (s == 1 || s == 3)
		return "speech";
	if (s == 2)
		return "post";
	return "?";
}

static void debug_output_state(echos_handle_t *h)
{
	int tick;
	echos_speech_state_t *dac = H_speech(h, H_DAC_SPEECH);
	echos_speech_state_t *adc = H_speech(h, H_ADC_SPEECH);

	if (*H32(h, H_AES_DISABLE) != 0)
		return;

	tick = *H32(h, H_DEBUG_TICK) + 1;
	*H32(h, H_DEBUG_TICK) = tick;
	if (tick <= 0x31)
		return;

	printk(KERN_INFO "echos: state far=%s near=%s\n",
		speech_fsm_sym((unsigned)dac->fsm),
		speech_fsm_sym((unsigned)adc->fsm));
	*H32(h, H_DEBUG_TICK) = 0;
}


void *AECLib_Open(T_AEC_INPUT *info)
{
	echos_handle_t *h;
	const T_AEC_PARAMS *aec;
	int sr;
	int v;

	if (!info || !info->cb_fun.Malloc || !info->cb_fun.Free)
		return NULL;

	aec = &info->m_info.m_Private.m_aec;

	printk(KERN_INFO "echos: version %s\n", AECS_VERSION_STRING);
	printk(KERN_INFO "echos: ring size %u bytes (dac/adc)\n", ECHOS_RING_DATA_SIZE);
	printk(KERN_INFO "echos: allocating %u bytes\n", ECHOS_HANDLE_SIZE);

	h = (echos_handle_t *)info->cb_fun.Malloc(ECHOS_HANDLE_SIZE);
	if (!h) {
		printk(KERN_ERR "echos: failed to allocate handle\n");
		return NULL;
	}
	memset(h, 0, sizeof(*h));

	H_store_fn(h, H_MEM_ALLOC, (void *)info->cb_fun.Malloc);
	H_store_fn(h, H_MEM_FREE, (void *)info->cb_fun.Free);

	*HU16(h, H_SAMPLE_BITS) = info->m_info.m_BitsPerSample;
	*HU32(h, H_SAMPLE_RATE) = info->m_info.m_SampleRate;
	*HU16(h, H_CHANNELS) = info->m_info.m_Channels;
	sr = (int)info->m_info.m_SampleRate;

	*H32(h, H_AES_DISABLE) = (int32_t)aec->m_aecBypass;

	v = aec->AdcCutTime ? (int)aec->AdcCutTime : ECHOS_DEF_WARMUP_MS;
	*H32(h, H_WARMUP_SAMPLES) = echos_div1000(v * sr);

	v = aec->AdcMinSpeechPow ? (int)aec->AdcMinSpeechPow : ECHOS_DEF_ABS_THR;
	*H32(h, H_NEAR_ABS_Q14) = v << 14;
	{
		int seed = v * 0x3000;
		H_speech(h, H_ADC_SPEECH)->noise_q14 = (uint32_t)seed;
		*HU32(h, 0x88) = (uint32_t)seed;
		H_speech(h, H_NEAR_PRE_SPEECH)->noise_q14 = (uint32_t)seed;
	}

	v = aec->DacMinSpeechPow ? (int)aec->DacMinSpeechPow : ECHOS_DEF_ABS_THR;
	*H32(h, H_FAR_ABS_Q14) = v << 14;
	{
		int seed = v << 13;
		H_speech(h, H_DAC_SPEECH)->noise_q14 = (uint32_t)seed;
		H_speech(h, H_FAR_PRE_SPEECH)->noise_q14 = (uint32_t)seed;
	}

	v = aec->AdcSpeechHoldTime ? (int)aec->AdcSpeechHoldTime : ECHOS_DEF_MAX_ACTIVE_MS;
	*H32(h, H_NEAR_MAX_ACT) = ms_to_frames(v, sr);

	v = aec->DacSpeechHoldTime ? (int)aec->DacSpeechHoldTime : ECHOS_DEF_MAX_ACTIVE_MS;
	*H32(h, H_FAR_MAX_ACT) = ms_to_frames(v, sr);

	v = aec->AdcSpeechMultiple ? (int)aec->AdcSpeechMultiple : ECHOS_DEF_ENTER_THR;
	*H32(h, H_NEAR_ENTER) = v;

	v = aec->DacSpeechMultiple ? (int)aec->DacSpeechMultiple : ECHOS_DEF_ENTER_THR;
	*H32(h, H_FAR_ENTER) = v;

	v = aec->AdcConvergTime ? (int)aec->AdcConvergTime : ECHOS_DEF_HANGOVER_MS;
	*H32(h, H_NEAR_HANG) = ms_to_frames(v, sr);

	v = aec->DacConvergTime ? (int)aec->DacConvergTime : ECHOS_DEF_HANGOVER_MS;
	*H32(h, H_FAR_HANG) = ms_to_frames(v, sr);

	v = aec->m_tail ? (int)aec->m_tail : 0;
	{
		int t = v + 800;
		if (t < 0)
			t += 63;
		*H32(h, H_DELAY_FRAMES) = t >> 6;
	}

	*H16(h, H_FAR_GAIN) = 0;
	*H16(h, H_FAR_STEP_UP) = ECHOS_FAR_STEP_UP;
	*H16(h, H_FAR_STEP_DN) = ECHOS_FAR_STEP_DOWN;
	*H16(h, H_FAR_GAIN_FLOOR) = 0;
	*H16(h, H_NEAR_GAIN) = 0;
	*H16(h, H_NEAR_STEP_UP) = ECHOS_NEAR_STEP_UP;
	*H16(h, H_NEAR_STEP_DN) = ECHOS_NEAR_STEP_DOWN;

	*H32(h, H_NEAR_MODE) = 0;
	*H32(h, H_AGC_ENABLE) = (int32_t)aec->m_PreprocessEna;
	if (*H32(h, H_AGC_ENABLE))
		echos_agc_init(h, info);

	*H16(h, H_THR_COPY) = (int16_t)ECHOS_DEF_ENTER_THR;
	*HU16(h, H_DA_VOLUME) = aec->DacVolume;

	printk(KERN_INFO "echos: initialized\n");
	return h;
}

int AECLib_Close(void *p_aec)
{
	echos_handle_t *h = (echos_handle_t *)p_aec;
	AEC_CALLBACK_FUN_FREE freefn;

	if (!h)
		return 0;

	freefn = H_free_fn(h);
	printk(KERN_INFO "echos: released\n");

	if (*H32(h, H_AGC_HEAP)) {
		freefn((void *)(uintptr_t)(uint32_t)*H32(h, H_AGC_HEAP));
		*H32(h, H_AGC_HEAP) = 0;
	}
	freefn(h);
	return 1;
}

int AECLib_SetDaVolume(void *p_aec, uint16_t volume)
{
	echos_handle_t *h = (echos_handle_t *)p_aec;

	if (!h)
		return 0;
	*HU16(h, H_DA_VOLUME) = volume;
	return 1;
}

int AECLib_FarPreprocess(void *p_aec, T_AEC_BUF *p_aec_buf)
{
	echos_handle_t *h = (echos_handle_t *)p_aec;
	const int16_t *in = NULL;
	int16_t *out;
	uint32_t in_len = 0, out_len;
	int is_near = 0;
	int enable;
	echos_speech_state_t *st;
	echos_speech_cfg_t cfg;
	uint32_t off = 0, produced = 0;
	uint32_t chunk = ECHOS_FRAME_BYTES;

	if (!h || !p_aec_buf)
		return -1;

	memset(&cfg, 0, sizeof(cfg));

	if (p_aec_buf->buf_far) {
		is_near = 0;
		in = (const int16_t *)p_aec_buf->buf_far;
		in_len = p_aec_buf->len_far;
		enable = *H32(h, H_FAR_PRE_ENABLE);
		st = H_speech(h, H_FAR_PRE_SPEECH);
		cfg.max_active     = (uint32_t)*H32(h, H_FAR_MAX_ACT);
		cfg.enter_thr_q14  = (uint32_t)*H32(h, H_FAR_ENTER);
		cfg.exit_thr_q14   = ECHOS_Q14_ONE;
		cfg.abs_min_q14    = (uint32_t)*H32(h, H_FAR_ABS_Q14);
		cfg.enter_hangover = *H32(h, H_FAR_HANG);
		cfg.leave_hangover = 0;
	} else if (p_aec_buf->buf_near) {
		is_near = 1;
		in = (const int16_t *)p_aec_buf->buf_near;
		in_len = p_aec_buf->len_near;
		enable = *H32(h, H_NEAR_PRE_ENABLE);
		st = H_speech(h, H_NEAR_PRE_SPEECH);
		cfg.max_active     = (uint32_t)*H32(h, H_NEAR_MAX_ACT);
		cfg.enter_thr_q14  = (uint32_t)*H32(h, H_NEAR_ENTER);
		cfg.exit_thr_q14   = ECHOS_Q14_ONE;
		cfg.abs_min_q14    = (uint32_t)*H32(h, H_NEAR_ABS_Q14);
		cfg.enter_hangover = *H32(h, H_NEAR_HANG);
		cfg.leave_hangover = 12;
	} else {
		return -1;
	}

	out = (int16_t *)p_aec_buf->buf_out;
	out_len = p_aec_buf->len_out;
	if (!in || !out)
		return -1;

	if (is_near) {
		echos_filter_dc_notch16(in,
		                        (int16_t)*H16(h, H_THR_COPY),
		                        (int16_t *)(uintptr_t)in,
		                        (int)(in_len >> 1),
		                        (int32_t *)(HB(h) + H_DC_NOTCH),
		                        1);
	}

	if (!enable) {
		memcpy(out, in, in_len);
		return (int)in_len;
	}

	for (off = 0; off < in_len; off += chunk) {
		int det;
		uint32_t n = chunk;

		if (in_len - off < n)
			n = in_len - off;
		if (produced + n > out_len) {
			printk(KERN_ERR "echos: output buffer too small (in=%u out=%u)\n",
			       in_len, out_len);
			return (int)in_len;
		}

		det = echos_speechDetect(st,
		                         (const int16_t *)((const uint8_t *)in + off),
		                         (int)(n >> 1),
		                         &cfg);
		(void)det;
		echos_memcpy((uint8_t *)out + produced, (const uint8_t *)in + off, (int)n);
		produced += n;
	}
	return (int)in_len;
}

int AECLib_Control(void *p_aec, T_AEC_BUF *p_aec_buf)
{
	echos_handle_t *h = (echos_handle_t *)p_aec;
	uint32_t want;
	int avail;
	int produced = 0;
	int16_t *out;
	echos_pcm_ring_t *adc;

	if (!h || !p_aec_buf)
		return -1;

	adc = H_ring(h, H_ADC_RING);

	want = p_aec_buf->len_out;
	if ((int)want > 0x800) {
		printk(KERN_ERR "echos: output length %u too large (limit %u)\n", want, 0x800u);
		want = 0x800;
	}
	if (want & 0x3f) {
		printk(KERN_ERR "echos: output length %u not multiple of %u\n",
		       want, ECHOS_FRAME_BYTES);
		want = (want >> 6) << 6;
	}

	avail = checkBufDataLen(adc);
	if (avail < (int)want)
		return 0;

	out = (int16_t *)p_aec_buf->buf_out;
	if (adc->overflow)
		printk(KERN_WARNING "echos: adc ring overflow\n");
	if (*H32(h, H_DAC_OVERFLOW))
		printk(KERN_WARNING "echos: dac ring overflow\n");

	while (adc->rd != adc->wr && (int)(want - (uint32_t)produced) > 0x3f) {
		T_AEC_BUF frame;
		int16_t *ring_pcm = (int16_t *)(adc->data + adc->rd);
		int16_t *dst = (int16_t *)((uint8_t *)out + produced);
		int bytes;
		int slot;

		debug_output_state(h);

		memset(&frame, 0, sizeof(frame));
		frame.buf_near = ring_pcm;
		frame.len_near = ECHOS_FRAME_BYTES;
		frame.buf_out = dst;
		frame.len_out = ECHOS_FRAME_BYTES;
		AECLib_FarPreprocess(h, &frame);

		slot = (int)(adc->rd >> 6);
		echos_AdcDetec(h, dst, slot);

		if (*H32(h, H_AES_DISABLE) == 0) {
			uint8_t flag = HB(h)[H_MUFFLER_FLAGS + slot];
			if (flag)
				bytes = echos_muffler(h, dst, dst, ECHOS_FRAME_SAMPLES);
			else
				bytes = echos_speech(h, dst, dst, ECHOS_FRAME_SAMPLES);
		} else {
			bytes = ECHOS_FRAME_BYTES;
			*H32(h, H_OUT_ACTIVE) = 1;
			echos_memcpy(dst, ring_pcm, ECHOS_FRAME_BYTES);
		}

		if (*H32(h, H_AGC_ENABLE) && *H32(h, H_OUT_ACTIVE))
			bytes = echos_agc_proc(h, dst, dst, bytes >> 1);

		adc->rd += ECHOS_FRAME_BYTES;
		if (adc->rd > ECHOS_RING_MAX_IDX)
			adc->rd = 0;
		produced += bytes;
	}
	return produced;
}

void AECLib_AdcInt(void *p_aec, uint8_t *in, int32_t len)
{
	echos_handle_t *h = (echos_handle_t *)p_aec;
	const uint8_t *p;
	int remain;
	echos_pcm_ring_t *adc;

	if (!h)
		return;

	adc = H_ring(h, H_ADC_RING);

	if ((uint32_t)*H32(h, H_WARMUP_COUNT) < (uint32_t)*H32(h, H_WARMUP_SAMPLES)) {
		*H32(h, H_WARMUP_COUNT) =
		    *H32(h, H_WARMUP_COUNT) + (int32_t)((uint32_t)len >> 1);
		return;
	}

	p = in;
	remain = (int)len;
	while (remain > 0) {
		const void *frame = NULL;
		int consumed = 0;
		int rc = echos_save_pcm(adc, p, remain, &frame, &consumed);
		if (rc < 0)
			adc->overflow = 1;
		else
			adc->overflow = 0;
		remain -= consumed;
		p += consumed;
		if (!frame)
			break;
		(void)*H32(h, H_AES_DISABLE);
	}
	if (remain > 0)
		echos_save_pcmtotmp(adc, p, remain);
}

void AECLib_DacInt(void *p_aec, uint8_t *in, int32_t len)
{
	echos_handle_t *h = (echos_handle_t *)p_aec;
	uint8_t *p;
	int remain;
	echos_pcm_ring_t *dac;

	if (!h)
		return;

	dac = H_ring(h, H_DAC_RING);
	p = in;
	remain = (int)len;

	while (remain > 0) {
		const void *frame = NULL;
		int consumed = 0;
		int rc = echos_save_pcm(dac, p, remain, &frame, &consumed);

		if (rc < 0) {
			echos_save_pcmtotmp(dac, p, remain);
			dac->overflow = 1;
		} else {
			dac->overflow = 0;
		}

		if (frame) {
			uint32_t *det_idx = HU32(h, H_DAC_RD);
			echos_DacDetec(h, (const int16_t *)frame, 0);
			*det_idx += ECHOS_FRAME_BYTES;
			if (*det_idx > ECHOS_RING_MAX_IDX)
				*det_idx = 0;
		}

		if (*H32(h, H_AES_DISABLE) == 0) {
			int ns = consumed >> 1;
			if (*H32(h, H_ADC_MODE) == 1)
				echos_fadeOut(h, (int16_t *)p, ns);
			else
				echos_fadeIn(h, (int16_t *)p, ns);
		} else {
			*H32(h, H_VOL_ENABLE) = 1;
		}

		if (*H32(h, H_VOL_ENABLE)) {
			int ns = consumed >> 1;
			echos_volume((int16_t *)p, ns, *HU16(h, H_DA_VOLUME));
		}

		remain -= consumed;
		p += consumed;
		if (consumed <= 0 && rc < 0)
			break;
	}
}
