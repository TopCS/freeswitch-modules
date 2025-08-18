# Generate build_info.h with current timestamp and git hash

if(NOT DEFINED BUILD_INFO_OUT)
  message(FATAL_ERROR "BUILD_INFO_OUT not set")
endif()

# Compute timestamp in ISO-8601 UTC
string(TIMESTAMP NOW "%Y-%m-%dT%H:%M:%SZ" UTC)

# Compute git hash if possible
set(HASH "unknown")
if(DEFINED SRC_DIR)
  execute_process(
    COMMAND git -C "${SRC_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if("${HASH}" STREQUAL "")
    set(HASH "unknown")
  endif()
endif()

set(TYPE "unknown")
if(DEFINED BUILD_TYPE)
  set(TYPE "${BUILD_TYPE}")
endif()

file(WRITE "${BUILD_INFO_OUT}" "#ifndef MOD_DIALOGFLOW_BUILD_INFO_H\n")
file(APPEND "${BUILD_INFO_OUT}" "#define MOD_DIALOGFLOW_BUILD_INFO_H\n")
file(APPEND "${BUILD_INFO_OUT}" "#undef MOD_DIALOGFLOW_BUILD_DATE\n")
file(APPEND "${BUILD_INFO_OUT}" "#define MOD_DIALOGFLOW_BUILD_DATE \"${NOW}\"\n")
file(APPEND "${BUILD_INFO_OUT}" "#undef MOD_DIALOGFLOW_GIT_HASH\n")
file(APPEND "${BUILD_INFO_OUT}" "#define MOD_DIALOGFLOW_GIT_HASH \"${HASH}\"\n")
file(APPEND "${BUILD_INFO_OUT}" "#undef MOD_DIALOGFLOW_BUILD_TYPE\n")
file(APPEND "${BUILD_INFO_OUT}" "#define MOD_DIALOGFLOW_BUILD_TYPE \"${TYPE}\"\n")
file(APPEND "${BUILD_INFO_OUT}" "#endif /* MOD_DIALOGFLOW_BUILD_INFO_H */\n")
