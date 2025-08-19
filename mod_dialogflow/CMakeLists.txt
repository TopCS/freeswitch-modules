cmake_minimum_required(VERSION 3.18)
project(mod_dialogflow VERSION 1.0.0 DESCRIPTION "Dialogflow CX module for FreeSWITCH")

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_SHARED_LIBRARY_PREFIX "")

option(ENABLE_LOCAL "Add /usr/local/freeswitch pkgconfig path" ON)
set(GENS_DIR "" CACHE PATH "Path to generated Google APIs C++ sources root (contains google/...")
# Allow overriding the FreeSWITCH modules installation directory
set(FS_MOD_DIR "" CACHE PATH "Override FreeSWITCH modules dir (e.g., /usr/lib/freeswitch/mod)")

# Embed build metadata (git hash, date)
execute_process(
  COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD
  OUTPUT_VARIABLE GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT GIT_HASH)
  set(GIT_HASH "unknown")
endif()
string(TIMESTAMP BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)

if(ENABLE_LOCAL)
  set(ENV{PKG_CONFIG_PATH} "/usr/local/freeswitch/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

if(NOT GENS_DIR)
  message(FATAL_ERROR "GENS_DIR is required and must point to the generated googleapis C++ sources root")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FreeSWITCH REQUIRED IMPORTED_TARGET freeswitch)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc)
pkg_check_modules(Protobuf REQUIRED IMPORTED_TARGET protobuf)
pkg_check_modules(SPEEXDSP REQUIRED speexdsp)

# Determine FreeSWITCH modules installation directory from override or pkg-config
if(NOT FS_MOD_DIR OR FS_MOD_DIR STREQUAL "")
  pkg_get_variable(FS_MOD_DIR freeswitch modulesdir)
endif()
if(NOT FS_MOD_DIR OR FS_MOD_DIR STREQUAL "")
  # Fallbacks: Debian default first, then /usr/local
  if(EXISTS "/usr/lib/freeswitch/mod")
    set(FS_MOD_DIR "/usr/lib/freeswitch/mod")
  else()
    set(FS_MOD_DIR "/usr/local/freeswitch/mod")
  endif()
endif()
message(STATUS "FreeSWITCH modules dir: ${FS_MOD_DIR}")

file(GLOB_RECURSE GENS_SOURCES
  "${GENS_DIR}/google/cloud/dialogflow/cx/v3/*.cc"
  "${GENS_DIR}/google/api/*.cc"
  "${GENS_DIR}/google/longrunning/*.cc"
  "${GENS_DIR}/google/rpc/*.cc"
  "${GENS_DIR}/google/type/*.cc"
)

add_library(mod_dialogflow SHARED
  mod_dialogflow.c
  mod_dialogflow.h
  google_glue.cpp
  google_glue.h
  parser.cpp
  parser.h
  ${GENS_SOURCES}
)

target_compile_definitions(mod_dialogflow PRIVATE NOMINMAX)
target_compile_definitions(mod_dialogflow PRIVATE 
  MOD_DIALOGFLOW_VERSION="${PROJECT_VERSION}"
  MOD_DIALOGFLOW_GIT_HASH="${GIT_HASH}"
  MOD_DIALOGFLOW_BUILD_TYPE="$<CONFIG>"
)
target_include_directories(mod_dialogflow PRIVATE
  ${GENS_DIR}
)

# Make generated headers (build_info.h) visible
target_include_directories(mod_dialogflow PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Generate build_info.h at build time with a fresh timestamp (and git hash)
add_custom_target(gen_build_info ALL
  COMMAND ${CMAKE_COMMAND}
          -DBUILD_INFO_OUT=${CMAKE_CURRENT_BINARY_DIR}/build_info.h
          -DBUILD_TYPE=$<CONFIG>
          -DSRC_DIR=${CMAKE_SOURCE_DIR}
          -P ${CMAKE_SOURCE_DIR}/cmake/GenBuildInfo.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/build_info.h
  COMMENT "Generating build_info.h with current timestamp"
)
add_dependencies(mod_dialogflow gen_build_info)

# Suppress deprecation warnings in generated sources
foreach(src ${GENS_SOURCES})
  set_source_files_properties(${src} PROPERTIES COMPILE_FLAGS "-Wno-deprecated-declarations -Wno-unused-parameter")
endforeach()

target_include_directories(mod_dialogflow PRIVATE ${SPEEXDSP_INCLUDE_DIRS})
target_link_libraries(mod_dialogflow PRIVATE
  PkgConfig::FreeSWITCH
  PkgConfig::GRPCPP
  PkgConfig::GRPC
  PkgConfig::Protobuf
  ${SPEEXDSP_LIBRARIES}
  pthread
)

# Append any extra linker flags from pkg-config for speexdsp
target_link_options(mod_dialogflow PRIVATE ${SPEEXDSP_LDFLAGS_OTHER})

install(TARGETS mod_dialogflow DESTINATION ${FS_MOD_DIR})

# Packaging (.deb via CPack)
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "freeswitch-mod-dialogflow")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Maintainer <noreply@example.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")

# Detect Debian architecture for correct naming (e.g., amd64 instead of x86_64)
set(CPACK_DEBIAN_ARCHITECTURE "")
execute_process(
  COMMAND dpkg --print-architecture
  OUTPUT_VARIABLE DPKG_ARCH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(DPKG_ARCH)
  set(CPACK_DEBIAN_ARCHITECTURE "${DPKG_ARCH}")
else()
  # Fallback mapping
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(CPACK_DEBIAN_ARCHITECTURE "amd64")
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(CPACK_DEBIAN_ARCHITECTURE "arm64")
  else()
    set(CPACK_DEBIAN_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endif()

set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${CPACK_DEBIAN_ARCHITECTURE}")

# Optional: bundle gRPC/protobuf shared libs and set RPATH so the module can load them from its install dir
option(BUNDLE_GRPC_DEPS "Bundle gRPC/protobuf shared libs into the module dir" OFF)
if (BUNDLE_GRPC_DEPS)
  # Ensure the loader searches alongside the module
  set_target_properties(mod_dialogflow PROPERTIES INSTALL_RPATH "\$ORIGIN")
  # Copy a minimal set of deps from /usr/local/lib (where our Docker images install gRPC/protobuf)
  set(GRPC_LIB_DIR "/usr/local/lib")
  install(DIRECTORY ${GRPC_LIB_DIR}/ DESTINATION ${FS_MOD_DIR}
    FILES_MATCHING
      PATTERN "libgrpc*.so*"
      PATTERN "libprotobuf*.so*"
      PATTERN "libabsl_*.so*"
      PATTERN "libre2*.so*"
      PATTERN "libcares*.so*"
      PATTERN "libupb*.so*"
      PATTERN "libaddress_sorting*.so*"
  )
endif()

# Runtime deps (best-effort names for Debian)
if (BUNDLE_GRPC_DEPS)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "freeswitch, libspeexdsp1")
else()
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "freeswitch, libspeexdsp1, libprotobuf32, libgrpc++1.51, libgrpc29")
endif()

include(CPack)
