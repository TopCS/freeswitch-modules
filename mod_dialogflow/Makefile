BASE=../../../..

# Build and link additional C++ sources needed by the module
# The FreeSWITCH modmake.rules will include these objects when linking.
LOCAL_OBJS = google_glue.lo parser.lo

# Link against gRPC C++ and protobuf (and transitively absl, etc.)
# Put into LDFLAGS_POST so they are not treated as prerequisites.
LOCAL_LDFLAGS_POST += $(shell pkg-config --libs grpc++ grpc protobuf 2>/dev/null)

# Add grpc++ and protobuf cflags (include dirs) when available
LOCAL_CFLAGS += $(shell pkg-config --cflags grpc++ protobuf 2>/dev/null)

GENS_DIR=/home/andrea-batazzi/dev/gens

# Add generated Dialogflow headers
LOCAL_CFLAGS += -I $(GENS_DIR)

# Build generated protos into local obj dir to avoid writing under GENS_DIR
GENS_CC := $(shell find $(GENS_DIR) -type f -name '*.cc' 2>/dev/null)
GENS_LO := $(patsubst $(GENS_DIR)/%.cc,gens_objs/%.lo,$(GENS_CC))

LOCAL_OBJS += $(GENS_LO)

gens_objs/%.lo: $(GENS_DIR)/%.cc
	@mkdir -p $(dir $@)
	@echo Compiling $<...
	@if test ! -z $(VERBOSE) ; then echo $(LTCXXCOMPILE) -c -o $@ $< ; fi
	@$(LTCXXCOMPILE) -c -o $@ $< || exit 1

include $(BASE)/build/modmake.rules
