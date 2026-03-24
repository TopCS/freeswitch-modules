#include "mod_ces.h"
#include "ces_glue.h"
#include "ces_parser.h"
#include "common/audio_spooler.h"

#include <switch.h>
#include <grpcpp/grpcpp.h>

#include "google/cloud/ces/v1beta/session_service.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ces = google::cloud::ces::v1beta;

namespace {

static std::multimap<std::string, std::string> audioFiles;
static bool hasDefaultCredentials = false;

struct PendingTurnAudio {
	int32_t turnIndex = -1;
	std::string bytes;
};

static cJSON* build_error_json(const char* msg, int code, const char* category, bool retryable, const char* details = NULL) {
	cJSON* json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "msg", msg ? msg : "");
	cJSON_AddNumberToObject(json, "code", code);
	cJSON_AddStringToObject(json, "category", category ? category : "unknown");
	cJSON_AddItemToObject(json, "retryable", cJSON_CreateBool(retryable));
	if (details && *details) {
		cJSON_AddStringToObject(json, "details", details);
	}
	return json;
}

static void emit_error_json(switch_core_session_t* session, ces_error_handler_t errorHandler,
	const char* msg, int code, const char* category, bool retryable, const char* details = NULL) {
	if (!errorHandler) {
		return;
	}
	cJSON* json = build_error_json(msg, code, category, retryable, details);
	char* body = cJSON_PrintUnformatted(json);
	cJSON_Delete(json);
	if (body) {
		errorHandler(session, body);
		free(body);
	}
}

static void categorize_status(const grpc::Status& status, const char** category, bool* retryable) {
	const char* cat = "unknown";
	bool retry = false;

	switch (status.error_code()) {
		case grpc::StatusCode::UNAUTHENTICATED:
		case grpc::StatusCode::PERMISSION_DENIED:
			cat = "auth";
			break;
		case grpc::StatusCode::RESOURCE_EXHAUSTED:
			cat = "quota";
			break;
		case grpc::StatusCode::UNAVAILABLE:
			cat = "network";
			retry = true;
			break;
		case grpc::StatusCode::DEADLINE_EXCEEDED:
			cat = "timeout";
			retry = true;
			break;
		case grpc::StatusCode::INTERNAL:
			cat = "server";
			retry = true;
			break;
		case grpc::StatusCode::NOT_FOUND:
			cat = "not_found";
			break;
		case grpc::StatusCode::UNIMPLEMENTED:
			cat = "unimplemented";
			break;
		default:
			break;
	}

	*category = cat;
	*retryable = retry;
}

static bool ces_scaffold_mode_enabled(switch_channel_t* channel) {
	const char* channelVar = switch_channel_get_variable(channel, "CES_ENABLE_SCAFFOLD_ONLY");
	if (channelVar) {
		return switch_true(channelVar);
	}
	const char* envVar = std::getenv("CES_ENABLE_SCAFFOLD_ONLY");
	return envVar && switch_true(envVar);
}

static const char* choose_endpoint(switch_channel_t* channel) {
	const char* endpoint = switch_channel_get_variable(channel, "CES_ENDPOINT");
	if (!zstr(endpoint)) {
		return endpoint;
	}
	return "ces.googleapis.com:443";
}

static std::string create_session_path(const char* projectId, const char* location, const char* appId, const char* sessionId) {
	std::ostringstream path;
	path << "projects/" << (projectId ? projectId : "")
	     << "/locations/" << (location ? location : "")
	     << "/apps/" << (appId ? appId : "")
	     << "/sessions/" << (sessionId ? sessionId : "");
	return path.str();
}

static std::string choose_audio_suffix(ces::AudioEncoding encoding) {
	switch (encoding) {
		case ces::LINEAR16:
			return ".wav";
		case ces::MULAW:
			return ".mulaw";
		case ces::ALAW:
			return ".alaw";
		default:
			return ".bin";
	}
}

