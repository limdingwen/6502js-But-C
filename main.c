#include <xcb/xcb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

/* RESERVED MEMORY BLOCKS
*  0x0100 to 0x01FF for stack (top to bottom)
*  0x0200 to 0x05FF for screen
*  0x0800 onwards for code
*/

// Config
#define TOTAL_MEM 65536
#define PC_START 0x0600
#define STACK_TOP 0x01FF
#define SCREEN_START 0x0200
#define SCREEN_LENGTH 0x0400
#define SCREEN_WIDTH 0x0020
#define SCREEN_HEIGHT 0x0020
#define PIXEL_SIZE 10
#define FRAME_INTERVAL 16666666
#define LIMIT_ENABLE 1
#define LIMIT_KHZ 10
#define START_DELAY 500000000
#define DEBUG_COREDUMP 1
#define DEBUG_COREDUMP_START 0x0100
#define DEBUG_COREDUMP_END 0x01FF
#define DEBUG_STEP 0
#define DEBUG_LOG 1
#define HALT_ON_INVALID 1

// Config colors
// Must change rendering " & 0xf" code if changing color count!
#define COLOR_COUNT 16
const float colors_rgb[] = {
	0.00, 0.00, 0.00,
	1.00, 1.00, 1.00,
	1.00, 0.00, 0.00,
	0.00, 1.00, 1.00,
	1.00, 0.00, 1.00,
	0.00, 1.00, 0.00,
	0.00, 0.00, 1.00,
	1.00, 1.00, 0.00,
	1.00, 0.50, 0.00,
	0.50, 0.25, 0.25,
	1.00, 0.50, 0.50,
	0.25, 0.25, 0.25,
	0.50, 0.50, 0.50,
	0.50, 1.00, 0.50,
	0.50, 0.50, 1.00,
	0.75, 0.75, 0.75
};

// Get nanoseconds
unsigned long long get_clock_ns() {
	struct timespec ts;
	if (clock_gettime (CLOCK_MONOTONIC, &ts) == 0)
		return (unsigned long long)(ts.tv_sec * 1000000000 + ts.tv_nsec);
	else return 0;
}

// Widely used sim state
struct sim_state { uint16_t *pc; uint8_t *ac; uint8_t *x; uint8_t *y;
	uint8_t *sr; uint8_t *sp; uint8_t* mem; bool *halt; bool *no_pc_inc; };

// Write memory and registers to STDOUT for debug
void coredump(struct sim_state s) {
	if (!DEBUG_COREDUMP) return;
	for (int i = DEBUG_COREDUMP_START; i <= DEBUG_COREDUMP_END; i += 0x10) {
		printf("%04x: ", i);
		for (int j = i; j < i + 0x10; j++)
			printf("%02x ", s.mem[j]);
		puts("");
	}
	printf("PC:%04x, AC:%02x, X:%02x, Y:%02x, SP:%02x\n",
		*s.pc, *s.ac, *s.x, *s.y, *s.sp);
}

// Datatype converters 
uint16_t ftoi16(float f) { return (uint16_t) (f * 65535); }
uint16_t i8to16(uint8_t h, uint8_t l) { return (uint16_t)h << 8 | l; }

// Bit operations
uint8_t bit_set(uint8_t a, int n, int x) {
	return (a & ~(1UL << n)) | (x << n);
}
int bit_get(uint8_t a, int n) { return a >> n & 1; }

// Instruction helpers
uint8_t sr_nz(uint8_t *sr, uint8_t a) { // TODO: Make functional(?)
	*sr = bit_set(*sr, 1, a == 0); // Zero
	*sr = bit_set(*sr, 7, bit_get(a, 7)); // Negative
	return a;
}
void push(uint8_t *mem, uint8_t *sp, uint8_t a) {
	mem[(*sp)-- + 0x0100] = a;
}
uint8_t pop(uint8_t *mem, uint8_t *sp) { return mem[++(*sp) + 0x0100]; }
void cmp(struct sim_state s, uint8_t a, uint8_t g) {
	if (a < g) {
		*s.sr = bit_set(*s.sr, 1, 0);
		*s.sr = bit_set(*s.sr, 0, 0);
	}
	else if (a == g) {
		*s.sr = bit_set(*s.sr, 7, 0);
		*s.sr = bit_set(*s.sr, 1, 1);
		*s.sr = bit_set(*s.sr, 0, 1);
	}
	else if (a > g) {
		*s.sr = bit_set(*s.sr, 1, 0);
		*s.sr = bit_set(*s.sr, 0, 1);
	}
}

