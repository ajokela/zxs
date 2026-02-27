/*
 * z80_test.c — Basic test harness for the Z80 emulator
 *
 * Loads small code sequences into memory, steps through them,
 * and verifies register/flag state.
 */

#include "z80.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t memory[65536];

static uint8_t mem_read(void *ctx, uint16_t addr) {
    (void)ctx;
    return memory[addr];
}

static void mem_write(void *ctx, uint16_t addr, uint8_t val) {
    (void)ctx;
    memory[addr] = val;
}

static uint8_t io_read(void *ctx, uint16_t port) {
    (void)ctx; (void)port;
    return 0xFF;
}

static void io_write(void *ctx, uint16_t port, uint8_t val) {
    (void)ctx; (void)port; (void)val;
}

static z80 cpu;
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static void reset(void) {
    memset(memory, 0, sizeof(memory));
    z80_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    cpu.io_read = io_read;
    cpu.io_write = io_write;
}

static void load(uint16_t addr, const uint8_t *code, int len) {
    memcpy(&memory[addr], code, len);
}

static void run_steps(int n) {
    for (int i = 0; i < n; i++)
        z80_step(&cpu);
}

static void test_check(const char *name, int cond) {
    tests_run++;
    if (cond) {
        tests_passed++;
        printf("  %-50s PASS\n", name);
    } else {
        tests_failed++;
        printf("  %-50s FAIL\n", name);
    }
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static void test_nop(void) {
    reset();
    uint8_t code[] = { 0x00 };
    load(0, code, sizeof(code));
    int clk = z80_step(&cpu);
    test_check("NOP", cpu.pc == 1 && clk == 4);
}

static void test_ld_reg_imm(void) {
    reset();
    uint8_t code[] = {
        0x3E, 0x42, 0x06, 0x10, 0x0E, 0x20,
        0x16, 0x30, 0x1E, 0x40, 0x26, 0x50, 0x2E, 0x60,
    };
    load(0, code, sizeof(code));
    run_steps(7);
    test_check("LD r, n (all registers)",
        cpu.a == 0x42 && cpu.b == 0x10 && cpu.c == 0x20 &&
        cpu.d == 0x30 && cpu.e == 0x40 && cpu.h == 0x50 && cpu.l == 0x60);
}

static void test_ld_reg_reg(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x42, 0x47, 0x48 };
    load(0, code, sizeof(code));
    run_steps(3);
    test_check("LD r, r' (B=A, C=B)",
        cpu.a == 0x42 && cpu.b == 0x42 && cpu.c == 0x42);
}

static void test_ld_rp_nn(void) {
    reset();
    uint8_t code[] = {
        0x01, 0x34, 0x12, 0x11, 0x78, 0x56,
        0x21, 0xBC, 0x9A, 0x31, 0xF0, 0xDE,
    };
    load(0, code, sizeof(code));
    run_steps(4);
    test_check("LD rp, nn",
        cpu.b == 0x12 && cpu.c == 0x34 &&
        cpu.d == 0x56 && cpu.e == 0x78 &&
        cpu.h == 0x9A && cpu.l == 0xBC &&
        cpu.sp == 0xDEF0);
}

static void test_add_a(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x10, 0x06, 0x20, 0x80 };
    load(0, code, sizeof(code));
    run_steps(3);
    test_check("ADD A, B (0x10 + 0x20)",
        cpu.a == 0x30 && !(cpu.f & Z80_ZF) && !(cpu.f & Z80_CF));
}

static void test_add_overflow(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x80, 0x06, 0x80, 0x80 };
    load(0, code, sizeof(code));
    run_steps(3);
    test_check("ADD A, B overflow (0x80 + 0x80)",
        cpu.a == 0x00 && (cpu.f & Z80_ZF) && (cpu.f & Z80_CF) &&
        (cpu.f & Z80_PF));
}

static void test_sub_a(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x30, 0x06, 0x10, 0x90 };
    load(0, code, sizeof(code));
    run_steps(3);
    test_check("SUB B (0x30 - 0x10)",
        cpu.a == 0x20 && (cpu.f & Z80_NF) && !(cpu.f & Z80_CF));
}