static void stop_playback(switch_core_session_t* session) {
	const char* suuid = switch_core_session_get_uuid(session);
	char args[256];
	switch_stream_handle_t stream = { 0 };

	snprintf(args, sizeof(args), "%s all", suuid ? suuid : "");
	SWITCH_STANDARD_STREAM(stream);
	(void) switch_api_execute("uuid_break", args, NULL, &stream);
	switch_safe_free(stream.data);
}

static switch_status_t hanguphook(switch_core_session_t *session) {
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_channel_state_t state = switch_channel_get_state(channel);

	if (state == CS_HANGUP || state == CS_ROUTING) {
		char * sessionId = switch_core_session_get_uuid(session);
		typedef std::multimap<std::string, std::string>::iterator MMAPIterator;
		std::pair<MMAPIterator, MMAPIterator> result = audioFiles.equal_range(sessionId);
		for (MMAPIterator it = result.first; it != result.second; ++it) {
			std::remove(it->second.c_str());
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
				"ces_session_cleanup: removed audio file %s\n", it->second.c_str());
		}
		audioFiles.erase(sessionId);
		switch_core_event_hook_remove_state_change(session, hanguphook);
	}
	return SWITCH_STATUS_SUCCESS;
}

class CesStreamer {
public:
	CesStreamer(switch_core_session_t* session, const char* lang, const char* projectId, const char* appId,
		const char* location, const char* sessionId, uint32_t sampleRate, bool scaffoldOnly)
		: m_lang(lang ? lang : ""),
		  m_projectId(projectId ? projectId : ""),
		  m_appId(appId ? appId : ""),
		  m_location(location ? location : ""),
		  m_sessionId(sessionId ? sessionId : ""),
		  m_sampleRate(sampleRate),
		  m_scaffoldOnly(scaffoldOnly),
		  m_outputEncoding(ces::LINEAR16),
		  m_started(false),
		  m_paused(false),
		  m_writesDone(false),
		  m_finishCalled(false) {
		switch_channel_t* channel = switch_core_session_get_channel(session);
		const char* endpoint = choose_endpoint(channel);
		const char* deployment = switch_channel_get_variable(channel, "CES_DEPLOYMENT");
		const char* entryAgent = switch_channel_get_variable(channel, "CES_ENTRY_AGENT");
		const char* textStreaming = switch_channel_get_variable(channel, "CES_ENABLE_TEXT_STREAMING");
		const char* inputRate = switch_channel_get_variable(channel, "CES_AUDIO_SAMPLE_RATE");
		if (!zstr(deployment)) {
			m_deployment = deployment;
		}
		if (!zstr(entryAgent)) {
			m_entryAgent = entryAgent;
		}
		m_enableTextStreaming = textStreaming && switch_true(textStreaming);
		if (!zstr(inputRate)) {
			int overrideRate = atoi(inputRate);
			if (overrideRate >= 8000 && overrideRate <= 48000) {
				m_sampleRate = (uint32_t)overrideRate;
			}
		}

		(void) m_spool.configure(session, m_sessionId, m_sampleRate);
		m_sessionPath = create_session_path(m_projectId.c_str(), m_location.c_str(), m_appId.c_str(), m_sessionId.c_str());
		switch_channel_set_variable(channel, "CES_SESSION_PATH", m_sessionPath.c_str());
		switch_channel_set_variable(channel, "CES_ENDPOINT", endpoint);

		if (m_scaffoldOnly) {
			return;
		}

		std::string credsInput;
		const char* var = switch_channel_get_variable(channel, "GOOGLE_APPLICATION_CREDENTIALS");
		if (!zstr(var)) {
			credsInput = var;
		}

		if (credsInput.empty() && !hasDefaultCredentials) {
			throw std::runtime_error("missing credentials");
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
			"CES endpoint is %s, location is %s, project is %s, app is %s\n",
			endpoint, m_location.c_str(), m_projectId.c_str(), m_appId.c_str());

		if (!credsInput.empty()) {
			std::string json = credsInput;
			bool read_from_file = false;
			if (credsInput[0] == '/' || credsInput.rfind(".json") == credsInput.size() - 5) {
				std::ifstream fs(credsInput);
				if (fs.good()) {
					json.assign((std::istreambuf_iterator<char>(fs)), std::istreambuf_iterator<char>());
					read_from_file = true;
				} else if (!hasDefaultCredentials) {
					throw std::runtime_error("GOOGLE_APPLICATION_CREDENTIALS path not readable and no default credentials configured");
				} else {
					json.clear();
				}
			}
			if (!json.empty()) {
				auto callCreds = grpc::ServiceAccountJWTAccessCredentials(json, INT64_MAX);
				auto channelCreds = grpc::SslCredentials(grpc::SslCredentialsOptions());
				auto creds = grpc::CompositeChannelCredentials(channelCreds, callCreds);
				m_channel = grpc::CreateChannel(endpoint, creds);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
					"CES using %s credentials for channel\n", read_from_file ? "file" : "inline JSON");
			}
		}

