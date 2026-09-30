#include <cstdlib>
#include <cstdio>

#include <switch.h>
#include <switch_json.h>
#include <grpcpp/grpcpp.h>
#include <string.h>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <stdexcept>
#include <cerrno>

#include <regex>

#include <fstream>
#include <string>
#include <sstream>
#include <map>
#include <set>
#include <vector>
#include <thread>
#include <chrono>
#include <deque>
#include <cctype>
#include <ctime>
#include <memory>
#include <algorithm>
#include <inttypes.h>

#include "google/protobuf/duration.pb.h"

#include "google/cloud/dialogflow/cx/v3/session.grpc.pb.h"
#include "google/cloud/texttospeech/v1/cloud_tts.grpc.pb.h"

#include "mod_dialogflow.h"
#include "google_glue.h"
#include "common/audio_spooler.h"
#include "parser.h"

using google::cloud::dialogflow::cx::v3::Sessions;
using google::cloud::dialogflow::cx::v3::StreamingDetectIntentRequest;
using google::cloud::dialogflow::cx::v3::StreamingDetectIntentResponse;
using google::cloud::dialogflow::cx::v3::AudioEncoding;
using google::cloud::dialogflow::cx::v3::InputAudioConfig;
using google::cloud::dialogflow::cx::v3::OutputAudioConfig;
using google::cloud::dialogflow::cx::v3::SynthesizeSpeechConfig;
using google::cloud::dialogflow::cx::v3::QueryInput;
using google::cloud::dialogflow::cx::v3::QueryResult;
using google::cloud::dialogflow::cx::v3::StreamingRecognitionResult;
using google::cloud::dialogflow::cx::v3::EventInput;
using google::cloud::dialogflow::cx::v3::OutputAudioEncoding;
using google::cloud::dialogflow::cx::v3::SsmlVoiceGender;
using google::cloud::texttospeech::v1::TextToSpeech;
using google::cloud::texttospeech::v1::SynthesizeSpeechRequest;
using TtsAudioEncoding = google::cloud::texttospeech::v1::AudioEncoding;
using google::cloud::dialogflow::cx::v3::QueryParameters;
using google::rpc::Status;
using google::protobuf::Struct;
using google::protobuf::Value;
using google::protobuf::MapPair;

static uint64_t playCount = 0;
static std::multimap<std::string, std::string> audioFiles;
static bool hasDefaultCredentials = false;
static std::mutex backchannelCacheMutex;
static std::map<std::string, std::string> backchannelAudioCache;
static std::mutex backchannelLoopsMutex;
static std::map<std::string, std::shared_ptr<struct BackchannelLoop>> backchannelLoops;
static std::mutex backchannelWarmupMutex;
static std::set<std::string> backchannelWarmupVoices;
static std::vector<std::thread> backchannelWarmupThreads;

struct BackchannelLoop {
    std::string uuid;
    std::string operation;
    std::string voice;
    std::string language;
    std::vector<std::string> audioFiles;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopping = false;
    bool finished = false;
};

static uint64_t duration_to_samples(const google::protobuf::Duration& d, uint32_t sampleRate) {
    int64_t seconds = d.seconds();
    int32_t nanos = d.nanos();
    int64_t totalNanos = seconds * 1000000000LL + nanos;
    if (totalNanos <= 0) {
        return 0;
    }
    // Avoid overflow by operating in double then casting.
    double samples = (static_cast<double>(totalNanos) / 1000000000.0) * static_cast<double>(sampleRate);
    if (samples <= 0.0) {
        return 0;
    }
    return static_cast<uint64_t>(samples + 0.5);
}

static double samples_to_ms(uint64_t samples, uint32_t sampleRate) {
    if (!sampleRate) {
        return 0.0;
    }
    double seconds = static_cast<double>(samples) / static_cast<double>(sampleRate);
    return seconds * 1000.0;
}

static uint64_t current_time_ms() {
    return switch_micro_time_now() / 1000;
}

static void set_proto_duration_ms(google::protobuf::Duration* duration, uint64_t ms) {
    if (!duration) {
        return;
    }
    duration->set_seconds(static_cast<int64_t>(ms / 1000));
    duration->set_nanos(static_cast<int32_t>((ms % 1000) * 1000000));
}

static uint16_t read_le16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

static uint32_t read_le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0] |
        (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24));
}

static bool get_wav_duration_ms(const std::string& audio, uint64_t* durationMs, uint32_t* detectedRate = nullptr) {
    if (durationMs) {
        *durationMs = 0;
    }
    if (detectedRate) {
        *detectedRate = 0;
    }
    if (audio.size() < 44) {
        return false;
    }

    const unsigned char* data = reinterpret_cast<const unsigned char*>(audio.data());
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }

    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint32_t sampleRate = 0;
    uint32_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= audio.size()) {
        const unsigned char* chunk = data + pos;
        uint32_t chunkSize = read_le32(chunk + 4);
        size_t next = pos + 8 + chunkSize + (chunkSize % 2);
        if (next > audio.size()) {
            break;
        }

        if (memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
            channels = read_le16(chunk + 10);
            sampleRate = read_le32(chunk + 12);
            bitsPerSample = read_le16(chunk + 22);
        } else if (memcmp(chunk, "data", 4) == 0) {
            dataSize = chunkSize;
        }

        pos = next;
    }

    if (!channels || !sampleRate || !bitsPerSample || !dataSize) {
        return false;
    }

    uint32_t bytesPerSample = static_cast<uint32_t>(channels) * static_cast<uint32_t>(bitsPerSample / 8);
    if (!bytesPerSample) {
        return false;
    }

    uint64_t sampleFrames = dataSize / bytesPerSample;
    if (!sampleFrames) {
        return false;
    }

    if (detectedRate) {
        *detectedRate = sampleRate;
    }
    if (durationMs) {
        *durationMs = (sampleFrames * 1000ULL) / sampleRate;
    }
    return true;
}

static bool get_audio_file_duration_ms(const std::string& path, uint32_t fallbackRate, uint64_t* durationMs, uint32_t* detectedRate = nullptr) {
    if (durationMs) {
        *durationMs = 0;
    }
    if (detectedRate) {
        *detectedRate = 0;
    }
    if (path.empty()) {
        return false;
    }

    switch_file_handle_t fh = {};
    fh.channels = 1;
    fh.native_rate = fallbackRate ? fallbackRate : 8000;
    switch_status_t status = switch_core_file_open(&fh, path.c_str(), fh.channels, fh.native_rate,
        SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT, NULL);
    if (status != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
            "get_audio_file_duration_ms: switch_core_file_open failed for %s (status=%d rate=%u)\n",
            path.c_str(), status, fh.native_rate);
        return false;
    }

    uint32_t samplerate = fh.samplerate ? fh.samplerate : fallbackRate;
    uint64_t sampleCount = static_cast<uint64_t>(fh.sample_count ? fh.sample_count : fh.samples_in);
    switch_core_file_close(&fh);

    if (!samplerate || !sampleCount) {
        return false;
    }

    if (detectedRate) {
        *detectedRate = samplerate;
    }
    if (durationMs) {
        *durationMs = (sampleCount * 1000ULL) / samplerate;
    }
    return true;
}

static void reset_interruptible_playback_state(struct cap_cb* cb) {
    if (!cb) {
        return;
    }
    cb->interruptible_playback_active = SWITCH_FALSE;
    cb->interruptible_playback_started_ms = 0;
    cb->interruptible_playback_no_barge_ms = 0;
    cb->vad_talking_ms = 0;
    if (cb->vad) {
        switch_vad_reset(cb->vad);
    }
}

static void arm_interruptible_playback_state(struct cap_cb* cb, uint64_t noBargeInMs) {
    if (!cb) {
        return;
    }
    cb->interruptible_playback_active = SWITCH_TRUE;
    cb->interruptible_playback_started_ms = current_time_ms();
    cb->interruptible_playback_no_barge_ms = noBargeInMs;
    cb->vad_talking_ms = 0;
    if (cb->vad) {
        switch_vad_reset(cb->vad);
    }
    cb->last_interruptible_playback_started_ms = cb->interruptible_playback_started_ms;
    cb->last_local_barge_break_ms = 0;
    cb->last_first_recognition_ms = 0;
    cb->awaiting_first_recognition_after_playback = SWITCH_TRUE;
}

static bool should_log_barge_timing(switch_channel_t* channel) {
    const char* value = switch_channel_get_variable(channel, "DIALOGFLOW_LOG_BARGE_TIMING");
    return value ? switch_true(value) : false;
}

static void maybe_log_barge_timing(struct cap_cb* cb, switch_core_session_t* session, const StreamingRecognitionResult& rr, bool is_eou) {
    if (!cb || !session) {
        return;
    }
    switch_channel_t* channel = switch_core_session_get_channel(session);
    if (!channel || !cb->awaiting_first_recognition_after_playback || !cb->last_interruptible_playback_started_ms || !should_log_barge_timing(channel)) {
        return;
    }

    uint64_t nowMs = current_time_ms();
    cb->last_first_recognition_ms = nowMs;
    cb->awaiting_first_recognition_after_playback = SWITCH_FALSE;

    uint64_t playbackToRecogMs = 0;
    if (nowMs >= cb->last_interruptible_playback_started_ms) {
        playbackToRecogMs = nowMs - cb->last_interruptible_playback_started_ms;
    }
    uint64_t breakToRecogMs = 0;
    if (cb->last_local_barge_break_ms && nowMs >= cb->last_local_barge_break_ms) {
        breakToRecogMs = nowMs - cb->last_local_barge_break_ms;
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
        "Dialogflow barge timing: playback_to_recog=%" PRIu64 "ms break_to_recog=%" PRIu64 "ms local_break=%s final=%s eou=%s transcript='%s'\n",
        playbackToRecogMs,
        breakToRecogMs,
        cb->last_local_barge_break_ms ? "true" : "false",
        rr.is_final() ? "true" : "false",
        is_eou ? "true" : "false",
        rr.transcript().c_str());
}

static bool response_allows_playback_interruption(const StreamingDetectIntentResponse& response) {
    if (!response.has_detect_intent_response()) {
        return false;
    }

    const auto& dir = response.detect_intent_response();
    if (!dir.has_query_result()) {
        return false;
    }

    const auto& messages = dir.query_result().response_messages();
    for (const auto& message : messages) {
        if (message.has_output_audio_text() && message.output_audio_text().allow_playback_interruption()) {
            return true;
        }
        if (message.has_play_audio() && message.play_audio().allow_playback_interruption()) {
            return true;
        }
        if (message.has_text() && message.text().allow_playback_interruption()) {
            return true;
        }
        if (message.has_mixed_audio()) {
            for (const auto& segment : message.mixed_audio().segments()) {
                if (segment.allow_playback_interruption()) {
                    return true;
                }
            }
        }
    }

    return false;
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

static void stop_backchannel(switch_core_session_t* session) {
    google_dialogflow_backchannel_stop(session);
}

// Forward declaration for internal stop helper defined later in this file
extern "C" switch_status_t google_dialogflow_session_stop(switch_core_session_t *session, int channelIsClosing);

static switch_status_t hanguphook(switch_core_session_t *session) {
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_channel_state_t state = switch_channel_get_state(channel);

	if (state == CS_HANGUP || state == CS_ROUTING) {
		char * sessionId = switch_core_session_get_uuid(session);
		typedef std::multimap<std::string, std::string>::iterator MMAPIterator;
		std::pair<MMAPIterator, MMAPIterator> result = audioFiles.equal_range(sessionId);
		for (MMAPIterator it = result.first; it != result.second; it++) {
			std::string filename = it->second;
			std::remove(filename.c_str());
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
				"google_dialogflow_session_cleanup: removed audio file %s\n", filename.c_str());
		}
		audioFiles.erase(sessionId);
		switch_core_event_hook_remove_state_change(session, hanguphook);
	}
	return SWITCH_STATUS_SUCCESS;
}

static  void parseEventParams(Struct* grpcParams, cJSON* json) {
	auto* map = grpcParams->mutable_fields();
	int count = cJSON_GetArraySize(json);
	for (int i = 0; i < count; i++) {
		cJSON* prop = cJSON_GetArrayItem(json, i);
		if (prop) {
			google::protobuf::Value v;
			switch (prop->type) {
				case cJSON_False:
				case cJSON_True:
					v.set_bool_value(prop->type == cJSON_True);
					break;

				case cJSON_Number:
					v.set_number_value(prop->valuedouble);
					break;

				case cJSON_String:
					v.set_string_value(prop->valuestring);
					break;

				case cJSON_Array:
				case cJSON_Object:
				case cJSON_Raw:
				case cJSON_NULL:
					continue;
			}
			map->insert(MapPair<std::string, Value>(prop->string, v));
		}
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parseEventParams: added %lu event params\n", (unsigned long) map->size());
}

static inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static bool parseDoubleStrict(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    out = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        return false;
    }
    return true;
}

static void splitCSV(const char* csv, std::vector<std::string>& out) {
    if (!csv || !*csv) return;
    std::string s(csv);
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(',', start);
        std::string token = s.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        token = trim(token);
        if (!token.empty()) out.push_back(token);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
}

static bool hasAllowedPrefix(const std::vector<std::string>& prefixes, const char* name) {
    if (prefixes.empty()) return true;
    if (!name) return false;
    for (const auto& p : prefixes) {
        if (strncmp(name, p.c_str(), p.size()) == 0) return true;
    }
    return false;
}

static cJSON* collectChannelVarsAsJSON(switch_core_session_t* session) {
    switch_channel_t* channel = switch_core_session_get_channel(session);
    const char* pass = switch_channel_get_variable(channel, "DIALOGFLOW_PASS_ALL_CHANNEL_VARS");
    if (!(pass && switch_true(pass))) return nullptr;

    std::vector<std::string> prefixes;
    splitCSV(switch_channel_get_variable(channel, "DIALOGFLOW_VAR_PREFIXES"), prefixes);

    cJSON* root = cJSON_CreateObject();

    // Create a temporary event, copy channel data, then iterate headers
    switch_event_t* ev = nullptr;
    if (switch_event_create(&ev, SWITCH_EVENT_CHANNEL_DATA) == SWITCH_STATUS_SUCCESS) {
        switch_channel_event_set_data(channel, ev);
        for (switch_event_header_t* hp = ev->headers; hp; hp = hp->next) {
            if (!hasAllowedPrefix(prefixes, hp->name)) continue;
            if (hp->name && hp->value) {
                // Store as strings; parseEventParams will treat as string values
                cJSON_AddItemToObject(root, hp->name, cJSON_CreateString(hp->value));
            }
        }
        switch_event_destroy(&ev);
    }
    if (root && root->child) {
        char* dump = cJSON_PrintUnformatted(root);
        if (dump) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                "collectChannelVarsAsJSON gathered %d entries: %s\n", cJSON_GetArraySize(root), dump);
            cJSON_free(dump);
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                "collectChannelVarsAsJSON gathered %d entries (unable to serialize)\n", cJSON_GetArraySize(root));
        }
    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
            "collectChannelVarsAsJSON no channel vars eligible for injection\n");
    }
    return root;
}

