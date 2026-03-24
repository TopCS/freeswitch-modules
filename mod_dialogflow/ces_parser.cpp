#include "ces_parser.h"

#include "google/cloud/ces/v1beta/session_service.pb.h"

#include <string>

namespace ces = google::cloud::ces::v1beta;

using google::protobuf::Struct;
using google::protobuf::Value;

namespace {

static cJSON* json_string(const std::string& s) {
	return cJSON_CreateString(s.c_str());
}

static cJSON* parse_end_session(const ces::EndSession& end_session) {
	cJSON* json = cJSON_CreateObject();
	if (end_session.has_metadata()) {
		cJSON_AddItemToObject(json, "metadata", CESParser::parseStruct(end_session.metadata()));
	}
	return json;
}

static cJSON* parse_diagnostic_info(const ces::SessionOutput::DiagnosticInfo& info) {
	cJSON* json = cJSON_CreateObject();
	cJSON_AddNumberToObject(json, "messages_count", info.messages_size());
	cJSON_AddItemToObject(json, "has_root_span", cJSON_CreateBool(info.has_root_span()));
	return json;
}

}

const std::string& CESParser::parseAudio(const ces::BidiSessionServerMessage& response) {
	static const std::string empty;
	if (response.has_session_output() && response.session_output().has_audio()) {
		return response.session_output().audio();
	}
	return empty;
}

cJSON* CESParser::parse(const ces::BidiSessionServerMessage& response) {
	cJSON* json = cJSON_CreateObject();

	if (response.has_recognition_result()) {
		const auto& rr = response.recognition_result();
		cJSON_AddStringToObject(json, "message_type", "recognition_result");
		cJSON* jrr = cJSON_CreateObject();
		cJSON_AddItemToObject(jrr, "transcript", json_string(rr.transcript()));
		cJSON_AddItemToObject(json, "recognition_result", jrr);
	}

	if (response.has_session_output()) {
		const auto& so = response.session_output();
		cJSON_AddStringToObject(json, "message_type", "session_output");
		cJSON* jso = cJSON_CreateObject();
		cJSON_AddNumberToObject(jso, "turn_index", so.turn_index());
		cJSON_AddItemToObject(jso, "turn_completed", cJSON_CreateBool(so.turn_completed()));
		if (so.has_text()) {
			cJSON_AddItemToObject(jso, "text", json_string(so.text()));
		}
		if (so.has_audio()) {
			cJSON_AddNumberToObject(jso, "audio_size", (double) so.audio().size());
		}
		if (so.has_payload()) {
			cJSON_AddItemToObject(jso, "payload", parseStruct(so.payload()));
		}
		if (so.has_end_session()) {
			cJSON_AddItemToObject(jso, "end_session", parse_end_session(so.end_session()));
		}
		if (so.has_diagnostic_info()) {
			cJSON_AddItemToObject(jso, "diagnostic_info", parse_diagnostic_info(so.diagnostic_info()));
		}
		cJSON_AddItemToObject(json, "session_output", jso);
	}

	if (response.has_interruption_signal()) {
		const auto& signal = response.interruption_signal();
		cJSON_AddStringToObject(json, "message_type", "interruption_signal");
		cJSON* js = cJSON_CreateObject();
		cJSON_AddItemToObject(js, "barge_in", cJSON_CreateBool(signal.barge_in()));
		cJSON_AddItemToObject(json, "interruption_signal", js);
	}

	if (response.has_end_session()) {
		cJSON_AddStringToObject(json, "message_type", "end_session");
		cJSON_AddItemToObject(json, "end_session", parse_end_session(response.end_session()));
	}

	if (response.has_go_away()) {
		cJSON_AddStringToObject(json, "message_type", "go_away");
		cJSON_AddItemToObject(json, "go_away", cJSON_CreateBool(1));
	}

	return json;
}

cJSON* CESParser::parseStruct(const Struct& s) {
	cJSON* obj = cJSON_CreateObject();
	for (const auto& it : s.fields()) {
		cJSON_AddItemToObject(obj, it.first.c_str(), parseValue(it.second));
	}
	return obj;
}

cJSON* CESParser::parseValue(const Value& v) {
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
			return parseStruct(v.struct_value());
		case Value::kListValue: {
			cJSON* arr = cJSON_CreateArray();
			for (const auto& e : v.list_value().values()) {
				cJSON_AddItemToArray(arr, parseValue(e));
			}
			return arr;
		}
		default:
			return cJSON_CreateNull();
	}
}
