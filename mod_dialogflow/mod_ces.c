/*
 *
 * mod_ces.c -- FreeSWITCH module scaffold for Customer Engagement Suite
 *
 */
#include "mod_ces.h"
#include "ces_glue.h"
#include "common/fs_event_utils.h"
#include "build_info.h"
#include <switch_json.h>

#ifndef MOD_CES_VERSION
#define MOD_CES_VERSION "unknown"
#endif
#ifndef MOD_CES_GIT_HASH
#define MOD_CES_GIT_HASH "unknown"
#endif
#ifndef MOD_CES_BUILD_DATE
#define MOD_CES_BUILD_DATE "unknown"
#endif
#ifndef MOD_CES_BUILD_TYPE
#define MOD_CES_BUILD_TYPE "unknown"
#endif

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ces_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_ces_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_ces_load);

SWITCH_MODULE_DEFINITION(mod_ces, mod_ces_load, mod_ces_shutdown, NULL);

static switch_status_t ces_do_stop(switch_core_session_t *session);
static switch_bool_t g_reserved_transcription = SWITCH_FALSE;
static switch_bool_t g_reserved_response = SWITCH_FALSE;
static switch_bool_t g_reserved_audio = SWITCH_FALSE;
static switch_bool_t g_reserved_end_of_turn = SWITCH_FALSE;
static switch_bool_t g_reserved_interruption = SWITCH_FALSE;
static switch_bool_t g_reserved_error = SWITCH_FALSE;
static switch_bool_t g_reserved_audio_snippet = SWITCH_FALSE;

static const char* mod_ces_version_str(void) {
	return "mod_ces/" MOD_CES_VERSION " (git " MOD_CES_GIT_HASH ", built " MOD_CES_BUILD_DATE ", " MOD_CES_BUILD_TYPE ")";
}

SWITCH_STANDARD_API(ces_api_version_function)
{
	stream->write_function(stream, "%s\n", mod_ces_version_str());
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(ces_api_capture_function);

static const fs_channel_var_header_map_t k_ces_event_headers[] = {
	{ "CES_SESSION_ID", "CES-Session-Id" },
	{ "CES_PROJECT", "CES-Project" },
	{ "CES_APP", "CES-App" },
	{ "CES_LOCATION", "CES-Location" },
};

static void ces_response_handler(switch_core_session_t* session, const char * type, char * json) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "json payload for type %s: %s.\n", type, json);
	fs_fire_custom_event_with_body(session, type, json, k_ces_event_headers,
		sizeof(k_ces_event_headers) / sizeof(k_ces_event_headers[0]));
}

static void ces_error_handler(switch_core_session_t* session, const char * json) {
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "CES error: %s\n", json);
	fs_fire_custom_event_with_body(session, CES_EVENT_ERROR, json, k_ces_event_headers,
		sizeof(k_ces_event_headers) / sizeof(k_ces_event_headers[0]));

	ces_do_stop(session);
}

static switch_bool_t ces_capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_INIT.\n");
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_CLOSE.\n");
		ces_session_stop(session, 1);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Finished SWITCH_ABC_TYPE_CLOSE.\n");
		break;

	case SWITCH_ABC_TYPE_READ:
		return ces_frame(bug, user_data);

	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