		if (!m_channel) {
			auto creds = grpc::GoogleDefaultCredentials();
			m_channel = grpc::CreateChannel(endpoint, creds);
		}

		m_stub = ces::SessionService::NewStub(m_channel);
	}

	~CesStreamer() {
		try {
			m_spool.cleanup();
		} catch (...) {
		}
	}

	void startStream(switch_core_session_t* session, const char* welcomeEvent, const char* text) {
		if (m_scaffoldOnly) {
			return;
		}

		if (!zstr(welcomeEvent)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
				"CES start received event '%s', but BidiRunSession audio flow does not support event starts; ignoring it\n",
				welcomeEvent);
		}

		m_context = std::make_shared<grpc::ClientContext>();
		m_streamer = m_stub->BidiRunSession(m_context.get());

		ces::BidiSessionClientMessage configMsg;
		ces::SessionConfig* config = configMsg.mutable_config();
		config->set_session(m_sessionPath);
		if (!m_deployment.empty()) {
			config->set_deployment(m_deployment);
		}
		if (!m_entryAgent.empty()) {
			config->set_entry_agent(m_entryAgent);
		}
		config->set_enable_text_streaming(m_enableTextStreaming);
		auto* inputAudio = config->mutable_input_audio_config();
		inputAudio->set_audio_encoding(ces::LINEAR16);
		inputAudio->set_sample_rate_hertz((int32_t)m_sampleRate);
		auto* outputAudio = config->mutable_output_audio_config();
		outputAudio->set_audio_encoding(m_outputEncoding);
		outputAudio->set_sample_rate_hertz((int32_t)m_sampleRate);

		if (!m_streamer->Write(configMsg)) {
			throw std::runtime_error("failed to send CES SessionConfig");
		}

		if (!zstr(text)) {
			ces::BidiSessionClientMessage textMsg;
			textMsg.mutable_realtime_input()->set_text(text);
			if (!m_streamer->Write(textMsg)) {
				throw std::runtime_error("failed to send initial CES text input");
			}
		}

		m_started = true;
	}

	bool write(switch_core_session_t* session, const void* data, uint32_t datalen) {
		if (!data || !datalen) {
			return true;
		}

		size_t sampleCount = datalen / sizeof(int16_t);
		if (sampleCount) {
			m_spool.ingest(session, reinterpret_cast<const int16_t*>(data), sampleCount);
		}

		if (m_scaffoldOnly || !m_started || m_writesDone || !m_streamer || m_paused.load()) {
			return true;
		}

		ces::BidiSessionClientMessage inputMsg;
		inputMsg.mutable_realtime_input()->set_audio(std::string(reinterpret_cast<const char*>(data), datalen));
		return m_streamer->Write(inputMsg);
	}

	bool read(ces::BidiSessionServerMessage* response) {
		return m_streamer && response ? m_streamer->Read(response) : false;
	}

	void writesDone() {
		if (m_scaffoldOnly || !m_streamer || m_writesDone) {
			return;
		}
		m_writesDone = true;
		m_streamer->WritesDone();
	}

	void cancel() {
		if (m_context) {
			m_context->TryCancel();
		}
	}

	grpc::Status finish() {
		if (m_scaffoldOnly || !m_streamer) {
			return grpc::Status::OK;
		}
		std::lock_guard<std::mutex> lock(m_finishMutex);
		if (!m_finishCalled) {
			m_finishStatus = m_streamer->Finish();
			m_finishCalled = true;
		}
		return m_finishStatus;
	}

	bool isSpooling() const {
		return m_spool.enabled == SWITCH_TRUE;
	}

	switch_status_t captureSnippet(switch_core_session_t* session, uint64_t startMs, uint64_t durationMs,
		const std::string& tag, std::string& outPath, uint64_t& actualStartMs, uint64_t& actualEndMs) {
		return m_spool.capture(session, startMs, durationMs, tag, outPath, actualStartMs, actualEndMs);
	}

	void stopSpool() {
		m_spool.cleanup();
	}

	bool scaffoldOnly() const {
		return m_scaffoldOnly;
	}

	ces::AudioEncoding outputEncoding() const {
		return m_outputEncoding;
	}

	uint32_t sampleRate() const {
		return m_sampleRate;
	}

	void setPaused(bool paused) {
		m_paused.store(paused);
	}

	bool isPaused() const {
		return m_paused.load();
	}

