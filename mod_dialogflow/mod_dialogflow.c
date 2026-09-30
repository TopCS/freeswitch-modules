/* 
 *
 * mod_dialogflow.c -- Freeswitch module for running a google dialogflow
 *
 */
#include "mod_dialogflow.h"
#include "google_glue.h"
#include "common/fs_event_utils.h"
#include "build_info.h"
#include <switch_json.h>

#define DEFAULT_INTENT_TIMEOUT_SECS (30)
#define DIALOGFLOW_INTENT "dialogflow_intent"
#define DIALOGFLOW_INTENT_AUDIO_FILE "dialogflow_intent_audio_file"
#define DIALOGFLOW_API_CAPTURE_SYNTAX "<uuid> <start_ms> <duration_ms> [tag]"
#define DIALOGFLOW_API_BACKCHANNEL_SYNTAX "<uuid> start <audio-file> | <uuid> start-operation <lookup|payments> | <uuid> stop"
#define DIALOGFLOW_EVENT_BACKCHANNEL "mod_dialogflow::backchannel"

#ifndef MOD_DIALOGFLOW_VERSION
#define MOD_DIALOGFLOW_VERSION "unknown"
#endif
#ifndef MOD_DIALOGFLOW_GIT_HASH
#define MOD_DIALOGFLOW_GIT_HASH "unknown"
#endif
#ifndef MOD_DIALOGFLOW_BUILD_DATE
#define MOD_DIALOGFLOW_BUILD_DATE "unknown"
#endif
#ifndef MOD_DIALOGFLOW_BUILD_TYPE
#define MOD_DIALOGFLOW_BUILD_TYPE "unknown"
#endif

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dialogflow_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_dialogflow_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_dialogflow_load);

SWITCH_MODULE_DEFINITION(mod_dialogflow, mod_dialogflow_load, mod_dialogflow_shutdown, NULL);

static switch_status_t do_stop(switch_core_session_t *session);
static switch_event_node_t *g_backchannel_event_node = NULL;
static switch_bool_t g_reserved_backchannel = SWITCH_FALSE;

static switch_status_t dialogflow_backchannel_control(switch_core_session_t *session, const char *action, const char *value)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *uuid = switch_core_session_get_uuid(session);
	const char *active = switch_channel_get_variable(channel, "DIALOGFLOW_BACKCHANNEL_ACTIVE");
	char args[MAX_PATHLEN + 300] = { 0 };
	switch_stream_handle_t api_stream = { 0 };
	switch_status_t status = SWITCH_STATUS_FALSE;

	if (!strcmp(action, "start-operation")) {
		if (zstr(value) || (strcmp(value, "lookup") && strcmp(value, "payments"))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
				"Invalid backchannel operation; expected lookup or payments\n");
			return SWITCH_STATUS_FALSE;
		}
		if (!switch_channel_ready(channel)) return SWITCH_STATUS_FALSE;
		if (active && switch_true(active)) return SWITCH_STATUS_SUCCESS;
		switch_channel_set_variable(channel, "DIALOGFLOW_BACKCHANNEL_ACTIVE", "true");
		switch_channel_set_variable(channel, "DIALOGFLOW_BACKCHANNEL_OPERATION", "true");
		status = google_dialogflow_backchannel_start(session, value);
		if (status != SWITCH_STATUS_SUCCESS) {
			switch_channel_set_variable(channel, "DIALOGFLOW_BACKCHANNEL_ACTIVE", NULL);
			switch_channel_set_variable(channel, "DIALOGFLOW_BACKCHANNEL_OPERATION", NULL);
		}
	} else if (!strcmp(action, "start")) {
		if (!value || zstr(value) || strlen(value) > MAX_PATHLEN || strpbrk(value, " \t\r\n")) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
				"Invalid backchannel audio file path\n");
			return SWITCH_STATUS_FALSE;
		}
		if (!switch_channel_ready(channel)) {
			return SWITCH_STATUS_FALSE;
		}
		if (active && switch_true(active)) {
			return SWITCH_STATUS_SUCCESS;
		}
		switch_channel_set_variable(channel, "DIALOGFLOW_BACKCHANNEL_OPERATION", NULL);
		switch_channel_set_variable(channel, "DIALOGFLOW_BACKCHANNEL_ACTIVE", "true");
		snprintf(args, sizeof(args), "%s %s aleg", uuid, value);
		SWITCH_STANDARD_STREAM(api_stream);
		status = switch_api_execute("uuid_broadcast", args, NULL, &api_stream);
		if (status != SWITCH_STATUS_SUCCESS || (api_stream.data && !strncmp(api_stream.data, "-ERR", 4))) {
			status = SWITCH_STATUS_FALSE;
			switch_channel_set_variable(channel, "DIALOGFLOW_BACKCHANNEL_ACTIVE", NULL);
		}
	} else if (!strcmp(action, "stop")) {
		google_dialogflow_backchannel_stop(session);
		status = SWITCH_STATUS_SUCCESS;
	}

	switch_safe_free(api_stream.data);
	return status;
}

