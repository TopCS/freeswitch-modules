#include "parser.h"
#include <switch.h>

using namespace google::protobuf;

static cJSON* json_string(const std::string& s) {
    return cJSON_CreateString(s.c_str());
}

const std::string& GRPCParser::parseAudio(const dfx::StreamingDetectIntentResponse& response) {
    static const std::string empty;
    if (response.has_detect_intent_response()) {
        return response.detect_intent_response().output_audio();
    }
    return empty;
}

cJSON* GRPCParser::parse(const dfx::StreamingDetectIntentResponse& response) {
    cJSON * json = cJSON_CreateObject();

    // recognition_result
    if (response.has_recognition_result()) {
        const auto& rr = response.recognition_result();
        cJSON* jrr = cJSON_CreateObject();
        cJSON_AddItemToObject(jrr, "transcript", json_string(rr.transcript()));
        cJSON_AddItemToObject(jrr, "is_final", cJSON_CreateBool(rr.is_final()));
        cJSON_AddItemToObject(jrr, "message_type", json_string(dfx::StreamingRecognitionResult::MessageType_Name(rr.message_type())));
        cJSON_AddItemToObject(json, "recognition_result", jrr);
    }

    // detect_intent_response
    if (response.has_detect_intent_response()) {
        const auto& dir = response.detect_intent_response();
        cJSON_AddItemToObject(json, "response_id", json_string(dir.response_id()));

        // query_result
        if (dir.has_query_result()) {
            const auto& qr = dir.query_result();
            cJSON* jqr = cJSON_CreateObject();
            // Prefer match.intent (CX) over deprecated qr.intent
            if (qr.has_match() && qr.match().has_intent()) {
                const auto& mi = qr.match().intent();
                cJSON* jintent = cJSON_CreateObject();
                cJSON_AddItemToObject(jintent, "name", json_string(mi.name()));
                cJSON_AddItemToObject(jintent, "display_name", json_string(mi.display_name()));
                cJSON_AddItemToObject(jqr, "intent", jintent);
            } else if (qr.has_intent()) {
                cJSON* jintent = cJSON_CreateObject();
                cJSON_AddItemToObject(jintent, "name", json_string(qr.intent().name()));
                cJSON_AddItemToObject(jintent, "display_name", json_string(qr.intent().display_name()));
                cJSON_AddItemToObject(jqr, "intent", jintent);
            }
            cJSON_AddItemToObject(jqr, "language_code", json_string(qr.language_code()));
            if (qr.has_current_page()) {
                const auto& pg = qr.current_page();
                cJSON* jpage = cJSON_CreateObject();
                cJSON_AddItemToObject(jpage, "name", json_string(pg.name()));
                cJSON_AddItemToObject(jpage, "display_name", json_string(pg.display_name()));
                cJSON_AddItemToObject(jqr, "current_page", jpage);
            }
            if (qr.has_parameters()) {
                cJSON_AddItemToObject(jqr, "parameters", parse(qr.parameters()));
            }
            cJSON_AddItemToObject(json, "query_result", jqr);
        }

        // output_audio_config
        if (dir.has_output_audio_config()) {
            const auto& cfg = dir.output_audio_config();
            cJSON* jcfg = cJSON_CreateObject();
            cJSON_AddItemToObject(jcfg, "audio_encoding", json_string(dfx::OutputAudioEncoding_Name(cfg.audio_encoding())));
            cJSON_AddItemToObject(jcfg, "sample_rate_hertz", cJSON_CreateNumber(cfg.sample_rate_hertz()));
            cJSON_AddItemToObject(json, "output_audio_config", jcfg);
        }
    }

    return json;
}

cJSON* GRPCParser::parseStruct(const Struct& s) {
    return parse(s);
}

cJSON* GRPCParser::parse(const Struct& s) {
    cJSON* obj = cJSON_CreateObject();
    for (const auto& it : s.fields()) {
        cJSON_AddItemToObject(obj, it.first.c_str(), parse(it.second));
    }
    return obj;
}

cJSON* GRPCParser::parse(const Value& v) {
    switch (v.kind_case()) {
        case Value::kNullValue:
            return cJSON_CreateNull();
        case Value::kNumberValue:
            return cJSON_CreateNumber(v.number_value());
        case Value::kStringValue:
            return json_string(v.string_value());
        case Value::kBoolValue:
            return cJSON_CreateBool(v.bool_value());
        case Value::kStructValue:
            return parse(v.struct_value());
        case Value::kListValue: {
            cJSON* arr = cJSON_CreateArray();
            for (const auto& e : v.list_value().values()) {
                cJSON_AddItemToArray(arr, parse(e));
            }
            return arr;
        }
        default:
            return cJSON_CreateNull();
    }
}
