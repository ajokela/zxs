#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Test framework ──────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 0; \
    } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        printf("  FAIL: %s: expected 0x%X, got 0x%X (line %d)\n", \
               msg, (unsigned)(b), (unsigned)(a), __LINE__); \
        return 0; \
    } \
} while(0)

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("%-60s", #fn); \
    if (fn()) { tests_passed++; printf("PASS\n"); } \
    else { tests_failed++; printf("FAILED\n"); } \
} while(0)

/* ── Test memory/IO ──────────────────────────────────────────────── */

static uint8_t test_mem[65536];
static uint8_t io_ports[256];
static uint8_t last_out_port;
static uint8_t last_out_val;

static uint8_t test_read(void *ctx, uint16_t addr) {
    (void)ctx;
    return test_mem[addr];
}

static void test_write(void *ctx, uint16_t addr, uint8_t val) {
    (void)ctx;
    test_mem[addr] = val;
}

static uint8_t test_in(void *ctx, uint16_t port) {
    (void)ctx;
    return io_ports[port & 0xFF];
}

static void test_out(void *ctx, uint16_t port, uint8_t val) {
    (void)ctx;
    last_out_port = port & 0xFF;
    last_out_val = val;
    io_ports[port & 0xFF] = val;
}

static void setup_cpu(z80_t *cpu) {
    memset(test_mem, 0, sizeof(test_mem));
    memset(io_ports, 0, sizeof(io_ports));
    z80_init(cpu);
    cpu->mem_read = test_read;
    cpu->mem_write = test_write;
    cpu->io_in = test_in;
    cpu->io_out = test_out;
    cpu->A = 0; cpu->F = 0;
    cpu->SP = 0xFFFF;
    last_out_port = 0;
    last_out_val = 0;
}


/* ── Tests ───────────────────────────────────────────────────────── */

static int test_nop(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    test_mem[0] = 0x00; /* NOP */
    int t = z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 1, "PC");
    ASSERT_EQ(t, 4, "T-states");
    return 1;
}

static int test_ld_reg_imm(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    /* LD B, 0x42 */
    test_mem[0] = 0x06; test_mem[1] = 0x42;
    /* LD C, 0x37 */
    test_mem[2] = 0x0E; test_mem[3] = 0x37;
    /* LD D, 0x11 */
    test_mem[4] = 0x16; test_mem[5] = 0x11;
    /* LD E, 0x22 */
    test_mem[6] = 0x1E; test_mem[7] = 0x22;
    /* LD H, 0x33 */
    test_mem[8] = 0x26; test_mem[9] = 0x33;
    /* LD L, 0x44 */
    test_mem[10] = 0x2E; test_mem[11] = 0x44;
    /* LD A, 0xFF */
    test_mem[12] = 0x3E; test_mem[13] = 0xFF;

    z80_step(&cpu); ASSERT_EQ(cpu.B, 0x42, "B");
    z80_step(&cpu); ASSERT_EQ(cpu.C, 0x37, "C");
    z80_step(&cpu); ASSERT_EQ(cpu.D, 0x11, "D");
    z80_step(&cpu); ASSERT_EQ(cpu.E, 0x22, "E");
    z80_step(&cpu); ASSERT_EQ(cpu.H, 0x33, "H");
    z80_step(&cpu); ASSERT_EQ(cpu.L, 0x44, "L");
    z80_step(&cpu); ASSERT_EQ(cpu.A, 0xFF, "A");
    return 1;
}

static int test_ld_reg_reg(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x42;
    test_mem[0] = 0x48; /* LD C, B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.C, 0x42, "C=B");
    return 1;
}

static int test_ld_reg16_imm(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    /* LD BC, 0x1234 */
    test_mem[0] = 0x01; test_mem[1] = 0x34; test_mem[2] = 0x12;
    /* LD DE, 0x5678 */
    test_mem[3] = 0x11; test_mem[4] = 0x78; test_mem[5] = 0x56;
    /* LD HL, 0x9ABC */
    test_mem[6] = 0x21; test_mem[7] = 0xBC; test_mem[8] = 0x9A;
    /* LD SP, 0xDEF0 */
    test_mem[9] = 0x31; test_mem[10] = 0xF0; test_mem[11] = 0xDE;

    int t;
    t = z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x12, "B"); ASSERT_EQ(cpu.C, 0x34, "C"); ASSERT_EQ(t, 10, "T");
    t = z80_step(&cpu);
    ASSERT_EQ(cpu.D, 0x56, "D"); ASSERT_EQ(cpu.E, 0x78, "E");
    t = z80_step(&cpu);
    ASSERT_EQ(cpu.H, 0x9A, "H"); ASSERT_EQ(cpu.L, 0xBC, "L");
    t = z80_step(&cpu);
    ASSERT_EQ(cpu.SP, 0xDEF0, "SP");
    return 1;
}

static int test_ld_indirect(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x42;
    cpu.B = 0x80; cpu.C = 0x00;
    test_mem[0] = 0x02; /* LD (BC), A */
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x8000], 0x42, "(BC)");

    cpu.PC = 0;
    cpu.A = 0;
    test_mem[0] = 0x0A; /* LD A, (BC) */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x42, "A=(BC)");
    return 1;
}

static int test_ld_hl_indirect(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0x50; cpu.L = 0x00;
    test_mem[0x5000] = 0xAB;
    test_mem[0] = 0x7E; /* LD A, (HL) */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0xAB, "A=(HL)");

    cpu.PC = 0;
    cpu.A = 0x99;
    test_mem[0] = 0x77; /* LD (HL), A */
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5000], 0x99, "(HL)=A");
    return 1;
}

static int test_ld_nn_a(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x55;
    /* LD (0x4000), A */
    test_mem[0] = 0x32; test_mem[1] = 0x00; test_mem[2] = 0x40;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x4000], 0x55, "(nn)=A");

    cpu.PC = 0; cpu.A = 0;
    /* LD A, (0x4000) */
    test_mem[0] = 0x3A; test_mem[1] = 0x00; test_mem[2] = 0x40;
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x55, "A=(nn)");
    return 1;
}

static int test_ld_nn_hl(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0xAB; cpu.L = 0xCD;
    /* LD (0x3000), HL */
    test_mem[0] = 0x22; test_mem[1] = 0x00; test_mem[2] = 0x30;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x3000], 0xCD, "low");
    ASSERT_EQ(test_mem[0x3001], 0xAB, "high");

    cpu.PC = 0; cpu.H = 0; cpu.L = 0;
    /* LD HL, (0x3000) */
    test_mem[0] = 0x2A; test_mem[1] = 0x00; test_mem[2] = 0x30;
    z80_step(&cpu);
    ASSERT_EQ(cpu.H, 0xAB, "H"); ASSERT_EQ(cpu.L, 0xCD, "L");
    return 1;
}