static void logMergedParamsJSON(switch_core_session_t* session, const char* label, cJSON* json) {
    if (!json) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
            "%s: <null JSON>\n", label);
        return;
    }
    char* dump = cJSON_PrintUnformatted(json);
    if (dump) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
            "%s: %s\n", label, dump);
        cJSON_free(dump);
    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
            "%s: <unable to serialize JSON>\n", label);
    }
}

static void logQueryParamsSummary(switch_core_session_t* session, const char* label, const QueryParameters& qp) {
    const auto& fields = qp.parameters().fields();
    std::ostringstream keys;
    bool first = true;
    for (const auto& kv : fields) {
        if (!first) keys << ",";
        keys << kv.first;
        first = false;
    }
    std::string keyList = keys.str();
    const std::string& channel = qp.channel();
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
        "%s channel='%s' field_count=%lu fields=[%s]\n",
        label,
        channel.empty() ? "<unset>" : channel.c_str(),
        (unsigned long)fields.size(),
        keyList.empty() ? "<none>" : keyList.c_str());
}

static void addOrReplaceJSONField(cJSON* target, const char* key, cJSON* value) {
    if (!target || !key || !value) {
        if (value) cJSON_Delete(value);
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(target, key);
    cJSON_AddItemToObject(target, key, value);
}

void tokenize(std::string const &str, const char delim, std::vector<std::string> &out) {
    size_t start = 0;
    size_t end = 0;
		bool finished = false;
		do {
			end = str.find(delim, start);
			if (end == std::string::npos) {
				finished = true;
				out.push_back(str.substr(start));
			}
			else {
				out.push_back(str.substr(start, end - start));
				start = ++end;
			}
		} while (!finished);
}

class GStreamer {
public:
    GStreamer(switch_core_session_t *session, const char* lang, char* projectId, char* event, char* text, uint32_t sampleRate) :
            m_lang(lang), m_sessionId(), m_environment("draft"), m_regionId("us"), m_agentId(""),
            m_speakingRate(), m_pitch(), m_volume(), m_voiceName(""), m_voiceGender(""), m_effects(""),
            m_sentimentAnalysis(false), m_finished(false), m_packets(0), m_needConfig(false),
            m_paused(false),
            m_startedWithEvent(false), m_rotatedToAudio(false), m_sampleRate(sampleRate), m_outputEncoding(OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_LINEAR_16),
            m_totalSamples(0), m_turnStartSample(0), m_turnIndex(0) {
		const char* var;
		switch_channel_t* channel = switch_core_session_get_channel(session);

        // Allow overriding the Dialogflow session id via channel var
        const char* sid = switch_channel_get_variable(channel, "DIALOGFLOW_SESSION_ID");
        if (sid && *sid) {
            m_sessionId.assign(sid);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                "GStreamer: using provided Dialogflow session id '%s'\n", m_sessionId.c_str());
        } else {
            m_sessionId.assign(switch_core_session_get_uuid(session));
        }
		std::vector<std::string> tokens;
		const char delim = ':';
		tokenize(projectId, delim, tokens);
		int idx = 0;
		for (auto &s: tokens) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer: token %d: '%s'\n", idx, s.c_str());
            if (0 == idx) m_projectId = s;
            else if (1 == idx && s.length() > 0) m_agentId = s;
            else if (2 == idx && s.length() > 0) m_environment = s;
            else if (3 == idx && s.length() > 0) m_regionId = s;
            else if (4 == idx && s.length() > 0) {
                double v; if (parseDoubleStrict(s, v)) m_speakingRate = v; else switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "GStreamer: ignoring non-numeric speakingRate '%s'\n", s.c_str());
            }
            else if (5 == idx && s.length() > 0) {
                double v; if (parseDoubleStrict(s, v)) m_pitch = v; else switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "GStreamer: ignoring non-numeric pitch '%s'\n", s.c_str());
            }
            else if (6 == idx && s.length() > 0) {
                double v; if (parseDoubleStrict(s, v)) m_volume = v; else switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "GStreamer: ignoring non-numeric volume '%s'\n", s.c_str());
            }
            else if (7 == idx && s.length() > 0) m_voiceName = s;
            else if (8 == idx && s.length() > 0) m_voiceGender = s;
            else if (9 == idx && s.length() > 0) m_effects = s;
            else if (10 == idx && s.length() > 0) m_sentimentAnalysis = (s == "true");
			idx++;
		}
		switch_channel_set_variable(channel, "DIALOGFLOW_TTS_VOICE_NAME", m_voiceName.empty() ? NULL : m_voiceName.c_str());
		switch_channel_set_variable(channel, "DIALOGFLOW_TTS_LANGUAGE", m_lang.empty() ? NULL : m_lang.c_str());
		switch_channel_set_variable(channel, "DIALOGFLOW_TTS_SPEAKING_RATE", m_speakingRate ? std::to_string(m_speakingRate).c_str() : NULL);
		switch_channel_set_variable(channel, "DIALOGFLOW_TTS_PITCH", m_pitch ? std::to_string(m_pitch).c_str() : NULL);
		switch_channel_set_variable(channel, "DIALOGFLOW_TTS_VOLUME_GAIN_DB", m_volume ? std::to_string(m_volume).c_str() : NULL);
		switch_channel_set_variable(channel, "DIALOGFLOW_TTS_EFFECTS_PROFILE_ID", m_effects.empty() ? NULL : m_effects.c_str());

		std::string endpoint = "dialogflow.googleapis.com";
		if (0 != m_regionId.compare("us")) {
			endpoint = m_regionId;
			endpoint.append("-dialogflow.googleapis.com:443");
		}

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
            "GStreamer dialogflow CX endpoint is %s, region is %s, project is %s, agent is %s, environment is %s\n", 
            endpoint.c_str(), m_regionId.c_str(), m_projectId.c_str(), m_agentId.c_str(), m_environment.c_str());        

        // Allow overriding output sample rate and encoding via channel vars
        if ((var = switch_channel_get_variable(channel, "DIALOGFLOW_OUTPUT_SAMPLE_RATE"))) {
            int v = atoi(var);
            if (v >= 8000 && v <= 48000) m_sampleRate = v;
        }
        if ((var = switch_channel_get_variable(channel, "DIALOGFLOW_OUTPUT_ENCODING"))) {
            std::string enc = var; for (auto& c : enc) c = tolower(c);
            if (enc == "mp3") m_outputEncoding = OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_MP3;
            else if (enc == "opus" || enc == "ogg" || enc == "ogg_opus") m_outputEncoding = OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_OGG_OPUS;
            else m_outputEncoding = OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_LINEAR_16;
        }

        (void) m_spool.configure(session, m_sessionId, m_sampleRate);
        m_totalSamples = m_spool.totalSamples;

			if ((var = switch_channel_get_variable(channel, "GOOGLE_APPLICATION_CREDENTIALS"))) {
				std::string input = var;
				std::string json = input;
				bool read_from_file = false;
				if (!input.empty() && (input[0] == '/' || input.rfind(".json") == input.size() - 5)) {
					// Looks like a path; try to read file
					std::ifstream fs(input);
					if (fs.good()) {
						json.assign((std::istreambuf_iterator<char>(fs)), std::istreambuf_iterator<char>());
						read_from_file = true;
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "GStreamer credentials path not readable: %s; falling back to default creds if available\n", input.c_str());
						if (!hasDefaultCredentials) {
							// No ADC available; fail fast rather than attempting a stream that will hard-fail
							throw std::runtime_error("GOOGLE_APPLICATION_CREDENTIALS path not readable and no default credentials configured");
						}
					}
				}

				if (!json.empty()) {
					auto callCreds = grpc::ServiceAccountJWTAccessCredentials(json, INT64_MAX);
					auto channelCreds = grpc::SslCredentials(grpc::SslCredentialsOptions());
					auto creds = grpc::CompositeChannelCredentials(channelCreds, callCreds);
					m_channel = grpc::CreateChannel(endpoint, creds);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer using %s credentials for channel\n", read_from_file ? "file" : "inline JSON");
				} else {
					auto creds = grpc::GoogleDefaultCredentials();
					m_channel = grpc::CreateChannel(endpoint, creds);
				}
			} else {
				auto creds = grpc::GoogleDefaultCredentials();
				m_channel = grpc::CreateChannel(endpoint, creds);
			}
    }

    ~GStreamer() {
        try { m_spool.cleanup(); } catch (...) {}
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
            "GStreamer::~GStreamer wrote %u packets %p\n", m_packets, this);
    }

    void startStream(switch_core_session_t *session, const char* event, const char* text) {
        char szSession[256];
        switch_channel_t* channel = switch_core_session_get_channel(session);

		m_request = std::make_shared<StreamingDetectIntentRequest>();
		m_context= std::make_shared<grpc::ClientContext>();
		m_stub = Sessions::NewStub(m_channel);

        // Use environment-specific session path when provided (non-draft)
        if (!m_environment.empty() && strcasecmp(m_environment.c_str(), "draft") != 0) {
            snprintf(szSession, 256, "projects/%s/locations/%s/agents/%s/environments/%s/sessions/%s",
                     m_projectId.c_str(), m_regionId.c_str(), m_agentId.c_str(), m_environment.c_str(), m_sessionId.c_str());
        } else {
            snprintf(szSession, 256, "projects/%s/locations/%s/agents/%s/sessions/%s",
                     m_projectId.c_str(), m_regionId.c_str(), m_agentId.c_str(), m_sessionId.c_str());
        }

        // Sanity log: print the composed CX session path once per call for observability
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
            "Dialogflow CX session path: %s (project=%s agent=%s region=%s env=%s session_id=%s)\n",
            szSession, m_projectId.c_str(), m_agentId.c_str(), m_regionId.c_str(), m_environment.c_str(), m_sessionId.c_str());

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "GStreamer::startStream session %s, event %s, text %s %p\n", szSession, event, text, this);

        // Expose DF metadata as channel vars for event headers
        switch_channel_set_variable(channel, "DF_SESSION_PATH", szSession);
        switch_channel_set_variable(channel, "DF_SESSION_ID", m_sessionId.c_str());
        switch_channel_set_variable(channel, "DF_PROJECT", m_projectId.c_str());
        if (!m_agentId.empty()) switch_channel_set_variable(channel, "DF_AGENT", m_agentId.c_str());
        switch_channel_set_variable(channel, "DF_REGION", m_regionId.c_str());
        switch_channel_set_variable(channel, "DF_ENVIRONMENT", m_environment.c_str());

        m_request->set_session(szSession);
        auto* queryInput = m_request->mutable_query_input();
        if (event) {
            auto* eventInput = queryInput->mutable_event();
            eventInput->set_event(event);
            queryInput->set_language_code(m_lang.c_str());
            m_startedWithEvent = true;
            // Mark start of an event-driven turn
            markTurnStart();
            // Merge optional parameters: 5th arg JSON, DIALOGFLOW_CHANNEL, DIALOGFLOW_PARAMS, optional channel vars
            cJSON* root = cJSON_CreateObject();
            bool have_params = false;
            if (text) {
                cJSON* json = cJSON_Parse(text);
                if (json) {
                    logMergedParamsJSON(session, "GStreamer::startStream event payload JSON", json);
                    have_params = true;
                    for (cJSON* it = json->child; it; it = it->next) {
                        addOrReplaceJSONField(root, it->string, cJSON_Duplicate(it, 1));
                    }
                    cJSON_Delete(json);
                } else {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "GStreamer::startStream 'text' argument not valid JSON for params: %s\n", text);
                }
            }
            const char* ch = switch_channel_get_variable(channel, "DIALOGFLOW_CHANNEL");
            if (ch && *ch) {
                // Set top-level QueryParameters.channel
                m_request->mutable_query_params()->set_channel(ch);
                m_qpChannel = ch;
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                    "GStreamer::startStream applying DIALOGFLOW_CHANNEL='%s' to QueryParameters\n", ch);
                // Also include in parameters for agent usage if desired
                have_params = true;
                cJSON_AddItemToObject(root, "channel", cJSON_CreateString(ch));
            }
            const char* js = switch_channel_get_variable(channel, "DIALOGFLOW_PARAMS");
            if (js && *js) {
                cJSON* json = cJSON_Parse(js);
                if (json) {
                    logMergedParamsJSON(session, "GStreamer::startStream DIALOGFLOW_PARAMS JSON", json);
                    // Remember original request params JSON for event echoing
                    m_requestParamsJSON = js;
                    have_params = true;
                    // If JSON includes a top-level "channel", set QueryParameters.channel too
                    cJSON* chv = cJSON_GetObjectItemCaseSensitive(json, "channel");
                    if (cJSON_IsString(chv) && chv->valuestring && chv->valuestring[0]) {
                        m_request->mutable_query_params()->set_channel(chv->valuestring);
                        m_qpChannel = chv->valuestring;
                    }
                    for (cJSON* it = json->child; it; it = it->next) {
                        addOrReplaceJSONField(root, it->string, cJSON_Duplicate(it, 1));
                    }
                    cJSON_Delete(json);
                } else {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "GStreamer::startStream DIALOGFLOW_PARAMS not valid JSON: %s\n", js);
                }
            }
            // Optionally include all channel variables
            if (cJSON* chvars = collectChannelVarsAsJSON(session)) {
                have_params = true;
                for (cJSON* it = chvars->child; it; it = it->next) {
                    addOrReplaceJSONField(root, it->string, cJSON_Duplicate(it, 1));
                }
                cJSON_Delete(chvars);
            }
            if (have_params) {
                logMergedParamsJSON(session, "GStreamer::startStream merged parameters JSON (event)", root);
                auto* qp = m_request->mutable_query_params();
                auto* params = qp->mutable_parameters();
                parseEventParams(params, root);
            }
            cJSON_Delete(root);
            // After an event-driven prompt, prepare to start the next user turn with audio
            m_needConfig.store(true);
        }
        else if (text) {
            if (text[0] == '{') {
                cJSON* json = cJSON_Parse(text);
                if (json) {
                    logMergedParamsJSON(session, "GStreamer::startStream text payload JSON", json);
                    auto* qp = m_request->mutable_query_params();
                    auto* params = qp->mutable_parameters();
                    parseEventParams(params, json);
                    // If JSON includes a top-level "channel", set QueryParameters.channel too
                    cJSON* chv = cJSON_GetObjectItemCaseSensitive(json, "channel");
                    if (cJSON_IsString(chv) && chv->valuestring && chv->valuestring[0]) {
                        qp->set_channel(chv->valuestring);
                    }
                    cJSON_Delete(json);
                    // Optionally inject channel variables alongside provided JSON
                    if (cJSON* chvars = collectChannelVarsAsJSON(session)) {
                        parseEventParams(params, chvars);
                        cJSON_Delete(chvars);
                    }
                    auto* audio_input = queryInput->mutable_audio();
                    auto* audio_config = audio_input->mutable_config();
                    audio_config->set_sample_rate_hertz((int)m_sampleRate);
                    audio_config->set_audio_encoding(AudioEncoding::AUDIO_ENCODING_LINEAR_16);
                    audio_config->set_single_utterance(true);
                    // Mark start of a text+audio turn (we'll send audio next)
                    markTurnStart();
                    queryInput->set_language_code(m_lang.c_str());
                } else {
                    auto* textInput = queryInput->mutable_text();
                    textInput->set_text(text);
                    queryInput->set_language_code(m_lang.c_str());
                    // Start next turn in audio mode after sending a plain text input
                    m_needConfig.store(true);
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                        "GStreamer::startStream initial request uses plain text input (no JSON)\n");
                }
            } else {
                auto* textInput = queryInput->mutable_text();
                textInput->set_text(text);
                queryInput->set_language_code(m_lang.c_str());
                // Start next turn in audio mode after sending a plain text input
                m_needConfig.store(true);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                    "GStreamer::startStream initial request uses plain text input\n");
                // Optionally inject channel variables alongside plain text
                if (cJSON* chvars = collectChannelVarsAsJSON(session)) {
                    auto* qp = m_request->mutable_query_params();
                    auto* params = qp->mutable_parameters();
                    parseEventParams(params, chvars);
                    cJSON_Delete(chvars);
                }
            }
            // Treat text start similar to event (first stream is non-audio)
            m_startedWithEvent = true;
            // Also honor DIALOGFLOW_CHANNEL and DIALOGFLOW_PARAMS when starting with text/JSON form
            const char* ch = switch_channel_get_variable(channel, "DIALOGFLOW_CHANNEL");
            if (ch && *ch) {
                m_request->mutable_query_params()->set_channel(ch);
                m_qpChannel = ch;
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                    "GStreamer::startStream applying DIALOGFLOW_CHANNEL='%s' to QueryParameters\n", ch);
            }
            const char* js = switch_channel_get_variable(channel, "DIALOGFLOW_PARAMS");
            if (js && *js) {
                cJSON* json2 = cJSON_Parse(js);
                if (json2) {
                    logMergedParamsJSON(session, "GStreamer::startStream DIALOGFLOW_PARAMS JSON", json2);
                    m_requestParamsJSON = js;
                    cJSON* chv2 = cJSON_GetObjectItemCaseSensitive(json2, "channel");
                    if (cJSON_IsString(chv2) && chv2->valuestring && chv2->valuestring[0]) {
                        m_request->mutable_query_params()->set_channel(chv2->valuestring);
                        m_qpChannel = chv2->valuestring;
                    }
                    cJSON_Delete(json2);
                }
            }
        }
        else {
            auto* audio_input = queryInput->mutable_audio();
            auto* audio_config = audio_input->mutable_config();
            audio_config->set_sample_rate_hertz((int)m_sampleRate);
            audio_config->set_audio_encoding(AudioEncoding::AUDIO_ENCODING_LINEAR_16);
            audio_config->set_single_utterance(true);
            queryInput->set_language_code(m_lang.c_str());
            // Mark start of initial audio-configured turn
            markTurnStart();
            // Optionally inject channel variables even for pure audio start
            if (cJSON* chvars = collectChannelVarsAsJSON(session)) {
                auto* qp = m_request->mutable_query_params();
                auto* params = qp->mutable_parameters();
                parseEventParams(params, chvars);
                cJSON_Delete(chvars);
            }
        }
        logQueryParamsSummary(session, "GStreamer::startStream final QueryParameters state", m_request->query_params());
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::startStream requesting OutputAudio in LINEAR16 @%uHz; custom params? %s\n",
                          (unsigned)m_sampleRate, isAnyOutputAudioConfigChanged() ? "yes" : "no");
        auto* outputAudioConfig = m_request->mutable_output_audio_config();
        outputAudioConfig->set_sample_rate_hertz((int)m_sampleRate);
        outputAudioConfig->set_audio_encoding(m_outputEncoding);

        if (isAnyOutputAudioConfigChanged()) {
            auto* synthesizeSpeechConfig = outputAudioConfig->mutable_synthesize_speech_config();
            if (m_speakingRate) synthesizeSpeechConfig->set_speaking_rate(m_speakingRate);
            if (m_pitch) synthesizeSpeechConfig->set_pitch(m_pitch);
            if (m_volume) synthesizeSpeechConfig->set_volume_gain_db(m_volume);
            if (!m_effects.empty()) synthesizeSpeechConfig->add_effects_profile_id(m_effects);

            auto* voice = synthesizeSpeechConfig->mutable_voice();
            if (!m_voiceName.empty()) voice->set_name(m_voiceName);
            if (!m_voiceGender.empty()) {
                SsmlVoiceGender gender = SsmlVoiceGender::SSML_VOICE_GENDER_UNSPECIFIED;
                switch (toupper(m_voiceGender[0]))
                {
                    case 'F': gender = SsmlVoiceGender::SSML_VOICE_GENDER_FEMALE; break;
                    case 'M': gender = SsmlVoiceGender::SSML_VOICE_GENDER_MALE; break;
                    case 'N': gender = SsmlVoiceGender::SSML_VOICE_GENDER_NEUTRAL; break;
                }
                voice->set_ssml_gender(gender);
            }
        }

        if (m_sentimentAnalysis) {
            auto* queryParameters = m_request->mutable_query_params();
            queryParameters->set_analyze_query_text_sentiment(true);
        }

		m_streamer = m_stub->StreamingDetectIntent(m_context.get());
		m_streamer->Write(*m_request);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::startStream initial request sent; waiting for responses\n");
	}
	bool write(switch_core_session_t* session, void* data, uint32_t datalen) {
		if (m_finished) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::write not writing because we are finished, %p\n", this);
			return false;
		}

        // If paused (e.g., while playing agent audio to caller), do not advance the turn
        if (m_paused.load()) {
            return true; // treat as success but skip sending to Dialogflow
        }

        auto* qi = m_request->mutable_query_input();
        qi->clear_text();
        qi->clear_event();
        qi->clear_intent();
        auto* ai = qi->mutable_audio();
		bool sendConfig = m_needConfig.exchange(false);
        if (sendConfig) {
            auto* audio_config = ai->mutable_config();
            audio_config->set_sample_rate_hertz((int)m_sampleRate);
            audio_config->set_audio_encoding(AudioEncoding::AUDIO_ENCODING_LINEAR_16);
            audio_config->set_single_utterance(true);
            maybeApplyPendingBargeIn(audio_config, session, "GStreamer::write");
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::write sent new audio config to start next turn\n");
            // Mark start of a new turn when sending a fresh audio config
            markTurnStart();
        } else {
            ai->clear_config();
        }
        ai->set_audio(reinterpret_cast<const char*>(data), datalen);

        size_t sampleCount = datalen / sizeof(int16_t);
        if (sampleCount) {
            m_spool.ingest(session, reinterpret_cast<const int16_t*>(data), sampleCount);
            m_totalSamples = m_spool.totalSamples;
        }

		m_packets++;
    return m_streamer->Write(*m_request);

	}
	bool read(StreamingDetectIntentResponse* response) {
		return m_streamer->Read(response);
	}
	grpc::Status finish() {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::finish %p\n", this);
		if (m_finished) {
			grpc::Status ok;
			return ok;
		}
		m_finished = true;
		return m_streamer->Finish();
	}
    void writesDone() {
        m_streamer->WritesDone();
    }

    bool isFinished() {
        return m_finished;
    }

    void cancel() {
        if (m_context) m_context->TryCancel();
    }

    bool isStopping() const { return m_finished; }

    bool isAnyOutputAudioConfigChanged() {
        return m_speakingRate|| m_pitch || m_volume || !m_voiceName.empty() || !m_voiceGender.empty() || !m_effects.empty();
    }

    void setNeedConfig() {
        m_needConfig.store(true);
    }

    void setPendingBargeIn(uint64_t totalDurationMs, uint64_t noBargeInDurationMs) {
        if (!totalDurationMs) {
            clearPendingBargeIn();
            return;
        }
        m_pendingBargeIn.store(true);
        m_pendingBargeInTotalMs = totalDurationMs;
        m_pendingBargeInNoBargeMs = std::min(noBargeInDurationMs, totalDurationMs);
    }

    void clearPendingBargeIn() {
        m_pendingBargeIn.store(false);
        m_pendingBargeInTotalMs = 0;
        m_pendingBargeInNoBargeMs = 0;
    }

    void setPaused(bool paused) {
        m_paused.store(paused);
    }

    bool isPaused() const { return m_paused.load(); }

    const std::string& qpChannel() const { return m_qpChannel; }
    const std::string& requestParamsJSON() const { return m_requestParamsJSON; }
    const std::string& sessionId() const { return m_sessionId; }
    uint32_t sampleRate() const { return m_sampleRate; }
    uint64_t currentTurnIndex() const { return m_turnIndex; }
    uint64_t turnStartSampleCount() const { return m_turnStartSample; }
    uint64_t totalSamplesSent() const { return m_totalSamples; }
    bool isSpooling() const { return m_spool.enabled == SWITCH_TRUE; }
    switch_status_t captureSnippet(switch_core_session_t* session, uint64_t startMs, uint64_t durationMs,
            const std::string& tag, std::string& outPath, uint64_t& actualStartMs, uint64_t& actualEndMs) {
        return m_spool.capture(session, startMs, durationMs, tag, outPath, actualStartMs, actualEndMs);
    }
    void stopSpool() {
        m_spool.cleanup();
    }

    // Turn timing markers
    void markTurnStart() {
        m_turnStartMs = switch_micro_time_now() / 1000;
        m_finalRecogMs = 0; m_eouMs = 0; m_detectMs = 0;
        m_turnStartSample = m_spool.totalSamples;
        ++m_turnIndex;
        m_totalSamples = m_spool.totalSamples;
    }
    void markFinalRecog() { if (!m_finalRecogMs) m_finalRecogMs = switch_micro_time_now() / 1000; }
    void markEOU() { if (!m_eouMs) m_eouMs = switch_micro_time_now() / 1000; }
    void markDetect() { if (!m_detectMs) m_detectMs = switch_micro_time_now() / 1000; }
    uint64_t turnStartMs() const { return m_turnStartMs; }
    uint64_t finalRecogMs() const { return m_finalRecogMs; }
    uint64_t eouMs() const { return m_eouMs; }
    uint64_t detectMs() const { return m_detectMs; }

    void rotateToAudioConfig(switch_core_session_t* session) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
            "GStreamer: rotating stream to audio mode for next user turn\n");
        switch_channel_t* channel = switch_core_session_get_channel(session);
        // Gracefully finish current stream
        try { m_streamer->WritesDone(); } catch (...) {}
        try { m_streamer->Finish(); } catch (...) {}

        // Create a new stream configured for audio input
        m_request = std::make_shared<StreamingDetectIntentRequest>();
        m_context = std::make_shared<grpc::ClientContext>();
        // Reuse same session path
        char szSession[256];
        if (!m_environment.empty() && strcasecmp(m_environment.c_str(), "draft") != 0) {
            snprintf(szSession, 256, "projects/%s/locations/%s/agents/%s/environments/%s/sessions/%s",
                     m_projectId.c_str(), m_regionId.c_str(), m_agentId.c_str(), m_environment.c_str(), m_sessionId.c_str());
        } else {
            snprintf(szSession, 256, "projects/%s/locations/%s/agents/%s/sessions/%s",
                     m_projectId.c_str(), m_regionId.c_str(), m_agentId.c_str(), m_sessionId.c_str());
        }
        m_request->set_session(szSession);

        auto* qi = m_request->mutable_query_input();
        auto* audio_input = qi->mutable_audio();
        auto* audio_config = audio_input->mutable_config();
        audio_config->set_sample_rate_hertz((int)m_sampleRate);
        audio_config->set_audio_encoding(AudioEncoding::AUDIO_ENCODING_LINEAR_16);
        audio_config->set_single_utterance(true);
        maybeApplyPendingBargeIn(audio_config, session, "GStreamer::rotateToAudioConfig");
        qi->set_language_code(m_lang.c_str());

        // Always request output audio
        auto* outputAudioConfig = m_request->mutable_output_audio_config();
        outputAudioConfig->set_sample_rate_hertz((int)m_sampleRate);
        outputAudioConfig->set_audio_encoding(m_outputEncoding);
        if (isAnyOutputAudioConfigChanged()) {
            auto* synthesizeSpeechConfig = outputAudioConfig->mutable_synthesize_speech_config();
            if (m_speakingRate) synthesizeSpeechConfig->set_speaking_rate(m_speakingRate);
            if (m_pitch) synthesizeSpeechConfig->set_pitch(m_pitch);
            if (m_volume) synthesizeSpeechConfig->set_volume_gain_db(m_volume);
            if (!m_effects.empty()) synthesizeSpeechConfig->add_effects_profile_id(m_effects);
            auto* voice = synthesizeSpeechConfig->mutable_voice();
            if (!m_voiceName.empty()) voice->set_name(m_voiceName);
            if (!m_voiceGender.empty()) {
                SsmlVoiceGender gender = SsmlVoiceGender::SSML_VOICE_GENDER_UNSPECIFIED;
                switch (toupper(m_voiceGender[0]))
                {
                    case 'F': gender = SsmlVoiceGender::SSML_VOICE_GENDER_FEMALE; break;
                    case 'M': gender = SsmlVoiceGender::SSML_VOICE_GENDER_MALE; break;
                    case 'N': gender = SsmlVoiceGender::SSML_VOICE_GENDER_NEUTRAL; break;
                }
                voice->set_ssml_gender(gender);
            }
        }

        // Re-apply query params (channel)
        auto* qp = m_request->mutable_query_params();
        if (!m_qpChannel.empty()) {
            qp->set_channel(m_qpChannel);
            // keep header var in sync
            switch_channel_set_variable(channel, "DF_CHANNEL", m_qpChannel.c_str());
        }
        if (m_sentimentAnalysis) qp->set_analyze_query_text_sentiment(true);

        // Also expose the logical channel as a channel var for headers, if present
        if (!m_qpChannel.empty()) switch_channel_set_variable(channel, "DF_CHANNEL", m_qpChannel.c_str());

        m_needConfig.store(false);
        m_streamer = m_stub->StreamingDetectIntent(m_context.get());
        // Mark start of the new audio-configured turn
        markTurnStart();
        m_streamer->Write(*m_request);
        // Subsequent writes will send only audio bytes
    }