private:
	std::string m_lang;
	std::string m_projectId;
	std::string m_appId;
	std::string m_location;
	std::string m_sessionId;
	std::string m_sessionPath;
	std::string m_deployment;
	std::string m_entryAgent;
	uint32_t m_sampleRate;
	bool m_scaffoldOnly;
	bool m_enableTextStreaming = false;
	ces::AudioEncoding m_outputEncoding;
	std::shared_ptr<grpc::ClientContext> m_context;
	std::shared_ptr<grpc::Channel> m_channel;
	std::unique_ptr<ces::SessionService::Stub> m_stub;
	std::unique_ptr<grpc::ClientReaderWriterInterface<ces::BidiSessionClientMessage, ces::BidiSessionServerMessage> > m_streamer;
	AudioSpooler m_spool;
	bool m_started;
	std::atomic<bool> m_paused;
	bool m_writesDone;
	std::mutex m_finishMutex;
	bool m_finishCalled;
	grpc::Status m_finishStatus;
};

static void destroy_ces_cb(struct ces_cap_cb* cb) {
	if (!cb) {
		return;
	}
	if (cb->streamer) {
		CesStreamer* streamer = static_cast<CesStreamer*>(cb->streamer);
		delete streamer;
		cb->streamer = NULL;
	}
	if (cb->resampler) {
		speex_resampler_destroy(cb->resampler);
		cb->resampler = NULL;
	}
}

static void emit_json_response(struct ces_cap_cb* cb, switch_core_session_t* session, const char* type, cJSON* json) {
	if (!cb || !cb->responseHandler || !json) {
		return;
	}
	char* body = cJSON_PrintUnformatted(json);
	if (body) {
		cb->responseHandler(session, type, body);
		free(body);
	}
}

static std::string write_turn_audio_file(struct ces_cap_cb* cb, switch_core_session_t* session,
	CesStreamer* streamer, int32_t turnIndex, const std::string& audio) {
	std::ostringstream path;
	path << SWITCH_GLOBAL_dirs.temp_dir << SWITCH_PATH_SEPARATOR
	     << cb->sessionId << "_" << turnIndex << "_" << (switch_micro_time_now() / 1000)
	     << choose_audio_suffix(streamer->outputEncoding());

	std::ofstream file(path.str(), std::ofstream::binary);
	file << audio;
	file.close();

	audioFiles.insert(std::pair<std::string, std::string>(cb->sessionId, path.str()));
	return path.str();
}