static int test_add_a(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x10; cpu.B = 0x20;
    test_mem[0] = 0x80; /* ADD A, B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x30, "A");
    ASSERT(!(cpu.F & Z80_ZF), "not zero");
    ASSERT(!(cpu.F & Z80_CF), "no carry");
    ASSERT(!(cpu.F & Z80_NF), "not subtract");
    return 1;
}

static int test_add_overflow(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x7F; cpu.B = 0x01;
    test_mem[0] = 0x80; /* ADD A, B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x80, "A");
    ASSERT(cpu.F & Z80_SF, "sign");
    ASSERT(cpu.F & Z80_PF, "overflow");
    ASSERT(cpu.F & Z80_HF, "half carry");
    return 1;
}

static int test_add_carry(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0xFF; cpu.B = 0x01;
    test_mem[0] = 0x80; /* ADD A, B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x00, "A");
    ASSERT(cpu.F & Z80_CF, "carry");
    ASSERT(cpu.F & Z80_ZF, "zero");
    return 1;
}

static int test_adc(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x10; cpu.B = 0x20;
    cpu.F = Z80_CF; /* carry set */
    test_mem[0] = 0x88; /* ADC A, B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x31, "A=0x10+0x20+carry");
    return 1;
}

static int test_sub(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x30; cpu.B = 0x10;
    test_mem[0] = 0x90; /* SUB B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x20, "A");
    ASSERT(cpu.F & Z80_NF, "subtract flag");
    ASSERT(!(cpu.F & Z80_CF), "no borrow");
    return 1;
}

static int test_sub_borrow(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x10; cpu.B = 0x20;
    test_mem[0] = 0x90; /* SUB B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0xF0, "A");
    ASSERT(cpu.F & Z80_CF, "borrow");
    ASSERT(cpu.F & Z80_SF, "sign");
    return 1;
}

static int test_sbc(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x30; cpu.B = 0x10;
    cpu.F = Z80_CF;
    test_mem[0] = 0x98; /* SBC A, B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x1F, "A=0x30-0x10-carry");
    return 1;
}

static int test_and(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0xF0; cpu.B = 0x3C;
    test_mem[0] = 0xA0; /* AND B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x30, "A");
    ASSERT(cpu.F & Z80_HF, "half carry set");
    ASSERT(!(cpu.F & Z80_NF), "not subtract");
    ASSERT(!(cpu.F & Z80_CF), "no carry");
    return 1;
}

static int test_or(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0xF0; cpu.B = 0x0F;
    test_mem[0] = 0xB0; /* OR B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0xFF, "A");
    ASSERT(!(cpu.F & Z80_ZF), "not zero");
    return 1;
}

static int test_xor(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0xFF; cpu.B = 0xFF;
    test_mem[0] = 0xA8; /* XOR B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x00, "A");
    ASSERT(cpu.F & Z80_ZF, "zero");
    ASSERT(cpu.F & Z80_PF, "parity even");
    return 1;
}

static int test_cp(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x42; cpu.B = 0x42;
    test_mem[0] = 0xB8; /* CP B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x42, "A unchanged");
    ASSERT(cpu.F & Z80_ZF, "zero (equal)");
    ASSERT(cpu.F & Z80_NF, "subtract");
    return 1;
}

static int test_cp_f3f5(void) {
    /* CP sets F3/F5 from the operand, not the result */
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x00;
    test_mem[0] = 0xFE; test_mem[1] = 0x28; /* CP 0x28 */
    z80_step(&cpu);
    ASSERT(cpu.F & Z80_F3, "F3 from operand");
    ASSERT(cpu.F & Z80_F5, "F5 from operand");
    return 1;
}

static int test_inc_reg(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0xFF;
    test_mem[0] = 0x04; /* INC B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x00, "B");
    ASSERT(cpu.F & Z80_ZF, "zero");
    ASSERT(cpu.F & Z80_HF, "half carry");
    return 1;
}

static int test_inc_overflow(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x7F;
    test_mem[0] = 0x04; /* INC B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x80, "B");
    ASSERT(cpu.F & Z80_PF, "overflow");
    ASSERT(cpu.F & Z80_SF, "sign");
    return 1;
}

static int test_dec_reg(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x01;
    test_mem[0] = 0x05; /* DEC B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x00, "B");
    ASSERT(cpu.F & Z80_ZF, "zero");
    ASSERT(cpu.F & Z80_NF, "subtract");
    return 1;
}

static int test_dec_underflow(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x80;
    test_mem[0] = 0x05; /* DEC B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x7F, "B");
    ASSERT(cpu.F & Z80_PF, "overflow");
    ASSERT(cpu.F & Z80_HF, "half carry");
    return 1;
}

static int test_inc_dec_16(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x12; cpu.C = 0xFF;
    test_mem[0] = 0x03; /* INC BC */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x13, "B"); ASSERT_EQ(cpu.C, 0x00, "C");

    cpu.PC = 0;
    test_mem[0] = 0x0B; /* DEC BC */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x12, "B"); ASSERT_EQ(cpu.C, 0xFF, "C");
    return 1;
}

static int test_add_hl_rp(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0x10; cpu.L = 0x00;
    cpu.B = 0x20; cpu.C = 0x00;
    test_mem[0] = 0x09; /* ADD HL, BC */
    z80_step(&cpu);
    ASSERT_EQ(cpu.H, 0x30, "H"); ASSERT_EQ(cpu.L, 0x00, "L");
    ASSERT(!(cpu.F & Z80_NF), "not subtract");
    return 1;
}

static int test_add_hl_carry(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0xFF; cpu.L = 0xFF;
    cpu.B = 0x00; cpu.C = 0x01;
    test_mem[0] = 0x09; /* ADD HL, BC */
    z80_step(&cpu);
    ASSERT_EQ(cpu.H, 0x00, "H"); ASSERT_EQ(cpu.L, 0x00, "L");
    ASSERT(cpu.F & Z80_CF, "carry");
    return 1;
}

static int test_rlca(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x85; /* 10000101 */
    test_mem[0] = 0x07; /* RLCA */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x0B, "A rotated left"); /* 00001011 */
    ASSERT(cpu.F & Z80_CF, "carry=old bit 7");
    return 1;
}

static int test_rrca(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x85; /* 10000101 */
    test_mem[0] = 0x0F; /* RRCA */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0xC2, "A rotated right"); /* 11000010 */
    ASSERT(cpu.F & Z80_CF, "carry=old bit 0");
    return 1;
}

static int test_rla(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x85; /* 10000101 */
    cpu.F = Z80_CF;
    test_mem[0] = 0x17; /* RLA */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x0B, "A"); /* 00001011 - old carry into bit 0 */
    ASSERT(cpu.F & Z80_CF, "carry=old bit 7");
    return 1;
}

static int test_rra(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x85; /* 10000101 */
    cpu.F = Z80_CF;
    test_mem[0] = 0x1F; /* RRA */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0xC2, "A"); /* 11000010 - old carry into bit 7 */
    ASSERT(cpu.F & Z80_CF, "carry=old bit 0");
    return 1;
}

static int test_cb_rlc(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x85;
    test_mem[0] = 0xCB; test_mem[1] = 0x00; /* RLC B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x0B, "B");
    ASSERT(cpu.F & Z80_CF, "carry");
    return 1;
}

static int test_cb_rrc(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x85;
    test_mem[0] = 0xCB; test_mem[1] = 0x08; /* RRC B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0xC2, "B");
    ASSERT(cpu.F & Z80_CF, "carry");
    return 1;
}

static int test_cb_sla(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x85;
    test_mem[0] = 0xCB; test_mem[1] = 0x20; /* SLA B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x0A, "B");
    ASSERT(cpu.F & Z80_CF, "carry=old bit 7");
    return 1;
}

static int test_cb_sra(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x85;
    test_mem[0] = 0xCB; test_mem[1] = 0x28; /* SRA B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0xC2, "B"); /* sign bit preserved */
    ASSERT(cpu.F & Z80_CF, "carry=old bit 0");
    return 1;
}

static int test_cb_srl(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x85;
    test_mem[0] = 0xCB; test_mem[1] = 0x38; /* SRL B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x42, "B");
    ASSERT(cpu.F & Z80_CF, "carry=old bit 0");
    return 1;
}

static int test_cb_bit(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x80;
    test_mem[0] = 0xCB; test_mem[1] = 0x78; /* BIT 7, B */
    z80_step(&cpu);
    ASSERT(!(cpu.F & Z80_ZF), "bit 7 set");
    ASSERT(cpu.F & Z80_HF, "H flag");

    cpu.PC = 0;
    test_mem[0] = 0xCB; test_mem[1] = 0x40; /* BIT 0, B */
    z80_step(&cpu);
    ASSERT(cpu.F & Z80_ZF, "bit 0 clear");
    return 1;
}