static switch_status_t ces_start_capture(switch_core_session_t *session, switch_media_bug_flag_t flags, char* lang, char* projectId, char* appId, char* location, char* event, char* text)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_codec_implementation_t read_impl = { 0 };
	struct ces_cap_cb *cb = NULL;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (switch_channel_get_private(channel, MY_CES_BUG_NAME)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "a ces session is already running on this channel, we will stop it.\n");
		ces_do_stop(session);
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "channel must have at least early media to run ces.\n");
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "starting ces with project %s, app %s, location %s, language %s, event %s, text %s.\n",
		projectId, appId, location, lang, event, text);

	switch_core_session_get_read_impl(session, &read_impl);
	if (SWITCH_STATUS_FALSE == ces_session_init(session, ces_response_handler, ces_error_handler,
		read_impl.samples_per_second, lang, projectId, appId, location, event, text, &cb)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing ces session.\n");
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	if ((status = switch_core_media_bug_add(session, "ces", NULL, ces_capture_callback, (void *) cb, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error adding bug.\n");
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	switch_channel_set_private(channel, MY_CES_BUG_NAME, bug);

done:
	if (status == SWITCH_STATUS_FALSE) {
		if (cb) destroyCesChannelUserData(cb);
	}

	return status;
}

static switch_status_t ces_do_stop(switch_core_session_t *session)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = switch_channel_get_private(channel, MY_CES_BUG_NAME);

	if (bug) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Received user command to stop ces.\n");
		status = ces_session_stop(session, 0);
	}

	return status;
}

#define CES_API_START_SYNTAX "<uuid> project-id app-id location lang-code [event] [text]"
SWITCH_STANDARD_API(ces_api_start_function)
{
	char *mycmd = NULL, *argv[10] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_media_bug_flag_t flags = SMBF_READ_STREAM | SMBF_READ_STREAM | SMBF_READ_PING;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (zstr(cmd) || argc < 5) {
		stream->write_function(stream, "-USAGE: %s\n", CES_API_START_SYNTAX);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;

		if ((lsession = switch_core_session_locate(argv[0]))) {
			char *event = NULL;
			char *text = NULL;
			char *projectId = argv[1];
			char *appId = argv[2];
			char *location = argv[3];
			char *lang = argv[4];
			if (argc > 5) {
				event = argv[5];
			}
			if (argc > 6) {
				text = argv[6];
			}
			status = ces_start_capture(lsession, flags, lang, projectId, appId, location, event, text);
			switch_core_session_rwunlock(lsession);
		}
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "+OK Success\n");
	} else {
		stream->write_function(stream, "-ERR Operation Failed\n");
	}

done:
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

#define CES_API_CAPTURE_SYNTAX "<uuid> <start_ms> <duration_ms> [tag]"
SWITCH_STANDARD_API(ces_api_capture_function)
{
	char *mycmd = NULL, *argv[5] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, 5);
	}

	if (zstr(cmd) || argc < 3) {
		stream->write_function(stream, "-USAGE: %s\n", CES_API_CAPTURE_SYNTAX);
		goto done;
	}

	const char* uuid = argv[0];
	uint64_t start_ms = (uint64_t) strtoull(argv[1], NULL, 10);
	uint64_t duration_ms = (uint64_t) strtoull(argv[2], NULL, 10);
	const char* tag = (argc >= 4 && !zstr(argv[3])) ? argv[3] : "snippet";

	switch_core_session_t* lsession = switch_core_session_locate(uuid);
	if (!lsession) {
		stream->write_function(stream, "-ERR invalid uuid %s\n", uuid);
		goto done;
	}

	switch_channel_t* channel = switch_core_session_get_channel(lsession);
	switch_media_bug_t* bug = switch_channel_get_private(channel, MY_CES_BUG_NAME);
	if (!bug) {
		stream->write_function(stream, "-ERR ces not active on %s\n", uuid);
		goto cleanup;
	}

	struct ces_cap_cb* cb = (struct ces_cap_cb*) switch_core_media_bug_get_user_data(bug);
	if (!cb) {
		stream->write_function(stream, "-ERR ces session unavailable\n");
		goto cleanup;
	}

	char path[MAX_CES_PATHLEN] = {0};
	uint64_t actual_start = 0;
	uint64_t actual_end = 0;

	status = ces_capture_snippet(cb, lsession, start_ms, duration_ms, tag, path, sizeof(path), &actual_start, &actual_end);
	if (status != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR capture failed (spool disabled or window unavailable)\n");
		goto cleanup;
	}

	cJSON* body = cJSON_CreateObject();
	if (!body) {
		stream->write_function(stream, "-ERR unable to allocate JSON body\n");
		status = SWITCH_STATUS_FALSE;
		goto cleanup;
	}
	cJSON_AddStringToObject(body, "path", path);
	cJSON_AddNumberToObject(body, "start_ms", (double)actual_start);
	cJSON_AddNumberToObject(body, "end_ms", (double)actual_end);
	cJSON_AddNumberToObject(body, "duration_ms", (double)((actual_end > actual_start) ? (actual_end - actual_start) : 0));
	cJSON_AddStringToObject(body, "tag", tag);
	cJSON_AddStringToObject(body, "session_id", uuid);

	char* payload = cJSON_PrintUnformatted(body);
	cJSON_Delete(body);
	if (!payload) {
		stream->write_function(stream, "-ERR unable to allocate JSON payload\n");
		status = SWITCH_STATUS_FALSE;
		goto cleanup;
	}

	fs_fire_custom_event_with_body(lsession, CES_EVENT_AUDIO_SNIPPET, payload, k_ces_event_headers,
		sizeof(k_ces_event_headers) / sizeof(k_ces_event_headers[0]));
	stream->write_function(stream, "+OK %s\n", path);
	free(payload);

cleanup:
	switch_core_session_rwunlock(lsession);

done:
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

