#include <xcb/xcb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* RESERVED MEMORY BLOCKS
*  0x0100 to 0x01FF for stack
*  0x0200 to 0x05FF for screen
*  0x0800 onwards for code
*/

// Config
#define TOTAL_MEM 65536
#define PC_START 0x0800
#define SCREEN_START 0x0200
#define SCREEN_LENGTH 0x0400
#define SCREEN_WIDTH 0x0020
#define SCREEN_HEIGHT 0x0020
#define PIXEL_SIZE 20
#define TEST_BIN_NAME "demos/3colours.bin"
#define DEBUG_COREDUMP 1
#define DEBUG_COREDUMP_START PC_START
#define DEBUG_COREDUMP_END PC_START + 32

// Connect to X
void create_connection(xcb_connection_t **c, xcb_screen_t **s) {
	*c = xcb_connect(NULL, NULL);
	*s = xcb_setup_roots_iterator(xcb_get_setup(*c)).data;
}

// Create X window
xcb_window_t create_window(xcb_connection_t *c, xcb_screen_t *s) {
	xcb_window_t w = xcb_generate_id(c);
	uint32_t m = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	uint32_t v[] = {
		s->black_pixel,
		XCB_EVENT_MASK_EXPOSURE
	};
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root,
		0, 0, SCREEN_WIDTH * PIXEL_SIZE, SCREEN_HEIGHT * PIXEL_SIZE, 10,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, m, v);
	xcb_map_window(c, w);
	xcb_flush(c);
	return w;
}

// Converts RGB float values to 16bit values
uint16_t ftoi16(float f) {
	return (uint16_t) (f * 65535);
}

// Creates a new context from RGB color
xcb_gcontext_t color_gc(xcb_connection_t *c, xcb_window_t w, xcb_screen_t *s,
		float r, float g, float b) {
	// Get new color
	xcb_alloc_color_reply_t *rep = xcb_alloc_color_reply(c,
		xcb_alloc_color(c, s->default_colormap,
			ftoi16(r), ftoi16(g), ftoi16(b)),
		NULL);

	// Create context from color
	xcb_gcontext_t gc = xcb_generate_id(c);
	uint32_t v[] = { rep->pixel };
	xcb_create_gc(c, gc, w, XCB_GC_FOREGROUND, v);
	free(rep);
	return gc;
}

// Create all 16 colors
#define COLOR_GC(I, R, G, B) col[I] = color_gc(c, w, s, R, G, B)
void fill_color_gcs(xcb_connection_t *c, xcb_window_t w, xcb_screen_t *s,
		xcb_gcontext_t *col) {
	COLOR_GC(0, 0, 0, 0);
	COLOR_GC(1, 1, 1, 1);
	COLOR_GC(2, 1, 0, 0);
	COLOR_GC(3, 0, 1, 1);
	COLOR_GC(4, 1, 0, 1);
	COLOR_GC(5, 0, 1, 0);
	COLOR_GC(6, 0, 0, 1);
	COLOR_GC(7, 1, 1, 0);
	COLOR_GC(8, 1, 0.5, 0);
	COLOR_GC(9, 0.5, 0.25, 0.25);
	COLOR_GC(10, 1, 0.5, 0.5);
	COLOR_GC(11, 0.25, 0.25, 0.25);
	COLOR_GC(12, 0.5, 0.5, 0.5);
	COLOR_GC(13, 0.5, 1, 0.5);
	COLOR_GC(14, 0.5, 0.5, 1);
	COLOR_GC(15, 0.75, 0.75, 0.75);
}

// Load code into PC_START in memory
void load_code(char* fn, uint8_t* m) {
	FILE* fp;
	fp = fopen(fn, "rb");
	fread(m + PC_START, TOTAL_MEM - PC_START, 1, fp);
	fclose(fp);
}

// Write memory to STDOUT for debug
void coredump(uint8_t* m) {
	if (!DEBUG_COREDUMP) return;
	for (int i = DEBUG_COREDUMP_START; i <= DEBUG_COREDUMP_END; i += 0xf) {
		printf("%04x: ", i);
		for (int j = i; j < i + 0xf; j++) {
			printf("%02x ", m[j]);
		}
		puts("");
	}
}

// Instruction helpers
uint8_t bit_set(uint8_t a, int n, int x) {
	return (a & ~(1UL << n)) | (x << n);
}
int bit_get(uint8_t a, int n) { return a >> n & 1; }
uint8_t sr_nz(uint8_t *sr, uint8_t a) {
	*sr = bit_set(*sr, 1, a == 0); // Zero
	*sr = bit_set(*sr, 7, bit_get(a, 7)); // Negative
	return a;
}

// Instructions
struct regs { uint16_t *pc; uint8_t *ac; uint8_t *x; uint8_t *y; uint8_t *sr;
	uint8_t *sp; uint8_t* mem; bool *halt; };
#define INS_DEF(N) void ins_##N(uint8_t (*get)(struct regs), \
	void (*set)(uint8_t, struct regs), struct regs r)