// Instructions
// TODO: Inline addr code into main loop; pass in get and set flag+value only
#define INS_DEF(N) void ins_##N(uint16_t (*get)(struct sim_state), \
	void (*set)(uint8_t, struct sim_state), struct sim_state s)
INS_DEF(JMP) { *s.pc = (*get)(s); *s.no_pc_inc = true; }
INS_DEF(ADC) {
	uint16_t t = *s.ac + (*get)(s) + bit_get(*s.sr, 0);
	*s.sr = bit_set(*s.sr, 0, bit_get(t, 8)); // Carry
	bool v = !(bit_get(*s.ac, 7) ^ bit_get((*get)(s), 7)) && // Same sign? 
		bit_get(*s.ac, 7) ^ bit_get(t, 7); // Different sign for result?
	*s.sr = bit_set(*s.sr, 6, v); // Overflow
	*s.ac = sr_nz(s.sr, t);
}
INS_DEF(AND) { *s.ac = sr_nz(s.sr, *s.ac & (*get)(s)); }
INS_DEF(ASL) {
	*s.sr = bit_set(*s.sr, 0, bit_get((*get)(s), 7));
	(*set)(sr_nz(s.sr, (*get)(s) << 1), s);
}
INS_DEF(BRK) { *s.halt = true; }
INS_DEF(BEQ) { if (bit_get(*s.sr, 1)) { *s.pc = (*get)(s); } }
INS_DEF(BMI) { if (bit_get(*s.sr, 7)) { *s.pc = (*get)(s); } }
INS_DEF(BNE) { if (!bit_get(*s.sr, 1)) { *s.pc = (*get)(s); } }
INS_DEF(BPL) { if (!bit_get(*s.sr, 7)) { *s.pc = (*get)(s); } }
INS_DEF(CLC) { *s.sr = bit_set(*s.sr, 0, 0); }
INS_DEF(CMP) { cmp(s, *s.ac, (*get)(s)); }
INS_DEF(CPX) { cmp(s, *s.x, (*get)(s)); }
INS_DEF(CPY) { cmp(s, *s.y, (*get)(s)); }
INS_DEF(DEC) { (*set)(sr_nz(s.sr, (*get)(s) - 1), s); }
INS_DEF(DEX) { *s.x = sr_nz(s.sr, *s.x - 1); }
INS_DEF(DEY) { *s.y = sr_nz(s.sr, *s.y - 1); }
INS_DEF(INC) { (*set)(sr_nz(s.sr, (*get)(s) + 1), s); }
INS_DEF(INX) { *s.x = sr_nz(s.sr, *s.x + 1); }
INS_DEF(INY) { *s.y = sr_nz(s.sr, *s.y + 1); }
INS_DEF(JSR) {
	uint16_t ret_addr = *s.pc + 3; // Differs from real 6502
	push(s.mem, s.sp, ret_addr >> 8); // Push ret_h
	push(s.mem, s.sp, ret_addr & 0xff); // Push ret_l
	*s.pc = i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1]);
	*s.no_pc_inc = true;
}
INS_DEF(LDA) { *s.ac = sr_nz(s.sr, (*get)(s)); }
INS_DEF(LDX) { *s.x = sr_nz(s.sr, (*get)(s)); }
INS_DEF(LDY) { *s.y = sr_nz(s.sr, (*get)(s)); }
INS_DEF(LSR) {
	*s.sr = bit_set(*s.sr, 0, bit_get((*get)(s), 0));
	(*set)(sr_nz(s.sr, (*get)(s) >> 1), s);
}
INS_DEF(ORA) { *s.ac = sr_nz(s.sr, (*get)(s) | *s.ac); }
INS_DEF(PHA) { push(s.mem, s.sp, *s.ac); }
INS_DEF(PLA) { *s.ac = pop(s.mem, s.sp); }
INS_DEF(RTS) {
	uint8_t ret_l = pop(s.mem, s.sp);
	uint8_t ret_h = pop(s.mem, s.sp);
	*s.pc = i8to16(ret_h, ret_l);
	*s.no_pc_inc = true;
} 
INS_DEF(SBC) {
	// 2's complement, where the extra 1 is from carry
	// If carry is 0, then we effectively subtract 1 more
	uint8_t b = ~(*get)(s) + bit_get(*s.sr, 0);
	uint16_t t = *s.ac + b;
	*s.sr = bit_set(*s.sr, 0, bit_get(t, 8)); // Carry
	bool v = !(bit_get(*s.ac, 7) ^ bit_get((*get)(s), 7)) && // Same sign? 
		bit_get(*s.ac, 7) ^ bit_get(t, 7); // Different sign for result?
	*s.sr = bit_set(*s.sr, 6, v); // Overflow
	*s.ac = sr_nz(s.sr, t);
}
INS_DEF(STA) { (*set)(*s.ac, s); }
INS_DEF(STX) { (*set)(*s.x, s); }
INS_DEF(STY) { (*set)(*s.y, s); }
INS_DEF(TAX) { *s.x = sr_nz(s.sr, *s.ac); }
INS_DEF(TAY) { *s.y = sr_nz(s.sr, *s.ac); }
INS_DEF(TXA) { *s.ac = sr_nz(s.sr, *s.x); }
INS_DEF(TYA) { *s.ac = sr_nz(s.sr, *s.y); }
#undef INS_DEF