static bool autoplay_enabled(switch_channel_t* channel) {
	const char* ap = switch_channel_get_variable(channel, "CES_AUTOPLAY");
	return ap && switch_true(ap);
}

static bool autoplay_sync_enabled(switch_channel_t* channel) {
	const char* syncVar = switch_channel_get_variable(channel, "CES_AUTOPLAY_SYNC");
	const bool allowBargeIn = switch_true(switch_channel_get_variable(channel, "CES_BARGE_IN"));
	if (syncVar == NULL) {
		return !allowBargeIn;
	}
	return switch_true(syncVar);
}

static void play_turn_audio(struct ces_cap_cb* cb, switch_core_session_t* session, CesStreamer* streamer, const std::string& path) {
	switch_channel_t* channel = switch_core_session_get_channel(session);
	if (!autoplay_enabled(channel)) {
		return;
	}

	bool autoplaySync = autoplay_sync_enabled(channel);
	if (autoplaySync) {
		switch_mutex_lock(cb->mutex);
		if (streamer) {
			streamer->setPaused(true);
		}
		switch_mutex_unlock(cb->mutex);

		switch_status_t st = switch_ivr_play_file(session, NULL, path.c_str(), NULL);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
			"Auto-playing CES audio synchronously: %s (status=%d)\n", path.c_str(), st);

		switch_mutex_lock(cb->mutex);
		if (streamer) {
			streamer->setPaused(false);
		}
		switch_mutex_unlock(cb->mutex);
	} else {
		char args[1024];
		switch_stream_handle_t stream = { 0 };
		snprintf(args, sizeof(args), "%s %s aleg", cb->sessionId, path.c_str());
		SWITCH_STANDARD_STREAM(stream);
		switch_status_t st = switch_api_execute("uuid_broadcast", args, NULL, &stream);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
			"Auto-playing CES audio via uuid_broadcast: %s (status=%d)\n", args, st);
		switch_safe_free(stream.data);
	}
}

static void flush_turn_audio(struct ces_cap_cb* cb, switch_core_session_t* session,
	CesStreamer* streamer, PendingTurnAudio* pending, bool playIfConfigured) {
	if (!pending || !streamer || pending->turnIndex < 0 || pending->bytes.empty()) {
		return;
	}

	std::string path = write_turn_audio_file(cb, session, streamer, pending->turnIndex, pending->bytes);
	cJSON* json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "path", path.c_str());
	cJSON_AddNumberToObject(json, "turn_index", pending->turnIndex);
	cJSON_AddNumberToObject(json, "bytes", (double) pending->bytes.size());
	emit_json_response(cb, session, CES_EVENT_AUDIO_PROVIDED, json);
	cJSON_Delete(json);

	if (playIfConfigured) {
		play_turn_audio(cb, session, streamer, path);
	}

	pending->turnIndex = -1;
	pending->bytes.clear();
}

static bool should_emit_response_event(const ces::BidiSessionServerMessage& response) {
	if (!response.has_session_output()) {
		return false;
	}
	const auto& so = response.session_output();
	return so.has_text() || so.has_payload() || so.has_end_session() || so.turn_completed() || so.has_diagnostic_info();
}

