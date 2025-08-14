#include <cstdlib>

#include <switch.h>
#include <switch_json.h>
#include <grpcpp/grpcpp.h>
#include <string.h>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <stdexcept>

#include <regex>

#include <fstream>
#include <string>
#include <sstream>
#include <map>
#include <set>

#include "google/cloud/dialogflow/cx/v3/session.grpc.pb.h"

#include "mod_dialogflow.h"
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
using google::rpc::Status;
using google::protobuf::Struct;
using google::protobuf::Value;
using google::protobuf::MapPair;

static uint64_t playCount = 0;
static std::multimap<std::string, std::string> audioFiles;
static bool hasDefaultCredentials = false;

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
    return root;
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
    GStreamer(switch_core_session_t *session, const char* lang, char* projectId, char* event, char* text) :
            m_lang(lang), m_sessionId(switch_core_session_get_uuid(session)), m_environment("draft"), m_regionId("us"), m_agentId(""),
            m_speakingRate(), m_pitch(), m_volume(), m_voiceName(""), m_voiceGender(""), m_effects(""),
            m_sentimentAnalysis(false), m_finished(false), m_packets(0), m_needConfig(false),
            m_paused(false),
            m_startedWithEvent(false), m_rotatedToAudio(false) {
		const char* var;
		switch_channel_t* channel = switch_core_session_get_channel(session);
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
            else if (4 == idx && s.length() > 0) m_speakingRate = stod(s);
            else if (5 == idx && s.length() > 0) m_pitch = stod(s);
            else if (6 == idx && s.length() > 0) m_volume = stod(s);
            else if (7 == idx && s.length() > 0) m_voiceName = s;
            else if (8 == idx && s.length() > 0) m_voiceGender = s;
            else if (9 == idx && s.length() > 0) m_effects = s;
            else if (10 == idx && s.length() > 0) m_sentimentAnalysis = (s == "true");
            idx++;
        }

		std::string endpoint = "dialogflow.googleapis.com";
		if (0 != m_regionId.compare("us")) {
			endpoint = m_regionId;
			endpoint.append("-dialogflow.googleapis.com:443");
		}

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
            "GStreamer dialogflow CX endpoint is %s, region is %s, project is %s, agent is %s, environment is %s\n", 
            endpoint.c_str(), m_regionId.c_str(), m_projectId.c_str(), m_agentId.c_str(), m_environment.c_str());        

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
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "GStreamer credentials path not readable: %s; falling back to default creds\n", input.c_str());
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
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::~GStreamer wrote %u packets %p\n", m_packets, this);		
	}

    void startStream(switch_core_session_t *session, const char* event, const char* text) {
		char szSession[256];

		m_request = std::make_shared<StreamingDetectIntentRequest>();
		m_context= std::make_shared<grpc::ClientContext>();
		m_stub = Sessions::NewStub(m_channel);

        snprintf(szSession, 256, "projects/%s/locations/%s/agents/%s/sessions/%s", 
                    m_projectId.c_str(), m_regionId.c_str(), m_agentId.c_str(), m_sessionId.c_str());

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "GStreamer::startStream session %s, event %s, text %s %p\n", szSession, event, text, this);

        m_request->set_session(szSession);
        auto* queryInput = m_request->mutable_query_input();
        switch_channel_t* channel = switch_core_session_get_channel(session);
        if (event) {
            auto* eventInput = queryInput->mutable_event();
            eventInput->set_event(event);
            queryInput->set_language_code(m_lang.c_str());
            m_startedWithEvent = true;
            // Merge optional parameters: 5th arg JSON, DIALOGFLOW_CHANNEL, DIALOGFLOW_PARAMS, optional channel vars
            cJSON* root = cJSON_CreateObject();
            bool have_params = false;
            if (text) {
                cJSON* json = cJSON_Parse(text);
                if (json) {
                    have_params = true;
                    for (cJSON* it = json->child; it; it = it->next) {
                        cJSON_ReplaceItemInObject(root, it->string, cJSON_Duplicate(it, 1));
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
                // Also include in parameters for agent usage if desired
                have_params = true;
                cJSON_AddItemToObject(root, "channel", cJSON_CreateString(ch));
            }
            const char* js = switch_channel_get_variable(channel, "DIALOGFLOW_PARAMS");
            if (js && *js) {
                cJSON* json = cJSON_Parse(js);
                if (json) {
                    have_params = true;
                    // If JSON includes a top-level "channel", set QueryParameters.channel too
                    cJSON* chv = cJSON_GetObjectItemCaseSensitive(json, "channel");
                    if (cJSON_IsString(chv) && chv->valuestring && chv->valuestring[0]) {
                        m_request->mutable_query_params()->set_channel(chv->valuestring);
                        m_qpChannel = chv->valuestring;
                    }
                    for (cJSON* it = json->child; it; it = it->next) {
                        cJSON_ReplaceItemInObject(root, it->string, cJSON_Duplicate(it, 1));
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
                    cJSON_ReplaceItemInObject(root, it->string, cJSON_Duplicate(it, 1));
                }
                cJSON_Delete(chvars);
            }
            if (have_params) {
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
                    audio_config->set_sample_rate_hertz(16000);
                    audio_config->set_audio_encoding(AudioEncoding::AUDIO_ENCODING_LINEAR_16);
                    audio_config->set_single_utterance(true);
                    queryInput->set_language_code(m_lang.c_str());
                } else {
                    auto* textInput = queryInput->mutable_text();
                    textInput->set_text(text);
                    queryInput->set_language_code(m_lang.c_str());
                    // Start next turn in audio mode after sending a plain text input
                    m_needConfig.store(true);
                }
            } else {
                auto* textInput = queryInput->mutable_text();
                textInput->set_text(text);
                queryInput->set_language_code(m_lang.c_str());
                // Start next turn in audio mode after sending a plain text input
                m_needConfig.store(true);
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
            }
            const char* js = switch_channel_get_variable(channel, "DIALOGFLOW_PARAMS");
            if (js && *js) {
                cJSON* json2 = cJSON_Parse(js);
                if (json2) {
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
            audio_config->set_sample_rate_hertz(16000);
            audio_config->set_audio_encoding(AudioEncoding::AUDIO_ENCODING_LINEAR_16);
            audio_config->set_single_utterance(true);
            queryInput->set_language_code(m_lang.c_str());
            // Optionally inject channel variables even for pure audio start
            if (cJSON* chvars = collectChannelVarsAsJSON(session)) {
                auto* qp = m_request->mutable_query_params();
                auto* params = qp->mutable_parameters();
                parseEventParams(params, chvars);
                cJSON_Delete(chvars);
            }
        }
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::startStream requesting OutputAudio in LINEAR16 @16000Hz; custom params? %s\n",
                          isAnyOutputAudioConfigChanged() ? "yes" : "no");
        auto* outputAudioConfig = m_request->mutable_output_audio_config();
        outputAudioConfig->set_sample_rate_hertz(16000);
        outputAudioConfig->set_audio_encoding(OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_LINEAR_16);

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
	bool write(void* data, uint32_t datalen) {
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
            audio_config->set_sample_rate_hertz(16000);
            audio_config->set_audio_encoding(AudioEncoding::AUDIO_ENCODING_LINEAR_16);
            audio_config->set_single_utterance(true);
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::write sent new audio config to start next turn\n");
        } else {
            ai->clear_config();
        }
        ai->set_audio(reinterpret_cast<const char*>(data), datalen);

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

    bool isAnyOutputAudioConfigChanged() {
        return m_speakingRate|| m_pitch || m_volume || !m_voiceName.empty() || !m_voiceGender.empty() || !m_effects.empty();
    }

    void setNeedConfig() {
        m_needConfig.store(true);
    }

    void setPaused(bool paused) {
        m_paused.store(paused);
    }

    bool isPaused() const { return m_paused.load(); }

    void rotateToAudioConfig(switch_core_session_t* session) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
            "GStreamer: rotating stream to audio mode for next user turn\n");
        // Gracefully finish current stream
        try { m_streamer->WritesDone(); } catch (...) {}
        try { m_streamer->Finish(); } catch (...) {}

        // Create a new stream configured for audio input
        m_request = std::make_shared<StreamingDetectIntentRequest>();
        m_context = std::make_shared<grpc::ClientContext>();
        // Reuse same session path
        char szSession[256];
        snprintf(szSession, 256, "projects/%s/locations/%s/agents/%s/sessions/%s",
                 m_projectId.c_str(), m_regionId.c_str(), m_agentId.c_str(), m_sessionId.c_str());
        m_request->set_session(szSession);

        auto* qi = m_request->mutable_query_input();
        auto* audio_input = qi->mutable_audio();
        auto* audio_config = audio_input->mutable_config();
        audio_config->set_sample_rate_hertz(16000);
        audio_config->set_audio_encoding(AudioEncoding::AUDIO_ENCODING_LINEAR_16);
        audio_config->set_single_utterance(true);
        qi->set_language_code(m_lang.c_str());

        // Always request output audio
        auto* outputAudioConfig = m_request->mutable_output_audio_config();
        outputAudioConfig->set_sample_rate_hertz(16000);
        outputAudioConfig->set_audio_encoding(OutputAudioEncoding::OUTPUT_AUDIO_ENCODING_LINEAR_16);
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
        }
        if (m_sentimentAnalysis) qp->set_analyze_query_text_sentiment(true);

        m_streamer = m_stub->StreamingDetectIntent(m_context.get());
        m_streamer->Write(*m_request);
        // Subsequent writes will send only audio bytes
    }

