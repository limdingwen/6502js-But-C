#include "os.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define PIXEL_SIZE 10 // How big is a fake pixel?
#define FRAME_INTERVAL 16666666 // In ns
#define LIMIT_ENABLE 1
#define LIMIT_KHZ 30
#define START_DELAY 500000000 // In ns
#define DEBUG_COREDUMP 1 // Coredumps on exit, also enables for step coredump
#define DEBUG_COREDUMP_START 0x0100
#define DEBUG_COREDUMP_END 0x01FF
#define DEBUG_STEP 0 // Allows user to step per cycle
#define DEBUG_BREAKPOINT 0 // Goes into stepping mode when breakpoint reached
#define DEBUG_BREAKPOINT_MODE 1 // 0 = addr, 1 = ins_count
#define DEBUG_BREAKPOINT_VALUE 822
#define DEBUG_LOG 1 // Logs all instructions processed
#define DEBUG_DIFFLOG 0 // Generates memory and reg changes, to compare & debug
#define DEBUG_DIFFLOG_FILE "difflog_mine.txt"
#define HALT_ON_INVALID 1

// Config colors
// Must change rendering " & 0xf" code if changing color count!
#define COLOR_COUNT 16
const float colors[] = {
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
unsigned long long get_clock_ns(void) {
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
	printf("PC:%04x, AC:%02x, X:%02x, Y:%02x, SP:%02x, SR:%02x\n",
		*s.pc, *s.ac, *s.x, *s.y, *s.sp, *s.sr);
}

// Prints a difflog line for registers
void print_difflog(FILE *fp, unsigned long long ins_count, uint8_t *mem,
		uint16_t pc, char* name, uint8_t old, uint8_t new) {
	fprintf(fp, "%llu: Ins %02x @ %04x, %s %02x -> %02x\n", ins_count, mem[pc],
		pc, name, old, new);
}

// Datatype converters 
uint16_t i8to16(uint8_t h, uint8_t l) { return (uint16_t)h << 8 | l; }

// Bit operations
uint8_t bit_set(uint8_t a, int n, int x) {
	return (a & ~(1UL << n)) | (x << n);
}
int bit_get(uint16_t a, int n) { return a >> n & 1; }

// Instruction helpers
uint8_t sr_nz(uint8_t *sr, uint8_t a) {
	*sr = bit_set(*sr, 1, a == 0); // Zero
	*sr = bit_set(*sr, 7, bit_get(a, 7)); // Negative
	return a;
}
void push(uint8_t *mem, uint8_t *sp, uint8_t a) {
	mem[(*sp)-- + 0x0100] = a;
	//*sp = *sp - 1;
}
uint8_t pop(uint8_t *mem, uint8_t *sp) { return mem[++(*sp) + 0x0100]; }
void cmp(struct sim_state s, uint8_t reg, uint8_t get) {
	uint8_t t = reg - get;
	if (reg < get) {
		*s.sr = bit_set(*s.sr, 7, bit_get(t, 7));
		*s.sr = bit_set(*s.sr, 1, 0);
		*s.sr = bit_set(*s.sr, 0, 0);
	}
	else if (reg == get) {
		*s.sr = bit_set(*s.sr, 7, 0);
		*s.sr = bit_set(*s.sr, 1, 1);
		*s.sr = bit_set(*s.sr, 0, 1); // NOTE: 6502asm differs here...?
	}
	else if (reg > get) {
		*s.sr = bit_set(*s.sr, 7, bit_get(t, 7));
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
INS_DEF(BCC) { if (!bit_get(*s.sr, 0)) { *s.pc = (*get)(s); } }
INS_DEF(BCS) { if (bit_get(*s.sr, 0)) { *s.pc = (*get)(s); } }
INS_DEF(BEQ) { if (bit_get(*s.sr, 1)) { *s.pc = (*get)(s); } }
INS_DEF(BMI) { if (bit_get(*s.sr, 7)) { *s.pc = (*get)(s); } }
INS_DEF(BNE) { if (!bit_get(*s.sr, 1)) { *s.pc = (*get)(s); } }
INS_DEF(BPL) { if (!bit_get(*s.sr, 7)) { *s.pc = (*get)(s); } }
INS_DEF(BVC) { if (!bit_get(*s.sr, 6)) { *s.pc = (*get)(s); } }
INS_DEF(BRK) { *s.halt = true; }
INS_DEF(CLC) { *s.sr = bit_set(*s.sr, 0, 0); }
INS_DEF(CMP) { cmp(s, *s.ac, (*get)(s)); }
INS_DEF(CPX) { cmp(s, *s.x, (*get)(s)); }
INS_DEF(CPY) { cmp(s, *s.y, (*get)(s)); }
INS_DEF(DEC) { (*set)(sr_nz(s.sr, (*get)(s) - 1), s); }
INS_DEF(DEX) { *s.x = sr_nz(s.sr, *s.x - 1); }
INS_DEF(DEY) { *s.y = sr_nz(s.sr, *s.y - 1); }
INS_DEF(EOR) { *s.ac = sr_nz(s.sr, (*get)(s) ^ *s.ac); }
INS_DEF(INC) { (*set)(sr_nz(s.sr, (*get)(s) + 1), s); }
INS_DEF(INX) { *s.x = sr_nz(s.sr, *s.x + 1); }
INS_DEF(INY) { *s.y = sr_nz(s.sr, *s.y + 1); }
INS_DEF(JSR) {
	uint16_t ret_addr = *s.pc + 2;
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
INS_DEF(NOP) { /* :D */ }
INS_DEF(ORA) { *s.ac = sr_nz(s.sr, (*get)(s) | *s.ac); }
INS_DEF(PHA) { push(s.mem, s.sp, *s.ac); }
INS_DEF(PLA) { *s.ac = sr_nz(s.sr, pop(s.mem, s.sp)); }
INS_DEF(ROL) {
	int old_c = bit_get(*s.sr, 0);
	*s.sr = bit_set(*s.sr, 0, bit_get((*get)(s), 7));
	(*set)(sr_nz(s.sr, (*get)(s) << 1 | old_c), s);
}
INS_DEF(ROR) {
	int old_c = bit_get(*s.sr, 0);
	*s.sr = bit_set(*s.sr, 0, bit_get((*get)(s), 0));
	(*set)(sr_nz(s.sr, (*get)(s) >> 1 | old_c << 7), s);
}
INS_DEF(RTS) {
	uint8_t ret_l = pop(s.mem, s.sp);
	uint8_t ret_h = pop(s.mem, s.sp);
	*s.pc = i8to16(ret_h, ret_l) + 1; // Emulate real 6502 RTS
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
INS_DEF(SEC) { *s.sr = bit_set(*s.sr, 0, 1); }
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
		+ *s.x/* + bit_get(*s.sr, 0)*/];,
	s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])
		+ *s.x/* + bit_get(*s.sr, 0)*/] = a;);