static void test_and_or_xor(void) {
    reset();
    uint8_t code[] = { 0x3E, 0xFF, 0x06, 0x0F, 0xA0 };
    load(0, code, sizeof(code));
    run_steps(3);
    test_check("AND B (0xFF & 0x0F)",
        cpu.a == 0x0F && (cpu.f & Z80_HF));

    reset();
    uint8_t code2[] = { 0x3E, 0xF0, 0x06, 0x0F, 0xB0 };
    load(0, code2, sizeof(code2));
    run_steps(3);
    test_check("OR B (0xF0 | 0x0F)", cpu.a == 0xFF);

    reset();
    uint8_t code3[] = { 0x3E, 0xFF, 0x06, 0xFF, 0xA8 };
    load(0, code3, sizeof(code3));
    run_steps(3);
    test_check("XOR B (0xFF ^ 0xFF)",
        cpu.a == 0x00 && (cpu.f & Z80_ZF) && (cpu.f & Z80_PF));
}

static void test_inc_dec(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x00, 0x3C, 0x3C, 0x3D };
    load(0, code, sizeof(code));
    run_steps(4);
    test_check("INC/DEC A", cpu.a == 0x01);
}

static void test_inc_dec_rp(void) {
    reset();
    uint8_t code[] = { 0x01, 0xFF, 0x00, 0x03, 0x0B };
    load(0, code, sizeof(code));
    run_steps(3);
    test_check("INC/DEC BC", cpu.b == 0x00 && cpu.c == 0xFF);
}

static void test_push_pop(void) {
    reset();
    uint8_t code[] = {
        0x31, 0x00, 0x10, 0x01, 0x34, 0x12,
        0xC5, 0x01, 0x00, 0x00, 0xC1,
    };
    load(0, code, sizeof(code));
    run_steps(5);
    test_check("PUSH/POP BC",
        cpu.b == 0x12 && cpu.c == 0x34 && cpu.sp == 0x1000);
}

static void test_jp(void) {
    reset();
    uint8_t code[] = { 0xC3, 0x10, 0x00 };
    memory[0x0010] = 0x3E;
    memory[0x0011] = 0x42;
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("JP nn", cpu.a == 0x42 && cpu.pc == 0x0012);
}

static void test_jr(void) {
    reset();
    uint8_t code[] = { 0x18, 0x02, 0x3E, 0x00, 0x3E, 0x42 };
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("JR d", cpu.a == 0x42);
}

static void test_call_ret(void) {
    reset();
    uint8_t code[] = { 0x31, 0x00, 0x10, 0xCD, 0x10, 0x00, 0x76 };
    memory[0x0010] = 0x3E;
    memory[0x0011] = 0x42;
    memory[0x0012] = 0xC9;
    load(0, code, sizeof(code));
    run_steps(4);
    test_check("CALL/RET", cpu.a == 0x42 && cpu.pc == 0x0006);
}

static void test_djnz(void) {
    reset();
    uint8_t code[] = { 0x06, 0x05, 0x3E, 0x00, 0x3C, 0x10, 0xFD };
    load(0, code, sizeof(code));
    run_steps(2 + 5 * 2);
    test_check("DJNZ loop", cpu.a == 0x05 && cpu.b == 0x00);
}

static void test_ex_af(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x42, 0x08, 0x3E, 0x00, 0x08 };
    load(0, code, sizeof(code));
    run_steps(4);
    test_check("EX AF, AF'", cpu.a == 0x42);
}

static void test_exx(void) {
    reset();
    uint8_t code[] = { 0x01, 0x34, 0x12, 0xD9, 0x01, 0x00, 0x00, 0xD9 };
    load(0, code, sizeof(code));
    run_steps(4);
    test_check("EXX", cpu.b == 0x12 && cpu.c == 0x34);
}

static void test_ld_mem(void) {
    reset();
    uint8_t code[] = { 0x21, 0x00, 0x80, 0x36, 0x42, 0x7E };
    load(0, code, sizeof(code));
    run_steps(3);
    test_check("LD (HL), n and LD A, (HL)",
        cpu.a == 0x42 && memory[0x8000] == 0x42);
}

