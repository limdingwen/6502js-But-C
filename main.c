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
#define LOAD_START 0x0600
#define PC_START 0x0600
#define STACK_TOP 0x01FF
#define SCREEN_START 0x0200
#define SCREEN_LENGTH 0x0400
#define SCREEN_WIDTH 0x0020
#define SCREEN_HEIGHT 0x0020
#define PIXEL_SIZE 10 // How big is a fake pixel?
#define FRAME_INTERVAL 16666666 // In ns
#define DEFAULT_LIMIT_ENABLE 1
#define DEFAULT_LIMIT_KHZ 30
#define START_DELAY 500000000 // In ns
#define DEBUG_COREDUMP 1 // Coredumps on exit, also enables for step coredump
#define DEBUG_COREDUMP_START 0x0000
#define DEBUG_COREDUMP_END 0x00FF
#define DEBUG_STEP 0 // Allows user to step per cycle
#define DEBUG_BREAKPOINT 1 // Goes into stepping mode when breakpoint reached
#define DEBUG_BREAKPOINT_MODE 2 // 0 = addr, 1 = ins_count, 2 = trapped
#define DEBUG_BREAKPOINT_VALUE 106688
#define DEBUG_LOG 0 // Logs all instructions processed
#define DEBUG_LOG_CMP 1 // Log compares (useful for Klaus tests) 
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
void coredump(struct sim_state s, uint16_t begin, uint16_t end) {
	if (!DEBUG_COREDUMP) return;
	for (int i = begin; i <= end; i += 0x10) {
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
uint8_t bit_set(uint8_t operand, int bit_pos, int bit_value) {
	return (operand & ~(1UL << bit_pos)) | (bit_value << bit_pos);
}
int bit_get(uint16_t operand, int bit_pos) { return operand >> bit_pos & 1; }

// Instruction helpers
uint8_t sr_nz(uint8_t *sr, uint8_t a) {
	*sr = bit_set(*sr, 1, a == 0); // Zero
	*sr = bit_set(*sr, 7, bit_get(a, 7)); // Negative
	return a;
}
void push(uint8_t *mem, uint8_t *sp, uint8_t new_value) {
	mem[(*sp)-- + 0x0100] = new_value;
	//*sp = *sp - 1;
}
uint8_t pop(uint8_t *mem, uint8_t *sp) { return mem[++(*sp) + 0x0100]; }
void cmp(struct sim_state s, uint8_t reg, uint8_t get) {
	if (DEBUG_LOG && DEBUG_LOG_CMP)
		printf("Comparing reg=%x, mem=%x\n", reg, get);
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
// Counting from LSB (assuming little endian)
uint8_t nibble_get(uint8_t number, int nibble) {
	return (number >> (4 * nibble)) & 0xF;
}
// Manually calculate BCD addition
uint16_t bcd_add(uint8_t left, uint8_t right, bool carry) {
	bool carry_one = carry;
	uint16_t result = 0;
	for (int i = 0; i < 2; i++) { // 2 nibbles for 1 byte
		uint8_t nibble_result = nibble_get(left, i) + nibble_get(right, i);
		// Carry the one...
		if (carry_one) nibble_result += 1;
		carry_one = false;
		while (nibble_result > 9) {
			nibble_result -= 10;
			carry_one = true;
		}
		// Put into results, one digit (nibble) at a time
		result |= nibble_result << (4 * i);
	}
	// Final carry
	if (carry_one) result |= 0x100;
	return result;
}
// Manually calculate BCD subtraction 
uint16_t bcd_sub(uint8_t left, uint8_t right, bool carry) {
	bool carry_one = !carry;
	uint16_t result = 0;
	for (int i = 0; i < 2; i++) { // 2 nibbles for 1 byte
		int8_t nibble_result = nibble_get(left, i) - nibble_get(right, i);
		// Carry the one... if negative
		if (carry_one) nibble_result -= 1;
		carry_one = false;
		while (nibble_result < 0) { // nibble_result should be positive after
			nibble_result += 10;
			carry_one = true;
		}
		// Put into results, one digit (nibble) at a time
		result |= nibble_result << (4 * i);
	}
	// Final carry
	if (!carry_one) result |= 0x100;
	return result;
}
void adc(struct sim_state s, uint8_t input, bool sub) {
	uint16_t t;
	if (bit_get(*s.sr, 3)) { // BCD
		if (sub) t = bcd_sub(*s.ac, input, bit_get(*s.sr, 0)); 
		else t = bcd_add(*s.ac, input, bit_get(*s.sr, 0));
	}
	else { // Binary ADC/SBC
		if (sub) input = ~input;
		t = *s.ac + input + bit_get(*s.sr, 0);
	}
	*s.sr = bit_set(*s.sr, 0, bit_get(t, 8)); // Carry
	bool v = !(bit_get(*s.ac, 7) ^ bit_get(input, 7)) && // Same sign? 
		bit_get(*s.ac, 7) ^ bit_get(t, 7); // Different sign for result?
	*s.sr = bit_set(*s.sr, 6, v); // Overflow
	*s.ac = sr_nz(s.sr, t);
}

// Instructions
#define INS_DEF(N) void ins_##N(uint16_t (*get)(struct sim_state), \
	void (*set)(uint8_t, struct sim_state), struct sim_state s)
INS_DEF(JMP) { *s.pc = (*get)(s); *s.no_pc_inc = true; }
INS_DEF(ADC) { adc(s, (*get)(s), false); }
INS_DEF(AND) { *s.ac = sr_nz(s.sr, *s.ac & (*get)(s)); }
INS_DEF(ASL) {
	*s.sr = bit_set(*s.sr, 0, bit_get((*get)(s), 7));
	(*set)(sr_nz(s.sr, (*get)(s) << 1), s);
}
INS_DEF(BCC) { if (!bit_get(*s.sr, 0)) { *s.pc = (*get)(s); } }
INS_DEF(BCS) { if (bit_get(*s.sr, 0)) { *s.pc = (*get)(s); } }
INS_DEF(BEQ) { if (bit_get(*s.sr, 1)) { *s.pc = (*get)(s); } }
INS_DEF(BIT) {
	// A AND M
	sr_nz(s.sr, *s.ac & (*get)(s));
	// M7 -> N, M6 -> V
	*s.sr = bit_set(*s.sr, 7, bit_get((*get)(s), 7));
	*s.sr = bit_set(*s.sr, 6, bit_get((*get)(s), 6));
}
INS_DEF(BMI) { if (bit_get(*s.sr, 7)) { *s.pc = (*get)(s); } }
INS_DEF(BNE) { if (!bit_get(*s.sr, 1)) { *s.pc = (*get)(s); } }
INS_DEF(BPL) { if (!bit_get(*s.sr, 7)) { *s.pc = (*get)(s); } }
INS_DEF(BVC) { if (!bit_get(*s.sr, 6)) { *s.pc = (*get)(s); } }
INS_DEF(BVS) { if (bit_get(*s.sr, 6)) { *s.pc = (*get)(s); } }
INS_DEF(BRK) { *s.halt = true; }
INS_DEF(CLC) { *s.sr = bit_set(*s.sr, 0, 0); }
INS_DEF(CLD) { *s.sr = bit_set(*s.sr, 3, 0); }
INS_DEF(CLI) { *s.sr = bit_set(*s.sr, 2, 0); }
INS_DEF(CLV) { *s.sr = bit_set(*s.sr, 6, 0); }
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
INS_DEF(PHP) {
	uint8_t to_push = *s.sr;
	// Set break and bit 5 to 1
	to_push = bit_set(to_push, 4, 1);
	to_push = bit_set(to_push, 5, 1);
	push(s.mem, s.sp, to_push);
}
INS_DEF(PLA) { *s.ac = sr_nz(s.sr, pop(s.mem, s.sp)); }
INS_DEF(PLP) {
	uint8_t old_sr = *s.sr;
	*s.sr = pop(s.mem, s.sp);
	// Restore old break and bit 5
	*s.sr = bit_set(*s.sr, 4, bit_get(old_sr, 4));
	*s.sr = bit_set(*s.sr, 5, bit_get(old_sr, 5));
}
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
INS_DEF(RTI) {
	// Essentially a PLP and then a RTS, but w/o + 1
	ins_PLP(get, set, s);
	uint8_t ret_l = pop(s.mem, s.sp);
	uint8_t ret_h = pop(s.mem, s.sp);
	*s.pc = i8to16(ret_h, ret_l);
	*s.no_pc_inc = true;
}
INS_DEF(RTS) {
	uint8_t ret_l = pop(s.mem, s.sp);
	uint8_t ret_h = pop(s.mem, s.sp);
	*s.pc = i8to16(ret_h, ret_l) + 1; // Emulate real 6502 RTS
	*s.no_pc_inc = true;
} 
// Just flip the bits man... and then do ADC
// Trying to do 2s complement manually WILL result in pain by overflow.
INS_DEF(SBC) { adc(s, (*get)(s), true); }
INS_DEF(SEC) { *s.sr = bit_set(*s.sr, 0, 1); }
INS_DEF(SED) { *s.sr = bit_set(*s.sr, 3, 1); }
INS_DEF(SEI) { *s.sr = bit_set(*s.sr, 2, 1); }
INS_DEF(STA) { (*set)(*s.ac, s); }
INS_DEF(STX) { (*set)(*s.x, s); }
INS_DEF(STY) { (*set)(*s.y, s); }
INS_DEF(TAX) { *s.x = sr_nz(s.sr, *s.ac); }
INS_DEF(TAY) { *s.y = sr_nz(s.sr, *s.ac); }
INS_DEF(TSX) { *s.x = sr_nz(s.sr, *s.sp); }
INS_DEF(TXA) { *s.ac = sr_nz(s.sr, *s.x); }
INS_DEF(TXS) { *s.sp = *s.x; } // TSX sets NZ - TXS does not
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
ADDR_DEF(ind_dir, 3,
	uint16_t hhll = i8to16(s.mem[*s.pc + 2], s.mem[*s.pc + 1]);
	return i8to16(s.mem[hhll + 1], s.mem[hhll]);, );
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
	uint8_t zp = s.mem[*s.pc + 1] + *s.x; // Force wraparound
	return s.mem[zp];,
	uint8_t zp = s.mem[*s.pc + 1] + *s.x; // Force wraparound
	s.mem[zp] = a;);
ADDR_DEF(zpg_y, 2,
	uint8_t zp = s.mem[*s.pc + 1] + *s.y; // Force wraparound
	return s.mem[zp];,
	uint8_t zp = s.mem[*s.pc + 1] + *s.y; // Force wraparound
	s.mem[zp] = a;);
#undef ADDR_DEF

// Opcodes
// TODO: Add cycles
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
	o[0x40] = (struct opcode){ ins_RTI, &addr_impl };
	o[0x50] = (struct opcode){ ins_BVC, &addr_rel };
	o[0x60] = (struct opcode){ ins_RTS, &addr_impl };
	o[0x70] = (struct opcode){ ins_BVS, &addr_rel };
	// 0x80 undef
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
	o[0x21] = (struct opcode){ ins_AND, &addr_x_ind };
	o[0x31] = (struct opcode){ ins_AND, &addr_ind_y };
	o[0x41] = (struct opcode){ ins_EOR, &addr_x_ind };
	o[0x51] = (struct opcode){ ins_EOR, &addr_ind_y };
	o[0x61] = (struct opcode){ ins_ADC, &addr_x_ind };
	o[0x71] = (struct opcode){ ins_ADC, &addr_ind_y };
	o[0x81] = (struct opcode){ ins_STA, &addr_x_ind };
	o[0x91] = (struct opcode){ ins_STA, &addr_ind_y };
	o[0xA1] = (struct opcode){ ins_LDA, &addr_x_ind };
	o[0xB1] = (struct opcode){ ins_LDA, &addr_ind_y };
	o[0xC1] = (struct opcode){ ins_CMP, &addr_x_ind };
	o[0xD1] = (struct opcode){ ins_CMP, &addr_ind_y };
	o[0xE1] = (struct opcode){ ins_SBC, &addr_x_ind };
	o[0xF1] = (struct opcode){ ins_SBC, &addr_ind_y };
	// -2
	// 0x02 to 0x92 undef
	o[0xA2] = (struct opcode){ ins_LDX, &addr_imm };
	// 0xB2 to 0xf2 undef
	// -3
	// 0x03 to 0xf3 undef
	// -4
	// 0x04 to 0x14 undef
	o[0x24] = (struct opcode){ ins_BIT, &addr_zpg };
	// 0x34 to 0x74 undef
	o[0x84] = (struct opcode){ ins_STY, &addr_zpg };
	o[0x94] = (struct opcode){ ins_STY, &addr_zpg_x };
	o[0xA4] = (struct opcode){ ins_LDY, &addr_zpg };
	o[0xB4] = (struct opcode){ ins_LDY, &addr_zpg_x };
	o[0xC4] = (struct opcode){ ins_CPY, &addr_zpg };
	// 0xD4 undef
	o[0xE4] = (struct opcode){ ins_CPX, &addr_zpg };
	// 0xF4 undef
	// -5
	o[0x05] = (struct opcode){ ins_ORA, &addr_zpg };
	o[0x15] = (struct opcode){ ins_ORA, &addr_zpg_x };
	o[0x25] = (struct opcode){ ins_AND, &addr_zpg };
	o[0x35] = (struct opcode){ ins_AND, &addr_zpg_x };
	o[0x45] = (struct opcode){ ins_EOR, &addr_zpg };
	o[0x55] = (struct opcode){ ins_EOR, &addr_zpg_x };
	o[0x65] = (struct opcode){ ins_ADC, &addr_zpg };
	o[0x75] = (struct opcode){ ins_ADC, &addr_zpg_x };
	o[0x85] = (struct opcode){ ins_STA, &addr_zpg };
	o[0x95] = (struct opcode){ ins_STA, &addr_zpg_x };
	o[0xA5] = (struct opcode){ ins_LDA, &addr_zpg };
	o[0xB5] = (struct opcode){ ins_LDA, &addr_zpg_x };
	o[0xC5] = (struct opcode){ ins_CMP, &addr_zpg };
	o[0xD5] = (struct opcode){ ins_CMP, &addr_zpg_x };
	o[0xE5] = (struct opcode){ ins_SBC, &addr_zpg };
	o[0xF5] = (struct opcode){ ins_SBC, &addr_zpg_x };
	// -6
	o[0x06] = (struct opcode){ ins_ASL, &addr_zpg };
	o[0x16] = (struct opcode){ ins_ASL, &addr_zpg_x };
	o[0x26] = (struct opcode){ ins_ROL, &addr_zpg };
	o[0x36] = (struct opcode){ ins_ROL, &addr_zpg_x };
	o[0x46] = (struct opcode){ ins_LSR, &addr_zpg };
	o[0x56] = (struct opcode){ ins_LSR, &addr_zpg_x };
	o[0x66] = (struct opcode){ ins_ROR, &addr_zpg };
	o[0x76] = (struct opcode){ ins_ROR, &addr_zpg_x };
	o[0x86] = (struct opcode){ ins_STX, &addr_zpg };
	o[0x96] = (struct opcode){ ins_STX, &addr_zpg_y };
	o[0xA6] = (struct opcode){ ins_LDX, &addr_zpg };
	o[0xB6] = (struct opcode){ ins_LDX, &addr_zpg_y };
	o[0xC6] = (struct opcode){ ins_DEC, &addr_zpg };
	o[0xD6] = (struct opcode){ ins_DEC, &addr_zpg_x };
	o[0xE6] = (struct opcode){ ins_INC, &addr_zpg };
	o[0xF6] = (struct opcode){ ins_INC, &addr_zpg_x };
	// -7
	// 0x07 to 0xf7 undef
	// -8
	o[0x08] = (struct opcode){ ins_PHP, &addr_impl };
	o[0x18] = (struct opcode){ ins_CLC, &addr_impl };
	o[0x28] = (struct opcode){ ins_PLP, &addr_impl };
	o[0x38] = (struct opcode){ ins_SEC, &addr_impl };
	o[0x48] = (struct opcode){ ins_PHA, &addr_impl };
	o[0x58] = (struct opcode){ ins_CLI, &addr_impl };
	o[0x68] = (struct opcode){ ins_PLA, &addr_impl };
	o[0x78] = (struct opcode){ ins_SEI, &addr_impl };
	o[0x88] = (struct opcode){ ins_DEY, &addr_impl };
	o[0x98] = (struct opcode){ ins_TYA, &addr_impl };
	o[0xA8] = (struct opcode){ ins_TAY, &addr_impl };
	o[0xB8] = (struct opcode){ ins_CLV, &addr_impl };
	o[0xC8] = (struct opcode){ ins_INY, &addr_impl };
	o[0xD8] = (struct opcode){ ins_CLD, &addr_impl };
	o[0xE8] = (struct opcode){ ins_INX, &addr_impl };
	o[0xF8] = (struct opcode){ ins_SED, &addr_impl };
	// -9
	o[0x09] = (struct opcode){ ins_ORA, &addr_imm };
	o[0x19] = (struct opcode){ ins_ORA, &addr_abs_y };
	o[0x29] = (struct opcode){ ins_AND, &addr_imm };
	o[0x39] = (struct opcode){ ins_AND, &addr_abs_y };
	o[0x49] = (struct opcode){ ins_EOR, &addr_imm };
	o[0x59] = (struct opcode){ ins_EOR, &addr_abs_y };
	o[0x69] = (struct opcode){ ins_ADC, &addr_imm };
	o[0x79] = (struct opcode){ ins_ADC, &addr_abs_y };
	// 0x89 undef
	o[0x99] = (struct opcode){ ins_STA, &addr_abs_y };
	o[0xA9] = (struct opcode){ ins_LDA, &addr_imm };
	o[0xB9] = (struct opcode){ ins_LDA, &addr_abs_y };
	o[0xC9] = (struct opcode){ ins_CMP, &addr_imm };
	o[0xD9] = (struct opcode){ ins_CMP, &addr_abs_y };
	o[0xE9] = (struct opcode){ ins_SBC, &addr_imm };
	o[0xF9] = (struct opcode){ ins_SBC, &addr_abs_y };
	// -A
	o[0x0A] = (struct opcode){ ins_ASL, &addr_ac };
	// 0x1a undef
	o[0x2A] = (struct opcode){ ins_ROL, &addr_ac };
	// 0x3a undef
	o[0x4A] = (struct opcode){ ins_LSR, &addr_ac };
	// 0x5a undef
	o[0x6A] = (struct opcode){ ins_ROR, &addr_ac };
	// 0x7a undef
	o[0x8A] = (struct opcode){ ins_TXA, &addr_impl };
	o[0x9A] = (struct opcode){ ins_TXS, &addr_impl };
	o[0xAA] = (struct opcode){ ins_TAX, &addr_impl };
	o[0xBA] = (struct opcode){ ins_TSX, &addr_impl };
	o[0xCA] = (struct opcode){ ins_DEX, &addr_impl };
	// 0xda undef
	o[0xEA] = (struct opcode){ ins_NOP, &addr_impl };
	// 0xfa undef
	// -B
	// 0x0b to 0xfb undef
	// -C
	// 0x0c to 0x1c undef
	o[0x2C] = (struct opcode){ ins_BIT, &addr_abs };
	// 0x3c undef
	o[0x4C] = (struct opcode){ ins_JMP, &addr_abs_dir };
	// 0x5c undef
	o[0x6C] = (struct opcode){ ins_JMP, &addr_ind_dir };
	// 0x7c undef
	o[0x8C] = (struct opcode){ ins_STY, &addr_abs };
	// 0x9c undef
	o[0xAC] = (struct opcode){ ins_LDY, &addr_abs };
	o[0xBC] = (struct opcode){ ins_LDY, &addr_abs_x };
	o[0xCC] = (struct opcode){ ins_CPY, &addr_abs };
	// 0xdc undef
	o[0xEC] = (struct opcode){ ins_CPX, &addr_abs };
	// 0xfc undef
	// -D
	o[0x0D] = (struct opcode){ ins_ORA, &addr_abs };
	o[0x1D] = (struct opcode){ ins_ORA, &addr_abs_x };
	o[0x2D] = (struct opcode){ ins_AND, &addr_abs };
	o[0x3D] = (struct opcode){ ins_AND, &addr_abs_x };
	o[0x4D] = (struct opcode){ ins_EOR, &addr_abs };
	o[0x5D] = (struct opcode){ ins_EOR, &addr_abs_x };
	o[0x6D] = (struct opcode){ ins_ADC, &addr_abs };
	o[0x7D] = (struct opcode){ ins_ADC, &addr_abs_x };
	o[0x8D] = (struct opcode){ ins_STA, &addr_abs };
	o[0x9D] = (struct opcode){ ins_STA, &addr_abs_x };
	o[0xAD] = (struct opcode){ ins_LDA, &addr_abs };
	o[0xBD] = (struct opcode){ ins_LDA, &addr_abs_x };
	o[0xCD] = (struct opcode){ ins_CMP, &addr_abs };
	o[0xDD] = (struct opcode){ ins_CMP, &addr_abs_x };
	o[0xED] = (struct opcode){ ins_SBC, &addr_abs };
	o[0xFD] = (struct opcode){ ins_SBC, &addr_abs_x };
	// -E
	o[0x0E] = (struct opcode){ ins_ASL, &addr_abs };
	o[0x1E] = (struct opcode){ ins_ASL, &addr_abs_x };
	o[0x2E] = (struct opcode){ ins_ROL, &addr_abs };
	o[0x3E] = (struct opcode){ ins_ROL, &addr_abs_x };
	o[0x4E] = (struct opcode){ ins_LSR, &addr_abs };
	o[0x5E] = (struct opcode){ ins_LSR, &addr_abs_x };
	o[0x6E] = (struct opcode){ ins_ROR, &addr_abs };
	o[0x7E] = (struct opcode){ ins_ROR, &addr_abs_x };
	o[0x8E] = (struct opcode){ ins_STX, &addr_abs };
	// 0x9e undef
	o[0xAE] = (struct opcode){ ins_LDX, &addr_abs };
	o[0xBE] = (struct opcode){ ins_LDX, &addr_abs_y };
	o[0xCE] = (struct opcode){ ins_DEC, &addr_abs };
	o[0xDE] = (struct opcode){ ins_DEC, &addr_abs_x };
	o[0xEE] = (struct opcode){ ins_INC, &addr_abs };
	o[0xFE] = (struct opcode){ ins_INC, &addr_abs_x };
	// -F
	// 0x0f to 0xff undef
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
			puts("Usage: 6502 file.bin [options]");
			puts("Options:");
			printf("-unlimited: Run with no speed limiter (default: %s)\n",
				DEFAULT_LIMIT_ENABLE ? "limited" : "unlimited");
			printf("-s(speed_in_khz): Set speed limit (default: %d)\n",
				DEFAULT_LIMIT_KHZ); 
			return 0;
		}
	}
	// If have arg, fileNameBuf is just argv[1]
	else {
		strcpy(fileNameBuf, argv[1]);
	}

	// Handle command line: -unlimited
	bool limit_enable = DEFAULT_LIMIT_ENABLE;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-unlimited") == 0) {
			limit_enable = false;
			break;
		}
	}

	// Handle command line: -s[speed]
	unsigned long limit_khz = DEFAULT_LIMIT_KHZ;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "-s", 2) == 0) {
			unsigned long input = strtol(argv[i] + 2, NULL, 10);
			if (input == 0) { // Also detects invalid input (strtol returns 0)
				puts("Invalid speed (must be integer, not 0)");
				return -1;
			}
			limit_khz = input;
			break;
		}
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
		fread(mem + LOAD_START, TOTAL_MEM - LOAD_START, 1, fp);
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
	unsigned long cycles_per_frame = (limit_khz * 1000) * // khz -> hz
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
		bool limited = limit_enable && cycles_this_frame >= cycles_per_frame;

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

			// Check for trapped PC
			uint16_t trapped_check_old_pc = reg_pc;

			// Execute!
			if (decoded.instruction)
				decoded.instruction(decoded.addr_mode->get,
					decoded.addr_mode->set, sim_state);
			else {
				if (DEBUG_LOG) printf("Invalid opcode %02x\n", op);
				if (HALT_ON_INVALID) halt = true;
			}

			// Increment PC unless instruction said not to
			if (!no_pc_inc) reg_pc += length;
			no_pc_inc = false; // Reset for next instruction

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
			bool addr_break = backup_pc == DEBUG_BREAKPOINT_VALUE;
			bool ins_count_break = ins_count == DEBUG_BREAKPOINT_VALUE;
			bool trapped_break = trapped_check_old_pc == reg_pc;
			bool should_break = false;
			switch (DEBUG_BREAKPOINT_MODE) {
				case 0: should_break = addr_break; break;
				case 1: should_break = ins_count_break; break;
				case 2: should_break = trapped_break; break;
			}
			if (DEBUG_BREAKPOINT && should_break) {
				switch (DEBUG_BREAKPOINT_MODE) {
					case 0:
					case 2:
						printf("Breaking at %04x\n", backup_pc);
						break;
					case 1:
						printf("Breaking at %llu\n", ins_count);
						break;
				}
				on_breakpoint = true;
			}

			// Let user step through instructions, or coredump
			if (DEBUG_STEP || (DEBUG_BREAKPOINT && on_breakpoint)) {
				while (true) {
					char cmd[32];
					fgets(cmd, 32, stdin);
					if (cmd[0] == '\n') break;
					else if (cmd[0] == 'c') {
						uint16_t begin = DEBUG_COREDUMP_START;
						uint16_t end = DEBUG_COREDUMP_END;
						char *split = strtok(cmd, " ");
						if (split) {
							split = strtok(NULL, " ");
							if (split) {
								begin = strtol(split, NULL, 16);
								end = begin;
								split = strtok(NULL, " ");
								if (split) {
									end = strtol(split, NULL, 16);
								}
							}
						}
						coredump(sim_state, begin, end);
					}
					else if (cmd[0] == 'r') on_breakpoint = false;
				}
			}

			// Log halt
			if (DEBUG_LOG && halt)
				puts("Halted.");

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
			coredump(sim_state, DEBUG_COREDUMP_START, DEBUG_COREDUMP_END);
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
