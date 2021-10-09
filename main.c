#include <xcb/xcb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

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
#define DEBUG_COREDUMP 1
#define DEBUG_COREDUMP_START SCREEN_START
#define DEBUG_COREDUMP_END SCREEN_START + 32
#define DEBUG_STEP 0
#define DEBUG_LOG 0

// Widely used structs
struct sim_state { uint16_t *pc; uint8_t *ac; uint8_t *x; uint8_t *y;
	uint8_t *sr; uint8_t *sp; uint8_t* mem; bool *halt; bool *no_pc_inc; };

// Get nanoseconds
unsigned long long get_clock_ns()
{
	struct timespec ts;
	if (clock_gettime (CLOCK_MONOTONIC, &ts) == 0)
		return (unsigned long long)(ts.tv_sec * 1000000000 + ts.tv_nsec);
	else return 0;
}

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
void fill_color_gcs(xcb_connection_t *c, xcb_window_t w, xcb_screen_t *s,
		xcb_gcontext_t *col) {
	col[0] = color_gc(c, w, s, 0, 0, 0);
	col[1] = color_gc(c, w, s, 1, 1, 1);
	col[2] = color_gc(c, w, s, 1, 0, 0);
	col[3] = color_gc(c, w, s, 0, 1, 1);
	col[4] = color_gc(c, w, s, 1, 0, 1);
	col[5] = color_gc(c, w, s, 0, 1, 0);
	col[6] = color_gc(c, w, s, 0, 0, 1);
	col[7] = color_gc(c, w, s, 1, 1, 0);
	col[8] = color_gc(c, w, s, 1, 0.5, 0);
	col[9] = color_gc(c, w, s, 0.5, 0.25, 0.25);
	col[10] = color_gc(c, w, s, 1, 0.5, 0.5);
	col[11] = color_gc(c, w, s, 0.25, 0.25, 0.25);
	col[12] = color_gc(c, w, s, 0.5, 0.5, 0.5);
	col[13] = color_gc(c, w, s, 0.5, 1, 0.5);
	col[14] = color_gc(c, w, s, 0.5, 0.5, 1);
	col[15] = color_gc(c, w, s, 0.75, 0.75, 0.75);
}

// Load code into PC_START in memory
void load_code(char* fn, uint8_t* m) {
	FILE* fp;
	fp = fopen(fn, "rb");
	fread(m + PC_START, TOTAL_MEM - PC_START, 1, fp);
	fclose(fp);
}

// Write memory and registers to STDOUT for debug
void coredump(struct sim_state s) {
	if (!DEBUG_COREDUMP) return;
	for (int i = DEBUG_COREDUMP_START; i <= DEBUG_COREDUMP_END; i += 0xf) {
		printf("%04x: ", i);
		for (int j = i; j < i + 0xf; j++)
			printf("%02x ", s.mem[j]);
		puts("");
	}
	printf("PC:%04x, AC:%02x, X:%02x, Y:%02x\n", *s.pc, *s.ac, *s.x, *s.y);
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
#define INS_DEF(N) void ins_##N(uint16_t (*get)(struct sim_state), \
	void (*set)(uint8_t, struct sim_state), struct sim_state s)
