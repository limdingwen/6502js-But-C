// Copyright 2021 Lim Ding Wen
//
// This file is part of 6502js But C.
// 
// 6502js But C is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// 6502js But C is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
// 
// You should have received a copy of the GNU Affero General Public License
// along with 6502js But C.  If not, see <https://www.gnu.org/licenses/>.

#include "os.h"

#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <stdlib.h>
#include <stdio.h>

xcb_connection_t *connection = NULL;
xcb_screen_t *screen = NULL;
xcb_window_t window = 0; 
xcb_intern_atom_reply_t *del_win_rep = NULL;
bool should_exit = false;
xcb_gcontext_t *xcb_colors = NULL;
struct xkb_state *keyboard_state;

int main(int argc, char **argv) {
	return our_main(argc, argv);
}

void os_create_window(const char *name, int width, int height) {
	// Connect to X
	connection = xcb_connect(NULL, NULL);
	screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;

	// Create and show window
	window = xcb_generate_id(connection);
	{
		uint32_t masks = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
		uint32_t values[] = {
			screen->black_pixel,
			XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
				XCB_EVENT_MASK_KEY_RELEASE
		};
		xcb_create_window(connection, XCB_COPY_FROM_PARENT, window,
			screen->root, 0, 0, width, height, 10,
			XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, masks, values);
	}
	xcb_map_window(connection, window);
	xcb_flush(connection);
	
	// Register for "delete window" event from window manager
	xcb_intern_atom_reply_t *protocol_rep = xcb_intern_atom_reply(
		connection, xcb_intern_atom(connection, 1, 12, "WM_PROTOCOLS"), 0);
	del_win_rep = xcb_intern_atom_reply(
		connection, xcb_intern_atom(connection, 0, 16, "WM_DELETE_WINDOW"), 0);
	xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
		(*protocol_rep).atom, 4, 32, 1, &(*del_win_rep).atom);

	// Get keyboard keymap and state
	{
		// FIXME: Need free
		xkb_x11_setup_xkb_extension(connection, XKB_X11_MIN_MAJOR_XKB_VERSION,
			XKB_X11_MIN_MINOR_XKB_VERSION, 0, NULL, NULL, NULL, NULL);
		struct xkb_context *xkb_context =
			xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		uint32_t keyboard_device = 
			xkb_x11_get_core_keyboard_device_id(connection);
		struct xkb_keymap *keymap = xkb_x11_keymap_new_from_device(xkb_context,
			connection, keyboard_device, XKB_KEYMAP_COMPILE_NO_FLAGS);
		keyboard_state = xkb_x11_state_new_from_device(keymap, connection,
			keyboard_device);
	}
}

uint16_t ftoi16(float f) { return (uint16_t) (f * 65535); }
void os_create_colormap(const float *rgb, int length) {
	xcb_colors = malloc(length * sizeof(xcb_gcontext_t));
	for (int i = 0; i < length; i++) {
		// Get color from list
		float r = rgb[i * 3 + 0];
		float g = rgb[i * 3 + 1];
		float b = rgb[i * 3 + 2];

		// Get color from colormap
		xcb_alloc_color_reply_t *rep = xcb_alloc_color_reply(connection,
			xcb_alloc_color(connection, screen->default_colormap,
				ftoi16(r), ftoi16(g), ftoi16(b)),
			NULL);

		// Create context from color
		xcb_gcontext_t gc = xcb_generate_id(connection);
		uint32_t value[] = { rep->pixel };
		xcb_create_gc(connection, gc, window, XCB_GC_FOREGROUND, value);
		xcb_colors[i] = gc;
		free(rep);
	}
}

bool os_choose_bin(char* path, int pathLength) {
	return false; // Not supported on Linux
}

bool os_should_exit(void) {
	return should_exit; // Set in poll event
}

bool os_poll_event(struct event *ev) {
	xcb_generic_event_t *event = xcb_poll_for_event(connection);
	bool found_event = false;
	if (event) {
		//printf("XCB Event %d\n", event->response_type);
		switch (event->response_type & ~0x80) {
			// Detect exiting
			case XCB_CLIENT_MESSAGE:
				if (((xcb_client_message_event_t*)event)->data.data32[0]
					== del_win_rep->atom) {
					should_exit = true;
				}
				found_event = false; // Event not processed by main.c
				break;

			// Detect redraw required
			case XCB_EXPOSE:
				ev->type = ET_EXPOSE;
				found_event = true;
				break;

			// Detect key press
			case XCB_KEY_PRESS: {
				// Get keycode
				uint8_t keycode =((xcb_key_press_event_t*)event)->detail;

				// Updates state; this struct tracks things like SHIFT, CTRL
				xkb_state_update_key(keyboard_state, keycode, XKB_KEY_DOWN);

				// Keycode + state = unicode
				char name_buffer[2]; // 1 character + \0
				xkb_state_key_get_utf8(keyboard_state, keycode, name_buffer,
					2);
				//printf("Keycode %d, Unicode %s\n", keycode, name_buffer);

				// Pass to main.c
				ev->type = ET_KEYPRESS;
				ev->kp_key = name_buffer[0];
				found_event = true;
				break;
			}

			case XCB_KEY_RELEASE: {
				// Updates state; this struct tracks things like SHIFT, CTRL
				uint8_t keycode =((xcb_key_press_event_t*)event)->detail;
				xkb_state_update_key(keyboard_state, keycode, XKB_KEY_UP);
				break;
			}
			
			// Ignore all other events
			default:
				found_event = false;
				break;
		}
		free(event);
	}
	return found_event;
}

void os_draw_rect(int x, int y, int w, int h, const float* rgb, int color) {
	xcb_rectangle_t pix_rect[] = {{x, y, w, h}};
	xcb_poly_fill_rectangle(connection, window, xcb_colors[color], 1, pix_rect);
}

void os_present(void) {
	xcb_flush(connection);
}

void os_close() {
	if (xcb_colors) free(xcb_colors);
}