static void *SWITCH_THREAD_FUNC ces_grpc_read_thread(switch_thread_t *thread, void *obj) {
	(void)thread;
	struct ces_cap_cb *cb = (struct ces_cap_cb *) obj;
	CesStreamer* streamer = static_cast<CesStreamer*>(cb->streamer);
	ces::BidiSessionServerMessage response;
	PendingTurnAudio pendingAudio;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "ces_grpc_read_thread: starting cb %p\n", (void *) cb);

	while (streamer && streamer->read(&response)) {
		switch_core_session_t* psession = switch_core_session_locate(cb->sessionId);
		if (!psession) {
			break;
		}

		switch_channel_t* channel = switch_core_session_get_channel(psession);
		if (cb->stopping || !switch_channel_ready(channel)) {
			switch_core_session_rwunlock(psession);
			break;
		}

		if (response.has_recognition_result()) {
			cJSON* json = CESParser::parse(response);
			emit_json_response(cb, psession, CES_EVENT_TRANSCRIPTION, json);
			cJSON_Delete(json);
		}

		if (response.has_session_output()) {
			const auto& output = response.session_output();
			if (output.has_audio()) {
				int32_t turnIndex = output.turn_index();
				if (pendingAudio.turnIndex >= 0 && pendingAudio.turnIndex != turnIndex && !pendingAudio.bytes.empty()) {
					flush_turn_audio(cb, psession, streamer, &pendingAudio, autoplay_enabled(channel));
				}
				if (pendingAudio.turnIndex < 0) {
					pendingAudio.turnIndex = turnIndex;
				}
				pendingAudio.bytes.append(output.audio());
			}

			if (should_emit_response_event(response)) {
				cJSON* json = CESParser::parse(response);
				emit_json_response(cb, psession, CES_EVENT_RESPONSE, json);
				if (output.turn_completed() || output.has_end_session()) {
					emit_json_response(cb, psession, CES_EVENT_END_OF_TURN, json);
				}
				cJSON_Delete(json);
			}
			if (output.turn_completed() || output.has_end_session()) {
				flush_turn_audio(cb, psession, streamer, &pendingAudio, true);
			}
		}

		if (response.has_interruption_signal()) {
			stop_playback(psession);
			cJSON* json = CESParser::parse(response);
			emit_json_response(cb, psession, CES_EVENT_INTERRUPTION, json);
			cJSON_Delete(json);
		}

		if (response.has_end_session()) {
			cJSON* json = CESParser::parse(response);
			emit_json_response(cb, psession, CES_EVENT_END_OF_TURN, json);
			cJSON_Delete(json);
			switch_mutex_lock(cb->mutex);
			if (!cb->stopping && streamer) {
				streamer->writesDone();
			}
			switch_mutex_unlock(cb->mutex);
		}

		if (response.has_go_away()) {
			cJSON* json = CESParser::parse(response);
			emit_json_response(cb, psession, CES_EVENT_END_OF_TURN, json);
			cJSON_Delete(json);
			switch_mutex_lock(cb->mutex);
			if (!cb->stopping && streamer) {
				streamer->writesDone();
			}
			switch_mutex_unlock(cb->mutex);
		}

		switch_core_session_rwunlock(psession);
		response.Clear();
	}

	switch_core_session_t* psession = switch_core_session_locate(cb->sessionId);
	if (psession) {
		if (!pendingAudio.bytes.empty()) {
			flush_turn_audio(cb, psession, streamer, &pendingAudio, autoplay_enabled(switch_core_session_get_channel(psession)));
		}
		grpc::Status status = streamer ? streamer->finish() : grpc::Status::OK;
		if (!status.ok() && !(cb->stopping && status.error_code() == grpc::StatusCode::CANCELLED)) {
			const char* category = "unknown";
			bool retryable = false;
			categorize_status(status, &category, &retryable);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_CRIT,
				"CES BidiRunSession finished with err %s (%d): %s\n",
				status.error_message().c_str(), status.error_code(), status.error_details().c_str());
			emit_error_json(psession, cb->errorHandler, status.error_message().c_str(),
				status.error_code(), category, retryable, status.error_details().c_str());
		}
		switch_core_session_rwunlock(psession);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "ces_grpc_read_thread exiting\n");
	return NULL;
}

} // namespace

