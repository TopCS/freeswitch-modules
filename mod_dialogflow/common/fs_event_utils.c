#include "fs_event_utils.h"

switch_status_t fs_fire_custom_event_with_body(
	switch_core_session_t* session,
	const char* subclass,
	const char* body,
	const fs_channel_var_header_map_t* header_map,
	size_t header_map_len)
{
	switch_event_t *event = NULL;
	switch_channel_t *channel;
	size_t i;

	if (!session || zstr(subclass)) {
		return SWITCH_STATUS_FALSE;
	}

	channel = switch_core_session_get_channel(session);
	if (!channel) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, subclass) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	switch_channel_event_set_data(channel, event);

	for (i = 0; header_map && i < header_map_len; ++i) {
		const char* value;
		if (zstr(header_map[i].channel_var) || zstr(header_map[i].event_header)) {
			continue;
		}
		value = switch_channel_get_variable(channel, header_map[i].channel_var);
		if (!zstr(value)) {
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, header_map[i].event_header, value);
		}
	}

	if (!zstr(body)) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Response", body);
		switch_event_add_body(event, "%s", body);
	}

	switch_event_fire(&event);
	return SWITCH_STATUS_SUCCESS;
}