private:
    void maybeApplyPendingBargeIn(InputAudioConfig* audio_config, switch_core_session_t* session, const char* context) {
        if (!audio_config || !m_pendingBargeIn.exchange(false)) {
            return;
        }

        auto* config = audio_config->mutable_barge_in_config();
        set_proto_duration_ms(config->mutable_no_barge_in_duration(), m_pendingBargeInNoBargeMs);
        set_proto_duration_ms(config->mutable_total_duration(), m_pendingBargeInTotalMs);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
            "%s applying Dialogflow barge_in_config total=%" PRIu64 "ms no_barge=%" PRIu64 "ms\n",
            context, m_pendingBargeInTotalMs, m_pendingBargeInNoBargeMs);
        m_pendingBargeInTotalMs = 0;
        m_pendingBargeInNoBargeMs = 0;
    }

    std::string m_sessionId;
    std::shared_ptr<grpc::ClientContext> m_context;
    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<Sessions::Stub> 	m_stub;
    std::unique_ptr< grpc::ClientReaderWriterInterface<StreamingDetectIntentRequest, StreamingDetectIntentResponse> > m_streamer;
    std::shared_ptr<StreamingDetectIntentRequest> m_request;
    std::string m_lang;
    std::string m_projectId;
    std::string m_agentId;
    std::string m_environment;
    std::string m_regionId;
    double m_speakingRate;
    double m_pitch;
    double m_volume;
    std::string m_effects;
    std::string m_voiceName;
    std::string m_voiceGender;
    bool m_sentimentAnalysis;
    bool m_finished;
    uint32_t m_packets;
    std::atomic<bool> m_needConfig;
    std::atomic<bool> m_paused;
    bool m_startedWithEvent;
    bool m_rotatedToAudio;
    std::string m_qpChannel;
    uint32_t m_sampleRate;
   OutputAudioEncoding m_outputEncoding;
    std::string m_requestParamsJSON;
    AudioSpooler m_spool;
    uint64_t m_totalSamples;
    uint64_t m_turnStartSample;
    uint64_t m_turnIndex;
    std::atomic<bool> m_pendingBargeIn{false};
    uint64_t m_pendingBargeInTotalMs{0};
    uint64_t m_pendingBargeInNoBargeMs{0};
    // Turn timing
    uint64_t m_turnStartMs = 0;
    uint64_t m_finalRecogMs = 0;
    uint64_t m_eouMs = 0;
    uint64_t m_detectMs = 0;
};