static int test_cb_set_res(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x00;
    test_mem[0] = 0xCB; test_mem[1] = 0xF8; /* SET 7, B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x80, "B set bit 7");

    cpu.PC = 0;
    test_mem[0] = 0xCB; test_mem[1] = 0xB8; /* RES 7, B */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x00, "B res bit 7");
    return 1;
}

static int test_jp(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    test_mem[0] = 0xC3; test_mem[1] = 0x00; test_mem[2] = 0x10; /* JP 0x1000 */
    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1000, "PC");
    return 1;
}

static int test_jp_cc(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.F = Z80_ZF;
    test_mem[0] = 0xCA; test_mem[1] = 0x00; test_mem[2] = 0x10; /* JP Z, 0x1000 */
    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1000, "taken");

    setup_cpu(&cpu);
    cpu.F = 0;
    test_mem[0] = 0xCA; test_mem[1] = 0x00; test_mem[2] = 0x10; /* JP Z, 0x1000 */
    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 3, "not taken");
    return 1;
}

static int test_jr(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    test_mem[0] = 0x18; test_mem[1] = 0x05; /* JR +5 */
    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 7, "PC=2+5");
    return 1;
}

static int test_jr_backward(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.PC = 0x10;
    test_mem[0x10] = 0x18; test_mem[0x11] = 0xFE; /* JR -2 (infinite loop) */
    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x10, "PC loops back");
    return 1;
}

static int test_jr_cc(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.F = Z80_ZF;
    test_mem[0] = 0x28; test_mem[1] = 0x05; /* JR Z, +5 */
    int t = z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 7, "taken");
    ASSERT_EQ(t, 12, "T-states taken");

    setup_cpu(&cpu);
    cpu.F = 0;
    test_mem[0] = 0x28; test_mem[1] = 0x05; /* JR Z, +5 */
    t = z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 2, "not taken");
    ASSERT_EQ(t, 7, "T-states not taken");
    return 1;
}

static int test_djnz(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 2;
    test_mem[0] = 0x10; test_mem[1] = 0xFE; /* DJNZ -2 */
    int t = z80_step(&cpu);
    ASSERT_EQ(cpu.B, 1, "B");
    ASSERT_EQ(cpu.PC, 0, "loops back");
    ASSERT_EQ(t, 13, "T taken");

    t = z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0, "B=0");
    ASSERT_EQ(cpu.PC, 2, "falls through");
    ASSERT_EQ(t, 8, "T not taken");
    return 1;
}

static int test_call_ret(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    test_mem[0] = 0xCD; test_mem[1] = 0x00; test_mem[2] = 0x10; /* CALL 0x1000 */
    test_mem[0x1000] = 0xC9; /* RET */
    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1000, "PC at subroutine");
    ASSERT_EQ(cpu.SP, 0xFFFC, "SP decremented");

    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x03, "returned");
    ASSERT_EQ(cpu.SP, 0xFFFE, "SP restored");
    return 1;
}

static int test_call_cc(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    cpu.F = Z80_ZF;
    test_mem[0] = 0xCC; test_mem[1] = 0x00; test_mem[2] = 0x10; /* CALL Z, 0x1000 */
    int t = z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1000, "taken");
    ASSERT_EQ(t, 17, "T taken");

    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    cpu.F = 0;
    test_mem[0] = 0xCC; test_mem[1] = 0x00; test_mem[2] = 0x10; /* CALL Z, 0x1000 */
    t = z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 3, "not taken");
    ASSERT_EQ(t, 10, "T not taken");
    return 1;
}

static int test_ret_cc(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFC;
    test_mem[0xFFFC] = 0x00; test_mem[0xFFFD] = 0x10; /* return to 0x1000 */
    cpu.F = Z80_ZF;
    test_mem[0] = 0xC8; /* RET Z */
    int t = z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1000, "taken");
    ASSERT_EQ(t, 11, "T taken");

    setup_cpu(&cpu);
    cpu.SP = 0xFFFC;
    cpu.F = 0;
    test_mem[0] = 0xC8; /* RET Z */
    t = z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 1, "not taken");
    ASSERT_EQ(t, 5, "T not taken");
    return 1;
}

static int test_push_pop(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    cpu.B = 0x12; cpu.C = 0x34;
    test_mem[0] = 0xC5; /* PUSH BC */
    test_mem[1] = 0xD1; /* POP DE */
    z80_step(&cpu);
    ASSERT_EQ(cpu.SP, 0xFFFC, "SP after push");
    z80_step(&cpu);
    ASSERT_EQ(cpu.D, 0x12, "D"); ASSERT_EQ(cpu.E, 0x34, "E");
    ASSERT_EQ(cpu.SP, 0xFFFE, "SP after pop");
    return 1;
}

static int test_push_pop_af(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    cpu.A = 0xAB; cpu.F = 0xCD;
    test_mem[0] = 0xF5; /* PUSH AF */
    test_mem[1] = 0x01; test_mem[2] = 0x00; test_mem[3] = 0x00; /* LD BC, 0 (clear regs) */
    test_mem[4] = 0xF1; /* POP AF */
    z80_step(&cpu);
    cpu.A = 0; cpu.F = 0;
    z80_step(&cpu);
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0xAB, "A"); ASSERT_EQ(cpu.F, 0xCD, "F");
    return 1;
}

static int test_rst(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    test_mem[0] = 0xFF; /* RST 38h */
    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x0038, "PC");
    ASSERT_EQ(cpu.SP, 0xFFFC, "SP");
    return 1;
}

static int test_halt(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    test_mem[0] = 0x76; /* HALT */
    z80_step(&cpu);
    ASSERT(cpu.halted, "halted");
    ASSERT_EQ(cpu.PC, 0, "PC stays at HALT");

    /* Step again while halted */
    int t = z80_step(&cpu);
    ASSERT_EQ(t, 4, "T while halted");
    ASSERT_EQ(cpu.PC, 0, "PC unchanged");
    return 1;
}

static int test_ex_af(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x11; cpu.F = 0x22;
    cpu.A_ = 0x33; cpu.F_ = 0x44;
    test_mem[0] = 0x08; /* EX AF, AF' */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x33, "A"); ASSERT_EQ(cpu.F, 0x44, "F");
    ASSERT_EQ(cpu.A_, 0x11, "A'"); ASSERT_EQ(cpu.F_, 0x22, "F'");
    return 1;
}

static int test_exx(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x01; cpu.C = 0x02; cpu.D = 0x03; cpu.E = 0x04;
    cpu.H = 0x05; cpu.L = 0x06;
    cpu.B_ = 0x11; cpu.C_ = 0x12; cpu.D_ = 0x13; cpu.E_ = 0x14;
    cpu.H_ = 0x15; cpu.L_ = 0x16;
    test_mem[0] = 0xD9; /* EXX */
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0x11, "B"); ASSERT_EQ(cpu.C, 0x12, "C");
    ASSERT_EQ(cpu.D, 0x13, "D"); ASSERT_EQ(cpu.E, 0x14, "E");
    ASSERT_EQ(cpu.H, 0x15, "H"); ASSERT_EQ(cpu.L, 0x16, "L");
    ASSERT_EQ(cpu.B_, 0x01, "B'"); ASSERT_EQ(cpu.C_, 0x02, "C'");
    return 1;
}

static int test_ex_de_hl(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.D = 0x12; cpu.E = 0x34;
    cpu.H = 0x56; cpu.L = 0x78;
    test_mem[0] = 0xEB; /* EX DE, HL */
    z80_step(&cpu);
    ASSERT_EQ(cpu.D, 0x56, "D"); ASSERT_EQ(cpu.E, 0x78, "E");
    ASSERT_EQ(cpu.H, 0x12, "H"); ASSERT_EQ(cpu.L, 0x34, "L");
    return 1;
}