// Address modes
struct addr { uint16_t (*get)(struct sim_state); 
	void (*set)(uint8_t, struct sim_state); int length; };
#define ADDR_DEF(N, LEN, GET, SET) \
	uint16_t addr_get_##N(struct sim_state s) { GET } \
	void addr_set_##N(uint8_t a, struct sim_state s) { SET } \
	const struct addr addr_##N = { .get = addr_get_##N, .set = addr_set_##N, \
	.length = LEN };
ADDR_DEF(ac, 1, return *s.ac;, *s.ac = a;);
ADDR_DEF(abs, 3, return s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])];,
	s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])] = a;);
ADDR_DEF(abs_dir, 3, return i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1]);, );
ADDR_DEF(abs_x, 3,
	return s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])
		+ *s.x + bit_get(*s.sr, 0)];,
	s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])
		+ *s.x + bit_get(*s.sr, 0)] = a;);
ADDR_DEF(imm, 2, return s.mem[*s.pc + 1];, );
ADDR_DEF(x_ind, 2,
	uint8_t zp_x = s.mem[*s.pc + 1] + *s.x;
	return s.mem[i8to16(s.mem[zp_x + 1], s.mem[zp_x])];,
	uint8_t zp_x = s.mem[*s.pc + 1] + *s.x;
	s.mem[i8to16(s.mem[zp_x + 1], s.mem[zp_x])] = a;);
ADDR_DEF(ind_y, 2,
	uint8_t zp = s.mem[*s.pc + 1];
	return s.mem[i8to16(s.mem[zp + 1], s.mem[zp]) + *s.y + bit_get(*s.sr, 0)];,
	uint8_t zp = s.mem[*s.pc + 1];
	s.mem[i8to16(s.mem[zp + 1], s.mem[zp]) + *s.y + bit_get(*s.sr, 0)] = a;);
ADDR_DEF(impl, 1, return 0;, );
ADDR_DEF(rel, 2, return *s.pc + (int8_t)s.mem[*s.pc + 1];, );
ADDR_DEF(zpg, 2,
	return s.mem[s.mem[*s.pc + 1]];, s.mem[s.mem[*s.pc + 1]] = a;);
ADDR_DEF(zpg_x, 2,
	return s.mem[s.mem[*s.pc + 1] + *s.x];,
	s.mem[s.mem[*s.pc + 1] + *s.x] = a;);
#undef ADDR_DEF