static void dialogflow_backchannel_event_handler(switch_event_t *event)
{
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	const char *action = switch_event_get_header(event, "Action");
	const char *audio_file = switch_event_get_header(event, "Audio-File");
	const char *operation = switch_event_get_header(event, "Operation");
	switch_core_session_t *session;
	switch_status_t status;

	if (zstr(uuid) || zstr(action) || (strcmp(action, "start") && strcmp(action, "stop")) ||
		(!strcmp(action, "start") && ((!zstr(audio_file) && !zstr(operation)) || (zstr(audio_file) && zstr(operation))))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"Ignoring malformed %s event (start requires exactly one of Audio-File or Operation)\n", DIALOGFLOW_EVENT_BACKCHANNEL);
		return;
	}

	if (!(session = switch_core_session_locate(uuid))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"Backchannel event for unknown channel %s\n", uuid);
		return;
	}

	status = dialogflow_backchannel_control(session,
		!strcmp(action, "start") && !zstr(operation) ? "start-operation" : action,
		!strcmp(action, "start") && !zstr(operation) ? operation : audio_file);
	if (status == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
			"Handled backchannel event action=%s\n", action);
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
			"Failed backchannel event action=%s\n", action);
	}
	switch_core_session_rwunlock(session);
}

static const char* mod_dialogflow_version_str(void) {
    return "mod_dialogflow/" MOD_DIALOGFLOW_VERSION " (git " MOD_DIALOGFLOW_GIT_HASH ", built " MOD_DIALOGFLOW_BUILD_DATE ", " MOD_DIALOGFLOW_BUILD_TYPE ")";
}