INS_DEF(JMP) { *s.pc = (*get)(s); *s.no_pc_inc = true; }
INS_DEF(ADC) {
	uint16_t t = *s.ac + (*get)(s) + bit_get(*s.sr, 0);
	*s.sr = bit_set(*s.sr, 0, bit_get(t, 8)); // Carry
	int v = !(bit_get(*s.ac, 7) ^ bit_get((*get)(s), 7)) && 
		bit_get(*s.ac, 7) ^ bit_get(t, 7);
	*s.sr = bit_set(*s.sr, 6, v); // Overflow
	*s.ac = sr_nz(s.sr, t);
}
INS_DEF(AND) { *s.ac = sr_nz(s.sr, *s.ac & (*get)(s)); }
INS_DEF(BRK) { *s.halt = true; }
INS_DEF(BNE) { if (!bit_get(*s.sr, 1)) { *s.pc = (*get)(s); } }
INS_DEF(CMP) {
	if (*s.ac < (*get)(s)) {
		*s.sr = bit_set(*s.sr, 1, 0);
		*s.sr = bit_set(*s.sr, 0, 0);
	}
	else if (*s.ac == (*get)(s)) {
		*s.sr = bit_set(*s.sr, 7, 0);
		*s.sr = bit_set(*s.sr, 1, 1);
		*s.sr = bit_set(*s.sr, 0, 1);
	}
	else if (*s.ac > (*get)(s)) {
		*s.sr = bit_set(*s.sr, 1, 0);
		*s.sr = bit_set(*s.sr, 0, 1);
	}
}
INS_DEF(DEC) { (*set)(sr_nz(s.sr, (*get)(s) - 1), s); }
INS_DEF(DEY) { *s.y = sr_nz(s.sr, *s.y - 1); }
INS_DEF(INC) { (*set)(sr_nz(s.sr, (*get)(s) + 1), s); }
INS_DEF(INX) { *s.x = sr_nz(s.sr, *s.x + 1); }
INS_DEF(LDA) { *s.ac = sr_nz(s.sr, (*get)(s)); }
INS_DEF(LDX) { *s.x = sr_nz(s.sr, (*get)(s)); }
INS_DEF(LDY) { *s.y = sr_nz(s.sr, (*get)(s)); }
INS_DEF(STA) { (*set)(*s.ac, s); }
INS_DEF(STX) { (*set)(*s.x, s); }

// Address modes
uint16_t i8to16(uint8_t h, uint8_t l) { return (uint16_t)h << 8 | l; }
struct addr { uint16_t (*get)(struct sim_state); 
	void (*set)(uint8_t, struct sim_state); };
#define ADDR_DEF(N, GET, SET) \
	uint16_t addr_get_##N(struct sim_state s) { GET } \
	void addr_set_##N(uint8_t a, struct sim_state s) { SET } \
	struct addr addr_##N = { .get = addr_get_##N, .set = addr_set_##N };
ADDR_DEF(abs, return s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])];,
	s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])] = a;);
ADDR_DEF(abs_dir, return i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1]);, );
ADDR_DEF(abs_x,
	return s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])
		+ *s.x + bit_get(*s.sr, 0)];,
	s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])
		+ *s.x + bit_get(*s.sr, 0)] = a;);
ADDR_DEF(imm, return s.mem[*s.pc + 1];, );
ADDR_DEF(x_ind,
	uint8_t zp_x = s.mem[*s.pc + 1] + *s.x;
	return s.mem[i8to16(s.mem[zp_x + 1], s.mem[zp_x ])];,
	uint8_t zp_x = s.mem[*s.pc + 1] + *s.x;
	s.mem[i8to16(s.mem[zp_x + 1], s.mem[zp_x])] = a;);
ADDR_DEF(impl, return 0;, );
ADDR_DEF(rel, return *s.pc + (int8_t)s.mem[*s.pc + 1];, );
ADDR_DEF(zpg, return s.mem[s.mem[*s.pc + 1]];, s.mem[s.mem[*s.pc + 1]] = a;);

