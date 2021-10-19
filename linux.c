#include "os.h"

#include <xcb/xcb.h>
#include <stdlib.h>
#include <stdio.h>

xcb_connection_t *connection = NULL;
xcb_screen_t *screen = NULL;
xcb_window_t window = 0; 
xcb_intern_atom_reply_t *del_win_rep = NULL;
bool should_exit = false;
xcb_gcontext_t *xcb_colors = NULL;

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
			XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS
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

bool os_choose_bin(char* path) {
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

			case XCB_KEY_PRESS: {
				// Hack just to try and get Adventure working
				uint8_t keycode =((xcb_key_press_event_t*)event)->detail;
				uint8_t ascii = 0;
				switch (keycode) {
					case 21: ascii = 'w'; break;
					case 8: ascii = 'a'; break;
					case 9: ascii = 's'; break;
					case 10: ascii = 'd'; break;
				}
				if (ascii) {
					ev->type = ET_KEYPRESS;
					ev->kp_key = ascii;
					found_event = true;
				}
				else {
					found_event = false;
				}
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