static std::vector<std::string> backchannel_prompts(const std::string& operation) {
    if (operation == "lookup") {
        return {
            "Attendi mentre recupero i dati.",
            "Sto ancora verificando le informazioni.",
            "Ancora un attimo, sto completando il controllo.",
            "Grazie per l'attesa, controllo gli ultimi dettagli."
        };
    }
    if (operation == "payments") {
        return {
            "Attendi mentre recupero i dettagli dei pagamenti.",
            "Sto ancora verificando le informazioni sui pagamenti.",
            "Ancora un attimo, sto completando il controllo.",
            "Grazie per l'attesa, controllo gli ultimi dettagli."
        };
    }
    return {};
}

static uint64_t backchannel_hash(const std::string& value) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::shared_ptr<grpc::Channel> create_tts_channel(switch_core_session_t* session) {
    switch_channel_t* channel = switch_core_session_get_channel(session);
    const char* credentialValue = switch_channel_get_variable(channel, "GOOGLE_APPLICATION_CREDENTIALS");
    std::string credentialsInput = credentialValue ? credentialValue : "";
    std::string credentialsJson = credentialsInput;
    if (!credentialsInput.empty() && (credentialsInput[0] == '/' ||
        (credentialsInput.size() >= 5 && credentialsInput.compare(credentialsInput.size() - 5, 5, ".json") == 0))) {
        std::ifstream credentialsFile(credentialsInput);
        if (!credentialsFile.good()) {
            throw std::runtime_error("TTS credentials file is not readable");
        }
        credentialsJson.assign(std::istreambuf_iterator<char>(credentialsFile), std::istreambuf_iterator<char>());
    }

    std::shared_ptr<grpc::ChannelCredentials> channelCredentials;
    if (!credentialsJson.empty()) {
        auto callCredentials = grpc::ServiceAccountJWTAccessCredentials(credentialsJson, INT64_MAX);
        channelCredentials = grpc::CompositeChannelCredentials(
            grpc::SslCredentials(grpc::SslCredentialsOptions()), callCredentials);
    } else {
        channelCredentials = grpc::GoogleDefaultCredentials();
    }
    return grpc::CreateChannel("texttospeech.googleapis.com:443", channelCredentials);
}

static std::string synthesize_backchannel_audio(
    switch_core_session_t* session,
    const std::string& voice,
    const std::string& language,
    const std::string& phrase) {
    if (voice.empty() || language.empty() || phrase.empty()) {
        throw std::runtime_error("TTS voice, language, and phrase are required");
    }

    switch_channel_t* channel = switch_core_session_get_channel(session);
    const char* rateValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_SPEAKING_RATE");
    const char* pitchValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_PITCH");
    const char* volumeValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_VOLUME_GAIN_DB");
    const char* effectsValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_EFFECTS_PROFILE_ID");
    std::string cacheKey = voice + "\n" + language + "\n" + phrase + "\n" +
        (rateValue ? rateValue : "") + "\n" + (pitchValue ? pitchValue : "") + "\n" +
        (volumeValue ? volumeValue : "") + "\n" + (effectsValue ? effectsValue : "") + "\n8000";
    {
        std::lock_guard<std::mutex> cacheLock(backchannelCacheMutex);
        auto cached = backchannelAudioCache.find(cacheKey);
        if (cached != backchannelAudioCache.end()) {
            std::ifstream cachedFile(cached->second, std::ios::binary);
            if (cachedFile.good()) return cached->second;
            backchannelAudioCache.erase(cached);
        }
    }

    const uint64_t hash = backchannel_hash(cacheKey);
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mod_dialogflow_backchannel_%016" PRIx64 ".wav", hash);
    {
        std::ifstream existingFile(path, std::ios::binary);
        if (existingFile.good()) {
            std::lock_guard<std::mutex> cacheLock(backchannelCacheMutex);
            backchannelAudioCache[cacheKey] = path;
            return path;
        }
    }

    auto channelHandle = create_tts_channel(session);
    auto stub = TextToSpeech::NewStub(channelHandle);
    SynthesizeSpeechRequest request;
    request.mutable_input()->set_text(phrase);
    request.mutable_voice()->set_name(voice);
    request.mutable_voice()->set_language_code(language);
    auto* audioConfig = request.mutable_audio_config();
    audioConfig->set_audio_encoding(TtsAudioEncoding::LINEAR16);
    audioConfig->set_sample_rate_hertz(8000);
    if (rateValue && *rateValue) audioConfig->set_speaking_rate(atof(rateValue));
    if (pitchValue && *pitchValue) audioConfig->set_pitch(atof(pitchValue));
    if (volumeValue && *volumeValue) audioConfig->set_volume_gain_db(atof(volumeValue));
    if (effectsValue && *effectsValue) audioConfig->add_effects_profile_id(effectsValue);

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    google::cloud::texttospeech::v1::SynthesizeSpeechResponse response;
    grpc::Status status = stub->SynthesizeSpeech(&context, request, &response);
    if (!status.ok()) {
        throw std::runtime_error("Cloud Text-to-Speech failed: " + status.error_message());
    }
    const std::string& audio = response.audio_content();
    if (audio.size() < 44 || audio.compare(0, 4, "RIFF") != 0 || audio.compare(8, 4, "WAVE") != 0) {
        throw std::runtime_error("Cloud Text-to-Speech returned invalid LINEAR16 WAV data");
    }

    std::string temporaryPath = std::string(path) + ".tmp." + std::to_string(switch_micro_time_now());
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        output.write(audio.data(), static_cast<std::streamsize>(audio.size()));
        if (!output.good()) {
            std::remove(temporaryPath.c_str());
            throw std::runtime_error("Unable to write synthesized backchannel audio");
        }
    }
    if (std::rename(temporaryPath.c_str(), path) != 0) {
        std::remove(temporaryPath.c_str());
        throw std::runtime_error("Unable to publish synthesized backchannel audio");
    }
    {
        std::lock_guard<std::mutex> cacheLock(backchannelCacheMutex);
        backchannelAudioCache[cacheKey] = path;
    }
    return path;
}

static void backchannel_broadcast(const std::shared_ptr<BackchannelLoop>& loop, const std::string& path) {
    switch_core_session_t* session = switch_core_session_locate(loop->uuid.c_str());
    if (!session) return;
    switch_channel_t* channel = switch_core_session_get_channel(session);
    const char* active = switch_channel_get_variable(channel, "DIALOGFLOW_BACKCHANNEL_ACTIVE");
    if (switch_channel_ready(channel) && active && switch_true(active)) {
        char args[MAX_PATHLEN + 300];
        snprintf(args, sizeof(args), "%s %s aleg", loop->uuid.c_str(), path.c_str());
        switch_stream_handle_t stream = { 0 };
        SWITCH_STANDARD_STREAM(stream);
        switch_status_t status = switch_api_execute("uuid_broadcast", args, NULL, &stream);
        const char* response = static_cast<const char*>(stream.data);
        if (status != SWITCH_STATUS_SUCCESS || (response && !strncmp(response, "-ERR", 4))) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                "Backchannel playback failed for %s: %s\n", loop->uuid.c_str(), response ? response : "no response");
        }
        switch_safe_free(stream.data);
    }
    switch_core_session_rwunlock(session);
}

static void backchannel_loop_worker(const std::shared_ptr<BackchannelLoop>& loop) {
    size_t promptIndex = 0;
    const uint32_t intervalsMs[] = { 9000, 11000, 10000, 12000 };
    const auto prompts = backchannel_prompts(loop->operation);
    if (prompts.empty()) {
        std::lock_guard<std::mutex> lock(loop->mutex);
        loop->finished = true;
        loop->condition.notify_all();
        return;
    }
    while (true) {
        {
            std::lock_guard<std::mutex> lock(loop->mutex);
            if (loop->stopping) break;
        }
        if (promptIndex >= loop->audioFiles.size()) {
            switch_core_session_t* session = switch_core_session_locate(loop->uuid.c_str());
            if (!session) break;
            try {
                loop->audioFiles.push_back(synthesize_backchannel_audio(
                    session, loop->voice, loop->language, prompts[promptIndex]));
            } catch (const std::exception& error) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                    "Unable to synthesize backchannel phrase: %s\n", error.what());
                if (loop->audioFiles.empty()) {
                    switch_core_session_rwunlock(session);
                    break;
                }
                loop->audioFiles.push_back(loop->audioFiles.front());
            }
            switch_core_session_rwunlock(session);
        }
        {
            std::lock_guard<std::mutex> lock(loop->mutex);
            if (loop->stopping) break;
        }
        const auto nextPromptTime = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(intervalsMs[promptIndex]);
        backchannel_broadcast(loop, loop->audioFiles[promptIndex]);
        const size_t nextPromptIndex = (promptIndex + 1) % prompts.size();
        if (nextPromptIndex >= loop->audioFiles.size()) {
            switch_core_session_t* session = switch_core_session_locate(loop->uuid.c_str());
            if (!session) break;
            try {
                loop->audioFiles.push_back(synthesize_backchannel_audio(
                    session, loop->voice, loop->language, prompts[nextPromptIndex]));
            } catch (const std::exception& error) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                    "Unable to synthesize backchannel phrase: %s\n", error.what());
                if (loop->audioFiles.empty()) {
                    switch_core_session_rwunlock(session);
                    break;
                }
                loop->audioFiles.push_back(loop->audioFiles.front());
            }
            switch_core_session_rwunlock(session);
        }
        std::unique_lock<std::mutex> lock(loop->mutex);
        const bool stopping = loop->condition.wait_until(lock,
            nextPromptTime, [loop] { return loop->stopping; });
        if (stopping) break;
        promptIndex = nextPromptIndex;
    }
    {
        std::lock_guard<std::mutex> lock(loop->mutex);
        loop->finished = true;
    }
    loop->condition.notify_all();
    std::lock_guard<std::mutex> loopsLock(backchannelLoopsMutex);
    auto found = backchannelLoops.find(loop->uuid);
    if (found != backchannelLoops.end() && found->second == loop) backchannelLoops.erase(found);
}

static void stop_backchannel_loop(const std::string& uuid) {
    std::shared_ptr<BackchannelLoop> loop;
    {
        std::lock_guard<std::mutex> lock(backchannelLoopsMutex);
        auto found = backchannelLoops.find(uuid);
        if (found != backchannelLoops.end()) loop = found->second;
    }
    if (!loop) return;
    {
        std::lock_guard<std::mutex> lock(loop->mutex);
        loop->stopping = true;
    }
    loop->condition.notify_all();
    std::unique_lock<std::mutex> lock(loop->mutex);
    loop->condition.wait_for(lock, std::chrono::seconds(6), [loop] { return loop->finished; });
}

static void killcb(struct cap_cb* cb) {
	if (cb) {
		if (cb->streamer) {
			GStreamer* p = (GStreamer *) cb->streamer;
			delete p;
			cb->streamer = NULL;
		}
		if (cb->vad) {
			switch_vad_destroy(&cb->vad);
			cb->vad = NULL;
		}
		if (cb->resampler) {
				speex_resampler_destroy(cb->resampler);
				cb->resampler = NULL;
		}
	}
}