static int test_ex_sp_hl(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0x8000;
    cpu.H = 0x12; cpu.L = 0x34;
    test_mem[0x8000] = 0x78; test_mem[0x8001] = 0x56;
    test_mem[0] = 0xE3; /* EX (SP), HL */
    z80_step(&cpu);
    ASSERT_EQ(cpu.H, 0x56, "H"); ASSERT_EQ(cpu.L, 0x78, "L");
    ASSERT_EQ(test_mem[0x8000], 0x34, "low"); ASSERT_EQ(test_mem[0x8001], 0x12, "high");
    return 1;
}

static int test_daa_add(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    /* 15 + 27 = 42 in BCD */
    cpu.A = 0x15;
    cpu.B = 0x27;
    test_mem[0] = 0x80; /* ADD A, B */
    test_mem[1] = 0x27; /* DAA */
    z80_step(&cpu); /* ADD: A = 0x3C */
    z80_step(&cpu); /* DAA: A = 0x42 */
    ASSERT_EQ(cpu.A, 0x42, "BCD result");
    return 1;
}

static int test_daa_sub(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    /* 42 - 15 = 27 in BCD */
    cpu.A = 0x42;
    cpu.B = 0x15;
    test_mem[0] = 0x90; /* SUB B */
    test_mem[1] = 0x27; /* DAA */
    z80_step(&cpu); /* SUB: A = 0x2D */
    z80_step(&cpu); /* DAA: A = 0x27 */
    ASSERT_EQ(cpu.A, 0x27, "BCD result");
    return 1;
}

static int test_cpl(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x55;
    test_mem[0] = 0x2F; /* CPL */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0xAA, "A complemented");
    ASSERT(cpu.F & Z80_HF, "H flag");
    ASSERT(cpu.F & Z80_NF, "N flag");
    return 1;
}

static int test_neg(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x01;
    test_mem[0] = 0xED; test_mem[1] = 0x44; /* NEG */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0xFF, "A negated");
    ASSERT(cpu.F & Z80_CF, "carry");
    ASSERT(cpu.F & Z80_NF, "subtract");

    /* NEG 0 = 0, no carry */
    setup_cpu(&cpu);
    cpu.A = 0x00;
    test_mem[0] = 0xED; test_mem[1] = 0x44;
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x00, "0 negated");
    ASSERT(!(cpu.F & Z80_CF), "no carry for 0");

    /* NEG 0x80 = 0x80 with overflow */
    setup_cpu(&cpu);
    cpu.A = 0x80;
    test_mem[0] = 0xED; test_mem[1] = 0x44;
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x80, "0x80 negated");
    ASSERT(cpu.F & Z80_PF, "overflow");
    return 1;
}

static int test_scf(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.F = 0;
    test_mem[0] = 0x37; /* SCF */
    z80_step(&cpu);
    ASSERT(cpu.F & Z80_CF, "carry set");
    ASSERT(!(cpu.F & Z80_NF), "N clear");
    ASSERT(!(cpu.F & Z80_HF), "H clear");
    return 1;
}

static int test_ccf(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.F = Z80_CF;
    test_mem[0] = 0x3F; /* CCF */
    z80_step(&cpu);
    ASSERT(!(cpu.F & Z80_CF), "carry complemented");
    ASSERT(cpu.F & Z80_HF, "H = old carry");
    return 1;
}

static int test_di_ei(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IFF1 = 1; cpu.IFF2 = 1;
    test_mem[0] = 0xF3; /* DI */
    z80_step(&cpu);
    ASSERT_EQ(cpu.IFF1, 0, "IFF1");
    ASSERT_EQ(cpu.IFF2, 0, "IFF2");

    test_mem[1] = 0xFB; /* EI */
    z80_step(&cpu);
    ASSERT_EQ(cpu.IFF1, 1, "IFF1");
    ASSERT_EQ(cpu.IFF2, 1, "IFF2");
    ASSERT_EQ(cpu.ei_delay, 1, "EI delay");

    /* After next instruction, EI delay clears */
    test_mem[2] = 0x00; /* NOP */
    z80_step(&cpu);
    ASSERT_EQ(cpu.ei_delay, 0, "delay cleared");
    return 1;
}

static int test_im_modes(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    test_mem[0] = 0xED; test_mem[1] = 0x46; /* IM 0 */
    z80_step(&cpu);
    ASSERT_EQ(cpu.IM, 0, "IM 0");

    test_mem[2] = 0xED; test_mem[3] = 0x56; /* IM 1 */
    z80_step(&cpu);
    ASSERT_EQ(cpu.IM, 1, "IM 1");

    test_mem[4] = 0xED; test_mem[5] = 0x5E; /* IM 2 */
    z80_step(&cpu);
    ASSERT_EQ(cpu.IM, 2, "IM 2");
    return 1;
}

static int test_interrupt_im1(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    cpu.IM = 1;
    cpu.IFF1 = 1; cpu.IFF2 = 1;
    cpu.PC = 0x1234;
    test_mem[0x0038] = 0xC9; /* RET at interrupt vector */

    z80_interrupt(&cpu, 0xFF);
    ASSERT_EQ(cpu.PC, 0x0038, "PC at 0x38");
    ASSERT_EQ(cpu.IFF1, 0, "IFF1 disabled");
    ASSERT_EQ(cpu.halted, 0, "not halted");

    /* Return from interrupt */
    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1234, "returned");
    return 1;
}

static int test_interrupt_im2(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    cpu.IM = 2;
    cpu.IFF1 = 1; cpu.IFF2 = 1;
    cpu.I = 0x80;
    cpu.PC = 0x1234;
    /* Vector table: I << 8 | data = 0x8020 -> points to 0x5000 */
    test_mem[0x8020] = 0x00; test_mem[0x8021] = 0x50;
    test_mem[0x5000] = 0xC9; /* RET */

    z80_interrupt(&cpu, 0x20);
    ASSERT_EQ(cpu.PC, 0x5000, "vectored to ISR");

    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1234, "returned");
    return 1;
}

static int test_nmi(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    cpu.IFF1 = 1; cpu.IFF2 = 1;
    cpu.PC = 0x1234;
    test_mem[0x0066] = 0xC9; /* RET at NMI vector */

    z80_nmi(&cpu);
    ASSERT_EQ(cpu.PC, 0x0066, "NMI vector");
    ASSERT_EQ(cpu.IFF1, 0, "IFF1 disabled");
    ASSERT_EQ(cpu.IFF2, 1, "IFF2 preserved");
    return 1;
}

static int test_ei_delay(void) {
    /* EI should not take effect until after the next instruction */
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    cpu.IM = 1;
    test_mem[0] = 0xFB; /* EI */
    test_mem[1] = 0x00; /* NOP */
    test_mem[2] = 0x00; /* NOP */

    z80_step(&cpu); /* EI - sets ei_delay */
    ASSERT_EQ(cpu.IFF1, 1, "IFF1 set");
    ASSERT_EQ(cpu.ei_delay, 1, "delay active");

    /* Interrupt during EI delay should be blocked */
    z80_interrupt(&cpu, 0xFF);
    ASSERT_EQ(cpu.PC, 1, "interrupt blocked during delay");

    z80_step(&cpu); /* NOP - clears delay */
    ASSERT_EQ(cpu.ei_delay, 0, "delay cleared");

    /* Now interrupt should work */
    z80_interrupt(&cpu, 0xFF);
    ASSERT_EQ(cpu.PC, 0x0038, "interrupt accepted");
    return 1;
}

static int test_in_out_n(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x42;
    test_mem[0] = 0xD3; test_mem[1] = 0x10; /* OUT (0x10), A */
    z80_step(&cpu);
    ASSERT_EQ(last_out_val, 0x42, "output value");

    cpu.PC = 0; cpu.A = 0;
    test_mem[0] = 0xDB; test_mem[1] = 0x10; /* IN A, (0x10) */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x42, "input value");
    return 1;
}