static void test_add_hl_rp(void) {
    reset();
    uint8_t code[] = { 0x21, 0x00, 0x10, 0x01, 0x00, 0x01, 0x09 };
    load(0, code, sizeof(code));
    run_steps(3);
    uint16_t hl = (cpu.h << 8) | cpu.l;
    test_check("ADD HL, BC", hl == 0x1100);
}

static void test_cp(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x42, 0xFE, 0x42 };
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("CP n (equal)",
        (cpu.f & Z80_ZF) && !(cpu.f & Z80_CF) && cpu.a == 0x42);

    reset();
    uint8_t code2[] = { 0x3E, 0x10, 0xFE, 0x20 };
    load(0, code2, sizeof(code2));
    run_steps(2);
    test_check("CP n (less)",
        !(cpu.f & Z80_ZF) && (cpu.f & Z80_CF) && cpu.a == 0x10);
}

static void test_cb_bit(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x80, 0xCB, 0x47 };
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("BIT 0, A (bit not set)", cpu.f & Z80_ZF);

    reset();
    uint8_t code2[] = { 0x3E, 0x80, 0xCB, 0x7F };
    load(0, code2, sizeof(code2));
    run_steps(2);
    test_check("BIT 7, A (bit set)", !(cpu.f & Z80_ZF));
}

static void test_cb_set_res(void) {
    reset();
    uint8_t code[] = {
        0x3E, 0x00, 0xCB, 0xC7, 0xCB, 0xCF,
        0xCB, 0xD7, 0xCB, 0x87,
    };
    load(0, code, sizeof(code));
    run_steps(5);
    test_check("SET/RES bits in A", cpu.a == 0x06);
}

static void test_cb_rotate(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x80, 0xCB, 0x07 };
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("RLC A (0x80)", cpu.a == 0x01 && (cpu.f & Z80_CF));
}

static void test_halt(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x42, 0x76 };
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("HALT", cpu.halted == 1 && cpu.a == 0x42);
}

static void test_di_ei(void) {
    reset();
    uint8_t code[] = { 0xFB, 0x00, 0xF3 };
    load(0, code, sizeof(code));
    z80_step(&cpu);
    test_check("EI delay", cpu.iff1 == 0);

    z80_step(&cpu);
    test_check("EI takes effect after next instruction", cpu.iff1 == 1);

    z80_step(&cpu);
    test_check("DI", cpu.iff1 == 0);
}

static void test_ex_de_hl(void) {
    reset();
    uint8_t code[] = { 0x21, 0x34, 0x12, 0x11, 0x78, 0x56, 0xEB };
    load(0, code, sizeof(code));
    run_steps(3);
    uint16_t hl = (cpu.h << 8) | cpu.l;
    uint16_t de = (cpu.d << 8) | cpu.e;
    test_check("EX DE, HL", hl == 0x5678 && de == 0x1234);
}

static void test_rst(void) {
    reset();
    uint8_t code[] = { 0x31, 0x00, 0x10, 0xFF };
    memory[0x0038] = 0x3E;
    memory[0x0039] = 0x99;
    load(0, code, sizeof(code));
    run_steps(3);
    test_check("RST 38h", cpu.a == 0x99 && cpu.pc == 0x003A);
}

static void test_jr_cc(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x01, 0xFE, 0x01, 0x28, 0x02, 0x3E, 0x00, 0x3E, 0x42 };
    load(0, code, sizeof(code));
    run_steps(4);
    test_check("JR Z, d (taken)", cpu.a == 0x42);
}

static void test_ldir(void) {
    reset();
    memory[0x1000] = 0x11;
    memory[0x1001] = 0x22;
    memory[0x1002] = 0x33;
    memory[0x1003] = 0x44;
    uint8_t code[] = {
        0x21, 0x00, 0x10, 0x11, 0x00, 0x20,
        0x01, 0x04, 0x00, 0xED, 0xB0,
    };
    load(0, code, sizeof(code));
    run_steps(3 + 4);
    test_check("LDIR (copy 4 bytes)",
        memory[0x2000] == 0x11 && memory[0x2001] == 0x22 &&
        memory[0x2002] == 0x33 && memory[0x2003] == 0x44 &&
        cpu.b == 0 && cpu.c == 0);
}