extern "C" {

switch_status_t ces_init(void) {
	const char* gcsServiceKeyFile = std::getenv("GOOGLE_APPLICATION_CREDENTIALS");
	if (gcsServiceKeyFile == NULL) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"\"GOOGLE_APPLICATION_CREDENTIALS\" environment variable is not set; CES authentication will use \"GOOGLE_APPLICATION_CREDENTIALS\" channel variable\n");
	} else {
		hasDefaultCredentials = true;
	}
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t ces_cleanup(void) {
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t ces_session_init(switch_core_session_t *session, ces_response_handler_t responseHandler, ces_error_handler_t errorHandler,
		uint32_t samples_per_second, char* lang, char* projectId, char* appId, char* location, char* welcomeEvent, char *text, struct ces_cap_cb **cb) {
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_memory_pool_t *pool = switch_core_session_get_pool(session);
	switch_threadattr_t *thd_attr = NULL;
	struct ces_cap_cb* local = (struct ces_cap_cb *) switch_core_session_alloc(session, sizeof(*local));
	bool scaffoldOnly = ces_scaffold_mode_enabled(channel);

	memset(local, 0, sizeof(*local));
	local->responseHandler = responseHandler;
	local->errorHandler = errorHandler;
	local->stopping = SWITCH_FALSE;
	switch_snprintf(local->sessionId, sizeof(local->sessionId), "%s", switch_core_session_get_uuid(session));
	switch_snprintf(local->lang, sizeof(local->lang), "%s", lang ? lang : "");
	switch_snprintf(local->projectId, sizeof(local->projectId), "%s", projectId ? projectId : "");
	switch_snprintf(local->appId, sizeof(local->appId), "%s", appId ? appId : "");
	switch_snprintf(local->location, sizeof(local->location), "%s", location ? location : "");

	if (!scaffoldOnly && !hasDefaultCredentials && !switch_channel_get_variable(channel, "GOOGLE_APPLICATION_CREDENTIALS")) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"missing credentials: GOOGLE_APPLICATION_CREDENTIALS must be supplied as env or channel var for CES\n");
		emit_error_json(session, errorHandler, "missing credentials", 16, "auth", false);
		return SWITCH_STATUS_FALSE;
	}

	if (switch_mutex_init(&local->mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing CES mutex\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_channel_set_variable(channel, "CES_SESSION_ID", local->sessionId);
	switch_channel_set_variable(channel, "CES_PROJECT", local->projectId);
	switch_channel_set_variable(channel, "CES_APP", local->appId);
	switch_channel_set_variable(channel, "CES_LOCATION", local->location);
	switch_channel_set_variable(channel, "CES_REGION", local->location);

	try {
		local->streamer = new CesStreamer(session, lang, projectId, appId, location, local->sessionId, samples_per_second, scaffoldOnly);
		((CesStreamer*)local->streamer)->startStream(session, welcomeEvent, text);
	} catch (const std::exception& e) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
			"CES init error: %s\n", e.what());
		const char* category = (strstr(e.what(), "credential") || strstr(e.what(), "Credentials") || strstr(e.what(), "missing credentials")) ? "auth" : "unknown";
		int code = strcmp(category, "auth") == 0 ? 16 : 13;
		emit_error_json(session, errorHandler, e.what(), code, category, false);
		status = SWITCH_STATUS_FALSE;
		goto done;
	} catch (...) {
		emit_error_json(session, errorHandler, "CES init failed", 13, "internal", false);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}

	if (scaffoldOnly) {
		if (responseHandler) {
			responseHandler(session, CES_EVENT_RESPONSE, (char*)"{\"status\":\"scaffold_only\",\"provider\":\"ces\",\"message\":\"Local CES scaffold mode active; remote backend not connected\"}");
		}
	} else {
		switch_core_event_hook_add_state_change(session, hanguphook);
		switch_threadattr_create(&thd_attr, pool);
		switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
		switch_thread_create(&local->thread, thd_attr, ces_grpc_read_thread, local, pool);
	}