SWITCH_STANDARD_API(dialogflow_api_version_function)
{
    stream->write_function(stream, "%s\n", mod_dialogflow_version_str());
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(dialogflow_api_capture_function);

SWITCH_STANDARD_API(dialogflow_api_backchannel_function)
{
	char *mycmd = NULL, *argv[4] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_core_session_t *lsession = NULL;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (argc < 2 || argc > 3 || (strcmp(argv[1], "start") && strcmp(argv[1], "start-operation") && strcmp(argv[1], "stop")) ||
		((!strcmp(argv[1], "start") || !strcmp(argv[1], "start-operation")) && (argc != 3 || zstr(argv[2]))) ||
		(!strcmp(argv[1], "stop") && argc != 2)) {
		stream->write_function(stream, "-USAGE: %s\n", DIALOGFLOW_API_BACKCHANNEL_SYNTAX);
		goto done;
	}

	if (!(lsession = switch_core_session_locate(argv[0]))) {
		stream->write_function(stream, "-ERR Channel not found\n");
		goto done;
	}
	status = dialogflow_backchannel_control(lsession, argv[1], argc == 3 ? argv[2] : NULL);

	if (status != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR Backchannel operation failed\n");
	} else {
		stream->write_function(stream, "+OK Backchannel %s\n", !strcmp(argv[1], "stop") ? "stopped" : "started");
	}

done:
	if (lsession) switch_core_session_rwunlock(lsession);
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

static const fs_channel_var_header_map_t k_dialogflow_event_headers[] = {
    { "DF_SESSION_PATH", "DF-Session-Path" },
    { "DF_SESSION_ID", "DF-Session-Id" },
    { "DF_PROJECT", "DF-Project" },
    { "DF_AGENT", "DF-Agent" },
    { "DF_REGION", "DF-Region" },
    { "DF_ENVIRONMENT", "DF-Environment" },
    { "DF_CHANNEL", "DF-Channel" },
    { "DF_RESPONSE_ID", "DF-Response-Id" },
    { "DF_INTENT", "DF-Intent" },
    { "DF_PAGE", "DF-Page" },
    { "DF_AUDIO_PATH", "DF-Audio-Path" },
};

static void responseHandler(switch_core_session_t* session, const char * type, char * json) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "json payload for type %s: %s.\n", type, json);
    fs_fire_custom_event_with_body(session, type, json, k_dialogflow_event_headers,
        sizeof(k_dialogflow_event_headers) / sizeof(k_dialogflow_event_headers[0]));
}
static void errorHandler(switch_core_session_t* session, const char * json) {
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Dialogflow error: %s\n", json);

    fs_fire_custom_event_with_body(session, DIALOGFLOW_EVENT_ERROR, json, NULL, 0);

    // Avoid re-entrant stop: if a stop is already in progress, skip
    const char* stopping = switch_channel_get_variable(channel, "DF_STOPPING");
    if (stopping && switch_true(stopping)) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Dialogflow stop already in progress; skipping additional stop from error handler.\n");
        return;
    }

	do_stop(session);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_INIT.\n");
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		{
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_CLOSE.\n");

			google_dialogflow_session_stop(session, 1);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Finished SWITCH_ABC_TYPE_CLOSE.\n");
		}
		break;
	
	case SWITCH_ABC_TYPE_READ:

		return google_dialogflow_frame(bug, user_data);
		break;

	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

static switch_status_t start_capture(switch_core_session_t *session, switch_media_bug_flag_t flags, char* lang, char*projectId, char* event, char* text)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_codec_implementation_t read_impl = { 0 };
	struct cap_cb *cb = NULL;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (switch_channel_get_private(channel, MY_BUG_NAME)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "a dialogflow is already running on this channel, we will stop it.\n");
		do_stop(session);
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "channel must have at least early media to run dialogflow.\n");
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "starting dialogflow with project %s, language %s, event %s, text %s.\n", 
		projectId, lang, event, text);

	switch_core_session_get_read_impl(session, &read_impl);
	if (SWITCH_STATUS_FALSE == google_dialogflow_session_init(session, responseHandler, errorHandler, 
		read_impl.samples_per_second, lang, projectId, event, text, &cb)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing google dialogflow session.\n");
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	if ((status = switch_core_media_bug_add(session, "dialogflow", NULL, capture_callback, (void *) cb, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error adding bug.\n");
		status = SWITCH_STATUS_FALSE;
		goto done;
	}
	switch_channel_set_private(channel, MY_BUG_NAME, bug);

done:
	if (status == SWITCH_STATUS_FALSE) {
		if (cb) destroyChannelUserData(cb);
	}

	return status;
}

static switch_status_t do_stop(switch_core_session_t *session)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = switch_channel_get_private(channel, MY_BUG_NAME);

	if (bug) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Received user command command to stop dialogflow.\n");
		// Mark as stopping to prevent re-entrant stops
		switch_channel_set_variable(channel, "DF_STOPPING", "true");
		status = google_dialogflow_session_stop(session, 0);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "stopped dialogflow.\n");
	}

	return status;
}