static void test_ix_ld(void) {
    reset();
    uint8_t code[] = {
        0xDD, 0x21, 0x00, 0x80,
        0x3E, 0x42,
        0xDD, 0x77, 0x05,
        0xDD, 0x7E, 0x05,
    };
    load(0, code, sizeof(code));
    run_steps(3);
    cpu.a = 0;
    z80_step(&cpu);
    test_check("LD (IX+d) / LD r, (IX+d)",
        cpu.a == 0x42 && memory[0x8005] == 0x42);
}

static void test_daa(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x15, 0xC6, 0x27, 0x27 };
    load(0, code, sizeof(code));
    run_steps(3);
    test_check("DAA (0x15 + 0x27 = 0x42 BCD)", cpu.a == 0x42);
}

static void test_cpl(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x55, 0x2F };
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("CPL",
        cpu.a == 0xAA && (cpu.f & Z80_HF) && (cpu.f & Z80_NF));
}

static void test_scf_ccf(void) {
    reset();
    uint8_t code[] = { 0x37 };
    load(0, code, sizeof(code));
    run_steps(1);
    test_check("SCF", cpu.f & Z80_CF);

    reset();
    uint8_t code2[] = { 0x37, 0x3F };
    load(0, code2, sizeof(code2));
    run_steps(2);
    test_check("CCF (after SCF)",
        !(cpu.f & Z80_CF) && (cpu.f & Z80_HF));
}

static void test_neg(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x01, 0xED, 0x44 };
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("NEG (0x01 -> 0xFF)",
        cpu.a == 0xFF && (cpu.f & Z80_SF) && (cpu.f & Z80_CF));
}

static void test_rlca_rrca(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x85, 0x07 };
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("RLCA (0x85)", cpu.a == 0x0B && (cpu.f & Z80_CF));

    reset();
    uint8_t code2[] = { 0x3E, 0x85, 0x0F };
    load(0, code2, sizeof(code2));
    run_steps(2);
    test_check("RRCA (0x85)", cpu.a == 0xC2 && (cpu.f & Z80_CF));
}

static void test_ed_ld_i_a(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x42, 0xED, 0x47, 0x3E, 0x00, 0xED, 0x57 };
    load(0, code, sizeof(code));
    run_steps(4);
    test_check("LD I, A / LD A, I", cpu.a == 0x42 && cpu.i == 0x42);
}

static void test_sbc_hl(void) {
    reset();
    uint8_t code[] = { 0x21, 0x00, 0x10, 0x01, 0x00, 0x01, 0x37, 0xED, 0x42 };
    load(0, code, sizeof(code));
    run_steps(4);
    uint16_t hl = (cpu.h << 8) | cpu.l;
    test_check("SBC HL, BC (0x1000 - 0x0100 - 1)", hl == 0x0EFF);
}

static void test_adc_hl(void) {
    reset();
    uint8_t code[] = { 0x21, 0xFF, 0x0F, 0x01, 0x01, 0x00, 0x37, 0xED, 0x4A };
    load(0, code, sizeof(code));
    run_steps(4);
    uint16_t hl = (cpu.h << 8) | cpu.l;
    test_check("ADC HL, BC (0x0FFF + 0x0001 + 1)",
        hl == 0x1001 && (cpu.f & Z80_HF));
}

static void test_cpir(void) {
    reset();
    memory[0x1000] = 0x11;
    memory[0x1001] = 0x22;
    memory[0x1002] = 0x42;
    memory[0x1003] = 0x44;
    uint8_t code[] = { 0x3E, 0x42, 0x21, 0x00, 0x10, 0x01, 0x04, 0x00, 0xED, 0xB1 };
    load(0, code, sizeof(code));
    run_steps(3 + 3);
    uint16_t hl = (cpu.h << 8) | cpu.l;
    test_check("CPIR (find 0x42)", (cpu.f & Z80_ZF) && hl == 0x1003);
}