done:
	if (status == SWITCH_STATUS_SUCCESS && cb) {
		*cb = local;
	}
	if (status != SWITCH_STATUS_SUCCESS) {
		destroy_ces_cb(local);
	}
	return status;
}

switch_status_t ces_session_stop(switch_core_session_t *session, int channelIsClosing) {
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_CES_BUG_NAME);

	if (bug) {
		struct ces_cap_cb *cb = (struct ces_cap_cb *) switch_core_media_bug_get_user_data(bug);
		switch_mutex_lock(cb->mutex);
		cb->stopping = SWITCH_TRUE;
		CesStreamer* streamer = static_cast<CesStreamer*>(cb->streamer);
		if (streamer) {
			streamer->cancel();
			streamer->writesDone();
		}
		switch_mutex_unlock(cb->mutex);

		if (cb->thread) {
			switch_status_t retval;
			switch_thread_join(&retval, cb->thread);
			cb->thread = NULL;
		}

		switch_mutex_lock(cb->mutex);
		if (streamer) {
			(void)streamer->finish();
		}
		destroy_ces_cb(cb);
		switch_channel_set_private(channel, MY_CES_BUG_NAME, NULL);
		if (!channelIsClosing) {
			switch_core_media_bug_remove(session, &bug);
		}
		switch_mutex_unlock(cb->mutex);
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}

switch_bool_t ces_frame(switch_media_bug_t *bug, void* user_data) {
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
	switch_frame_t frame = {};
	struct ces_cap_cb *cb = (struct ces_cap_cb *) user_data;

	frame.data = data;
	frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

	if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {
		CesStreamer* streamer = static_cast<CesStreamer*>(cb->streamer);
		if (streamer && !cb->stopping) {
			while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
				if (frame.datalen && !streamer->write(session, frame.data, frame.datalen)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						"CES audio write failed for session %s\n", cb->sessionId);
					break;
				}
			}
		}
		switch_mutex_unlock(cb->mutex);
	}
	return SWITCH_TRUE;
}

switch_status_t ces_capture_snippet(struct ces_cap_cb* cb, switch_core_session_t* session,
		uint64_t start_ms, uint64_t duration_ms, const char* tag,
		char* out_path, size_t out_path_len, uint64_t* actual_start_ms, uint64_t* actual_end_ms) {
	if (!cb || !cb->streamer || !session) {
		return SWITCH_STATUS_FALSE;
	}

	switch_status_t status = SWITCH_STATUS_FALSE;
	std::string path;
	uint64_t realStart = 0;
	uint64_t realEnd = 0;

	switch_mutex_lock(cb->mutex);
	CesStreamer* streamer = static_cast<CesStreamer*>(cb->streamer);
	if (streamer && streamer->isSpooling()) {
		status = streamer->captureSnippet(session, start_ms, duration_ms, tag ? tag : "snippet", path, realStart, realEnd);
	}
	switch_mutex_unlock(cb->mutex);

	if (status == SWITCH_STATUS_SUCCESS) {
		if (out_path && out_path_len) {
			switch_snprintf(out_path, out_path_len, "%s", path.c_str());
		}
		if (actual_start_ms) *actual_start_ms = realStart;
		if (actual_end_ms) *actual_end_ms = realEnd;
	}
	return status;
}

void ces_spool_cleanup(struct ces_cap_cb* cb, switch_core_session_t* session, switch_bool_t preserve) {
	(void) session;
	(void) preserve;
	if (!cb || !cb->streamer) {
		return;
	}
	switch_mutex_lock(cb->mutex);
	CesStreamer* streamer = static_cast<CesStreamer*>(cb->streamer);
	if (streamer) {
		streamer->stopSpool();
	}
	switch_mutex_unlock(cb->mutex);
}

void destroyCesChannelUserData(struct ces_cap_cb* cb) {
	destroy_ces_cb(cb);
}

}
