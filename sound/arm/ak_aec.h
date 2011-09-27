#ifndef AK_AEC_H
#define AK_AEC_H

#include <linux/types.h>
#include <linux/stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct echos_handle echos_handle_t;

#define AECS_VERSION_STRING	"EchoS Version V1.0.05"

typedef void *(*AEC_CALLBACK_FUN_MALLOC)(uint32_t size);
typedef void  (*AEC_CALLBACK_FUN_FREE)(void *mem);
typedef void  (*AEC_CALLBACK_FUN_PRINTF)(const char *format, ...);
typedef void  (*AEC_CALLBACK_FUN_RTC_DELAY)(uint32_t ticks);
typedef void  (*AEC_CALLBACK_FUN_NOTIFY)(uint32_t event);

typedef enum {
	AEC_TYPE_UNKNOWN = 0,
	AEC_TYPE_1,
	AEC_TYPE_2
} T_AEC_TYPE;

typedef struct {
	AEC_CALLBACK_FUN_MALLOC		Malloc;
	AEC_CALLBACK_FUN_FREE		Free;
	AEC_CALLBACK_FUN_PRINTF		printf;
	AEC_CALLBACK_FUN_RTC_DELAY	delay;
	AEC_CALLBACK_FUN_NOTIFY		notify;
} T_AEC_CB_FUNS;

typedef struct {
	uint32_t m_aecBypass;
	uint32_t m_framelen;
	uint32_t m_tail;

	uint32_t AdcMinSpeechPow;
	uint32_t AdcSpeechHoldTime;
	uint32_t AdcSpeechMultiple;
	int32_t  AdcConvergTime;
	uint32_t AdcCutTime;

	uint32_t DacMinSpeechPow;
	uint32_t DacSpeechHoldTime;
	uint32_t DacSpeechMultiple;
	uint32_t DacConvergTime;

	uint16_t DacFadeOutThreshold;
	uint16_t DacVolume;

	uint8_t  m_PreprocessEna;
	uint8_t  _pad_agc;
	uint16_t AGClevel;
	uint16_t maxGain;
	uint16_t minGain;
} T_AEC_PARAMS;

typedef struct {
	uint32_t	m_Type;
	uint32_t	m_SampleRate;
	uint16_t	m_Channels;
	uint16_t	m_BitsPerSample;
	union {
		T_AEC_PARAMS m_aec;
	} m_Private;
} T_AEC_IN_INFO;

typedef struct {
	T_AEC_CB_FUNS	cb_fun;
	T_AEC_IN_INFO	m_info;
} T_AEC_INPUT;

typedef struct {
	void	*buf_near;
	uint32_t len_near;
	void	*buf_far;
	uint32_t len_far;
	void	*buf_out;
	uint32_t len_out;
} T_AEC_BUF;

void *AECLib_Open(T_AEC_INPUT *info);
int   AECLib_Close(void *p_aec);
void  AECLib_AdcInt(void *p_aec, uint8_t *in, int32_t len);
void  AECLib_DacInt(void *p_aec, uint8_t *in, int32_t len);
int   AECLib_Control(void *p_aec, T_AEC_BUF *p_aec_buf);
int   AECLib_FarPreprocess(void *p_aec, T_AEC_BUF *p_aec_buf);
int   AECLib_SetDaVolume(void *p_aec, uint16_t volume);

#ifdef __cplusplus
}
#endif

#endif