ADDR_DEF(abs_y, 3,
	return s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])
		+ *s.y/* + bit_get(*s.sr, 0)*/];,
	s.mem[i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1])
		+ *s.y/* + bit_get(*s.sr, 0)*/] = a;);
ADDR_DEF(imm, 2, return s.mem[*s.pc + 1];, );
ADDR_DEF(x_ind, 2,
	uint8_t zp_x = s.mem[*s.pc + 1] + *s.x;
	return s.mem[i8to16(s.mem[zp_x + 1], s.mem[zp_x])];,
	uint8_t zp_x = s.mem[*s.pc + 1] + *s.x;
	s.mem[i8to16(s.mem[zp_x + 1], s.mem[zp_x])] = a;);
ADDR_DEF(ind_y, 2,
	uint8_t zp = s.mem[*s.pc + 1];
	return s.mem[i8to16(s.mem[zp + 1], s.mem[zp]) +
	*s.y/* + bit_get(*s.sr, 0)*/];,
	uint8_t zp = s.mem[*s.pc + 1];
	s.mem[i8to16(s.mem[zp + 1], s.mem[zp]) +
	*s.y/* + bit_get(*s.sr, 0)*/] = a;);
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
	o[0x30] = (struct opcode){ ins_BMI, &addr_rel };
	o[0x50] = (struct opcode){ ins_BVC, &addr_rel };
	o[0x60] = (struct opcode){ ins_RTS, &addr_impl };
	o[0x90] = (struct opcode){ ins_BCC, &addr_rel };
	o[0xA0] = (struct opcode){ ins_LDY, &addr_imm };
	o[0xB0] = (struct opcode){ ins_BCS, &addr_rel };
	o[0xC0] = (struct opcode){ ins_CPY, &addr_imm };
	o[0xD0] = (struct opcode){ ins_BNE, &addr_rel };
	o[0xE0] = (struct opcode){ ins_CPX, &addr_imm };
	o[0xF0] = (struct opcode){ ins_BEQ, &addr_rel };
	// -1
	o[0x01] = (struct opcode){ ins_ORA, &addr_x_ind };
	o[0x11] = (struct opcode){ ins_ORA, &addr_ind_y };
	o[0x41] = (struct opcode){ ins_EOR, &addr_x_ind };
	o[0x71] = (struct opcode){ ins_ADC, &addr_ind_y };
	o[0x81] = (struct opcode){ ins_STA, &addr_x_ind };
	o[0x91] = (struct opcode){ ins_STA, &addr_ind_y };
	o[0xB1] = (struct opcode){ ins_LDA, &addr_ind_y };
	o[0xA1] = (struct opcode){ ins_LDA, &addr_x_ind };
	o[0xD1] = (struct opcode){ ins_CMP, &addr_ind_y };
	// -2
	o[0xA2] = (struct opcode){ ins_LDX, &addr_imm };
	// -4
	o[0x84] = (struct opcode){ ins_STY, &addr_zpg };
	o[0xA4] = (struct opcode){ ins_LDY, &addr_zpg };
	o[0xE4] = (struct opcode){ ins_CPX, &addr_zpg };
	// -5
	o[0x05] = (struct opcode){ ins_ORA, &addr_zpg };
	o[0x25] = (struct opcode){ ins_AND, &addr_zpg };
	o[0x45] = (struct opcode){ ins_EOR, &addr_zpg };
	o[0x65] = (struct opcode){ ins_ADC, &addr_zpg };
	o[0x85] = (struct opcode){ ins_STA, &addr_zpg };
	o[0x95] = (struct opcode){ ins_STA, &addr_zpg_x };
	o[0xA5] = (struct opcode){ ins_LDA, &addr_zpg };
	o[0xB5] = (struct opcode){ ins_LDA, &addr_zpg_x };
	o[0xC5] = (struct opcode){ ins_CMP, &addr_zpg };
	o[0xE5] = (struct opcode){ ins_SBC, &addr_zpg };
	o[0xF5] = (struct opcode){ ins_SBC, &addr_zpg_x };
	// -6
	o[0x06] = (struct opcode){ ins_ASL, &addr_zpg };
	o[0x66] = (struct opcode){ ins_ROR, &addr_zpg };
	o[0x86] = (struct opcode){ ins_STX, &addr_zpg };
	o[0xA6] = (struct opcode){ ins_LDX, &addr_zpg };
	o[0xC6] = (struct opcode){ ins_DEC, &addr_zpg };
	o[0xE6] = (struct opcode){ ins_INC, &addr_zpg };
	// -8
	o[0x18] = (struct opcode){ ins_CLC, &addr_impl };
	o[0x38] = (struct opcode){ ins_SEC, &addr_impl };
	o[0x48] = (struct opcode){ ins_PHA, &addr_impl };
	o[0x68] = (struct opcode){ ins_PLA, &addr_impl };
	o[0x88] = (struct opcode){ ins_DEY, &addr_impl };
	o[0x98] = (struct opcode){ ins_TYA, &addr_impl };
	o[0xA8] = (struct opcode){ ins_TAY, &addr_impl };
	o[0xC8] = (struct opcode){ ins_INY, &addr_impl };
	o[0xE8] = (struct opcode){ ins_INX, &addr_impl };
	// -9
	o[0x29] = (struct opcode){ ins_AND, &addr_imm };
	o[0x49] = (struct opcode){ ins_EOR, &addr_imm };
	o[0x69] = (struct opcode){ ins_ADC, &addr_imm };
	o[0x99] = (struct opcode){ ins_STA, &addr_abs_y };
	o[0xA9] = (struct opcode){ ins_LDA, &addr_imm };
	o[0xC9] = (struct opcode){ ins_CMP, &addr_imm };
	o[0xB9] = (struct opcode){ ins_LDA, &addr_abs_y };
	o[0xE9] = (struct opcode){ ins_SBC, &addr_imm };
	// -A
	o[0x0A] = (struct opcode){ ins_ASL, &addr_ac };
	o[0x2A] = (struct opcode){ ins_ROL, &addr_ac };
	o[0x4A] = (struct opcode){ ins_LSR, &addr_ac };
	o[0x6A] = (struct opcode){ ins_ROR, &addr_ac };
	o[0x8A] = (struct opcode){ ins_TXA, &addr_impl };
	o[0xAA] = (struct opcode){ ins_TAX, &addr_impl };
	o[0xCA] = (struct opcode){ ins_DEX, &addr_impl };
	o[0xEA] = (struct opcode){ ins_NOP, &addr_impl };
	// -C
	o[0x4C] = (struct opcode){ ins_JMP, &addr_abs_dir };
	o[0x8C] = (struct opcode){ ins_STY, &addr_abs };
	o[0xAC] = (struct opcode){ ins_LDY, &addr_abs };
	o[0xBC] = (struct opcode){ ins_LDY, &addr_abs_x };
	// -D
	o[0x0D] = (struct opcode){ ins_ORA, &addr_abs };
	o[0x7D] = (struct opcode){ ins_ADC, &addr_abs_x };
	o[0x8D] = (struct opcode){ ins_STA, &addr_abs };
	o[0x9D] = (struct opcode){ ins_STA, &addr_abs_x };
	o[0xAD] = (struct opcode){ ins_LDA, &addr_abs };
	o[0xBD] = (struct opcode){ ins_LDA, &addr_abs_x };
	o[0xCD] = (struct opcode){ ins_CMP, &addr_abs };
	// -E
	o[0x8E] = (struct opcode){ ins_STX, &addr_abs };
	o[0xAE] = (struct opcode){ ins_LDX, &addr_abs };
	o[0xBE] = (struct opcode){ ins_LDX, &addr_abs_y };
	o[0xCE] = (struct opcode){ ins_DEC, &addr_abs };
	o[0xEE] = (struct opcode){ ins_INC, &addr_abs };
	o[0xFE] = (struct opcode){ ins_INC, &addr_abs_x };
}

