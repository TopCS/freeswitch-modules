#ifndef __CES_PARSER_H__
#define __CES_PARSER_H__

#include <switch_json.h>
#include <google/protobuf/struct.pb.h>
#include <string>

namespace google {
namespace cloud {
namespace ces {
namespace v1beta {
class BidiSessionServerMessage;
}
}
}
}

class CESParser {
public:
	static cJSON* parse(const google::cloud::ces::v1beta::BidiSessionServerMessage& response);
	static const std::string& parseAudio(const google::cloud::ces::v1beta::BidiSessionServerMessage& response);
	static cJSON* parseStruct(const google::protobuf::Struct& s);

private:
	static cJSON* parseValue(const google::protobuf::Value& v);
};

#endif
