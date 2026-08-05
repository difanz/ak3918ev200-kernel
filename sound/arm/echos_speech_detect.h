#ifndef ECHOS_SPEECH_DETECT_H
#define ECHOS_SPEECH_DETECT_H

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct echos_speech_state {
	int32_t  result;
	int32_t  hangover;
	int32_t  unused_08;
	int32_t  speech_hold;
	int32_t  active_cnt;
	int32_t  trail_cnt;
	uint32_t peak_q14;
	uint32_t noise_q14;
	uint32_t speech_lvl_q14;
	int32_t  fsm;
} echos_speech_state_t;

typedef struct echos_speech_cfg {
	uint32_t max_active;
	uint32_t enter_thr_q14;
	uint32_t exit_thr_q14;
	uint32_t abs_min_q14;
	int32_t  enter_hangover;
	int32_t  leave_hangover;
} echos_speech_cfg_t;

int echos_speechDetect(echos_speech_state_t *st,
                       const int16_t *pcm,
                       int nsamp,
                       const echos_speech_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif
