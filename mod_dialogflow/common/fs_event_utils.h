#ifndef __FS_EVENT_UTILS_H__
#define __FS_EVENT_UTILS_H__

#include <switch.h>
#include <stddef.h>

typedef struct fs_channel_var_header_map_s {
	const char* channel_var;
	const char* event_header;
} fs_channel_var_header_map_t;

switch_status_t fs_fire_custom_event_with_body(
	switch_core_session_t* session,
	const char* subclass,
	const char* body,
	const fs_channel_var_header_map_t* header_map,
	size_t header_map_len);

#endif