INS_DEF(ADC) {
	uint16_t t = *r.ac + (*get)(r) + bit_get(*r.sr, 0);
	*r.sr = bit_set(*r.sr, 0, bit_get(t, 8)); // Carry
	int v = !(bit_get(*r.ac, 7) ^ bit_get((*get)(r), 7)) && 
		bit_get(*r.ac, 7) ^ bit_get(t, 7);
	*r.sr = bit_set(*r.sr, 6, v); // Overflow
	*r.ac = sr_nz(r.sr, t);
}
INS_DEF(BRK) { *r.halt = true; }
INS_DEF(LDA) { *r.ac = sr_nz(r.sr, (*get)(r)); }
INS_DEF(STA) { (*set)(*r.ac, r); }

// Address modes
uint16_t i8to16(uint8_t h, uint8_t l) { return (uint16_t)h << 8 & l; }
struct addr { uint8_t (*get)(struct regs); 
	void (*set)(uint8_t, struct regs); };
#define ADDR_DEF(N, GET, SET) \
	uint8_t addr_get_##N(struct regs r) { GET } \
	void addr_set_##N(uint8_t a, struct regs r) { SET } \
	struct addr addr_##N = { .get = addr_get_##N, .set = addr_set_##N };
ADDR_DEF(abs, return r.mem[i8to16(r.mem[*r.pc + 2], r.mem[*r.pc + 1])];,
	r.mem[i8to16(r.mem[*r.pc + 2], r.mem[*r.pc + 1])] = a;);
ADDR_DEF(imm, return r.mem[*r.pc + 1];, );
ADDR_DEF(impl, return 0;, );

// Opcodes
struct opcode {
	void (*instruction)(uint8_t (*get)(struct regs), 
		void (*set)(uint8_t, struct regs), struct regs r);
	struct addr addr_mode;
};
#define OP_DEF(N, I, A) o[N] = (struct opcode){ ins_##I, addr_##A }
void construct_opcodes_table(struct opcode *o) {
	OP_DEF(0x00, BRK, impl);
	OP_DEF(0x8D, STA, abs);
	OP_DEF(0xA9, LDA, imm);
}

// Step current PC
void sim_step(struct regs) {
	
}

int main() {
	// Init X and colors
	xcb_connection_t *connection;
	xcb_screen_t *screen;
	create_connection(&connection, &screen);
	xcb_window_t window = create_window(connection, screen);
	// Make delete window atom to check for closing window (to extract)
	/*xcb_intern_atom_reply_t *del_win_rep = xcb_intern_atom_reply(
		connection,
		xcb_intern_atom(connection, false, 16, "WM_DELETE_WINDOW"),
		NULL);*/
	xcb_gcontext_t colors[16];
	fill_color_gcs(connection, window, screen, colors);

	// Init sim
	uint8_t mem[TOTAL_MEM] = {0};
	uint16_t reg_pc = 0;
	uint8_t reg_ac = 0;
	uint8_t reg_x = 0;
	uint8_t reg_y = 0;
	uint8_t reg_sr = 0;
	uint8_t reg_sp = 0;
	struct opcode opcodes[0x100] = {0};
	construct_opcodes_table(opcodes);
	load_code(TEST_BIN_NAME, mem);
	coredump(mem);

	// Test sim
	mem[SCREEN_START] = 0x5;
	mem[SCREEN_START + 10] = 0x3;
	mem[SCREEN_START + SCREEN_WIDTH * 10] = 0x8;

	// Main loop
	int running = true; // Is the entire app running? (Will quit if false)
	int halt = false; // Is the sim halted? (Only pauses the sim if true)
	while (running) {
		// Handle events
		xcb_generic_event_t *event = xcb_poll_for_event(connection);
		if (event) {
			printf("Event %d\n", event->response_type & ~0x80);
			switch (event->response_type & ~0x80) {
				/*case XCB_CLIENT_MESSAGE: {
					puts("Here");
					if (((xcb_client_message_event_t*)event)->data.data32[0]
						== del_win_rep->atom) {
						puts("Exiting...");
						running = false;
					}
					break;
				}*/
				case XCB_EXPOSE: {
					for (int i = 0; i < SCREEN_LENGTH; i++) {
						// Get details
						uint8_t pix = mem[SCREEN_START + i];
						int x = i % SCREEN_WIDTH;
						int y = i / SCREEN_WIDTH;
						int color = pix & 0xf; // 0x0 to 0xf colors only

						// Render
						xcb_rectangle_t pix_rect[] = 
							{{x * PIXEL_SIZE, y * PIXEL_SIZE,
							PIXEL_SIZE, PIXEL_SIZE}};
						xcb_poly_fill_rectangle(connection, window,
							colors[color], 1, pix_rect);
					}
					xcb_flush(connection);
					break;
				}
				default: break;
			}
			free(event);
		}

		// Update sim
	}

	return 0;
}