// Opcodes
struct opcode {
	void (*instruction)(uint16_t (*get)(struct sim_state), 
		void (*set)(uint8_t, struct sim_state), struct sim_state s);
	const struct addr *addr_mode;
};
void construct_opcodes_table(struct opcode *o) {
	// -0
	o[0x00] = (struct opcode){ ins_BRK, &addr_impl };
	o[0x10] = (struct opcode){ ins_BPL, &addr_rel };
	o[0x20] = (struct opcode){ ins_JSR, &addr_abs_dir };
	o[0x30] = (struct opcode){ ins_BPL, &addr_rel };
	o[0x60] = (struct opcode){ ins_RTS, &addr_impl };
	o[0xA0] = (struct opcode){ ins_LDY, &addr_imm };
	o[0xC0] = (struct opcode){ ins_CPY, &addr_imm };
	o[0xD0] = (struct opcode){ ins_BNE, &addr_rel };
	o[0xE0] = (struct opcode){ ins_CPX, &addr_imm };
	o[0xF0] = (struct opcode){ ins_BEQ, &addr_rel };
	// -1
	o[0x81] = (struct opcode){ ins_STA, &addr_x_ind };
	o[0x91] = (struct opcode){ ins_STA, &addr_ind_y };
	o[0xB1] = (struct opcode){ ins_LDA, &addr_ind_y };
	// -2
	o[0xA2] = (struct opcode){ ins_LDX, &addr_imm };
	// -4
	o[0x84] = (struct opcode){ ins_STY, &addr_zpg };
	// -5
	o[0x05] = (struct opcode){ ins_ORA, &addr_zpg };
	o[0x65] = (struct opcode){ ins_ADC, &addr_zpg };
	o[0x85] = (struct opcode){ ins_STA, &addr_zpg };
	o[0x95] = (struct opcode){ ins_STA, &addr_zpg_x };
	o[0xA5] = (struct opcode){ ins_LDA, &addr_abs };
	o[0xB5] = (struct opcode){ ins_LDA, &addr_zpg_x };
	o[0xF5] = (struct opcode){ ins_SBC, &addr_zpg_x };
	// -6
	o[0x86] = (struct opcode){ ins_STX, &addr_zpg };
	o[0xA6] = (struct opcode){ ins_LDX, &addr_zpg };
	o[0xC6] = (struct opcode){ ins_DEC, &addr_zpg };
	o[0xE6] = (struct opcode){ ins_INC, &addr_zpg };
	// -8
	o[0x18] = (struct opcode){ ins_CLC, &addr_impl };
	o[0x48] = (struct opcode){ ins_PHA, &addr_impl };
	o[0x68] = (struct opcode){ ins_PLA, &addr_impl };
	o[0x88] = (struct opcode){ ins_DEY, &addr_impl };
	o[0x98] = (struct opcode){ ins_TYA, &addr_impl };
	o[0xA8] = (struct opcode){ ins_TAY, &addr_impl };
	o[0xC8] = (struct opcode){ ins_INY, &addr_impl };
	o[0xE8] = (struct opcode){ ins_INX, &addr_impl };
	// -9
	o[0x29] = (struct opcode){ ins_AND, &addr_imm };
	o[0x69] = (struct opcode){ ins_ADC, &addr_imm };
	o[0xA9] = (struct opcode){ ins_LDA, &addr_imm };
	o[0xC9] = (struct opcode){ ins_CMP, &addr_imm };
	o[0xCD] = (struct opcode){ ins_CMP, &addr_abs };
	// -A
	o[0x0A] = (struct opcode){ ins_ASL, &addr_ac };
	o[0x4A] = (struct opcode){ ins_LSR, &addr_ac };
	o[0x8A] = (struct opcode){ ins_TXA, &addr_impl };
	o[0xAA] = (struct opcode){ ins_TAX, &addr_impl };
	o[0xCA] = (struct opcode){ ins_DEX, &addr_impl };
	// -C
	o[0x4C] = (struct opcode){ ins_JMP, &addr_abs_dir };
	o[0x8C] = (struct opcode){ ins_STY, &addr_abs };
	o[0xAC] = (struct opcode){ ins_LDY, &addr_abs };
	// -D
	o[0x0D] = (struct opcode){ ins_ORA, &addr_abs };
	o[0x8D] = (struct opcode){ ins_STA, &addr_abs };
	o[0x9D] = (struct opcode){ ins_STA, &addr_abs_x };
	o[0xAD] = (struct opcode){ ins_LDA, &addr_abs };
	o[0xBD] = (struct opcode){ ins_LDA, &addr_abs_x };
	// -E
	o[0x8E] = (struct opcode){ ins_STX, &addr_abs };
	o[0xAE] = (struct opcode){ ins_LDX, &addr_abs };
}

