cmake_minimum_required(VERSION 3.18)
project(mod_audio_fork
        VERSION 1.0.0
        DESCRIPTION "Fork audio to remote server over WebSockets (FreeSWITCH module)"
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_SHARED_LIBRARY_PREFIX "")

# Allow developing against a locally installed FreeSWITCH
option(ENABLE_LOCAL "Enable local compile/debug specific" OFF)
if(ENABLE_LOCAL)
    set(ENV{PKG_CONFIG_PATH} "/usr/local/freeswitch/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(PkgConfig REQUIRED)
find_package(SpeexDSP REQUIRED)

pkg_check_modules(FreeSWITCH REQUIRED IMPORTED_TARGET freeswitch)
pkg_get_variable(FS_MOD_DIR freeswitch modulesdir)
message(STATUS "FreeSWITCH modules dir: ${FS_MOD_DIR}")

# libwebsockets via pkg-config
pkg_check_modules(LibWebSockets REQUIRED IMPORTED_TARGET libwebsockets)

add_library(mod_audio_fork SHARED
    mod_audio_fork.c
    mod_audio_fork.h
    lws_glue.h
    lws_glue.cpp
    audio_pipe.hpp
    audio_pipe.cpp
    parser.hpp
    parser.cpp
    base64.hpp
)

set_property(TARGET mod_audio_fork PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(mod_audio_fork PRIVATE
    ${SPEEXDSP_INCLUDE_DIRS}
)

target_link_libraries(mod_audio_fork PRIVATE
    PkgConfig::FreeSWITCH
    PkgConfig::LibWebSockets
    ${SPEEXDSP_LIBRARIES}
    pthread
)

install(TARGETS mod_audio_fork
        DESTINATION ${FS_MOD_DIR})