int main(int argc, char** argv) {
	// =====
	// INIT
	// =====
	
	// Seed random
	srand((unsigned int)time(NULL));

	// Create window
	os_create_window("6502", 
		SCREEN_WIDTH * PIXEL_SIZE, SCREEN_HEIGHT * PIXEL_SIZE);
	os_create_colormap(colors, COLOR_COUNT);

	// Handle command line, if no arg, ask with OS file dialog
	char fileNameBuf[256];
	if (argc < 2) {
		// If no arg and true: fileNameBuf is set, continue
		// If no arg and false, halt with instructions
		if (!os_choose_bin(fileNameBuf)) {
			puts("Usage: 6502 file.bin");
			return 0;
		}
	}
	// If have arg, fileNameBuf is just argv[1]
	else {
		strcpy(fileNameBuf, argv[1]);
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
	uint8_t reg_sp = 0xFF;
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
		fp = fopen(fileNameBuf, "rb");
		if (!fp) {
			perror("Cannot read input binary file");
			return -1;
		}
		fread(mem + PC_START, TOTAL_MEM - PC_START, 1, fp);
		fclose(fp);
	}

	// Init debug difflog
	uint8_t *difflog_prev_mem;
	uint8_t difflog_prev_ac = 0;
	uint8_t difflog_prev_x = 0;
	uint8_t difflog_prev_y = 0;
	uint8_t difflog_prev_sr = 0;
	uint8_t difflog_prev_sp = 0xFF;
	FILE *difflog_fp;
	if (DEBUG_DIFFLOG) {
		if (!(difflog_fp = fopen(DEBUG_DIFFLOG_FILE, "w"))) {
			perror("Cannot write to difflog");
			return -1;
		}
		difflog_prev_mem = malloc(TOTAL_MEM);
		memcpy(difflog_prev_mem, mem, TOTAL_MEM);
	}

	// =====
	// INIT LOOP 
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
	unsigned long cycles_this_frame = 0;
	unsigned long cycles_per_frame = (LIMIT_KHZ * 1000) * // khz -> hz
		((float)FRAME_INTERVAL / 1000 / 1000 / 1000); // ns -> s
	
	// Init stepping
	bool on_breakpoint = false;

	// =====
	// START MAIN LOOP
	// =====
	
	while (running) {
		// =====
		// UPDATE
		// =====

		// Delayed start
		if (!started && get_clock_ns() - init_time > START_DELAY) {
			started = true;
			start_time = get_clock_ns(); // Start counting average speed
		}

		// Limit cycles per IO/frame, if enabled and we've done enough this I/O
		bool limited = LIMIT_ENABLE && cycles_this_frame >= cycles_per_frame;

		// Step sim
		if (started && !halt && !limited) {
			// Count cycles per I/O frame
			cycles_this_frame++;

			// Random $FE
			if (!DEBUG_DIFFLOG) mem[0xFE] = rand();
			// Suppress random if difflog (decided by coin flip)
			else mem[0xFE] = 7;

			// Fetch and decode opcode
			uint8_t op = mem[reg_pc];
			struct opcode decoded = opcodes[op];
			
			// Get instruction length
			int length = 1;
			if (decoded.addr_mode) length = decoded.addr_mode->length;

			// Log instruction
			if (DEBUG_LOG) {
				printf("%llu: Stepping %04x: ", ins_count, reg_pc);
				for (int i = 0; i < length; i++)
					printf("%02x ", mem[reg_pc + i]);
				puts("");
			}

			// Save PC for debugging purposes
			uint16_t backup_pc = reg_pc;

			// Execute!
			if (decoded.instruction)
				decoded.instruction(decoded.addr_mode->get,
					decoded.addr_mode->set, sim_state);
			else {
				if (DEBUG_LOG) printf("Invalid opcode %02x\n", op);
				if (HALT_ON_INVALID) halt = true;
			}

			// Debug difflog; compare RAM, print diffs
			if (DEBUG_DIFFLOG) {
				// Print differing memory addresses
				for (int i = 0; i < TOTAL_MEM; i++) {
					//if (i == 0xFE) continue; // Skip random
					if (mem[i] == difflog_prev_mem[i]) continue;
					fprintf(difflog_fp,
						"%llu: Ins %02x @ %04x, Memory %04x, %02x -> %02x\n",
						ins_count, mem[backup_pc], backup_pc, i,
						difflog_prev_mem[i], mem[i]);
					difflog_prev_mem[i] = mem[i];
				}

				// Print differing registers
				if (difflog_prev_ac != reg_ac)
					print_difflog(difflog_fp, ins_count, mem, backup_pc, "AC",
						difflog_prev_ac, reg_ac);
				if (difflog_prev_x != reg_x)
					print_difflog(difflog_fp, ins_count, mem, backup_pc, "X",
						difflog_prev_x, reg_x);
				if (difflog_prev_y != reg_y)
					print_difflog(difflog_fp, ins_count, mem, backup_pc, "Y",
						difflog_prev_y, reg_y);
				if (difflog_prev_sr != reg_sr)
					print_difflog(difflog_fp, ins_count, mem, backup_pc, "SR",
						difflog_prev_sr | 0x20, reg_sr | 0x20);
						// Workaround for 6502asm setting SR ignore bit
				/*if (difflog_prev_sp != reg_sp)
					print_difflog(difflog_fp, ins_count, mem, backup_pc, "SP",
						difflog_prev_sp, reg_sp);*/

				// Update old registers for next diff
				difflog_prev_ac = reg_ac;
				difflog_prev_x = reg_x;
				difflog_prev_y = reg_y;
				difflog_prev_sr = reg_sr;
				difflog_prev_sp = reg_sp;
			}

			// Break if on breakpoint
			if (DEBUG_BREAKPOINT && (DEBUG_BREAKPOINT_MODE ? ins_count :
					backup_pc) == DEBUG_BREAKPOINT_VALUE) {
				if (DEBUG_BREAKPOINT_MODE)
					printf("Breaking at %llu\n", ins_count);
				else
					printf("Breaking at %04x\n", backup_pc);
				on_breakpoint = true;
			}

			// Let user step through instructions, or coredump
			if (DEBUG_STEP || (DEBUG_BREAKPOINT && on_breakpoint)) {
				while (true) {
					char cmd = getchar();
					if (cmd == '\n') break;
					if (cmd == 'c') coredump(sim_state);
					if (cmd == 'o') on_breakpoint = false;
					getchar(); // Consume upcoming \n
				}
			}

			// Log halt
			if (DEBUG_LOG && halt)
				puts("Halted.");

			// Increment PC unless instruction said not to
			if (!no_pc_inc) reg_pc += length;
			no_pc_inc = false; // Reset for next instruction

			// Count instruction for average speed
			ins_count++;
		}

		// Run I/O every X nanoseconds, or if redraw is required
		unsigned long long new_frame_time = get_clock_ns();
		if (new_frame_time - prev_frame_time > FRAME_INTERVAL || full_redraw) {
			prev_frame_time = new_frame_time;

			// Reset cycles limiter for next I/O frame
			cycles_this_frame = 0;

			// =====
			// RENDER
			// =====
			
			bool dirty = false;
			for (int i = 0; i < SCREEN_LENGTH; i++) {
				// Render only if dirty, or if redraw is required
				uint8_t new_pix = mem[SCREEN_START + i];
				if (old_screen[i] == new_pix && !full_redraw) continue;
				dirty = true; // So we know to present later
				old_screen[i] = new_pix; // Update old screen buffer

				// Get pixel details
				int x = i % SCREEN_WIDTH;
				int y = i / SCREEN_WIDTH;
				int color = new_pix & 0xf; // 0x0 to 0xf colors only

				// Render
				os_draw_rect(x * PIXEL_SIZE, y * PIXEL_SIZE, PIXEL_SIZE,
					PIXEL_SIZE, colors, color);
			}
			if (dirty) os_present();
			full_redraw = false;

			// =====
			// HANDLE EVENTS
			// =====

			struct event e;
			while (os_poll_event(&e)) {
				switch (e.type) {
					case ET_KEYPRESS:
						mem[0xFF] = e.kp_key; // Put ASCII into memory
						break;
					case ET_EXPOSE:
						full_redraw = true; // Next update will be an I/O frame
						break;
					default: break;
				}
			}

			if (os_should_exit()) running = false;
		}

		// =====
		// YIELD
		// =====

		// Let the OS multitask, then come back to us ASAP
		// (Not sure if needed...)
		//usleep(0);

		// =====
		// HALT/QUIT
		// =====

		// Calculate average speed and print when either halted or quitting
		if ((halt || !running) && !avg_speed_done) {
			coredump(sim_state);
			avg_speed_done = true;
			unsigned long long diff = get_clock_ns() - start_time;
			double diff_s = (double)diff / 1000000000;
			double avg_speed = (double)ins_count / diff_s;
			printf("Processed %llu instructions in %f seconds.\n"
				"Average speed: %f Mhz.\n", ins_count, diff_s,
				avg_speed / 1000000);
		}
	}

	// =====
	// END MAIN LOOP
	// =====

	// Close debug difflog
	if (DEBUG_DIFFLOG) {
		fclose(difflog_fp);
		free(difflog_prev_mem);
	}

	// Close OS layer
	os_close();

	return 0;
}
