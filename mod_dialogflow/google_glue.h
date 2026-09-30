#ifndef __GOOGLE_GLUE_H__
#define __GOOGLE_GLUE_H__

struct cap_cb;

#ifdef __cplusplus
extern "C" {
#endif

switch_status_t google_dialogflow_init();
switch_status_t google_dialogflow_cleanup();
switch_status_t google_dialogflow_session_init(switch_core_session_t *session, responseHandler_t responseHandler, errorHandler_t errorHandler, 
		uint32_t samples_per_second, char* lang, char* projectId, char* welcomeEvent, char *text, struct cap_cb **cb);
switch_status_t google_dialogflow_session_stop(switch_core_session_t *session, int channelIsClosing);
switch_status_t google_dialogflow_backchannel_start(switch_core_session_t *session, const char *operation);
switch_status_t google_dialogflow_backchannel_warmup(switch_core_session_t *session);
void google_dialogflow_backchannel_stop(switch_core_session_t *session);
switch_bool_t google_dialogflow_frame(switch_media_bug_t *bug, void* user_data);
switch_status_t google_dialogflow_capture_snippet(struct cap_cb* cb, switch_core_session_t* session,
        uint64_t start_ms, uint64_t duration_ms, const char* tag,
        char* out_path, size_t out_path_len, uint64_t* actual_start_ms, uint64_t* actual_end_ms);
void google_dialogflow_spool_cleanup(struct cap_cb* cb, switch_core_session_t* session, switch_bool_t preserve);

void destroyChannelUserData(struct cap_cb* cb);

#ifdef __cplusplus
}
#endif
#endif