int main(int argc, char** argv) {
	// Seed random
	srand(time(NULL));

	// Handle command line
	if (argc != 2) {
		puts("Usage: 6502 file.bin");
		return 0;
	}

	// =====
	// INIT X
	// =====
	
	// Connect to X
	xcb_connection_t *connection;
	xcb_screen_t *screen;
	connection = xcb_connect(NULL, NULL);
	screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;

	// Create and show window
	xcb_window_t window = xcb_generate_id(connection);
	{
		uint32_t masks = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
		uint32_t values[] = {
			screen->black_pixel,
			XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS
		};
		xcb_create_window(connection, XCB_COPY_FROM_PARENT, window,
			screen->root, 0, 0, SCREEN_WIDTH * PIXEL_SIZE,
			SCREEN_HEIGHT * PIXEL_SIZE, 10, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			screen->root_visual, masks, values);
	}
	xcb_map_window(connection, window);
	xcb_flush(connection);
	
	// Register for "delete window" event from window manager
	xcb_intern_atom_reply_t *protocol_rep = xcb_intern_atom_reply(
		connection, xcb_intern_atom(connection, 1, 12, "WM_PROTOCOLS"), 0);
	xcb_intern_atom_reply_t *del_win_rep = xcb_intern_atom_reply(
		connection, xcb_intern_atom(connection, 0, 16, "WM_DELETE_WINDOW"), 0);
	xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
		(*protocol_rep).atom, 4, 32, 1, &(*del_win_rep).atom);

	// Create all 16 colors
	xcb_gcontext_t colors[COLOR_COUNT];
	for (int i = 0; i < COLOR_COUNT; i++) {
		// Get color from list
		float r = colors_rgb[i * 3 + 0];
		float g = colors_rgb[i * 3 + 1];
		float b = colors_rgb[i * 3 + 2];

		// Get color from colormap
		xcb_alloc_color_reply_t *rep = xcb_alloc_color_reply(connection,
			xcb_alloc_color(connection, screen->default_colormap,
				ftoi16(r), ftoi16(g), ftoi16(b)),
			NULL);

		// Create context from color
		xcb_gcontext_t gc = xcb_generate_id(connection);
		uint32_t value[] = { rep->pixel };
		xcb_create_gc(connection, gc, window, XCB_GC_FOREGROUND, value);
		colors[i] = gc;
		free(rep);
	}

	// =====
	// INIT SIM
	// =====
	
	// Init registers and memory
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
	
	// Init opcodes
	struct opcode opcodes[0x100] = {0};
	construct_opcodes_table(opcodes);

	// Load binary into memory
	{
		FILE* fp;
		fp = fopen(argv[1], "rb");
		if (!fp) {
			puts("Cannot read input binary file.");
			return -1;
		}
		fread(mem + PC_START, TOTAL_MEM - PC_START, 1, fp);
		fclose(fp);
	}

	// =====
	// INIT MISC
	// =====

	// Init main loop
	bool running = true; // Is the entire app running? (Will quit if false)

	// Init rendering
	unsigned long long prev_frame_time = 0;
	bool full_redraw = false;

	// Init delayed start
	unsigned long long init_time = get_clock_ns();
	bool started = false;

	// Init average speed
	unsigned long long start_time = 0;
	unsigned long long ins_count = 0;
	bool avg_speed_done = false;

	// Init speed limiting
	unsigned long long prev_limit_time = get_clock_ns();
	unsigned long long limit_interval_ns =
		(1 / ((float)LIMIT_KHZ * 1000)) * 1000000000;

	// =====
	// START MAIN LOOP
	// =====
	
	while (running) {
		// =====
		// HANDLE EVENTS
		// =====

		xcb_generic_event_t *event = xcb_poll_for_event(connection);
		if (event) {
			printf("Event %d\n", event->response_type);
			switch (event->response_type & ~0x80) {
				// Detect exiting
				case XCB_CLIENT_MESSAGE:
					if (((xcb_client_message_event_t*)event)->data.data32[0]
						== del_win_rep->atom) {
						puts("Exiting...");
						running = false;
					}
					break;

				// Detect redraw required
				case XCB_EXPOSE: full_redraw = true; break;

				case XCB_KEY_PRESS: {
					// Hack just to try and get Adventure working
					uint8_t keycode =((xcb_key_press_event_t*)event)->detail;
					uint8_t ascii;
					switch (keycode) {
						case 21: ascii = 'w'; break;
						case 8: ascii = 'a'; break;
						case 9: ascii = 's'; break;
						case 10: ascii = 'd'; break;
					}
					mem[0xFF] = ascii;
					//printf("Key pressed: %d\n", ascii);
					break;
				}
				
				// Ignore all other events
				default: break;
			}
			free(event);
		}

		// =====
		// UPDATE
		// =====

		// Delayed start
		if (get_clock_ns() - init_time > START_DELAY && !started) {
			started = true;
			start_time = get_clock_ns(); // Start counting average speed
		}

		// Step sim
		if (started && !halt) {
			// Fetch and decode opcode
			uint8_t op = mem[reg_pc];
			struct opcode decoded = opcodes[op];
			
			// Get instruction length
			int length = 1;
			if (decoded.addr_mode) length = decoded.addr_mode->length;

			// Log instruction
			if (DEBUG_LOG) {
				printf("Stepping %04x: ", reg_pc);
				for (int i = 0; i < length; i++)
					printf("%02x ", mem[reg_pc + i]);
				puts("");
			}

			// Execute!
			if (decoded.instruction)
				decoded.instruction(decoded.addr_mode->get,
					decoded.addr_mode->set, sim_state);
			else {
				if (DEBUG_LOG) printf("Invalid opcode %02x\n", op);
				if (HALT_ON_INVALID) halt = true;
			}

			// Log halt
			if (DEBUG_LOG && halt) {
				coredump(sim_state);
				puts("Halted.");
			}

			// Increment PC unless instruction said not to, then reset for next
			if (!no_pc_inc) reg_pc += length;
			no_pc_inc = false;

			// Random $FE
			mem[0xFE] = rand();

			// Count instruction for average speed
			ins_count++;
		}

		// Let user step through instructions, or coredump
		if (DEBUG_STEP) {
			while (true) {
				char cmd = getchar();
				if (cmd == '\n') break;
				if (cmd == 'c') coredump(sim_state);
				if (cmd != '\n') getchar(); // Consume upcoming \n
			}
		}

		// Calculate average speed and print when either halted or quitting
		if (!avg_speed_done && (halt || !running)) {
			avg_speed_done = true;
			unsigned long long diff = get_clock_ns() - start_time;
			double diff_s = (double)diff / 1000000000;
			double avg_speed = (double)ins_count / diff_s;
			printf("Processed %llu instructions in %f seconds.\n"
				"Average speed: %f Mhz.\n", ins_count, diff_s,
				avg_speed / 1000000);
		}

		// =====
		// RENDER
		// =====

		// Re-render every X nanoseconds, or if redraw is required
		unsigned long long new_frame_time = get_clock_ns();
		if (new_frame_time - prev_frame_time > FRAME_INTERVAL || full_redraw) {
			prev_frame_time = new_frame_time;
			bool dirty = false;
			for (int i = 0; i < SCREEN_LENGTH; i++) {
				// Render only if dirty, or if redraw is required
				uint8_t new_pix = mem[SCREEN_START + i];
				if (old_screen[i] == new_pix && !full_redraw) continue;
				dirty = true;
				old_screen[i] = new_pix;

				// Get pixel details
				int x = i % SCREEN_WIDTH;
				int y = i / SCREEN_WIDTH;
				int color = new_pix & 0xf; // 0x0 to 0xf colors only

				// Render
				xcb_rectangle_t pix_rect[] = 
					{{x * PIXEL_SIZE, y * PIXEL_SIZE,
					PIXEL_SIZE, PIXEL_SIZE}};
				xcb_poly_fill_rectangle(connection, window,
					colors[color], 1, pix_rect);
			}
			if (dirty) xcb_flush(connection);
		}
		full_redraw = false;

		// =====
		// LIMIT
		// =====
		
		unsigned long long new_limit_time = get_clock_ns();
		long limit_time_diff_ns =
			limit_interval_ns - (new_limit_time - prev_limit_time);
		long limit_time_diff_us = limit_time_diff_ns / 1000;
		if (LIMIT_ENABLE && limit_time_diff_us > 0) usleep(limit_time_diff_us);
		prev_limit_time = get_clock_ns();
	}

	// =====
	// END MAIN LOOP
	// =====

	return 0;
}