static void *SWITCH_THREAD_FUNC grpc_read_thread(switch_thread_t *thread, void *obj) {
	struct cap_cb *cb = (struct cap_cb *) obj;
	GStreamer* streamer = (GStreamer *) cb->streamer;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "grpc_read_thread: starting cb %p\n", (void *) cb);

	// Our contract: while we are reading, cb and cb->streamer will not be deleted

	// Read responses until there are no more
	StreamingDetectIntentResponse response;
	while (streamer->read(&response)) {  
		switch_core_session_t* psession = switch_core_session_locate(cb->sessionId);
		if (psession) {
			switch_channel_t* channel = switch_core_session_get_channel(psession);
			GRPCParser parser(psession);

            // If stopping requested, break out quickly
            if (cb->stopping || !switch_channel_ready(channel)) {
                switch_core_session_rwunlock(psession);
                return NULL;
            }

            if (response.has_detect_intent_response() || response.has_recognition_result()) {
                // Determine event type and whether to suppress interim transcripts
                const char* type = DIALOGFLOW_EVENT_TRANSCRIPTION;
                bool suppress = false;
                if (response.has_detect_intent_response()) {
                    type = DIALOGFLOW_EVENT_INTENT;
                } else if (response.has_recognition_result()) {
                    const auto& rr = response.recognition_result();
                    auto o = rr.message_type();
                    bool is_eou = (0 == StreamingRecognitionResult::MessageType_Name(o).compare("END_OF_SINGLE_UTTERANCE"));
                    if (is_eou || rr.is_final() || !rr.transcript().empty()) {
                        maybe_log_barge_timing(cb, psession, rr, is_eou);
                    }
                    if (cb->interruptible_playback_active && (is_eou || rr.is_final() || !rr.transcript().empty())) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
                            "grpc_read_thread: barge-in detected, stopping active playback (final=%s transcript='%s' eou=%s)\n",
                            rr.is_final() ? "true" : "false",
                            rr.transcript().c_str(),
                            is_eou ? "true" : "false");
                        stop_playback(psession);
                        reset_interruptible_playback_state(cb);
                    }
                    if (is_eou) {
                        type = DIALOGFLOW_EVENT_END_OF_UTTERANCE;
                        streamer->markEOU();
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                            "grpc_read_thread: END_OF_SINGLE_UTTERANCE received\n");
                    } else {
                        bool final_only = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_TRANSCRIPT_FINAL_ONLY"));
                        if (final_only && !rr.is_final()) {
                            suppress = true; // skip interim transcripts
                        }
                        if (rr.is_final()) {
                            streamer->markFinalRecog();
                        }
                    }
                }

                if (!suppress) {
                    // If this is a detect_intent_response, mark detection time before composing payload
                    if (response.has_detect_intent_response()) {
                        streamer->markDetect();
                    }
                    // Optional throttle for interim transcripts
                    if (type == DIALOGFLOW_EVENT_TRANSCRIPTION) {
                        bool final_only = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_TRANSCRIPT_FINAL_ONLY"));
                        if (!final_only) {
                            const char* th = switch_channel_get_variable(channel, "DIALOGFLOW_TRANSCRIPT_THROTTLE_MS");
                            int throttle = th ? atoi(th) : 0;
                            if (throttle > 0 && response.has_recognition_result()) {
                                const auto& rr = response.recognition_result();
                                if (!rr.is_final()) {
                                    uint64_t now_ms = switch_micro_time_now() / 1000;
                                    if (cb->lastTranscriptMs && (now_ms - cb->lastTranscriptMs < (uint64_t)throttle)) {
                                        // skip this interim due to throttle
                                        continue;
                                    }
                                    cb->lastTranscriptMs = now_ms;
                                }
                    }
                }
            }
            cJSON* jResponse = parser.parse(response);
            if (response.has_recognition_result()) {
                const auto& rrMeta = response.recognition_result();
                cJSON* jrr = cJSON_GetObjectItemCaseSensitive(jResponse, "recognition_result");
                if (jrr) {
                    double turnStartMs = samples_to_ms(streamer->turnStartSampleCount(), streamer->sampleRate());
                    cJSON_AddItemToObject(jrr, "turn_id", cJSON_CreateNumber((double)streamer->currentTurnIndex()));
                    cJSON_AddItemToObject(jrr, "turn_start_ms", cJSON_CreateNumber(turnStartMs));

                    uint64_t absStartSamples = streamer->turnStartSampleCount();
                    uint64_t absEndSamples = absStartSamples;
                    if (rrMeta.speech_word_info_size() > 0) {
                        absStartSamples += duration_to_samples(rrMeta.speech_word_info(0).start_offset(), streamer->sampleRate());
                        const auto& lastWord = rrMeta.speech_word_info(rrMeta.speech_word_info_size() - 1);
                        absEndSamples += duration_to_samples(lastWord.end_offset(), streamer->sampleRate());
                    } else if (rrMeta.is_final()) {
                        absEndSamples = streamer->totalSamplesSent();
                    }
                    if (rrMeta.is_final() && absEndSamples < streamer->totalSamplesSent()) {
                        absEndSamples = streamer->totalSamplesSent();
                    }
                    cJSON_AddItemToObject(jrr, "absolute_start_ms", cJSON_CreateNumber(samples_to_ms(absStartSamples, streamer->sampleRate())));
                    cJSON_AddItemToObject(jrr, "absolute_end_ms", cJSON_CreateNumber(samples_to_ms(absEndSamples, streamer->sampleRate())));
                    cJSON_AddItemToObject(jrr, "spool_available", cJSON_CreateBool(streamer->isSpooling()));
                }
            }
            // Optionally enrich with diagnostic_info and turn timing
            if (response.has_detect_intent_response()) {
                        stop_backchannel(psession);
                        const auto& dir2 = response.detect_intent_response();
                        if (dir2.has_query_result()) {
                            const auto& qr2 = dir2.query_result();
                            // Include diagnostic_info when requested (default: on)
                            const char* diagVar = switch_channel_get_variable(channel, "DIALOGFLOW_INCLUDE_DIAGNOSTIC_INFO");
                            bool include_diag = (diagVar == NULL) ? true : switch_true(diagVar);
                            if (include_diag && qr2.has_diagnostic_info()) {
                                cJSON* jqr = cJSON_GetObjectItemCaseSensitive(jResponse, "query_result");
                                if (jqr) {
                                    GRPCParser p2(psession);
                                    cJSON* diag = p2.parseStruct(qr2.diagnostic_info());
                                    if (diag) cJSON_AddItemToObject(jqr, "diagnostic_info", diag);
                                }
                            }
                            // Add coarse turn timing when we are emitting INTENT or final transcript
                            // Controlled by DIALOGFLOW_INCLUDE_TURN_TIMING (default: true)
                            const char* ttVar = switch_channel_get_variable(channel, "DIALOGFLOW_INCLUDE_TURN_TIMING");
                            bool include_timing = (ttVar == NULL) ? true : switch_true(ttVar);
                            bool add_timing = include_timing && (type == DIALOGFLOW_EVENT_INTENT);
                            if (!add_timing && include_timing && response.has_recognition_result()) {
                                const auto& rr2 = response.recognition_result();
                                add_timing = rr2.is_final();
                            }
                            if (add_timing) {
                                uint64_t t0 = streamer->turnStartMs();
                                uint64_t te = streamer->finalRecogMs() ? streamer->finalRecogMs() : streamer->eouMs();
                                uint64_t td = streamer->detectMs();
                                if (t0 && td) {
                                    cJSON* tt = cJSON_CreateObject();
                                    uint64_t total = td - t0;
                                    cJSON_AddItemToObject(tt, "total_ms", cJSON_CreateNumber((double)total));
                                    if (te && te >= t0 && td >= te) {
                                        uint64_t asr = te - t0;
                                        uint64_t post = td - te;
                                        cJSON_AddItemToObject(tt, "asr_ms", cJSON_CreateNumber((double)asr));
                                        cJSON_AddItemToObject(tt, "post_asr_ms", cJSON_CreateNumber((double)post));
                                        // Optional logging
                                        bool log_timing = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_LOG_TURN_TIMING"));
                                        if (log_timing) {
                                            const char* sid = streamer->sessionId().c_str();
                                            const char* intent = switch_channel_get_variable(channel, "DF_INTENT"); intent = intent ? intent : "";
                                            const char* page = switch_channel_get_variable(channel, "DF_PAGE"); page = page ? page : "";
                                            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
                                                "DF turn timing: total=%llums asr=%llums post_asr=%llums (session=%s intent='%s' page='%s')\n",
                                                (unsigned long long)total, (unsigned long long)asr, (unsigned long long)post, sid, intent, page);
                                        }
                                    }
                                    cJSON* jqr = cJSON_GetObjectItemCaseSensitive(jResponse, "query_result");
                                    if (!jqr) jqr = jResponse; // fallback top-level
                                    cJSON_AddItemToObject(jqr, "turn_timing", tt);
                                }
                            }
                        }
                    }
                    // Optionally include request query_params on all DF events
                    bool include_qp = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_INCLUDE_QUERY_PARAMS"));
                    if (include_qp) {
                        cJSON* jq = cJSON_CreateObject();
                        if (!streamer->qpChannel().empty()) {
                            cJSON_AddItemToObject(jq, "channel", cJSON_CreateString(streamer->qpChannel().c_str()));
                        }
                        if (!streamer->requestParamsJSON().empty()) {
                            cJSON* pl = cJSON_Parse(streamer->requestParamsJSON().c_str());
                            if (pl) cJSON_AddItemToObject(jq, "payload", pl);
                            else cJSON_AddItemToObject(jq, "payload", cJSON_CreateString(streamer->requestParamsJSON().c_str()));
                        }
                        cJSON_AddItemToObject(jResponse, "query_params", jq);
                    }
                    char* json = cJSON_PrintUnformatted(jResponse);
                    cb->responseHandler(psession, type, json);
                    free(json);
                    cJSON_Delete(jResponse);
                }
            }

			const std::string& audio = parser.parseAudio(response);
			bool playAudio = !audio.empty() ;
            if (response.has_detect_intent_response()) {
                const auto& dir = response.detect_intent_response();
                // Set response headers (via channel vars consumed by responseHandler)
                switch_channel_set_variable(channel, "DF_RESPONSE_ID", dir.response_id().c_str());
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                    "grpc_read_thread: detect_intent_response output_audio bytes=%zu config? %s\n",
                    audio.size(), dir.has_output_audio_config() ? "yes" : "no");
                // Decide if we should barge-in or block the next user turn during playback
                bool requested_barge_in = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_BARGE_IN"));
                bool force_barge_in = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_BARGE_IN_FORCE"));
                bool response_barge_in = response_allows_playback_interruption(response);
                bool allow_barge_in = requested_barge_in && (response_barge_in || force_barge_in);
                bool will_autoplay = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY"));
                bool autoplay_sync = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY_SYNC"));
                // Default to sync when AUTOPLAY is requested unless explicitly disabled
                if (will_autoplay && switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY_SYNC") == NULL) {
                    autoplay_sync = true;
                }
                if (requested_barge_in && !allow_barge_in) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
                        "grpc_read_thread: Dialogflow response is not interruptible; keeping playback blocking for this turn\n");
                }
                if (!allow_barge_in && will_autoplay && autoplay_sync && playAudio) {
                    // Pause streaming while we play the agent audio; we will resume afterwards
                    streamer->setPaused(true);
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                        "grpc_read_thread: pausing input during sync autoplay (barge-in disabled)\n");
                }

                // Handle auto actions: end-session / transfer-to-human based on intent name or parameters
                const char* end_int_var = switch_channel_get_variable(channel, "DIALOGFLOW_END_SESSION_INTENT");
                std::string end_intent = end_int_var && *end_int_var ? end_int_var : std::string("END SESSION");
                const char* xfer_int_var = switch_channel_get_variable(channel, "DIALOGFLOW_TRANSFER_INTENT");
                std::string xfer_intent = xfer_int_var && *xfer_int_var ? xfer_int_var : std::string("TRANSFER TO HUMAN");
                const char* end_page_var = switch_channel_get_variable(channel, "DIALOGFLOW_END_SESSION_PAGE");
                std::string end_page = end_page_var && *end_page_var ? end_page_var : std::string();
                const char* xfer_page_var = switch_channel_get_variable(channel, "DIALOGFLOW_TRANSFER_PAGE");
                std::string xfer_page = xfer_page_var && *xfer_page_var ? xfer_page_var : std::string();
                bool emit_only = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_ACTIONS_EMIT_ONLY"));

                auto toUpper = [](std::string s){ for (auto& c : s) c = toupper(c); return s; };

                if (dir.has_query_result()) {
                    const auto& qr = dir.query_result();
                    std::string disp;
                    bool have_intent = false;
                    if (qr.has_match() && qr.match().has_intent()) {
                        disp = qr.match().intent().display_name();
                        have_intent = true;
                    } else if (qr.has_intent()) { // deprecated in CX, kept for compatibility
                        disp = qr.intent().display_name();
                        have_intent = true;
                    }
                    if (!disp.empty()) switch_channel_set_variable(channel, "DF_INTENT", disp.c_str());
                    std::string Udisp = toUpper(disp);
                    std::string page_disp;
                    std::string page_name;
                    if (qr.has_current_page()) {
                        page_disp = qr.current_page().display_name();
                        page_name = qr.current_page().name();
                    }
                    // Detect page change by resource name (preferred) or display name
                    if (!page_disp.empty() || !page_name.empty()) {
                        // Toggle via DIALOGFLOW_EMIT_PAGE (default: true)
                        const char* pgVar = switch_channel_get_variable(channel, "DIALOGFLOW_EMIT_PAGE");
                        bool emit_page = (pgVar == NULL) ? true : switch_true(pgVar);
                        const char* prev_name = switch_channel_get_variable(channel, "DF_PAGE_NAME");
                        const char* prev_page = switch_channel_get_variable(channel, "DF_PAGE");
                        bool changed = false;
                        if (!page_name.empty()) {
                            changed = (!prev_name || strcmp(prev_name, page_name.c_str()) != 0);
                        } else {
                            changed = (!prev_page || strcasecmp(prev_page, page_disp.c_str()) != 0);
                        }
                        if (changed && emit_page) {
                            cJSON* j = cJSON_CreateObject();
                            // Include both resource name and display name when possible
                            if (!page_name.empty()) cJSON_AddItemToObject(j, "page_name", cJSON_CreateString(page_name.c_str()));
                            cJSON_AddItemToObject(j, "page_display_name", cJSON_CreateString(page_disp.c_str()));
                            // Include page parameters when available (from QueryResult)
                            if (qr.has_parameters()) {
                                cJSON* jp = parser.parseStruct(qr.parameters());
                                cJSON_AddItemToObject(j, "parameters", jp);
                            }
                            // Optionally include request query_params
                            bool include_qp = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_INCLUDE_QUERY_PARAMS"));
                            if (include_qp) {
                                cJSON* jq = cJSON_CreateObject();
                                if (!streamer->qpChannel().empty()) {
                                    cJSON_AddItemToObject(jq, "channel", cJSON_CreateString(streamer->qpChannel().c_str()));
                                }
                                if (!streamer->requestParamsJSON().empty()) {
                                    cJSON* pl = cJSON_Parse(streamer->requestParamsJSON().c_str());
                                    if (pl) cJSON_AddItemToObject(jq, "payload", pl);
                                    else cJSON_AddItemToObject(jq, "payload", cJSON_CreateString(streamer->requestParamsJSON().c_str()));
                                }
                                cJSON_AddItemToObject(j, "query_params", jq);
                            }
                            char* body = cJSON_PrintUnformatted(j);
                            cb->responseHandler(psession, DIALOGFLOW_EVENT_PAGE, body);
                            free(body);
                            cJSON_Delete(j);
                        }
                        if (!page_disp.empty()) switch_channel_set_variable(channel, "DF_PAGE", page_disp.c_str());
                        if (!page_name.empty()) switch_channel_set_variable(channel, "DF_PAGE_NAME", page_name.c_str());
                    }
                    std::string Upage = toUpper(page_disp);

                    // Optionally emit webhook error events based on QueryResult.webhook_statuses / diagnostic_info
                    do {
                        const char* emit_var = switch_channel_get_variable(channel, "DIALOGFLOW_EMIT_WEBHOOK_ERRORS");
                        bool emit_webhook_errors = (emit_var == NULL) ? true : switch_true(emit_var);
                        if (!emit_webhook_errors) break;

                        // Helper: map google.rpc.Status.code to category/retryable
                        auto categorize = [](int code, const char** cat, bool* retry) {
                            const char* c = "unknown"; bool r = false;
                            switch (code) {
                                case 16: // UNAUTHENTICATED
                                case 7:  // PERMISSION_DENIED
                                    c = "auth"; r = false; break;
                                case 8:  // RESOURCE_EXHAUSTED
                                    c = "quota"; r = false; break;
                                case 14: // UNAVAILABLE
                                    c = "network"; r = true; break;
                                case 4:  // DEADLINE_EXCEEDED
                                    c = "timeout"; r = true; break;
                                case 13: // INTERNAL
                                    c = "server"; r = true; break;
                                default:
                                    c = "unknown"; r = false; break;
                            }
                            *cat = c; *retry = r;
                        };

                        // Emit one event per failing webhook status
                        int ws_count = qr.webhook_statuses_size();
                        for (int i = 0; i < ws_count; ++i) {
                            const google::rpc::Status& st = qr.webhook_statuses(i);
                            if (st.code() == 0) continue; // OK
                            const char* cat; bool retry;
                            categorize(st.code(), &cat, &retry);
                            cJSON* j = cJSON_CreateObject();
                            cJSON_AddItemToObject(j, "index", cJSON_CreateNumber(i));
                            cJSON_AddItemToObject(j, "code", cJSON_CreateNumber(st.code()));
                            cJSON_AddItemToObject(j, "message", cJSON_CreateString(st.message().c_str()));
                            cJSON_AddItemToObject(j, "category", cJSON_CreateString(cat));
                            cJSON_AddItemToObject(j, "retryable", cJSON_CreateBool(retry));
                            // Include diagnostic_info when present
                            if (qr.has_diagnostic_info()) {
                                GRPCParser p(psession);
                                cJSON* diag = p.parseStruct(qr.diagnostic_info());
                                if (diag) cJSON_AddItemToObject(j, "diagnostic_info", diag);
                            }
                            // Include request query_params if requested
                            bool include_qp3 = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_INCLUDE_QUERY_PARAMS"));
                            if (include_qp3) {
                                cJSON* jq = cJSON_CreateObject();
                                if (!streamer->qpChannel().empty()) {
                                    cJSON_AddItemToObject(jq, "channel", cJSON_CreateString(streamer->qpChannel().c_str()));
                                }
                                if (!streamer->requestParamsJSON().empty()) {
                                    cJSON* pl = cJSON_Parse(streamer->requestParamsJSON().c_str());
                                    if (pl) cJSON_AddItemToObject(jq, "payload", pl);
                                    else cJSON_AddItemToObject(jq, "payload", cJSON_CreateString(streamer->requestParamsJSON().c_str()));
                                }
                                cJSON_AddItemToObject(j, "query_params", jq);
                            }
                            // Add context
                            if (!disp.empty()) cJSON_AddItemToObject(j, "intent_display_name", cJSON_CreateString(disp.c_str()));
                            if (!page_disp.empty()) cJSON_AddItemToObject(j, "page_display_name", cJSON_CreateString(page_disp.c_str()));
                            char* body = cJSON_PrintUnformatted(j);
                            cb->responseHandler(psession, DIALOGFLOW_EVENT_WEBHOOK_ERROR, body);
                            free(body);
                            cJSON_Delete(j);
                        }
                    } while (0);

                    bool acted = false;
                    // End session: match by page display name (if provided) or intent display name
                    bool end_match = false;
                    if (!end_page.empty() && Upage == toUpper(end_page)) end_match = true;
                    if (!end_match && have_intent && !end_intent.empty() && Udisp == toUpper(end_intent)) end_match = true;
                    if (end_match) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_NOTICE,
                            "DF END_SESSION match: intent='%s' page='%s' emit_only=%s\n",
                            disp.c_str(), page_disp.c_str(), emit_only ? "true" : "false");
                        // Build end_session JSON body (may be deferred)
                        cJSON* j = cJSON_CreateObject();
                        cJSON_AddItemToObject(j, "intent_display_name", cJSON_CreateString(disp.c_str()));
                        if (!page_disp.empty()) cJSON_AddItemToObject(j, "page_display_name", cJSON_CreateString(page_disp.c_str()));
                        // Include response parameters if any
                        if (qr.has_parameters()) {
                            cJSON* jp = parser.parseStruct(qr.parameters());
                            cJSON_AddItemToObject(j, "parameters", jp);
                        }
                        // Include request query_params if requested
                        bool include_qp = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_INCLUDE_QUERY_PARAMS"));
                        if (include_qp) {
                            cJSON* jq = cJSON_CreateObject();
                            if (!streamer->qpChannel().empty()) {
                                cJSON_AddItemToObject(jq, "channel", cJSON_CreateString(streamer->qpChannel().c_str()));
                            }
                            if (!streamer->requestParamsJSON().empty()) {
                                cJSON* pl = cJSON_Parse(streamer->requestParamsJSON().c_str());
                                if (pl) cJSON_AddItemToObject(jq, "payload", pl);
                                else cJSON_AddItemToObject(jq, "payload", cJSON_CreateString(streamer->requestParamsJSON().c_str()));
                            }
                            cJSON_AddItemToObject(j, "query_params", jq);
                        }
                        char* body = cJSON_PrintUnformatted(j);
                        cJSON_Delete(j);
                        bool defer_end = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_END_SESSION_AFTER_PLAYBACK"))
                                          && will_autoplay && autoplay_sync && dir.output_audio().size() > 0;
                        if (defer_end) {
                            switch_mutex_lock(cb->mutex);
                            if (cb->pending_end_session_json) free(cb->pending_end_session_json);
                            cb->pending_end_session_json = strdup(body);
                            switch_mutex_unlock(cb->mutex);
                            free(body);
                        } else {
                            cb->responseHandler(psession, DIALOGFLOW_EVENT_END_SESSION, body);
                            free(body);
                        }
                        if (!emit_only) {
                            // Stop DF session and hang up the call
                            google_dialogflow_session_stop(psession, 0);
                            switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
                            acted = true;
                        }
                    }
                    // Transfer
                    if (!acted) {
                        bool xfer_match = false;
                        if (!xfer_page.empty() && Upage == toUpper(xfer_page)) xfer_match = true;
                        if (!xfer_match && have_intent && !xfer_intent.empty() && Udisp == toUpper(xfer_intent)) xfer_match = true;
                        if (xfer_match) {
                        // qr already defined above
                        std::string exten;
                        std::string ctx;
                        std::string dp;

                        // Try to get destination from parameters
                        if (qr.has_parameters()) {
                            const auto& fields = qr.parameters().fields();
                            auto getStr = [&fields](const char* key, std::string& out) {
                                auto it = fields.find(key);
                                if (it != fields.end() && it->second.kind_case() == google::protobuf::Value::kStringValue) {
                                    if (!it->second.string_value().empty()) { out = it->second.string_value(); return true; }
                                }
                                return false;
                            };
                            getStr("transfer_to", exten) || getStr("transfer_target", exten) || getStr("destination", exten) || getStr("exten", exten);
                            getStr("context", ctx);
                            getStr("dialplan", dp);
                        }
                        // Fallback to channel vars
                        if (exten.empty()) {
                            const char* v = switch_channel_get_variable(channel, "DIALOGFLOW_TRANSFER_EXTEN");
                            if (v) exten = v;
                        }
                        const char* vctx = switch_channel_get_variable(channel, "DIALOGFLOW_TRANSFER_CONTEXT");
                        const char* vdp  = switch_channel_get_variable(channel, "DIALOGFLOW_TRANSFER_DIALPLAN");
                        if (ctx.empty() && vctx) ctx = vctx; if (ctx.empty()) ctx = "default";
                        if (dp.empty() && vdp) dp = vdp;  if (dp.empty()) dp = "XML";

                        if (!exten.empty()) {
                            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_NOTICE,
                                "DF TRANSFER match: exten=%s dialplan=%s context=%s intent='%s' page='%s' emit_only=%s\n",
                                exten.c_str(), dp.c_str(), ctx.c_str(), disp.c_str(), page_disp.c_str(), emit_only ? "true" : "false");
                            // Fire a transfer event with JSON body for external listeners
                            {
                                cJSON* j = cJSON_CreateObject();
                                cJSON_AddItemToObject(j, "exten", cJSON_CreateString(exten.c_str()));
                                cJSON_AddItemToObject(j, "context", cJSON_CreateString(ctx.c_str()));
                                cJSON_AddItemToObject(j, "dialplan", cJSON_CreateString(dp.c_str()));
                                cJSON_AddItemToObject(j, "intent_display_name", cJSON_CreateString(disp.c_str()));
                                if (!page_disp.empty()) cJSON_AddItemToObject(j, "page_display_name", cJSON_CreateString(page_disp.c_str()));
                                // Include response parameters if any (so listeners can see matched values)
                                if (qr.has_parameters()) {
                                    cJSON* jp = parser.parseStruct(qr.parameters());
                                    cJSON_AddItemToObject(j, "parameters", jp);
                                }
                                // Include request query_params if requested
                                bool include_qp2 = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_INCLUDE_QUERY_PARAMS"));
                                if (include_qp2) {
                                    cJSON* jq = cJSON_CreateObject();
                                    if (!streamer->qpChannel().empty()) {
                                        cJSON_AddItemToObject(jq, "channel", cJSON_CreateString(streamer->qpChannel().c_str()));
                                    }
                                    if (!streamer->requestParamsJSON().empty()) {
                                        cJSON* pl = cJSON_Parse(streamer->requestParamsJSON().c_str());
                                        if (pl) cJSON_AddItemToObject(jq, "payload", pl);
                                        else cJSON_AddItemToObject(jq, "payload", cJSON_CreateString(streamer->requestParamsJSON().c_str()));
                                    }
                                    cJSON_AddItemToObject(j, "query_params", jq);
                                }
                                char* body = cJSON_PrintUnformatted(j);
                                switch_event_t* ev = nullptr;
                                if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, DIALOGFLOW_EVENT_TRANSFER) == SWITCH_STATUS_SUCCESS) {
                                    switch_channel_event_set_data(channel, ev);
                                    switch_event_add_body(ev, "%s", body);
                                    switch_event_fire(&ev);
                                }
                                free(body);
                                cJSON_Delete(j);
                            }
                            if (!emit_only) {
                                // Stop DF session and transfer call
                                google_dialogflow_session_stop(psession, 0);
                                switch_status_t st = switch_ivr_session_transfer(psession, exten.c_str(), dp.c_str(), ctx.c_str());
                                if (st != SWITCH_STATUS_SUCCESS) {
                                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_ERROR,
                                        "Transfer failed to %s (dp=%s ctx=%s)\n", exten.c_str(), dp.c_str(), ctx.c_str());
                                }
                                acted = true;
                            }
                        } else {
                            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_WARNING,
                                "Transfer intent matched but no destination provided (params or DIALOGFLOW_TRANSFER_EXTEN)\n");
                        }
                        }
                    }
                    if (acted) {
                        // Do not attempt to play agent audio or continue loop; exit read loop
                        switch_core_session_rwunlock(psession);
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "grpc_read_thread: action taken; breaking read loop\n");
                        return NULL;
                    }
                }
            }

            // save audio
            if (playAudio && !cb->stopping) {
                // Do not attempt to play on a channel that is no longer ready
                if (!switch_channel_ready(channel)) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_WARNING,
                        "Channel not ready during DF audio_provided; skipping playback and exiting read loop\n");
                    switch_core_session_rwunlock(psession);
                    return NULL;
                }
				std::ostringstream s;
				s << SWITCH_GLOBAL_dirs.temp_dir << SWITCH_PATH_SEPARATOR <<
					cb->sessionId << "_" <<  ++playCount;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG, "grpc_read_thread: received audio to play\n");

				if (response.has_detect_intent_response() && response.detect_intent_response().has_output_audio_config()) {
					const OutputAudioConfig& cfg = response.detect_intent_response().output_audio_config();
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG, "grpc_read_thread: encoding is %d\n", cfg.audio_encoding());
					if (cfg.audio_encoding() == OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_MP3) {
						s << ".mp3";
					}
					else if (cfg.audio_encoding() == OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_OGG_OPUS) {
						s << ".opus";
					}
					else {
						s << ".wav";
					}
				}
				std::ofstream f(s.str(), std::ofstream::binary);
				f << audio;
				f.close();
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG, "grpc_read_thread: wrote audio to %s\n", s.str().c_str());

				// add the file to the list of files played for this session, 
				// we'll delete when session closes
				audioFiles.insert(std::pair<std::string, std::string>(cb->sessionId, s.str()));

                bool suppress_audio_body = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_SUPPRESS_AUDIO_EVENT_BODY"));
                if (suppress_audio_body) {
                    // Put path into header via channel var, send empty JSON {}
                    switch_channel_set_variable(channel, "DF_AUDIO_PATH", s.str().c_str());
                    char* json = strdup("{}");
                    cb->responseHandler(psession, DIALOGFLOW_EVENT_AUDIO_PROVIDED, json);
                    free(json);
                } else {
                    cJSON * jResponse = cJSON_CreateObject();
                    cJSON_AddItemToObject(jResponse, "path", cJSON_CreateString(s.str().c_str()));
                    char* json = cJSON_PrintUnformatted(jResponse);
                    cb->responseHandler(psession, DIALOGFLOW_EVENT_AUDIO_PROVIDED, json);
                    free(json);
                    cJSON_Delete(jResponse);
                }

				// Optional auto-play: play returned audio on the A leg when requested
				const char* ap = switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY");
				bool will_autoplay = ap && switch_true(ap);
				bool autoplay_sync = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY_SYNC"));
                bool requested_barge_in = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_BARGE_IN"));
                bool force_barge_in = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_BARGE_IN_FORCE"));
                bool response_barge_in = response_allows_playback_interruption(response);
                bool allow_barge_in = requested_barge_in && (response_barge_in || force_barge_in);
                uint64_t promptDurationMs = 0;
                uint32_t promptSampleRate = 0;
                uint64_t noBargeInMs = 0;
                const OutputAudioConfig* promptCfg = (response.has_detect_intent_response() && response.detect_intent_response().has_output_audio_config())
                    ? &response.detect_intent_response().output_audio_config() : NULL;
				if (will_autoplay && switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY_SYNC") == NULL) {
					autoplay_sync = true; // default to sync to avoid no_input during long prompts
				}
                if (allow_barge_in) {
                    const char* nb = switch_channel_get_variable(channel, "DIALOGFLOW_BARGE_IN_NO_BARGE_MS");
                    if (nb && *nb) {
                        noBargeInMs = static_cast<uint64_t>(strtoull(nb, NULL, 10));
                    }
                    bool haveDuration = false;
                    if (promptCfg && promptCfg->audio_encoding() == OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_LINEAR_16) {
                        haveDuration = get_wav_duration_ms(audio, &promptDurationMs, &promptSampleRate);
                        if (haveDuration) {
                            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
                                "Dialogflow native barge-in duration derived from WAV payload: total=%" PRIu64 "ms rate=%uHz\n",
                                promptDurationMs, promptSampleRate);
                        }
                    }
                    if (!haveDuration) {
                        haveDuration = get_audio_file_duration_ms(s.str(), streamer->sampleRate(), &promptDurationMs, &promptSampleRate);
                    }
                    if (!haveDuration) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_WARNING,
                            "Unable to determine prompt duration for native Dialogflow barge-in; falling back to blocking playback for %s\n",
                            s.str().c_str());
                        allow_barge_in = false;
                        if (autoplay_sync) {
                            switch_mutex_lock(cb->mutex);
                            streamer->setPaused(true);
                            switch_mutex_unlock(cb->mutex);
                        }
                    } else {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
                            "Dialogflow native barge-in armed for %s: total=%" PRIu64 "ms no_barge=%" PRIu64 "ms rate=%uHz\n",
                            s.str().c_str(), promptDurationMs, noBargeInMs, promptSampleRate);
                    }
                }
                if (will_autoplay && !cb->stopping) {
                    if (allow_barge_in) {
                        if (!cb->stopping && switch_channel_ready(channel)) {
                            switch_mutex_lock(cb->mutex);
                            arm_interruptible_playback_state(cb, noBargeInMs);
                            streamer->setPendingBargeIn(promptDurationMs, noBargeInMs);
                            if (streamer->isPaused()) streamer->setPaused(false);
                            streamer->rotateToAudioConfig(psession);
                            switch_mutex_unlock(cb->mutex);
                        }
                        char args[1024];
                        snprintf(args, sizeof(args), "%s %s aleg", cb->sessionId, s.str().c_str());
                        switch_stream_handle_t stream = { 0 };
                        SWITCH_STANDARD_STREAM(stream);
                        switch_status_t st = switch_api_execute("uuid_broadcast", args, NULL, &stream);
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
                            "Auto-playing Dialogflow audio in interruptible mode via uuid_broadcast: %s (status=%d)\n", args, st);
                        switch_safe_free(stream.data);
                    } else if (autoplay_sync) {
                        reset_interruptible_playback_state(cb);
                        // Play synchronously so we know when it finishes
                        switch_status_t st = switch_ivr_play_file(psession, NULL, s.str().c_str(), NULL);
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
                            "Auto-playing Dialogflow audio synchronously: %s (status=%d)\n", s.str().c_str(), st);
                        // If we deferred end_session, emit it now on success
                        const char* defer_end = switch_channel_get_variable(channel, "DIALOGFLOW_END_SESSION_AFTER_PLAYBACK");
                        if (st == SWITCH_STATUS_SUCCESS && defer_end && switch_true(defer_end)) {
                            switch_mutex_lock(cb->mutex);
                            if (cb->pending_end_session_json) {
                                char* tmp = cb->pending_end_session_json;
                                cb->pending_end_session_json = NULL;
                                switch_mutex_unlock(cb->mutex);
                                cb->responseHandler(psession, DIALOGFLOW_EVENT_END_SESSION, tmp);
                                free(tmp);
                            } else {
                                switch_mutex_unlock(cb->mutex);
                            }
                        }
                        // Resume streaming and rotate to a fresh audio-configured stream for next user turn
                        if (!allow_barge_in && !cb->stopping && switch_channel_ready(channel)) {
                            switch_mutex_lock(cb->mutex);
                            streamer->setPaused(false);
                            streamer->rotateToAudioConfig(psession);
                            switch_mutex_unlock(cb->mutex);
                        }
                    } else {
                        reset_interruptible_playback_state(cb);
                        if (!cb->stopping && switch_channel_ready(channel)) {
                            switch_mutex_lock(cb->mutex);
                            if (streamer->isPaused()) streamer->setPaused(false);
                            streamer->rotateToAudioConfig(psession);
                            switch_mutex_unlock(cb->mutex);
                        }
                        // Fallback: async broadcast (legacy behavior)
                        char args[1024];
                        snprintf(args, sizeof(args), "%s %s aleg", cb->sessionId, s.str().c_str());
                        switch_stream_handle_t stream = { 0 };
                        SWITCH_STANDARD_STREAM(stream);
                        switch_status_t st = switch_api_execute("uuid_broadcast", args, NULL, &stream);
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
                            "Auto-playing Dialogflow audio via uuid_broadcast: %s (status=%d)\n", args, st);
                        switch_safe_free(stream.data);
                    }
                } else {
                    reset_interruptible_playback_state(cb);
                    // Not auto-playing here. If we paused earlier, resume and rotate now.
                    if (!cb->stopping && switch_channel_ready(channel)) {
                        switch_mutex_lock(cb->mutex);
                        if (streamer->isPaused()) streamer->setPaused(false);
                        streamer->rotateToAudioConfig(psession);
                        switch_mutex_unlock(cb->mutex);
                    }
                }
			}
            else if (response.has_detect_intent_response() && !cb->stopping) {
                reset_interruptible_playback_state(cb);
                if (!switch_channel_ready(channel)) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_WARNING,
                        "Channel not ready during DF detect_intent_response without audio; exiting read loop\n");
                    switch_core_session_rwunlock(psession);
                    return NULL;
                }
                switch_mutex_lock(cb->mutex);
                if (streamer->isPaused()) streamer->setPaused(false);
                streamer->clearPendingBargeIn();
                streamer->rotateToAudioConfig(psession);
                switch_mutex_unlock(cb->mutex);
            }
			switch_core_session_rwunlock(psession);
		}
		else {
			break;
		}
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "dialogflow read loop is done\n");

	// finish the detect intent session: here is where we may get an error if credentials are invalid
	switch_core_session_t* psession = switch_core_session_locate(cb->sessionId);
	if (psession) {
	grpc::Status status = streamer->finish();
		if (!status.ok()) {
			std::ostringstream s;
			s << "{\"msg\": \"" << status.error_message() << "\", \"code\": " << status.error_code();
			// Categorize and mark retryable
			const int ec = status.error_code();
            const char* category = "unknown";
            bool retryable = false;
            switch (ec) {
                case grpc::StatusCode::UNAUTHENTICATED:
                case grpc::StatusCode::PERMISSION_DENIED: category = "auth"; break;
                case grpc::StatusCode::RESOURCE_EXHAUSTED: category = "quota"; break;
                case grpc::StatusCode::UNAVAILABLE: category = "network"; retryable = true; break;
                case grpc::StatusCode::DEADLINE_EXCEEDED: category = "timeout"; retryable = true; break;
                case grpc::StatusCode::INTERNAL: category = "server"; retryable = true; break;
                case grpc::StatusCode::NOT_FOUND: category = "not_found"; break;
                default: break;
            }
			s << ", \"category\": \"" << category << "\"";
			s << ", \"retryable\": " << (retryable ? "true" : "false");
			if (status.error_details().length() > 0) {
				s << ", \"details\": \"" << status.error_details() << "\"";
			}
			s << "}";
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "StreamingDetectIntentRequest finished with err %s (%d): %s\n", 
				status.error_message().c_str(), status.error_code(), status.error_details().c_str());
			cb->errorHandler(psession, s.str().c_str());
		}

		switch_core_session_rwunlock(psession);
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "dialogflow read thread exiting	\n");
	return NULL;
}