static int test_in_c(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x00; cpu.C = 0x20;
    io_ports[0x20] = 0x55;
    test_mem[0] = 0xED; test_mem[1] = 0x78; /* IN A, (C) */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x55, "A");
    ASSERT(!(cpu.F & Z80_NF), "N clear");
    return 1;
}

static int test_out_c(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x00; cpu.C = 0x20;
    cpu.A = 0xAA;
    test_mem[0] = 0xED; test_mem[1] = 0x79; /* OUT (C), A */
    z80_step(&cpu);
    ASSERT_EQ(last_out_val, 0xAA, "output");
    return 1;
}

static int test_ld_i_a(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x42;
    test_mem[0] = 0xED; test_mem[1] = 0x47; /* LD I, A */
    z80_step(&cpu);
    ASSERT_EQ(cpu.I, 0x42, "I");

    cpu.PC = 0;
    cpu.A = 0;
    test_mem[0] = 0xED; test_mem[1] = 0x57; /* LD A, I */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x42, "A=I");
    return 1;
}

static int test_ld_r_a(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x42;
    test_mem[0] = 0xED; test_mem[1] = 0x4F; /* LD R, A */
    z80_step(&cpu);
    /* R gets incremented by step(), so it won't be exactly 0x42 */
    /* But LD R,A should set it */

    cpu.PC = 0;
    cpu.A = 0;
    test_mem[0] = 0xED; test_mem[1] = 0x5F; /* LD A, R */
    z80_step(&cpu);
    /* A should be R value (which was set to 0x42, then incremented) */
    ASSERT(cpu.A != 0, "A loaded from R");
    return 1;
}

static int test_ed_ld_rp_nn(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0xAB; cpu.C = 0xCD;
    /* ED 43 nn nn = LD (nn), BC */
    test_mem[0] = 0xED; test_mem[1] = 0x43;
    test_mem[2] = 0x00; test_mem[3] = 0x50;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5000], 0xCD, "low");
    ASSERT_EQ(test_mem[0x5001], 0xAB, "high");

    cpu.PC = 0; cpu.B = 0; cpu.C = 0;
    /* ED 4B nn nn = LD BC, (nn) */
    test_mem[0] = 0xED; test_mem[1] = 0x4B;
    test_mem[2] = 0x00; test_mem[3] = 0x50;
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0xAB, "B"); ASSERT_EQ(cpu.C, 0xCD, "C");
    return 1;
}

static int test_sbc_hl(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0x50; cpu.L = 0x00;
    cpu.B = 0x20; cpu.C = 0x00;
    cpu.F = Z80_CF;
    /* ED 42 = SBC HL, BC */
    test_mem[0] = 0xED; test_mem[1] = 0x42;
    z80_step(&cpu);
    uint16_t hl = ((uint16_t)cpu.H << 8) | cpu.L;
    ASSERT_EQ(hl, 0x2FFF, "HL=0x5000-0x2000-1");
    ASSERT(cpu.F & Z80_NF, "N flag");
    return 1;
}

static int test_adc_hl(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0x50; cpu.L = 0x00;
    cpu.B = 0x20; cpu.C = 0x00;
    cpu.F = Z80_CF;
    /* ED 4A = ADC HL, BC */
    test_mem[0] = 0xED; test_mem[1] = 0x4A;
    z80_step(&cpu);
    uint16_t hl = ((uint16_t)cpu.H << 8) | cpu.L;
    ASSERT_EQ(hl, 0x7001, "HL=0x5000+0x2000+1");
    ASSERT(!(cpu.F & Z80_NF), "N clear");
    return 1;
}

static int test_ldi(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0x10; cpu.L = 0x00; /* HL = source */
    cpu.D = 0x20; cpu.E = 0x00; /* DE = dest */
    cpu.B = 0x00; cpu.C = 0x03; /* BC = count */
    test_mem[0x1000] = 0xAA;
    /* ED A0 = LDI */
    test_mem[0] = 0xED; test_mem[1] = 0xA0;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x2000], 0xAA, "copied");
    ASSERT_EQ(cpu.H, 0x10, "H"); ASSERT_EQ(cpu.L, 0x01, "L inc");
    ASSERT_EQ(cpu.D, 0x20, "D"); ASSERT_EQ(cpu.E, 0x01, "E inc");
    uint16_t bc = ((uint16_t)cpu.B << 8) | cpu.C;
    ASSERT_EQ(bc, 2, "BC dec");
    ASSERT(cpu.F & Z80_PF, "BC != 0");
    return 1;
}

static int test_ldir(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0x10; cpu.L = 0x00;
    cpu.D = 0x20; cpu.E = 0x00;
    cpu.B = 0x00; cpu.C = 0x03;
    test_mem[0x1000] = 0xAA;
    test_mem[0x1001] = 0xBB;
    test_mem[0x1002] = 0xCC;
    /* ED B0 = LDIR */
    test_mem[0] = 0xED; test_mem[1] = 0xB0;

    /* LDIR repeats until BC=0 */
    for (int i = 0; i < 10; i++) { /* safety limit */
        z80_step(&cpu);
        uint16_t bc = ((uint16_t)cpu.B << 8) | cpu.C;
        if (bc == 0) break;
    }
    ASSERT_EQ(test_mem[0x2000], 0xAA, "byte 0");
    ASSERT_EQ(test_mem[0x2001], 0xBB, "byte 1");
    ASSERT_EQ(test_mem[0x2002], 0xCC, "byte 2");
    uint16_t bc = ((uint16_t)cpu.B << 8) | cpu.C;
    ASSERT_EQ(bc, 0, "BC=0");
    ASSERT(!(cpu.F & Z80_PF), "PF clear when BC=0");
    return 1;
}

static int test_ldd(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0x10; cpu.L = 0x02;
    cpu.D = 0x20; cpu.E = 0x02;
    cpu.B = 0x00; cpu.C = 0x03;
    test_mem[0x1002] = 0xDD;
    /* ED A8 = LDD */
    test_mem[0] = 0xED; test_mem[1] = 0xA8;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x2002], 0xDD, "copied");
    ASSERT_EQ(cpu.L, 0x01, "L dec");
    ASSERT_EQ(cpu.E, 0x01, "E dec");
    return 1;
}

static int test_cpi(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x42;
    cpu.H = 0x10; cpu.L = 0x00;
    cpu.B = 0x00; cpu.C = 0x03;
    test_mem[0x1000] = 0x42; /* match */
    /* ED A1 = CPI */
    test_mem[0] = 0xED; test_mem[1] = 0xA1;
    z80_step(&cpu);
    ASSERT(cpu.F & Z80_ZF, "found match");
    ASSERT_EQ(cpu.L, 0x01, "HL incremented");
    uint16_t bc = ((uint16_t)cpu.B << 8) | cpu.C;
    ASSERT_EQ(bc, 2, "BC decremented");
    return 1;
}

static int test_cpir(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0xCC;
    cpu.H = 0x10; cpu.L = 0x00;
    cpu.B = 0x00; cpu.C = 0x05;
    test_mem[0x1000] = 0xAA;
    test_mem[0x1001] = 0xBB;
    test_mem[0x1002] = 0xCC; /* match here */
    test_mem[0x1003] = 0xDD;
    /* ED B1 = CPIR */
    test_mem[0] = 0xED; test_mem[1] = 0xB1;

    for (int i = 0; i < 20; i++) {
        z80_step(&cpu);
        if (cpu.F & Z80_ZF) break;
        uint16_t bc = ((uint16_t)cpu.B << 8) | cpu.C;
        if (bc == 0) break;
    }
    ASSERT(cpu.F & Z80_ZF, "found");
    uint16_t hl = ((uint16_t)cpu.H << 8) | cpu.L;
    ASSERT_EQ(hl, 0x1003, "HL past match");
    return 1;
}