#define DIALOGFLOW_API_START_SYNTAX "<uuid> project-id lang-code [event]"
SWITCH_STANDARD_API(dialogflow_api_start_function)
{
	char *mycmd = NULL, *argv[10] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_media_bug_flag_t flags = SMBF_READ_STREAM | SMBF_READ_STREAM | SMBF_READ_PING;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "command %s\n", cmd);
	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (zstr(cmd) || argc < 3) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n", cmd, argv[0], argv[1]);
		stream->write_function(stream, "-USAGE: %s\n", DIALOGFLOW_API_START_SYNTAX);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;

		if ((lsession = switch_core_session_locate(argv[0]))) {
			char *event = NULL;
			char *text = NULL;
			char *projectId = argv[1];
			char *lang = argv[2];
			if (argc > 3) {
				event = argv[3];
			}
			if (argc > 4) {
				if (0 == strcmp("none", event)) {
					event = NULL;
				}
				text = argv[4];
			}
			status = start_capture(lsession, flags, lang, projectId, event, text);
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

#define MAX_CAPTURE_ARGS 5
SWITCH_STANDARD_API(dialogflow_api_capture_function)
{
	char *mycmd = NULL, *argv[MAX_CAPTURE_ARGS] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, MAX_CAPTURE_ARGS);
	}

	if (zstr(cmd) || argc < 3) {
		stream->write_function(stream, "-USAGE: %s\n", DIALOGFLOW_API_CAPTURE_SYNTAX);
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
	switch_media_bug_t* bug = switch_channel_get_private(channel, MY_BUG_NAME);
	if (!bug) {
		stream->write_function(stream, "-ERR dialogflow not active on %s\n", uuid);
		goto cleanup;
	}

	struct cap_cb* cb = (struct cap_cb*) switch_core_media_bug_get_user_data(bug);
	if (!cb) {
		stream->write_function(stream, "-ERR dialogflow session unavailable\n");
		goto cleanup;
	}

	char path[MAX_PATHLEN] = {0};
	uint64_t actual_start = 0;
	uint64_t actual_end = 0;

	status = google_dialogflow_capture_snippet(cb, lsession, start_ms, duration_ms, tag, path, sizeof(path), &actual_start, &actual_end);
	if (status != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR capture failed (spool disabled or window unavailable)\n");
		goto cleanup;
	}

	cJSON* body = cJSON_CreateObject();
	if (body) {
		cJSON_AddStringToObject(body, "path", path);
		cJSON_AddNumberToObject(body, "start_ms", (double)actual_start);
		cJSON_AddNumberToObject(body, "end_ms", (double)actual_end);
		cJSON_AddNumberToObject(body, "duration_ms", (double)((actual_end > actual_start) ? (actual_end - actual_start) : 0));
		cJSON_AddStringToObject(body, "tag", tag);
		cJSON_AddStringToObject(body, "session_id", uuid);
		char* payload = cJSON_PrintUnformatted(body);
		cJSON_Delete(body);

		if (payload) {
			switch_event_t* event;
			switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, DIALOGFLOW_EVENT_AUDIO_SNIPPET);
			switch_channel_event_set_data(channel, event);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Response", payload);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "DF-Audio-Path", path);
			switch_event_add_body(event, "%s", payload);
			switch_event_fire(&event);

			stream->write_function(stream, "+OK %s\n", path);
			free(payload);
		} else {
			stream->write_function(stream, "-ERR unable to allocate JSON payload\n");
			status = SWITCH_STATUS_FALSE;
			goto cleanup;
		}
	} else {
		stream->write_function(stream, "-ERR unable to allocate JSON body\n");
		status = SWITCH_STATUS_FALSE;
		goto cleanup;
	}

cleanup:
	switch_core_session_rwunlock(lsession);

done:
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