extern "C" {
	switch_status_t google_dialogflow_backchannel_warmup(switch_core_session_t *session) {
		switch_channel_t* channel = switch_core_session_get_channel(session);
		const char* voiceValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_VOICE_NAME");
		const char* languageValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_LANGUAGE");
		if (zstr(voiceValue) || zstr(languageValue)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
				"Skipping backchannel TTS warm-up because no explicit Dialogflow voice is configured\n");
			return SWITCH_STATUS_FALSE;
		}
		const std::string voice(voiceValue);
		const std::string language(languageValue);
		const char* rateValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_SPEAKING_RATE");
		const char* pitchValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_PITCH");
		const char* volumeValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_VOLUME_GAIN_DB");
		const char* effectsValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_EFFECTS_PROFILE_ID");
		const std::string warmupKey = voice + "\n" + language + "\n" + (rateValue ? rateValue : "") + "\n" +
			(pitchValue ? pitchValue : "") + "\n" + (volumeValue ? volumeValue : "") + "\n" +
			(effectsValue ? effectsValue : "");
		{
			std::lock_guard<std::mutex> lock(backchannelWarmupMutex);
			if (!backchannelWarmupVoices.insert(warmupKey).second) return SWITCH_STATUS_SUCCESS;
		}
		const std::string uuid = switch_core_session_get_uuid(session);
		try {
			std::thread warmupThread([uuid, voice, language]() {
				switch_core_session_t* warmupSession = switch_core_session_locate(uuid.c_str());
				if (!warmupSession) return;
				const auto operations = { std::string("lookup"), std::string("payments") };
				for (const auto& operation : operations) {
					const auto prompts = backchannel_prompts(operation);
					if (!prompts.empty()) {
						try {
							const std::string path = synthesize_backchannel_audio(warmupSession, voice, language, prompts.front());
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(warmupSession), SWITCH_LOG_DEBUG,
								"Warmed backchannel TTS cache for voice=%s operation=%s file=%s\n",
								voice.c_str(), operation.c_str(), path.c_str());
						} catch (const std::exception& error) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(warmupSession), SWITCH_LOG_WARNING,
								"Backchannel TTS warm-up failed for voice=%s operation=%s: %s\n",
								voice.c_str(), operation.c_str(), error.what());
						}
					}
				}
				switch_core_session_rwunlock(warmupSession);
			});
			std::lock_guard<std::mutex> lock(backchannelWarmupMutex);
			backchannelWarmupThreads.emplace_back(std::move(warmupThread));
		} catch (const std::exception& error) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
				"Unable to start backchannel TTS warm-up: %s\n", error.what());
			return SWITCH_STATUS_FALSE;
		}
		return SWITCH_STATUS_SUCCESS;
	}

	switch_status_t google_dialogflow_backchannel_start(switch_core_session_t *session, const char *operationValue) {
		const std::string operation = operationValue ? operationValue : "";
		const auto prompts = backchannel_prompts(operation);
		if (prompts.empty()) return SWITCH_STATUS_FALSE;
		switch_channel_t* channel = switch_core_session_get_channel(session);
		const char* voiceValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_VOICE_NAME");
		const char* languageValue = switch_channel_get_variable(channel, "DIALOGFLOW_TTS_LANGUAGE");
		if (zstr(voiceValue) || zstr(languageValue)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
				"Operation backchannel requires an explicit Dialogflow TTS voice and language\n");
			return SWITCH_STATUS_FALSE;
		}

		auto loop = std::make_shared<BackchannelLoop>();
		loop->uuid = switch_core_session_get_uuid(session);
		loop->operation = operation;
		loop->voice = voiceValue;
		loop->language = languageValue;
		stop_backchannel_loop(loop->uuid);
		{
			std::lock_guard<std::mutex> lock(backchannelLoopsMutex);
			if (backchannelLoops.find(loop->uuid) != backchannelLoops.end()) return SWITCH_STATUS_SUCCESS;
			backchannelLoops[loop->uuid] = loop;
		}
		try {
			std::thread worker(backchannel_loop_worker, loop);
			worker.detach();
		} catch (const std::exception& error) {
			std::lock_guard<std::mutex> lock(backchannelLoopsMutex);
			backchannelLoops.erase(loop->uuid);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
				"Unable to start operation backchannel worker: %s\n", error.what());
			return SWITCH_STATUS_FALSE;
		}
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
			"Started intermittent operation backchannel: operation=%s voice=%s phrases=%zu interval=9-12s\n",
			operation.c_str(), voiceValue, prompts.size());
		return SWITCH_STATUS_SUCCESS;
	}

	void google_dialogflow_backchannel_stop(switch_core_session_t *session) {
		if (!session) return;
		switch_channel_t* channel = switch_core_session_get_channel(session);
		const char* uuid = switch_core_session_get_uuid(session);
		const char* active = switch_channel_get_variable(channel, "DIALOGFLOW_BACKCHANNEL_ACTIVE");
		const switch_bool_t wasActive = (active && switch_true(active)) ? SWITCH_TRUE : SWITCH_FALSE;
		switch_channel_set_variable(channel, "DIALOGFLOW_BACKCHANNEL_ACTIVE", NULL);
		if (wasActive) {
			switch_stream_handle_t stream = { 0 };
			SWITCH_STANDARD_STREAM(stream);
			(void) switch_api_execute("uuid_break", uuid, NULL, &stream);
			switch_safe_free(stream.data);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
				"Stopped backchannel playback\n");
		}
		stop_backchannel_loop(uuid ? uuid : "");
	}

	switch_status_t google_dialogflow_init() {
		const char* gcsServiceKeyFile = std::getenv("GOOGLE_APPLICATION_CREDENTIALS");
		if (NULL == gcsServiceKeyFile) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, 
				"\"GOOGLE_APPLICATION_CREDENTIALS\" environment variable is not set; authentication will use \"GOOGLE_APPLICATION_CREDENTIALS\" channel variable\n");
		}
		else {
			hasDefaultCredentials = true;
		}
		return SWITCH_STATUS_SUCCESS;
	}
	
	switch_status_t google_dialogflow_cleanup() {
		std::vector<std::thread> warmupThreads;
		{
			std::lock_guard<std::mutex> lock(backchannelWarmupMutex);
			warmupThreads.swap(backchannelWarmupThreads);
		}
		for (auto& thread : warmupThreads) {
			if (thread.joinable()) thread.join();
		}
		std::vector<std::shared_ptr<BackchannelLoop>> loops;
		{
			std::lock_guard<std::mutex> lock(backchannelLoopsMutex);
			for (const auto& entry : backchannelLoops) loops.push_back(entry.second);
		}
		for (const auto& loop : loops) {
			switch_core_session_t* session = switch_core_session_locate(loop->uuid.c_str());
			if (session) {
				google_dialogflow_backchannel_stop(session);
				switch_core_session_rwunlock(session);
			} else {
				{
					std::lock_guard<std::mutex> lock(loop->mutex);
					loop->stopping = true;
				}
				loop->condition.notify_all();
				std::unique_lock<std::mutex> lock(loop->mutex);
				loop->condition.wait_for(lock, std::chrono::seconds(6), [loop] { return loop->finished; });
			}
		}
		return SWITCH_STATUS_SUCCESS;
	}

	// start dialogflow on a channel
	switch_status_t google_dialogflow_session_init(
		switch_core_session_t *session, 
		responseHandler_t responseHandler, 
		errorHandler_t errorHandler, 
		uint32_t samples_per_second, 
		char* lang, 
		char* projectId, 
		char* event, 
		char* text,
		struct cap_cb **ppUserData
	) {
		switch_status_t status = SWITCH_STATUS_SUCCESS;
		switch_channel_t *channel = switch_core_session_get_channel(session);
		int err;
		const char* vadEnabled = NULL;
		const char* vadHold = NULL;
		const char* vadMode = NULL;
		switch_threadattr_t *thd_attr = NULL;
		switch_memory_pool_t *pool = switch_core_session_get_pool(session);
		struct cap_cb* cb = (struct cap_cb *) switch_core_session_alloc(session, sizeof(*cb));
		memset(cb, 0, sizeof(*cb));

		if (!hasDefaultCredentials && !switch_channel_get_variable(channel, "GOOGLE_APPLICATION_CREDENTIALS")) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, 
				"missing credentials: GOOGLE_APPLICATION_CREDENTIALS must be supplied as env (path) or channel var (JSON); emitting dialogflow::error and aborting.\n");
			if (errorHandler) {
				std::ostringstream s;
				s << "{\"msg\":\"missing credentials\",\"code\":16,\"category\":\"auth\",\"retryable\":false}";
				errorHandler(session, s.str().c_str());
			}
			status = SWITCH_STATUS_FALSE;
			goto done; 
		}

		strncpy(cb->sessionId, switch_core_session_get_uuid(session), 256);
		cb->responseHandler = responseHandler;
		cb->errorHandler = errorHandler;
		cb->stopping = SWITCH_FALSE;
		cb->vad = NULL;
		cb->interruptible_playback_active = SWITCH_FALSE;
		cb->barge_vad_enabled = SWITCH_TRUE;
		cb->vad_debug = SWITCH_FALSE;
		cb->barge_vad_hold_ms = 150;
		cb->interruptible_playback_started_ms = 0;
		cb->interruptible_playback_no_barge_ms = 0;
		cb->vad_talking_ms = 0;
		cb->last_interruptible_playback_started_ms = 0;
		cb->last_local_barge_break_ms = 0;
		cb->last_first_recognition_ms = 0;
		cb->awaiting_first_recognition_after_playback = SWITCH_FALSE;

		if (switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing mutex\n");
			status = SWITCH_STATUS_FALSE;
			goto done; 
		}

        strncpy(cb->lang, lang, MAX_LANG);
        strncpy(cb->projectId, projectId, MAX_PROJECT_ID);
        try {
            cb->streamer = new GStreamer(session, lang, projectId, event, text, samples_per_second);
            (void) google_dialogflow_backchannel_warmup(session);
        } catch (const std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT,
                "Dialogflow init error (construction): %s. Emitting dialogflow::error and aborting.\n", e.what());
            if (errorHandler) {
                const char* cat = (strstr(e.what(), "credential") || strstr(e.what(), "Credentials")) ? "auth" : "unknown";
                int code = (strcmp(cat, "auth") == 0) ? 16 : -1;
                std::ostringstream s; s << "{\"msg\":\"" << e.what() << "\",\"code\":" << code << ",\"category\":\"" << cat << "\",\"retryable\":false}";
                errorHandler(session, s.str().c_str());
            }
            status = SWITCH_STATUS_FALSE;
            goto done;
        } catch (...) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT,
                "Dialogflow init error (construction unknown). Emitting dialogflow::error and aborting.\n");
            if (errorHandler) errorHandler(session, "{\"msg\":\"construction failed\",\"code\":-1,\"category\":\"unknown\",\"retryable\":false}");
            status = SWITCH_STATUS_FALSE;
            goto done;
        }

        // Now start the gRPC stream, protect against exceptions
        try {
            ((GStreamer*)cb->streamer)->startStream(session, event, text);
        } catch (const std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT,
                "Dialogflow init error (startStream): %s. Emitting dialogflow::error and aborting.\n", e.what());
            if (errorHandler) {
                const char* cat = (strstr(e.what(), "credential") || strstr(e.what(), "Credentials")) ? "auth" : "unknown";
                int code = (strcmp(cat, "auth") == 0) ? 16 : -1;
                std::ostringstream s; s << "{\"msg\":\"" << e.what() << "\",\"code\":" << code << ",\"category\":\"" << cat << "\",\"retryable\":false}";
                errorHandler(session, s.str().c_str());
            }
            status = SWITCH_STATUS_FALSE;
            goto done;
        } catch (...) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT,
                "Dialogflow init error (startStream unknown). Emitting dialogflow::error and aborting.\n");
            if (errorHandler) errorHandler(session, "{\"msg\":\"startStream failed\",\"code\":-1,\"category\":\"unknown\",\"retryable\":false}");
            status = SWITCH_STATUS_FALSE;
            goto done;
        }
		// Resample from the channel read rate to the Dialogflow input sample rate
		cb->resampler = speex_resampler_init(1, (spx_uint32_t)samples_per_second, (spx_uint32_t)((GStreamer*)cb->streamer)->sampleRate(), SWITCH_RESAMPLE_QUALITY, &err);
		if (0 != err) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing resampler: %s.\n", 
						switch_channel_get_name(channel), speex_resampler_strerror(err));
			status = SWITCH_STATUS_FALSE;
			goto done;
		}
		vadEnabled = switch_channel_get_variable(channel, "DIALOGFLOW_BARGE_VAD");
		cb->barge_vad_enabled = (vadEnabled == NULL) ? SWITCH_TRUE : switch_true(vadEnabled);
		vadHold = switch_channel_get_variable(channel, "DIALOGFLOW_BARGE_VAD_HOLD_MS");
		if (!zstr(vadHold)) {
			int hold = atoi(vadHold);
			if (hold > 0) {
				cb->barge_vad_hold_ms = (uint32_t) hold;
			}
		}
		cb->vad_debug = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_VAD_DEBUG"));
		if (cb->barge_vad_enabled) {
			cb->vad = switch_vad_init((int)((GStreamer*)cb->streamer)->sampleRate(), 1);
			if (cb->vad) {
				vadMode = switch_channel_get_variable(channel, "DIALOGFLOW_VAD_MODE");
				switch_vad_set_mode(cb->vad, zstr(vadMode) ? -1 : atoi(vadMode));
				if (cb->vad_debug) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						"Dialogflow using FreeSWITCH VAD mode=%s hold=%ums\n",
						zstr(vadMode) ? "-1" : vadMode, cb->barge_vad_hold_ms);
				}
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
					"Unable to initialize FreeSWITCH VAD for local barge-in; relying on Dialogflow recognition only\n");
			}
		}

		// hangup hook to clear temp audio files
		switch_core_event_hook_add_state_change(session, hanguphook);

        // init throttling
        cb->lastTranscriptMs = 0;

        // create the read thread
		switch_threadattr_create(&thd_attr, pool);
		//switch_threadattr_detach_set(thd_attr, 1);
		switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
		switch_thread_create(&cb->thread, thd_attr, grpc_read_thread, cb, pool);

		*ppUserData = cb;
	
	done:
		if (status != SWITCH_STATUS_SUCCESS) {
			killcb(cb);
		}
		return status;
	}

	switch_status_t google_dialogflow_session_stop(switch_core_session_t *session, int channelIsClosing) {
		switch_channel_t *channel = switch_core_session_get_channel(session);
		switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
		stop_backchannel(session);

		if (bug) {
			struct cap_cb *cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
			switch_status_t st;

			// Behavior: optionally wait for sync playback to finish instead of breaking
			const char* waitVar = switch_channel_get_variable(channel, "DIALOGFLOW_STOP_WAIT_PLAYBACK");
			bool wait_playback = (waitVar == NULL) ? true : switch_true(waitVar);
			if (wait_playback) {
				const char* tv = switch_channel_get_variable(channel, "DIALOGFLOW_STOP_WAIT_TIMEOUT_MS");
				int timeout_ms = tv ? atoi(tv) : 10000;
				int waited = 0;
				bool paused = false;
				do {
					// Sample paused state without holding lock for long
					switch_mutex_lock(cb->mutex);
					GStreamer* streamer = (GStreamer *) cb->streamer;
					paused = (streamer && streamer->isPaused());
					switch_mutex_unlock(cb->mutex);
					if (!paused) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					waited += 50;
				} while (waited < timeout_ms);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
					"google_dialogflow_session_cleanup: playback %s after waiting %dms\n",
					paused ? "still active" : "finished", waited);
			} else {
				// Best-effort: stop any ongoing synchronous playback immediately via uuid_break
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "google_dialogflow_session_cleanup: requesting stop of any ongoing playback (uuid_break)\n");
				const char* suuid = switch_core_session_get_uuid(session);
				char args[256]; snprintf(args, sizeof(args), "%s all", suuid ? suuid : "");
				switch_stream_handle_t stream = { 0 }; SWITCH_STANDARD_STREAM(stream);
				( void ) switch_api_execute("uuid_break", args, NULL, &stream);
				switch_safe_free(stream.data);
			}

			// close connection and get final responses (avoid deadlock with read thread)
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "google_dialogflow_session_cleanup: acquiring lock\n");
			switch_mutex_lock(cb->mutex);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "google_dialogflow_session_cleanup: acquired lock\n");
			// Mark stopping to prevent the read thread from rotating/playing further
			cb->stopping = SWITCH_TRUE;
			GStreamer* streamer = (GStreamer *) cb->streamer;
			if (streamer) {
				// Cancel any in-flight stream to break out of read loop quickly
				streamer->cancel();
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "google_dialogflow_session_cleanup: sending writesDone..\n");
				streamer->writesDone();
			}
			// Release lock before joining; the read thread may lock it after playback
			switch_mutex_unlock(cb->mutex);

			if (cb->thread) {
				switch_status_t retval;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "google_dialogflow_session_cleanup: waiting for read thread to complete\n");
				switch_thread_join(&retval, cb->thread);
				cb->thread = NULL;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "google_dialogflow_session_cleanup: read thread completed\n");
			}

			// Reacquire lock and finalize cleanup
			switch_mutex_lock(cb->mutex);
			if (cb->streamer) {
				try { ((GStreamer*)cb->streamer)->finish(); } catch (...) {}
			}
			killcb(cb);
			switch_channel_set_private(channel, MY_BUG_NAME, NULL);
			if (!channelIsClosing) switch_core_media_bug_remove(session, &bug);
			switch_mutex_unlock(cb->mutex);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "google_dialogflow_session_cleanup: Closed google session\n");

			return SWITCH_STATUS_SUCCESS;
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s Bug is not attached.\n", switch_channel_get_name(channel));
		return SWITCH_STATUS_FALSE;
	}
	
	switch_bool_t google_dialogflow_frame(switch_media_bug_t *bug, void* user_data) {
		switch_core_session_t *session = switch_core_media_bug_get_session(bug);
		uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
		switch_frame_t frame = {};
		struct cap_cb *cb = (struct cap_cb *) user_data;
		switch_bool_t stop_interruptible_playback = SWITCH_FALSE;
		switch_bool_t stop_active_backchannel = SWITCH_FALSE;

		frame.data = data;
		frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

		if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {
			GStreamer* streamer = (GStreamer *) cb->streamer;
			if (streamer && !streamer->isFinished() && !cb->stopping) {
				while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
					if (frame.datalen) {
						spx_int16_t out[SWITCH_RECOMMENDED_BUFFER_SIZE];
						spx_uint32_t out_len = SWITCH_RECOMMENDED_BUFFER_SIZE;
						spx_uint32_t in_len = frame.samples;
						
						speex_resampler_process_interleaved_int(cb->resampler, (const spx_int16_t *) frame.data, (spx_uint32_t *) &in_len, &out[0], &out_len);
						const char* backchannelActive = switch_channel_get_variable(
							switch_core_session_get_channel(session), "DIALOGFLOW_BACKCHANNEL_ACTIVE");
						switch_bool_t backchannelPlaying = (backchannelActive && switch_true(backchannelActive)) ? SWITCH_TRUE : SWITCH_FALSE;
						if ((cb->interruptible_playback_active || backchannelPlaying) && cb->barge_vad_enabled && cb->vad && out_len > 0) {
							switch_vad_state_t vadState = switch_vad_process(cb->vad, &out[0], (unsigned int) out_len);
							if (cb->vad_debug &&
								(vadState == SWITCH_VAD_STATE_START_TALKING || vadState == SWITCH_VAD_STATE_STOP_TALKING)) {
								switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
									"Dialogflow VAD: %s (talking=%" PRIu64 "ms hold=%ums active=%s)\n",
									switch_vad_state2str(vadState),
									cb->vad_talking_ms,
									cb->barge_vad_hold_ms,
									cb->interruptible_playback_active ? "true" : "false");
							}
							uint64_t elapsedMs = 0;
							if (cb->interruptible_playback_started_ms) {
								uint64_t nowMs = current_time_ms();
								if (nowMs >= cb->interruptible_playback_started_ms) {
									elapsedMs = nowMs - cb->interruptible_playback_started_ms;
								}
							}
							if (elapsedMs >= cb->interruptible_playback_no_barge_ms) {
								if (vadState == SWITCH_VAD_STATE_START_TALKING || vadState == SWITCH_VAD_STATE_TALKING) {
									cb->vad_talking_ms += (uint64_t)(samples_to_ms(out_len, streamer->sampleRate()) + 0.5);
									if (!stop_interruptible_playback && cb->vad_talking_ms >= cb->barge_vad_hold_ms) {
										stop_interruptible_playback = SWITCH_TRUE;
										stop_active_backchannel = backchannelPlaying;
										cb->last_local_barge_break_ms = current_time_ms();
										reset_interruptible_playback_state(cb);
										switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
											"google_dialogflow_frame: local VAD barge-in triggered after %" PRIu64 "ms of playback and %" PRIu64 "ms of speech (hold=%ums backchannel=%s)\n",
											elapsedMs, cb->vad_talking_ms, cb->barge_vad_hold_ms, backchannelPlaying ? "true" : "false");
									}
								} else {
									cb->vad_talking_ms = 0;
								}
							} else if (vadState != SWITCH_VAD_STATE_NONE) {
								cb->vad_talking_ms = 0;
							}
						}
						
						streamer->write(session, &out[0], sizeof(spx_int16_t) * out_len);
					}
				}
			}
			else {
				//switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
				//	"google_dialogflow_frame: not sending audio because google channel has been closed\n");
			}
			switch_mutex_unlock(cb->mutex);
		}
		else {
			//switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
			//	"google_dialogflow_frame: not sending audio since failed to get lock on mutex\n");
		}
		if (stop_interruptible_playback) {
			if (stop_active_backchannel) {
				stop_backchannel(session);
			} else {
				stop_playback(session);
			}
		}
	return SWITCH_TRUE;
}

switch_status_t google_dialogflow_capture_snippet(struct cap_cb* cb, switch_core_session_t* session,
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
    GStreamer* streamer = (GStreamer*) cb->streamer;
    if (streamer->isSpooling()) {
        status = streamer->captureSnippet(session, start_ms, duration_ms,
            tag ? tag : "snippet", path, realStart, realEnd);
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

void google_dialogflow_spool_cleanup(struct cap_cb* cb, switch_core_session_t* session, switch_bool_t preserve) {
    (void)session;
    (void)preserve;
    if (!cb || !cb->streamer) {
        return;
    }
    switch_mutex_lock(cb->mutex);
    GStreamer* streamer = (GStreamer*) cb->streamer;
    streamer->stopSpool();
    switch_mutex_unlock(cb->mutex);
}

	void destroyChannelUserData(struct cap_cb* cb) {
		killcb(cb);
	}

}