static void test_rlc_mem(void) {
    reset();
    memory[0x8000] = 0x85;
    uint8_t code[] = { 0x21, 0x00, 0x80, 0xCB, 0x06 };
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("RLC (HL)", memory[0x8000] == 0x0B && (cpu.f & Z80_CF));
}

static void test_sla_sra_srl(void) {
    reset();
    uint8_t code[] = { 0x3E, 0x85, 0xCB, 0x27 };
    load(0, code, sizeof(code));
    run_steps(2);
    test_check("SLA A (0x85)", cpu.a == 0x0A && (cpu.f & Z80_CF));

    reset();
    uint8_t code2[] = { 0x3E, 0x85, 0xCB, 0x2F };
    load(0, code2, sizeof(code2));
    run_steps(2);
    test_check("SRA A (0x85, sign-preserving)",
        cpu.a == 0xC2 && (cpu.f & Z80_CF));

    reset();
    uint8_t code3[] = { 0x3E, 0x85, 0xCB, 0x3F };
    load(0, code3, sizeof(code3));
    run_steps(2);
    test_check("SRL A (0x85)", cpu.a == 0x42 && (cpu.f & Z80_CF));
}

static void test_im_modes(void) {
    reset();
    uint8_t code[] = { 0xED, 0x46, 0xED, 0x56, 0xED, 0x5E };
    load(0, code, sizeof(code));
    z80_step(&cpu);
    test_check("IM 0", cpu.im == 0);
    z80_step(&cpu);
    test_check("IM 1", cpu.im == 1);
    z80_step(&cpu);
    test_check("IM 2", cpu.im == 2);
}

static void test_nmi(void) {
    reset();
    uint8_t code[] = { 0x31, 0x00, 0x10, 0xFB, 0x00 };
    memory[0x0066] = 0x3E;
    memory[0x0067] = 0x66;
    load(0, code, sizeof(code));
    run_steps(3);
    z80_nmi(&cpu);
    z80_step(&cpu);
    test_check("NMI", cpu.a == 0x66 && cpu.iff1 == 0 && cpu.pc == 0x0068);
}

static void test_ret_cc(void) {
    reset();
    uint8_t code[] = { 0x31, 0x00, 0x10, 0xCD, 0x10, 0x00, 0x76 };
    memory[0x0010] = 0xAF;
    memory[0x0011] = 0xC8;
    load(0, code, sizeof(code));
    run_steps(4);
    test_check("RET Z (taken)", cpu.pc == 0x0006 && cpu.a == 0x00);
}

static void test_timing(void) {
    reset();
    uint8_t code[] = { 0x00, 0x3E, 0x42, 0x47, 0x80, 0xC3, 0x00, 0x00 };
    load(0, code, sizeof(code));
    int total = 0;
    total += z80_step(&cpu);
    total += z80_step(&cpu);
    total += z80_step(&cpu);
    total += z80_step(&cpu);
    total += z80_step(&cpu);
    test_check("T-state timing (NOP+LD+LD+ADD+JP)", total == 29);
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("Z80 Emulator Tests\n");
    printf("==================\n\n");

    test_nop();
    test_ld_reg_imm();
    test_ld_reg_reg();
    test_ld_rp_nn();
    test_add_a();
    test_add_overflow();
    test_sub_a();
    test_and_or_xor();
    test_inc_dec();
    test_inc_dec_rp();
    test_push_pop();
    test_jp();
    test_jr();
    test_call_ret();
    test_djnz();
    test_ex_af();
    test_exx();
    test_ld_mem();
    test_add_hl_rp();
    test_cp();
    test_cb_bit();
    test_cb_set_res();
    test_cb_rotate();
    test_halt();
    test_di_ei();
    test_ex_de_hl();
    test_rst();
    test_jr_cc();
    test_ldir();
    test_ix_ld();
    test_daa();
    test_cpl();
    test_scf_ccf();
    test_neg();
    test_rlca_rrca();
    test_ed_ld_i_a();
    test_sbc_hl();
    test_adc_hl();
    test_cpir();
    test_rlc_mem();
    test_sla_sra_srl();
    test_im_modes();
    test_nmi();
    test_ret_cc();
    test_timing();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_failed ? 1 : 0;
}