static int test_rrd(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x12;
    cpu.H = 0x50; cpu.L = 0x00;
    test_mem[0x5000] = 0x34;
    /* ED 67 = RRD */
    test_mem[0] = 0xED; test_mem[1] = 0x67;
    z80_step(&cpu);
    /* A low nibble = (HL) low nibble = 4, A = 0x14 */
    /* (HL) = (old A low << 4) | (old HL high) = 0x23 */
    ASSERT_EQ(cpu.A, 0x14, "A");
    ASSERT_EQ(test_mem[0x5000], 0x23, "(HL)");
    return 1;
}

static int test_rld(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x12;
    cpu.H = 0x50; cpu.L = 0x00;
    test_mem[0x5000] = 0x34;
    /* ED 6F = RLD */
    test_mem[0] = 0xED; test_mem[1] = 0x6F;
    z80_step(&cpu);
    /* A low nibble = (HL) high nibble = 3, A = 0x13 */
    /* (HL) = (old HL low << 4) | old A low = 0x42 */
    ASSERT_EQ(cpu.A, 0x13, "A");
    ASSERT_EQ(test_mem[0x5000], 0x42, "(HL)");
    return 1;
}

static int test_ix_load(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    /* LD IX, 0x1234 */
    test_mem[0] = 0xDD; test_mem[1] = 0x21;
    test_mem[2] = 0x34; test_mem[3] = 0x12;
    z80_step(&cpu);
    ASSERT_EQ(cpu.IX, 0x1234, "IX");
    return 1;
}

static int test_ix_indexed(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x5000;
    test_mem[0x5005] = 0xAB;
    /* LD A, (IX+5) = DD 7E 05 */
    test_mem[0] = 0xDD; test_mem[1] = 0x7E; test_mem[2] = 0x05;
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0xAB, "A=(IX+5)");
    return 1;
}

static int test_ix_store(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x5000;
    cpu.A = 0x99;
    /* LD (IX+3), A = DD 77 03 */
    test_mem[0] = 0xDD; test_mem[1] = 0x77; test_mem[2] = 0x03;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5003], 0x99, "(IX+3)");
    return 1;
}

static int test_ix_neg_offset(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x5005;
    test_mem[0x5000] = 0x77;
    /* LD A, (IX-5) = DD 7E FB (FB = -5 signed) */
    test_mem[0] = 0xDD; test_mem[1] = 0x7E; test_mem[2] = 0xFB;
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x77, "A=(IX-5)");
    return 1;
}

static int test_iy_load(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    /* LD IY, 0xABCD */
    test_mem[0] = 0xFD; test_mem[1] = 0x21;
    test_mem[2] = 0xCD; test_mem[3] = 0xAB;
    z80_step(&cpu);
    ASSERT_EQ(cpu.IY, 0xABCD, "IY");
    return 1;
}

static int test_iy_indexed(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IY = 0x6000;
    test_mem[0x6010] = 0xEE;
    /* LD B, (IY+0x10) = FD 46 10 */
    test_mem[0] = 0xFD; test_mem[1] = 0x46; test_mem[2] = 0x10;
    z80_step(&cpu);
    ASSERT_EQ(cpu.B, 0xEE, "B=(IY+16)");
    return 1;
}

static int test_ix_add(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x1000;
    cpu.B = 0x20; cpu.C = 0x00;
    /* ADD IX, BC = DD 09 */
    test_mem[0] = 0xDD; test_mem[1] = 0x09;
    z80_step(&cpu);
    ASSERT_EQ(cpu.IX, 0x3000, "IX");
    return 1;
}

static int test_ix_inc_dec(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x1234;
    test_mem[0] = 0xDD; test_mem[1] = 0x23; /* INC IX */
    z80_step(&cpu);
    ASSERT_EQ(cpu.IX, 0x1235, "IX inc");

    test_mem[2] = 0xDD; test_mem[3] = 0x2B; /* DEC IX */
    z80_step(&cpu);
    ASSERT_EQ(cpu.IX, 0x1234, "IX dec");
    return 1;
}

static int test_ix_push_pop(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    cpu.IX = 0xABCD;
    test_mem[0] = 0xDD; test_mem[1] = 0xE5; /* PUSH IX */
    test_mem[2] = 0xDD; test_mem[3] = 0x21; /* LD IX, 0 */
    test_mem[4] = 0x00; test_mem[5] = 0x00;
    test_mem[6] = 0xDD; test_mem[7] = 0xE1; /* POP IX */
    z80_step(&cpu);
    z80_step(&cpu);
    ASSERT_EQ(cpu.IX, 0x0000, "IX cleared");
    z80_step(&cpu);
    ASSERT_EQ(cpu.IX, 0xABCD, "IX restored");
    return 1;
}

static int test_ix_ex_sp(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0x8000;
    cpu.IX = 0x1234;
    test_mem[0x8000] = 0x78; test_mem[0x8001] = 0x56;
    /* EX (SP), IX = DD E3 */
    test_mem[0] = 0xDD; test_mem[1] = 0xE3;
    z80_step(&cpu);
    ASSERT_EQ(cpu.IX, 0x5678, "IX");
    ASSERT_EQ(test_mem[0x8000], 0x34, "low");
    ASSERT_EQ(test_mem[0x8001], 0x12, "high");
    return 1;
}

static int test_ix_jp(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x5000;
    /* JP (IX) = DD E9 */
    test_mem[0] = 0xDD; test_mem[1] = 0xE9;
    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x5000, "PC");
    return 1;
}

static int test_ixh_ixl(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x1234;
    /* LD A, IXH = DD 7C */
    test_mem[0] = 0xDD; test_mem[1] = 0x7C;
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x12, "A=IXH");

    /* LD A, IXL = DD 7D */
    test_mem[2] = 0xDD; test_mem[3] = 0x7D;
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x34, "A=IXL");
    return 1;
}

static int test_ix_inc_indexed(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x5000;
    test_mem[0x5003] = 0x41;
    /* INC (IX+3) = DD 34 03 */
    test_mem[0] = 0xDD; test_mem[1] = 0x34; test_mem[2] = 0x03;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5003], 0x42, "(IX+3) incremented");
    return 1;
}

static int test_ix_ld_n(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x5000;
    /* LD (IX+2), 0xAB = DD 36 02 AB */
    test_mem[0] = 0xDD; test_mem[1] = 0x36;
    test_mem[2] = 0x02; test_mem[3] = 0xAB;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5002], 0xAB, "(IX+2)=n");
    return 1;
}

static int test_ddcb_bit(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x5000;
    test_mem[0x5005] = 0x80; /* bit 7 set */
    /* BIT 7, (IX+5) = DD CB 05 7E */
    test_mem[0] = 0xDD; test_mem[1] = 0xCB;
    test_mem[2] = 0x05; test_mem[3] = 0x7E;
    z80_step(&cpu);
    ASSERT(!(cpu.F & Z80_ZF), "bit 7 is set");

    /* BIT 0, (IX+5) */
    cpu.PC = 0;
    test_mem[3] = 0x46; /* BIT 0 */
    z80_step(&cpu);
    ASSERT(cpu.F & Z80_ZF, "bit 0 is clear");
    return 1;
}

static int test_ddcb_set_res(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x5000;
    test_mem[0x5005] = 0x00;
    /* SET 3, (IX+5) = DD CB 05 DE */
    test_mem[0] = 0xDD; test_mem[1] = 0xCB;
    test_mem[2] = 0x05; test_mem[3] = 0xDE;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5005], 0x08, "bit 3 set");

    /* RES 3, (IX+5) = DD CB 05 9E */
    cpu.PC = 0;
    test_mem[3] = 0x9E;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5005], 0x00, "bit 3 reset");
    return 1;
}