private:
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
};

static void killcb(struct cap_cb* cb) {
	if (cb) {
		if (cb->streamer) {
			GStreamer* p = (GStreamer *) cb->streamer;
			delete p;
			cb->streamer = NULL;
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

				if (response.has_detect_intent_response() || response.has_recognition_result()) {
					cJSON* jResponse = parser.parse(response) ;
					char* json = cJSON_PrintUnformatted(jResponse);
					const char* type = DIALOGFLOW_EVENT_TRANSCRIPTION;

				if (response.has_detect_intent_response()) type = DIALOGFLOW_EVENT_INTENT;
				else {
					auto o = response.recognition_result().message_type();
                    if (0 == StreamingRecognitionResult::MessageType_Name(o).compare("END_OF_SINGLE_UTTERANCE")) {
                        type = DIALOGFLOW_EVENT_END_OF_UTTERANCE;
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                            "grpc_read_thread: END_OF_SINGLE_UTTERANCE received\n");
                    }
				}

				cb->responseHandler(psession, type, json);

				free(json);
				cJSON_Delete(jResponse);
			}

			const std::string& audio = parser.parseAudio(response);
			bool playAudio = !audio.empty() ;
            if (response.has_detect_intent_response()) {
                const auto& dir = response.detect_intent_response();
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                    "grpc_read_thread: detect_intent_response output_audio bytes=%zu config? %s\n",
                    audio.size(), dir.has_output_audio_config() ? "yes" : "no");
                // Decide if we should barge-in or block the next user turn during playback
                bool allow_barge_in = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_BARGE_IN"));
                bool will_autoplay = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY"));
                bool autoplay_sync = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY_SYNC"));
                // Default to sync when AUTOPLAY is requested unless explicitly disabled
                if (will_autoplay && switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY_SYNC") == NULL) {
                    autoplay_sync = true;
                }
                if (!allow_barge_in && will_autoplay && autoplay_sync && playAudio) {
                    // Pause streaming while we play the agent audio; we will resume afterwards
                    streamer->setPaused(true);
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                        "grpc_read_thread: pausing input during sync autoplay (barge-in disabled)\n");
                } else {
                    // Arm next listening turn now; audio config will be sent on next frame
                    streamer->setNeedConfig();
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                        "grpc_read_thread: arming next turn; will send new audio config on next frame\n");
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
                    std::string Udisp = toUpper(disp);
                    std::string page_disp;
                    if (qr.has_current_page()) {
                        page_disp = qr.current_page().display_name();
                    }
                    std::string Upage = toUpper(page_disp);
                    bool acted = false;
                    // End session: match by page display name (if provided) or intent display name
                    bool end_match = false;
                    if (!end_page.empty() && Upage == toUpper(end_page)) end_match = true;
                    if (!end_match && have_intent && !end_intent.empty() && Udisp == toUpper(end_intent)) end_match = true;
                    if (end_match) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_NOTICE,
                            "DF END_SESSION match: intent='%s' page='%s' emit_only=%s\n",
                            disp.c_str(), page_disp.c_str(), emit_only ? "true" : "false");
                        // Fire end_session event
                        cJSON* j = cJSON_CreateObject();
                        cJSON_AddItemToObject(j, "intent_display_name", cJSON_CreateString(disp.c_str()));
                        if (!page_disp.empty()) cJSON_AddItemToObject(j, "page_display_name", cJSON_CreateString(page_disp.c_str()));
                        char* body = cJSON_PrintUnformatted(j);
                        switch_event_t* ev = nullptr;
                        if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, DIALOGFLOW_EVENT_END_SESSION) == SWITCH_STATUS_SUCCESS) {
                            switch_channel_event_set_data(channel, ev);
                            switch_event_add_body(ev, "%s", body);
                            switch_event_fire(&ev);
                        }
                        free(body);
                        cJSON_Delete(j);
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
            if (playAudio) {
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

				cJSON * jResponse = cJSON_CreateObject();
				cJSON_AddItemToObject(jResponse, "path", cJSON_CreateString(s.str().c_str()));
				char* json = cJSON_PrintUnformatted(jResponse);

				cb->responseHandler(psession, DIALOGFLOW_EVENT_AUDIO_PROVIDED, json);
				free(json);
				cJSON_Delete(jResponse);

				// Optional auto-play: play returned audio on the A leg when requested
				const char* ap = switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY");
				bool will_autoplay = ap && switch_true(ap);
				bool autoplay_sync = switch_true(switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY_SYNC"));
				if (will_autoplay && switch_channel_get_variable(channel, "DIALOGFLOW_AUTOPLAY_SYNC") == NULL) {
					autoplay_sync = true; // default to sync to avoid no_input during long prompts
				}
                if (will_autoplay) {
                    if (autoplay_sync) {
                        // Play synchronously so we know when it finishes
                        switch_status_t st = switch_ivr_play_file(psession, NULL, s.str().c_str(), NULL);
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
                            "Auto-playing Dialogflow audio synchronously: %s (status=%d)\n", s.str().c_str(), st);
                        // Resume streaming and rotate to a fresh audio-configured stream for next user turn
                        switch_mutex_lock(cb->mutex);
                        streamer->setPaused(false);
                        streamer->rotateToAudioConfig(psession);
                        switch_mutex_unlock(cb->mutex);
                    } else {
                        // Fallback: async broadcast (legacy behavior)
                        char args[1024];
                        snprintf(args, sizeof(args), "%s %s aleg", cb->sessionId, s.str().c_str());
                        switch_stream_handle_t stream = { 0 };
                        SWITCH_STANDARD_STREAM(stream);
                        switch_status_t st = switch_api_execute("uuid_broadcast", args, NULL, &stream);
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
                            "Auto-playing Dialogflow audio via uuid_broadcast: %s (status=%d)\n", args, st);
                        switch_safe_free(stream.data);
                        // Rotate immediately to begin listening during async playback
                        switch_mutex_lock(cb->mutex);
                        if (streamer->isPaused()) streamer->setPaused(false);
                        streamer->rotateToAudioConfig(psession);
                        switch_mutex_unlock(cb->mutex);
                    }
                } else {
                    // Not auto-playing here. If we paused earlier, resume and rotate now.
                    switch_mutex_lock(cb->mutex);
                    if (streamer->isPaused()) streamer->setPaused(false);
                    streamer->rotateToAudioConfig(psession);
                    switch_mutex_unlock(cb->mutex);
                }
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
		switch_threadattr_t *thd_attr = NULL;
		switch_memory_pool_t *pool = switch_core_session_get_pool(session);
		struct cap_cb* cb = (struct cap_cb *) switch_core_session_alloc(session, sizeof(*cb));
		memset(cb, 0, sizeof(*cb));

		if (!hasDefaultCredentials && !switch_channel_get_variable(channel, "GOOGLE_APPLICATION_CREDENTIALS")) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, 
				"missing credentials: GOOGLE_APPLICATION_CREDENTIALS must be suuplied either as an env variable (path to file) or a channel variable (json string)\n");
			status = SWITCH_STATUS_FALSE;
			goto done; 
		}

		strncpy(cb->sessionId, switch_core_session_get_uuid(session), 256);
		cb->responseHandler = responseHandler;
		cb->errorHandler = errorHandler;

		if (switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing mutex\n");
			status = SWITCH_STATUS_FALSE;
			goto done; 
		}

        strncpy(cb->lang, lang, MAX_LANG);
        strncpy(cb->projectId, projectId, MAX_PROJECT_ID);
        try {
            cb->streamer = new GStreamer(session, lang, projectId, event, text);
        } catch (const std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "GStreamer construction failed: %s\n", e.what());
            status = SWITCH_STATUS_FALSE;
            goto done;
        } catch (...) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "GStreamer construction failed with unknown error\n");
            status = SWITCH_STATUS_FALSE;
            goto done;
        }

        // Now start the gRPC stream, protect against exceptions
        try {
            ((GStreamer*)cb->streamer)->startStream(session, event, text);
        } catch (const std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "GStreamer startStream failed: %s\n", e.what());
            status = SWITCH_STATUS_FALSE;
            goto done;
        } catch (...) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "GStreamer startStream failed with unknown error\n");
            status = SWITCH_STATUS_FALSE;
            goto done;
        }
		cb->resampler = speex_resampler_init(1, 8000, 16000, SWITCH_RESAMPLE_QUALITY, &err);
		if (0 != err) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing resampler: %s.\n", 
						switch_channel_get_name(channel), speex_resampler_strerror(err));
			status = SWITCH_STATUS_FALSE;
			goto done;
		}

		// hangup hook to clear temp audio files
		switch_core_event_hook_add_state_change(session, hanguphook);

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

		if (bug) {
			struct cap_cb *cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
			switch_status_t st;

			// close connection and get final responses
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "google_dialogflow_session_cleanup: acquiring lock\n");
			switch_mutex_lock(cb->mutex);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "google_dialogflow_session_cleanup: acquired lock\n");
			GStreamer* streamer = (GStreamer *) cb->streamer;
			if (streamer) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "google_dialogflow_session_cleanup: sending writesDone..\n");
				streamer->writesDone();
				streamer->finish();
			}
			if (cb->thread) {
				switch_status_t retval;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "google_dialogflow_session_cleanup: waiting for read thread to complete\n");
				switch_thread_join(&retval, cb->thread);
				cb->thread = NULL;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "google_dialogflow_session_cleanup: read thread completed\n");
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

		frame.data = data;
		frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

		if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {
			GStreamer* streamer = (GStreamer *) cb->streamer;
			if (streamer && !streamer->isFinished()) {
				while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
					if (frame.datalen) {
						spx_int16_t out[SWITCH_RECOMMENDED_BUFFER_SIZE];
						spx_uint32_t out_len = SWITCH_RECOMMENDED_BUFFER_SIZE;
						spx_uint32_t in_len = frame.samples;
						size_t written;
						
						speex_resampler_process_interleaved_int(cb->resampler, (const spx_int16_t *) frame.data, (spx_uint32_t *) &in_len, &out[0], &out_len);
						
						streamer->write( &out[0], sizeof(spx_int16_t) * out_len);
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
		return SWITCH_TRUE;
	}

	void destroyChannelUserData(struct cap_cb* cb) {
		killcb(cb);
	}

}
