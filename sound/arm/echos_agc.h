#ifndef ECHOS_AGC_H
#define ECHOS_AGC_H

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EchoS_AgcState {
	uint16_t target;
	uint16_t gain;
	int16_t  attack;
	int16_t  release;
	int16_t  fast_release;
	uint16_t _pad0a;
	int32_t  max_gain;
	int32_t  min_gain;
	int32_t  sample_acc;
	int32_t  gain_hold;
	int32_t  speech_prev;
	int32_t  min_gain_seen;
	int32_t  reserved_24;
	int32_t  reserved_28;
	uint16_t notch_r;
	uint16_t _pad2e;
	uint32_t sxx;
	uint32_t sxx_prev;
	uint32_t _pad38;
	uint32_t _pad3c;
} EchoS_AgcState;

#define ECHOS_AGC_OFFSET_IN_HANDLE  0x120C
#define ECHOS_NOTCH_STATE_OFFSET    0x124C

void echos_agc_init(void *handle, const void *config);
int  echos_agc_proc(void *handle, const int16_t *in, int16_t *out, int nsamples);
void echos_filter_dc_notch16(const int16_t *in, int16_t coef, int16_t *out,
                             int n, int32_t *state, int stride);

#ifdef __cplusplus
}
#endif

#endif