static int test_ddcb_rotate(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x5000;
    test_mem[0x5005] = 0x85;
    /* RLC (IX+5) = DD CB 05 06 */
    test_mem[0] = 0xDD; test_mem[1] = 0xCB;
    test_mem[2] = 0x05; test_mem[3] = 0x06;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5005], 0x0B, "rotated");
    ASSERT(cpu.F & Z80_CF, "carry");
    return 1;
}

static int test_ix_alu(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x10;
    cpu.IX = 0x5000;
    test_mem[0x5005] = 0x20;
    /* ADD A, (IX+5) = DD 86 05 */
    test_mem[0] = 0xDD; test_mem[1] = 0x86; test_mem[2] = 0x05;
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x30, "A");
    return 1;
}

static int test_ld_sp_hl(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0x50; cpu.L = 0x00;
    test_mem[0] = 0xF9; /* LD SP, HL */
    z80_step(&cpu);
    ASSERT_EQ(cpu.SP, 0x5000, "SP");
    return 1;
}

static int test_add_imm(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x10;
    test_mem[0] = 0xC6; test_mem[1] = 0x20; /* ADD A, 0x20 */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x30, "A");
    return 1;
}

static int test_sub_imm(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0x30;
    test_mem[0] = 0xD6; test_mem[1] = 0x10; /* SUB 0x10 */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x20, "A");
    return 1;
}

static int test_and_imm(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.A = 0xFF;
    test_mem[0] = 0xE6; test_mem[1] = 0x0F; /* AND 0x0F */
    z80_step(&cpu);
    ASSERT_EQ(cpu.A, 0x0F, "A");
    return 1;
}

static int test_t_states_ld(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    /* LD B, n = 7 T */
    test_mem[0] = 0x06; test_mem[1] = 0x42;
    int t = z80_step(&cpu);
    ASSERT_EQ(t, 7, "LD B,n");

    /* LD B, C = 4 T */
    test_mem[2] = 0x41;
    t = z80_step(&cpu);
    ASSERT_EQ(t, 4, "LD B,C");

    /* LD BC, nn = 10 T */
    setup_cpu(&cpu);
    test_mem[0] = 0x01; test_mem[1] = 0x00; test_mem[2] = 0x00;
    t = z80_step(&cpu);
    ASSERT_EQ(t, 10, "LD BC,nn");
    return 1;
}

static int test_t_states_jp_call(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    /* JP nn = 10 T */
    test_mem[0] = 0xC3; test_mem[1] = 0x10; test_mem[2] = 0x00;
    int t = z80_step(&cpu);
    ASSERT_EQ(t, 10, "JP nn");

    /* CALL nn = 17 T */
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    test_mem[0] = 0xCD; test_mem[1] = 0x00; test_mem[2] = 0x10;
    t = z80_step(&cpu);
    ASSERT_EQ(t, 17, "CALL nn");

    /* RET = 10 T */
    cpu.PC = 0; cpu.SP = 0xFFFE;
    test_mem[0xFFFE] = 0x00; test_mem[0xFFFF] = 0x00;
    test_mem[0] = 0xC9;
    t = z80_step(&cpu);
    ASSERT_EQ(t, 10, "RET");
    return 1;
}

static int test_t_states_cb(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    /* RLC B = 8 T */
    test_mem[0] = 0xCB; test_mem[1] = 0x00;
    int t = z80_step(&cpu);
    ASSERT_EQ(t, 8, "RLC B");

    /* BIT 0, B = 8 T */
    test_mem[2] = 0xCB; test_mem[3] = 0x40;
    t = z80_step(&cpu);
    ASSERT_EQ(t, 8, "BIT 0,B");

    /* BIT 0, (HL) = 12 T */
    setup_cpu(&cpu);
    cpu.H = 0x50; cpu.L = 0x00;
    test_mem[0] = 0xCB; test_mem[1] = 0x46;
    t = z80_step(&cpu);
    ASSERT_EQ(t, 12, "BIT 0,(HL)");
    return 1;
}

static int test_t_states_ix(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    /* LD IX, nn = DD 21 nn nn = 4+14 T-states... actually 14 total */
    test_mem[0] = 0xDD; test_mem[1] = 0x21;
    test_mem[2] = 0x00; test_mem[3] = 0x50;
    int t = z80_step(&cpu);
    ASSERT_EQ(t, 14, "LD IX,nn");

    /* LD A, (IX+d) = DD 7E d = 19 T */
    test_mem[4] = 0xDD; test_mem[5] = 0x7E; test_mem[6] = 0x00;
    t = z80_step(&cpu);
    ASSERT_EQ(t, 19, "LD A,(IX+d)");
    return 1;
}

static int test_r_register(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.R = 0;
    test_mem[0] = 0x00; /* NOP */
    test_mem[1] = 0x00;
    test_mem[2] = 0x00;
    z80_step(&cpu);
    z80_step(&cpu);
    z80_step(&cpu);
    ASSERT_EQ(cpu.R & 0x7F, 3, "R incremented 3 times");
    return 1;
}

static int test_r_bit7_preserved(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.R = 0x80;
    test_mem[0] = 0x00;
    z80_step(&cpu);
    ASSERT(cpu.R & 0x80, "bit 7 preserved");
    ASSERT_EQ(cpu.R & 0x7F, 1, "lower bits increment");
    return 1;
}

static int test_ld_hl_n(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.H = 0x50; cpu.L = 0x00;
    /* LD (HL), 0xAB */
    test_mem[0] = 0x36; test_mem[1] = 0xAB;
    int t = z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5000], 0xAB, "(HL)=n");
    ASSERT_EQ(t, 10, "T-states");
    return 1;
}

static int test_ini_outi(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.B = 0x03; cpu.C = 0x10;
    cpu.H = 0x50; cpu.L = 0x00;
    io_ports[0x10] = 0xAA;
    /* INI = ED A2 */
    test_mem[0] = 0xED; test_mem[1] = 0xA2;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5000], 0xAA, "byte read from port");
    ASSERT_EQ(cpu.B, 0x02, "B decremented");
    ASSERT_EQ(cpu.L, 0x01, "HL incremented");

    /* OUTI = ED A3 */
    test_mem[0x5001] = 0xBB;
    cpu.PC = 0;
    test_mem[0] = 0xED; test_mem[1] = 0xA3;
    z80_step(&cpu);
    ASSERT_EQ(last_out_val, 0xBB, "byte written to port");
    ASSERT_EQ(cpu.B, 0x01, "B decremented");
    return 1;
}

static int test_retn(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFC;
    test_mem[0xFFFC] = 0x34; test_mem[0xFFFD] = 0x12;
    cpu.IFF1 = 0; cpu.IFF2 = 1;
    /* RETN = ED 45 */
    test_mem[0] = 0xED; test_mem[1] = 0x45;
    z80_step(&cpu);
    ASSERT_EQ(cpu.PC, 0x1234, "PC");
    ASSERT_EQ(cpu.IFF1, 1, "IFF1 restored from IFF2");
    return 1;
}

static int test_interrupt_unhalts(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xFFFE;
    cpu.IM = 1;
    cpu.IFF1 = 1; cpu.IFF2 = 1;
    cpu.halted = 1;
    cpu.PC = 0x1000;

    z80_interrupt(&cpu, 0xFF);
    ASSERT_EQ(cpu.halted, 0, "unhalted");
    ASSERT_EQ(cpu.PC, 0x0038, "vectored");
    /* Return address should be 0x1001 (past the HALT instruction) */
    uint16_t ret = test_mem[cpu.SP] | ((uint16_t)test_mem[cpu.SP + 1] << 8);
    ASSERT_EQ(ret, 0x1000, "return to HALT addr");
    return 1;
}