// Opcodes
struct opcode {
	void (*instruction)(uint16_t (*get)(struct sim_state), 
		void (*set)(uint8_t, struct sim_state), struct sim_state s);
	struct addr *addr_mode;
};
void construct_opcodes_table(struct opcode *o) {
	o[0x00] = (struct opcode){ ins_BRK, &addr_impl };
	//o[0x60] = (struct opcode){ ins_RTS, &addr_impl };
	o[0xA0] = (struct opcode){ ins_LDY, &addr_imm };
	o[0xD0] = (struct opcode){ ins_BNE, &addr_rel };
	o[0x81] = (struct opcode){ ins_STA, &addr_x_ind };
	o[0xA2] = (struct opcode){ ins_LDX, &addr_imm };
	o[0x85] = (struct opcode){ ins_STA, &addr_zpg };
	o[0xA5] = (struct opcode){ ins_LDA, &addr_zpg };
	o[0x86] = (struct opcode){ ins_STX, &addr_zpg };
	o[0xA6] = (struct opcode){ ins_LDX, &addr_zpg };
	o[0xC6] = (struct opcode){ ins_DEC, &addr_zpg };
	o[0xE6] = (struct opcode){ ins_INC, &addr_zpg };
	o[0x88] = (struct opcode){ ins_DEY, &addr_impl };
	o[0xE8] = (struct opcode){ ins_INX, &addr_impl };
	o[0x29] = (struct opcode){ ins_AND, &addr_imm };
	o[0xA9] = (struct opcode){ ins_LDA, &addr_imm };
	o[0xC9] = (struct opcode){ ins_CMP, &addr_imm };
	o[0x4C] = (struct opcode){ ins_JMP, &addr_abs_dir };
	o[0x8D] = (struct opcode){ ins_STA, &addr_abs };
	o[0x9D] = (struct opcode){ ins_STA, &addr_abs_x };
}

// Step current PC
void sim_step(struct opcode *ops, struct sim_state s) {
	if (*s.halt) return;
	
	// Get opcode
	uint8_t op = s.mem[*s.pc];
	struct opcode decoded = ops[op];
	
	// Get instruction length
	int length;
	if (decoded.addr_mode == &addr_abs) length = 3;
	else if (decoded.addr_mode == &addr_abs_dir) length = 3;
	else if (decoded.addr_mode == &addr_abs_x) length = 3;
	else if (decoded.addr_mode == &addr_imm) length = 2;
	else if (decoded.addr_mode == &addr_x_ind) length = 2;
	else if (decoded.addr_mode == &addr_impl) length = 1;
	else if (decoded.addr_mode == &addr_rel) length = 2;
	else if (decoded.addr_mode == &addr_zpg) length = 2;
	else {
		length = 1;
		printf("Invalid addr mode %p for length\n", decoded.addr_mode);
	}

	// Step
	if (DEBUG_LOG) {
		printf("Stepping %04x: ", *s.pc);
		for (int i = 0; i < length; i++)
			printf("%02x ", s.mem[*s.pc + i]);
		puts("");
	}
	if (decoded.instruction)
		decoded.instruction(decoded.addr_mode->get, decoded.addr_mode->set, s);
	else printf("Invalid opcode %02x\n", op);
	if (DEBUG_LOG && *s.halt) puts("Halted.");
	if (!*s.no_pc_inc) *s.pc += length;
	*s.no_pc_inc = false;

	// Random $FE
	s.mem[0xFE] = rand();
}

// Render dirty pixels to X, unless full redraw is needed
void sim_render(uint8_t *mem, uint8_t *old_screen, xcb_connection_t *c,
	xcb_window_t w, xcb_gcontext_t *colors, bool full_redraw) {
	bool dirty = false;
	for (int i = 0; i < SCREEN_LENGTH; i++) {
		// Check dirty
		uint8_t new_pix = mem[SCREEN_START + i];
		if (!full_redraw && old_screen[i] == new_pix) continue;
		dirty = true;
		old_screen[i] = new_pix;

		// Get details
		int x = i % SCREEN_WIDTH;
		int y = i / SCREEN_WIDTH;
		int color = new_pix & 0xf; // 0x0 to 0xf colors only

		// Render
		xcb_rectangle_t pix_rect[] = 
			{{x * PIXEL_SIZE, y * PIXEL_SIZE,
			PIXEL_SIZE, PIXEL_SIZE}};
		xcb_poly_fill_rectangle(c, w, colors[color], 1, pix_rect);
	}
	if (dirty) xcb_flush(c);
}

