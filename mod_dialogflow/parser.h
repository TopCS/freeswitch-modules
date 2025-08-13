#ifndef __PARSER_H__
#define __PARSER_H__

#include <switch_json.h>
#include <grpcpp/grpcpp.h>
#include "google/cloud/dialogflow/cx/v3/session.grpc.pb.h"

namespace dfx = google::cloud::dialogflow::cx::v3;

class GRPCParser {
public:
    explicit GRPCParser(switch_core_session_t *session) : m_session(session) {}
    ~GRPCParser() {}

    cJSON* parse(const dfx::StreamingDetectIntentResponse& response);
    const std::string& parseAudio(const dfx::StreamingDetectIntentResponse& response);

private:
    // Helpers
    cJSON* parse(const google::protobuf::Struct& s);
    cJSON* parse(const google::protobuf::Value& v);

    switch_core_session_t *m_session;
};

#endif