static int test_ed_nn_sp(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.SP = 0xABCD;
    /* LD (0x5000), SP = ED 73 00 50 */
    test_mem[0] = 0xED; test_mem[1] = 0x73;
    test_mem[2] = 0x00; test_mem[3] = 0x50;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5000], 0xCD, "low");
    ASSERT_EQ(test_mem[0x5001], 0xAB, "high");

    /* LD SP, (0x5000) = ED 7B 00 50 */
    cpu.PC = 0; cpu.SP = 0;
    test_mem[0] = 0xED; test_mem[1] = 0x7B;
    test_mem[2] = 0x00; test_mem[3] = 0x50;
    z80_step(&cpu);
    ASSERT_EQ(cpu.SP, 0xABCD, "SP");
    return 1;
}

static int test_ix_ld_nn(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.IX = 0x1234;
    /* LD (0x5000), IX = DD 22 00 50 */
    test_mem[0] = 0xDD; test_mem[1] = 0x22;
    test_mem[2] = 0x00; test_mem[3] = 0x50;
    z80_step(&cpu);
    ASSERT_EQ(test_mem[0x5000], 0x34, "low");
    ASSERT_EQ(test_mem[0x5001], 0x12, "high");

    /* LD IX, (0x5000) = DD 2A 00 50 */
    cpu.PC = 0; cpu.IX = 0;
    test_mem[0] = 0xDD; test_mem[1] = 0x2A;
    test_mem[2] = 0x00; test_mem[3] = 0x50;
    z80_step(&cpu);
    ASSERT_EQ(cpu.IX, 0x1234, "IX");
    return 1;
}

static int test_t_state_accumulator(void) {
    z80_t cpu;
    setup_cpu(&cpu);
    cpu.t_states = 0;
    test_mem[0] = 0x00; /* NOP = 4T */
    test_mem[1] = 0x00; /* NOP = 4T */
    z80_step(&cpu);
    z80_step(&cpu);
    ASSERT_EQ((unsigned)cpu.t_states, 8, "total T-states");
    return 1;
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    printf("Z80 CPU Test Suite\n");
    printf("==================\n\n");

    /* Register loads */
    RUN_TEST(test_nop);
    RUN_TEST(test_ld_reg_imm);
    RUN_TEST(test_ld_reg_reg);
    RUN_TEST(test_ld_reg16_imm);
    RUN_TEST(test_ld_indirect);
    RUN_TEST(test_ld_hl_indirect);
    RUN_TEST(test_ld_nn_a);
    RUN_TEST(test_ld_nn_hl);
    RUN_TEST(test_ld_hl_n);
    RUN_TEST(test_ld_sp_hl);

    /* 8-bit ALU */
    RUN_TEST(test_add_a);
    RUN_TEST(test_add_overflow);
    RUN_TEST(test_add_carry);
    RUN_TEST(test_adc);
    RUN_TEST(test_sub);
    RUN_TEST(test_sub_borrow);
    RUN_TEST(test_sbc);
    RUN_TEST(test_and);
    RUN_TEST(test_or);
    RUN_TEST(test_xor);
    RUN_TEST(test_cp);
    RUN_TEST(test_cp_f3f5);
    RUN_TEST(test_add_imm);
    RUN_TEST(test_sub_imm);
    RUN_TEST(test_and_imm);

    /* INC/DEC */
    RUN_TEST(test_inc_reg);
    RUN_TEST(test_inc_overflow);
    RUN_TEST(test_dec_reg);
    RUN_TEST(test_dec_underflow);
    RUN_TEST(test_inc_dec_16);

    /* 16-bit arithmetic */
    RUN_TEST(test_add_hl_rp);
    RUN_TEST(test_add_hl_carry);
    RUN_TEST(test_sbc_hl);
    RUN_TEST(test_adc_hl);

    /* Rotates and shifts */
    RUN_TEST(test_rlca);
    RUN_TEST(test_rrca);
    RUN_TEST(test_rla);
    RUN_TEST(test_rra);
    RUN_TEST(test_cb_rlc);
    RUN_TEST(test_cb_rrc);
    RUN_TEST(test_cb_sla);
    RUN_TEST(test_cb_sra);
    RUN_TEST(test_cb_srl);

    /* BIT/SET/RES */
    RUN_TEST(test_cb_bit);
    RUN_TEST(test_cb_set_res);

    /* Jumps and branches */
    RUN_TEST(test_jp);
    RUN_TEST(test_jp_cc);
    RUN_TEST(test_jr);
    RUN_TEST(test_jr_backward);
    RUN_TEST(test_jr_cc);
    RUN_TEST(test_djnz);

    /* Calls and returns */
    RUN_TEST(test_call_ret);
    RUN_TEST(test_call_cc);
    RUN_TEST(test_ret_cc);

    /* Stack */
    RUN_TEST(test_push_pop);
    RUN_TEST(test_push_pop_af);
    RUN_TEST(test_rst);

    /* HALT */
    RUN_TEST(test_halt);

    /* Exchange */
    RUN_TEST(test_ex_af);
    RUN_TEST(test_exx);
    RUN_TEST(test_ex_de_hl);
    RUN_TEST(test_ex_sp_hl);

    /* DAA, CPL, NEG, SCF, CCF */
    RUN_TEST(test_daa_add);
    RUN_TEST(test_daa_sub);
    RUN_TEST(test_cpl);
    RUN_TEST(test_neg);
    RUN_TEST(test_scf);
    RUN_TEST(test_ccf);

    /* Interrupts */
    RUN_TEST(test_di_ei);
    RUN_TEST(test_im_modes);
    RUN_TEST(test_interrupt_im1);
    RUN_TEST(test_interrupt_im2);
    RUN_TEST(test_nmi);
    RUN_TEST(test_ei_delay);
    RUN_TEST(test_interrupt_unhalts);

    /* I/O */
    RUN_TEST(test_in_out_n);
    RUN_TEST(test_in_c);
    RUN_TEST(test_out_c);

    /* ED prefix */
    RUN_TEST(test_ld_i_a);
    RUN_TEST(test_ld_r_a);
    RUN_TEST(test_ed_ld_rp_nn);
    RUN_TEST(test_ed_nn_sp);
    RUN_TEST(test_retn);

    /* Block operations */
    RUN_TEST(test_ldi);
    RUN_TEST(test_ldir);
    RUN_TEST(test_ldd);
    RUN_TEST(test_cpi);
    RUN_TEST(test_cpir);
    RUN_TEST(test_ini_outi);
    RUN_TEST(test_rrd);
    RUN_TEST(test_rld);

    /* IX/IY */
    RUN_TEST(test_ix_load);
    RUN_TEST(test_ix_indexed);
    RUN_TEST(test_ix_store);
    RUN_TEST(test_ix_neg_offset);
    RUN_TEST(test_iy_load);
    RUN_TEST(test_iy_indexed);
    RUN_TEST(test_ix_add);
    RUN_TEST(test_ix_inc_dec);
    RUN_TEST(test_ix_push_pop);
    RUN_TEST(test_ix_ex_sp);
    RUN_TEST(test_ix_jp);
    RUN_TEST(test_ixh_ixl);
    RUN_TEST(test_ix_inc_indexed);
    RUN_TEST(test_ix_ld_n);
    RUN_TEST(test_ix_ld_nn);
    RUN_TEST(test_ix_alu);

    /* DDCB/FDCB */
    RUN_TEST(test_ddcb_bit);
    RUN_TEST(test_ddcb_set_res);
    RUN_TEST(test_ddcb_rotate);

    /* T-state timing */
    RUN_TEST(test_t_states_ld);
    RUN_TEST(test_t_states_jp_call);
    RUN_TEST(test_t_states_cb);
    RUN_TEST(test_t_states_ix);
    RUN_TEST(test_t_state_accumulator);

    /* R register */
    RUN_TEST(test_r_register);
    RUN_TEST(test_r_bit7_preserved);

    printf("\n==================\n");
    printf("Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed) printf(", %d FAILED", tests_failed);
    printf("\n");

    return tests_failed ? 1 : 0;
}