#define DIALOGFLOW_API_STOP_SYNTAX "<uuid>"
SWITCH_STANDARD_API(dialogflow_api_stop_function)
{
	char *mycmd = NULL, *argv[10] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "command %s\n", cmd);
	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (zstr(cmd) || argc != 1) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n", cmd, argv[0], argv[1]);
		stream->write_function(stream, "-USAGE: %s\n", DIALOGFLOW_API_STOP_SYNTAX);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;

		if ((lsession = switch_core_session_locate(argv[0]))) {
			status = do_stop(lsession);
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


/* Macro expands to: switch_status_t mod_dialogflow_load(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool) */
static switch_bool_t g_reserved_intent = SWITCH_FALSE;
static switch_bool_t g_reserved_transcription = SWITCH_FALSE;
static switch_bool_t g_reserved_eou = SWITCH_FALSE;
static switch_bool_t g_reserved_audio = SWITCH_FALSE;
static switch_bool_t g_reserved_error = SWITCH_FALSE;
static switch_bool_t g_reserved_transfer = SWITCH_FALSE;
static switch_bool_t g_reserved_end_session = SWITCH_FALSE;
static switch_bool_t g_reserved_webhook_error = SWITCH_FALSE;
static switch_bool_t g_reserved_page = SWITCH_FALSE;
static switch_bool_t g_reserved_audio_snippet = SWITCH_FALSE;

SWITCH_MODULE_LOAD_FUNCTION(mod_dialogflow_load)
{
    switch_api_interface_t *api_interface;

	/* create/register custom event message types */
	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_INTENT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_INTENT);
		return SWITCH_STATUS_TERM;
	} else g_reserved_intent = SWITCH_TRUE;
	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_TRANSCRIPTION) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_TRANSCRIPTION);
		return SWITCH_STATUS_TERM;
	} else g_reserved_transcription = SWITCH_TRUE;
	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_END_OF_UTTERANCE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_END_OF_UTTERANCE);
		return SWITCH_STATUS_TERM;
	} else g_reserved_eou = SWITCH_TRUE;
	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_AUDIO_PROVIDED) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_AUDIO_PROVIDED);
		return SWITCH_STATUS_TERM;
	} else g_reserved_audio = SWITCH_TRUE;
	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_AUDIO_SNIPPET) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_AUDIO_SNIPPET);
		return SWITCH_STATUS_TERM;
	} else g_reserved_audio_snippet = SWITCH_TRUE;

	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_ERROR) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_ERROR);
		return SWITCH_STATUS_TERM;
	} else g_reserved_error = SWITCH_TRUE;

	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_WEBHOOK_ERROR) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_WEBHOOK_ERROR);
		return SWITCH_STATUS_TERM;
	} else g_reserved_webhook_error = SWITCH_TRUE;

	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_PAGE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_PAGE);
		return SWITCH_STATUS_TERM;
	} else g_reserved_page = SWITCH_TRUE;

	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_TRANSFER) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_TRANSFER);
		return SWITCH_STATUS_TERM;
	} else g_reserved_transfer = SWITCH_TRUE;

	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_END_SESSION) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_END_SESSION);
		return SWITCH_STATUS_TERM;
	} else g_reserved_end_session = SWITCH_TRUE;
	if (switch_event_reserve_subclass(DIALOGFLOW_EVENT_BACKCHANNEL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", DIALOGFLOW_EVENT_BACKCHANNEL);
		return SWITCH_STATUS_TERM;
	} else g_reserved_backchannel = SWITCH_TRUE;


	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Google Dialogflow API loading.. %s\n", mod_dialogflow_version_str());

  if (SWITCH_STATUS_FALSE == google_dialogflow_init()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed initializing google dialogflow interface\n");
	}

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Google Dialogflow API successfully loaded: %s\n", mod_dialogflow_version_str());

    SWITCH_ADD_API(api_interface, "dialogflow_start", "Start a google dialogflow", dialogflow_api_start_function, DIALOGFLOW_API_START_SYNTAX);
    SWITCH_ADD_API(api_interface, "dialogflow_capture", "Capture a caller-side audio snippet for external ASR", dialogflow_api_capture_function, DIALOGFLOW_API_CAPTURE_SYNTAX);
    SWITCH_ADD_API(api_interface, "dialogflow_backchannel", "Start or stop a backchannel prompt", dialogflow_api_backchannel_function, DIALOGFLOW_API_BACKCHANNEL_SYNTAX);
    SWITCH_ADD_API(api_interface, "dialogflow_stop", "Terminate a google dialogflow", dialogflow_api_stop_function, DIALOGFLOW_API_STOP_SYNTAX);
    SWITCH_ADD_API(api_interface, "dialogflow_version", "Show mod_dialogflow version", dialogflow_api_version_function, "");

	switch_console_set_complete("add dialogflow_stop");
	switch_console_set_complete("add dialogflow_start project lang");
	switch_console_set_complete("add dialogflow_start project lang timeout-secs");
	switch_console_set_complete("add dialogflow_start project lang timeout-secs event");

	if (switch_event_bind_removable("mod_dialogflow_backchannel", SWITCH_EVENT_CUSTOM,
		DIALOGFLOW_EVENT_BACKCHANNEL, dialogflow_backchannel_event_handler, NULL,
		&g_backchannel_event_node) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT,
			"Failed to bind %s event listener\n", DIALOGFLOW_EVENT_BACKCHANNEL);
		return SWITCH_STATUS_TERM;
	}

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_dialogflow_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dialogflow_shutdown)
{
	if (g_backchannel_event_node) {
		switch_event_unbind(&g_backchannel_event_node);
	}
	google_dialogflow_cleanup();

	if (g_reserved_intent) { switch_event_free_subclass(DIALOGFLOW_EVENT_INTENT); g_reserved_intent = SWITCH_FALSE; }
	if (g_reserved_transcription) { switch_event_free_subclass(DIALOGFLOW_EVENT_TRANSCRIPTION); g_reserved_transcription = SWITCH_FALSE; }
	if (g_reserved_eou) { switch_event_free_subclass(DIALOGFLOW_EVENT_END_OF_UTTERANCE); g_reserved_eou = SWITCH_FALSE; }
	if (g_reserved_audio) { switch_event_free_subclass(DIALOGFLOW_EVENT_AUDIO_PROVIDED); g_reserved_audio = SWITCH_FALSE; }
	if (g_reserved_audio_snippet) { switch_event_free_subclass(DIALOGFLOW_EVENT_AUDIO_SNIPPET); g_reserved_audio_snippet = SWITCH_FALSE; }
	if (g_reserved_error) { switch_event_free_subclass(DIALOGFLOW_EVENT_ERROR); g_reserved_error = SWITCH_FALSE; }
	if (g_reserved_transfer) { switch_event_free_subclass(DIALOGFLOW_EVENT_TRANSFER); g_reserved_transfer = SWITCH_FALSE; }
	if (g_reserved_end_session) { switch_event_free_subclass(DIALOGFLOW_EVENT_END_SESSION); g_reserved_end_session = SWITCH_FALSE; }
	if (g_reserved_backchannel) { switch_event_free_subclass(DIALOGFLOW_EVENT_BACKCHANNEL); g_reserved_backchannel = SWITCH_FALSE; }
    if (g_reserved_webhook_error) { switch_event_free_subclass(DIALOGFLOW_EVENT_WEBHOOK_ERROR); g_reserved_webhook_error = SWITCH_FALSE; }
    if (g_reserved_page) { switch_event_free_subclass(DIALOGFLOW_EVENT_PAGE); g_reserved_page = SWITCH_FALSE; }

	return SWITCH_STATUS_SUCCESS;
}