int main() {
	// Init X and colors
	xcb_connection_t *connection;
	xcb_screen_t *screen;
	create_connection(&connection, &screen);
	xcb_window_t window = create_window(connection, screen);
	xcb_gcontext_t colors[16];
	fill_color_gcs(connection, window, screen, colors);
	
	// Make delete window atom to check for closing window
	xcb_intern_atom_reply_t *protocol_rep = xcb_intern_atom_reply(
		connection, xcb_intern_atom(connection, 1, 12, "WM_PROTOCOLS"), 0);
	xcb_intern_atom_reply_t *del_win_rep = xcb_intern_atom_reply(
		connection, xcb_intern_atom(connection, 0, 16, "WM_DELETE_WINDOW"), 0);
	xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
		(*protocol_rep).atom, 4, 32, 1, &(*del_win_rep).atom);

	// Init sim
	uint8_t mem[TOTAL_MEM] = {0};
	uint8_t old_screen[SCREEN_LENGTH] = {0};
	uint16_t reg_pc = PC_START;
	uint8_t reg_ac = 0;
	uint8_t reg_x = 0;
	uint8_t reg_y = 0;
	uint8_t reg_sr = 0;
	uint8_t reg_sp = 0;
	bool halt = false; // Is the sim halted? (Pauses the sim if true)
	bool no_pc_inc = false; // Hack to let opcodes tell sim not to inc pc once
	struct sim_state sim_state = { .pc = &reg_pc, .ac = &reg_ac, .x = &reg_x,
		.y = &reg_y, .sr = &reg_sr, .sp = &reg_sp, .mem = mem,
		.halt = &halt, .no_pc_inc = &no_pc_inc };
	struct opcode opcodes[0x100] = {0};
	construct_opcodes_table(opcodes);
	load_code("demos/backforth.bin", mem);

	// Test sim
	/*mem[SCREEN_START] = 0x5;
	mem[SCREEN_START + 10] = 0x3;
	mem[SCREEN_START + SCREEN_WIDTH * 10] = 0x8;*/

	// Main loop
	bool running = true; // Is the entire app running? (Will quit if false)
	unsigned long long prev_time = 0; // For rendering
	unsigned long long init_time = get_clock_ns(); // For delayed start
	bool started = false;
	unsigned long long start_time = 0; // For avg speed
	unsigned long long ins_count = 0; // For avg speed
	bool avg_speed_done = false;
	while (running) {
		// Handle events
		xcb_generic_event_t *event = xcb_poll_for_event(connection);
		if (event) {
			printf("Event %d\n", event->response_type);
			switch (event->response_type & ~0x80) {
				case XCB_CLIENT_MESSAGE: {
					if (((xcb_client_message_event_t*)event)->data.data32[0]
						== del_win_rep->atom) {
						puts("Exiting...");
						running = false;
					}
					break;
				}
				case XCB_EXPOSE: {
					// Render entire sim if window has to be redrawn
					sim_render(mem, old_screen, connection, window, colors, 
						true);
					break;
				}
				default: break;
			}
			free(event);
		}

		// Delayed start
		if (get_clock_ns() - init_time > 500000000 && !started) {
			started = true;
			start_time = get_clock_ns();
		}
		if (started) {
			// Update sim
			sim_step(opcodes, sim_state);
			ins_count++;

			// Debug step
			if (DEBUG_STEP) {
				char cmd = getchar();
				if (cmd == 'c') coredump(sim_state);
			}
		}
		
		// Calculate average speed
		if (!avg_speed_done && (halt || !running)) {
			avg_speed_done = true;
			unsigned long long diff = get_clock_ns() - start_time;
			double diff_s = (double)diff / 1000000000;
			double avg_speed = (double)ins_count / diff_s;
			printf("Processed %llu instructions in %f seconds.\n"
				"Average speed: %f Mhz.\n", ins_count, diff_s,
				avg_speed / 1000000);
		}

		// Render sim 60fps
		unsigned long long new_time = get_clock_ns();
		if (new_time - prev_time > 16666666/*|| new_time - prev_time < 0*/) {
			prev_time = new_time; // Record frame time for next frame
			sim_render(mem, old_screen, connection, window, colors, false);
		}
	}

	return 0;
}
