#ifndef __CES_GLUE_H__
#define __CES_GLUE_H__

#include "mod_ces.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ces_cap_cb;

switch_status_t ces_init(void);
switch_status_t ces_cleanup(void);
switch_status_t ces_session_init(switch_core_session_t *session, ces_response_handler_t responseHandler, ces_error_handler_t errorHandler,
		uint32_t samples_per_second, char* lang, char* projectId, char* appId, char* location, char* welcomeEvent, char *text, struct ces_cap_cb **cb);
switch_status_t ces_session_stop(switch_core_session_t *session, int channelIsClosing);
switch_bool_t ces_frame(switch_media_bug_t *bug, void* user_data);
switch_status_t ces_capture_snippet(struct ces_cap_cb* cb, switch_core_session_t* session,
		uint64_t start_ms, uint64_t duration_ms, const char* tag,
		char* out_path, size_t out_path_len, uint64_t* actual_start_ms, uint64_t* actual_end_ms);
void ces_spool_cleanup(struct ces_cap_cb* cb, switch_core_session_t* session, switch_bool_t preserve);
void destroyCesChannelUserData(struct ces_cap_cb* cb);

#ifdef __cplusplus
}
#endif

#endif
