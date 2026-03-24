#ifndef __MOD_CES_H__
#define __MOD_CES_H__

#include <switch.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#define MY_CES_BUG_NAME "__ces_bug__"
#define CES_EVENT_TRANSCRIPTION "ces::transcription"
#define CES_EVENT_RESPONSE "ces::response"
#define CES_EVENT_AUDIO_PROVIDED "ces::audio_provided"
#define CES_EVENT_END_OF_TURN "ces::end_of_turn"
#define CES_EVENT_INTERRUPTION "ces::interruption"
#define CES_EVENT_ERROR "ces::error"
#define CES_EVENT_AUDIO_SNIPPET "ces::audio_snippet"

#define MAX_CES_LANG (12)
#define MAX_CES_PROJECT_ID (128)
#define MAX_CES_APP_ID (128)
#define MAX_CES_LOCATION (64)
#define MAX_CES_PATHLEN (512)

typedef void (*ces_response_handler_t)(switch_core_session_t* session, const char * type, char* json);
typedef void (*ces_error_handler_t)(switch_core_session_t* session, const char * reason);

struct ces_cap_cb {
	switch_mutex_t *mutex;
	char sessionId[256];
	SpeexResamplerState *resampler;
	void* streamer;
	ces_response_handler_t responseHandler;
	ces_error_handler_t errorHandler;
	switch_thread_t* thread;
	char lang[MAX_CES_LANG];
	char projectId[MAX_CES_PROJECT_ID];
	char appId[MAX_CES_APP_ID];
	char location[MAX_CES_LOCATION];
	switch_bool_t stopping;
};

#endif