#define CES_API_STOP_SYNTAX "<uuid>"
SWITCH_STANDARD_API(ces_api_stop_function)
{
	char *mycmd = NULL, *argv[10] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (zstr(cmd) || argc != 1) {
		stream->write_function(stream, "-USAGE: %s\n", CES_API_STOP_SYNTAX);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;

		if ((lsession = switch_core_session_locate(argv[0]))) {
			status = ces_do_stop(lsession);
			switch_core_session_rwunlock(lsession);
		}
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "+OK Success\n");
	} else {
		stream->write_function(stream, "-ERR Operation Failed\n");
	}

done:
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_ces_load)
{
	switch_api_interface_t *api_interface;

	if (switch_event_reserve_subclass(CES_EVENT_TRANSCRIPTION) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", CES_EVENT_TRANSCRIPTION);
		return SWITCH_STATUS_TERM;
	} else g_reserved_transcription = SWITCH_TRUE;
	if (switch_event_reserve_subclass(CES_EVENT_RESPONSE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", CES_EVENT_RESPONSE);
		return SWITCH_STATUS_TERM;
	} else g_reserved_response = SWITCH_TRUE;
	if (switch_event_reserve_subclass(CES_EVENT_AUDIO_PROVIDED) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", CES_EVENT_AUDIO_PROVIDED);
		return SWITCH_STATUS_TERM;
	} else g_reserved_audio = SWITCH_TRUE;
	if (switch_event_reserve_subclass(CES_EVENT_END_OF_TURN) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", CES_EVENT_END_OF_TURN);
		return SWITCH_STATUS_TERM;
	} else g_reserved_end_of_turn = SWITCH_TRUE;
	if (switch_event_reserve_subclass(CES_EVENT_INTERRUPTION) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", CES_EVENT_INTERRUPTION);
		return SWITCH_STATUS_TERM;
	} else g_reserved_interruption = SWITCH_TRUE;
	if (switch_event_reserve_subclass(CES_EVENT_ERROR) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", CES_EVENT_ERROR);
		return SWITCH_STATUS_TERM;
	} else g_reserved_error = SWITCH_TRUE;
	if (switch_event_reserve_subclass(CES_EVENT_AUDIO_SNIPPET) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", CES_EVENT_AUDIO_SNIPPET);
		return SWITCH_STATUS_TERM;
	} else g_reserved_audio_snippet = SWITCH_TRUE;

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "CES API loading.. %s\n", mod_ces_version_str());
	if (SWITCH_STATUS_FALSE == ces_init()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed initializing ces interface\n");
		return SWITCH_STATUS_FALSE;
	}

	SWITCH_ADD_API(api_interface, "ces_start", "Start a CES session", ces_api_start_function, CES_API_START_SYNTAX);
	SWITCH_ADD_API(api_interface, "ces_capture", "Capture a caller-side audio snippet for external ASR", ces_api_capture_function, CES_API_CAPTURE_SYNTAX);
	SWITCH_ADD_API(api_interface, "ces_stop", "Terminate a CES session", ces_api_stop_function, CES_API_STOP_SYNTAX);
	SWITCH_ADD_API(api_interface, "ces_version", "Show mod_ces version", ces_api_version_function, "");
	switch_console_set_complete("add ces_stop");
	switch_console_set_complete("add ces_start project app location lang");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "CES API scaffold successfully loaded: %s\n", mod_ces_version_str());
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ces_shutdown)
{
	ces_cleanup();
	if (g_reserved_transcription) { switch_event_free_subclass(CES_EVENT_TRANSCRIPTION); g_reserved_transcription = SWITCH_FALSE; }
	if (g_reserved_response) { switch_event_free_subclass(CES_EVENT_RESPONSE); g_reserved_response = SWITCH_FALSE; }
	if (g_reserved_audio) { switch_event_free_subclass(CES_EVENT_AUDIO_PROVIDED); g_reserved_audio = SWITCH_FALSE; }
	if (g_reserved_end_of_turn) { switch_event_free_subclass(CES_EVENT_END_OF_TURN); g_reserved_end_of_turn = SWITCH_FALSE; }
	if (g_reserved_interruption) { switch_event_free_subclass(CES_EVENT_INTERRUPTION); g_reserved_interruption = SWITCH_FALSE; }
	if (g_reserved_error) { switch_event_free_subclass(CES_EVENT_ERROR); g_reserved_error = SWITCH_FALSE; }
	if (g_reserved_audio_snippet) { switch_event_free_subclass(CES_EVENT_AUDIO_SNIPPET); g_reserved_audio_snippet = SWITCH_FALSE; }
	return SWITCH_STATUS_SUCCESS;
}
