#include "z80.h"
#include <string.h>

/* Forward declarations */
static int exec_main_op(z80_t *c, uint8_t op);

/* ── Parity lookup table ─────────────────────────────────────────── */

static uint8_t parity_table[256];
static int parity_inited = 0;

static void init_parity(void) {
    if (parity_inited) return;
    for (int i = 0; i < 256; i++) {
        int bits = 0;
        for (int b = 0; b < 8; b++)
            if (i & (1 << b)) bits++;
        parity_table[i] = (bits & 1) ? 0 : Z80_PF;
    }
    parity_inited = 1;
}

/* ── Register pair helpers ───────────────────────────────────────── */

static inline uint16_t rp_bc(z80_t *c) { return ((uint16_t)c->B << 8) | c->C; }
static inline uint16_t rp_de(z80_t *c) { return ((uint16_t)c->D << 8) | c->E; }
static inline uint16_t rp_hl(z80_t *c) { return ((uint16_t)c->H << 8) | c->L; }
static inline uint16_t rp_af(z80_t *c) { return ((uint16_t)c->A << 8) | c->F; }

static inline void set_bc(z80_t *c, uint16_t v) { c->B = v >> 8; c->C = v & 0xFF; }
static inline void set_de(z80_t *c, uint16_t v) { c->D = v >> 8; c->E = v & 0xFF; }
static inline void set_hl(z80_t *c, uint16_t v) { c->H = v >> 8; c->L = v & 0xFF; }
static inline void set_af(z80_t *c, uint16_t v) { c->A = v >> 8; c->F = v & 0xFF; }

/* ── Memory access helpers ───────────────────────────────────────── */

static inline uint8_t rb(z80_t *c, uint16_t addr) {
    return c->mem_read(c->ctx, addr);
}

static inline void wb(z80_t *c, uint16_t addr, uint8_t val) {
    c->mem_write(c->ctx, addr, val);
}

static inline uint16_t rw(z80_t *c, uint16_t addr) {
    return rb(c, addr) | ((uint16_t)rb(c, addr + 1) << 8);
}

static inline void ww(z80_t *c, uint16_t addr, uint16_t val) {
    wb(c, addr, val & 0xFF);
    wb(c, addr + 1, val >> 8);
}

static inline uint8_t fetch8(z80_t *c) {
    return rb(c, c->PC++);
}

static inline uint16_t fetch16(z80_t *c) {
    uint16_t lo = fetch8(c);
    uint16_t hi = fetch8(c);
    return (hi << 8) | lo;
}

/* ── Stack helpers ───────────────────────────────────────────────── */

static inline void push16(z80_t *c, uint16_t val) {
    c->SP -= 2;
    ww(c, c->SP, val);
}

static inline uint16_t pop16(z80_t *c) {
    uint16_t val = rw(c, c->SP);
    c->SP += 2;
    return val;
}

/* ── I/O helpers ─────────────────────────────────────────────────── */

static inline uint8_t io_in(z80_t *c, uint16_t port) {
    return c->io_in(c->ctx, port);
}

static inline void io_out(z80_t *c, uint16_t port, uint8_t val) {
    c->io_out(c->ctx, port, val);
}

/* ── Flag helpers ────────────────────────────────────────────────── */

static inline uint8_t sz53(uint8_t val) {
    return (val & (Z80_SF | Z80_F5 | Z80_F3)) | (val == 0 ? Z80_ZF : 0);
}

static inline uint8_t sz53p(uint8_t val) {
    return sz53(val) | parity_table[val];
}

/* ── 8-bit register access by index ──────────────────────────────── */
/* Index: 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A */

static uint8_t get_reg8(z80_t *c, int idx) {
    switch (idx) {
        case 0: return c->B;
        case 1: return c->C;
        case 2: return c->D;
        case 3: return c->E;
        case 4: return c->H;
        case 5: return c->L;
        case 6: return rb(c, rp_hl(c));
        case 7: return c->A;
    }
    return 0;
}

static void set_reg8(z80_t *c, int idx, uint8_t val) {
    switch (idx) {
        case 0: c->B = val; break;
        case 1: c->C = val; break;
        case 2: c->D = val; break;
        case 3: c->E = val; break;
        case 4: c->H = val; break;
        case 5: c->L = val; break;
        case 6: wb(c, rp_hl(c), val); break;
        case 7: c->A = val; break;
    }
}


/* ── 16-bit register pair access ─────────────────────────────────── */
/* p index: 0=BC 1=DE 2=HL 3=SP */

static uint16_t get_rp(z80_t *c, int p) {
    switch (p) {
        case 0: return rp_bc(c);
        case 1: return rp_de(c);
        case 2: return rp_hl(c);
        case 3: return c->SP;
    }
    return 0;
}

static void set_rp(z80_t *c, int p, uint16_t val) {
    switch (p) {
        case 0: set_bc(c, val); break;
        case 1: set_de(c, val); break;
        case 2: set_hl(c, val); break;
        case 3: c->SP = val; break;
    }
}

/* p2 index: 0=BC 1=DE 2=HL 3=AF */
static uint16_t get_rp2(z80_t *c, int p) {
    switch (p) {
        case 0: return rp_bc(c);
        case 1: return rp_de(c);
        case 2: return rp_hl(c);
        case 3: return rp_af(c);
    }
    return 0;
}

static void set_rp2(z80_t *c, int p, uint16_t val) {
    switch (p) {
        case 0: set_bc(c, val); break;
        case 1: set_de(c, val); break;
        case 2: set_hl(c, val); break;
        case 3: set_af(c, val); break;
    }
}

/* ── Condition code evaluation ───────────────────────────────────── */
/* 0=NZ 1=Z 2=NC 3=C 4=PO 5=PE 6=P 7=M */

static int eval_cc(z80_t *c, int cc) {
    switch (cc) {
        case 0: return !(c->F & Z80_ZF);
        case 1: return  (c->F & Z80_ZF);
        case 2: return !(c->F & Z80_CF);
        case 3: return  (c->F & Z80_CF);
        case 4: return !(c->F & Z80_PF);
        case 5: return  (c->F & Z80_PF);
        case 6: return !(c->F & Z80_SF);
        case 7: return  (c->F & Z80_SF);
    }
    return 0;
}

/* ── ALU operations ──────────────────────────────────────────────── */

static void alu_add(z80_t *c, uint8_t val) {
    uint16_t r = c->A + val;
    uint8_t h = (c->A ^ val ^ r) & 0x10;
    uint8_t v = ((c->A ^ val ^ 0x80) & (c->A ^ r)) & 0x80;
    c->A = (uint8_t)r;
    c->F = sz53(c->A) | (r & 0x100 ? Z80_CF : 0) | (h ? Z80_HF : 0) | (v ? Z80_PF : 0);
}

static void alu_adc(z80_t *c, uint8_t val) {
    uint8_t carry = c->F & Z80_CF ? 1 : 0;
    uint16_t r = c->A + val + carry;
    uint8_t h = (c->A ^ val ^ r) & 0x10;
    uint8_t v = ((c->A ^ val ^ 0x80) & (c->A ^ r)) & 0x80;
    c->A = (uint8_t)r;
    c->F = sz53(c->A) | (r & 0x100 ? Z80_CF : 0) | (h ? Z80_HF : 0) | (v ? Z80_PF : 0);
}

static void alu_sub(z80_t *c, uint8_t val) {
    uint16_t r = c->A - val;
    uint8_t h = (c->A ^ val ^ r) & 0x10;
    uint8_t v = ((c->A ^ val) & (c->A ^ r)) & 0x80;
    c->A = (uint8_t)r;
    c->F = sz53(c->A) | Z80_NF | (r & 0x100 ? Z80_CF : 0) | (h ? Z80_HF : 0) | (v ? Z80_PF : 0);
}

static void alu_sbc(z80_t *c, uint8_t val) {
    uint8_t carry = c->F & Z80_CF ? 1 : 0;
    uint16_t r = c->A - val - carry;
    uint8_t h = (c->A ^ val ^ r) & 0x10;
    uint8_t v = ((c->A ^ val) & (c->A ^ r)) & 0x80;
    c->A = (uint8_t)r;
    c->F = sz53(c->A) | Z80_NF | (r & 0x100 ? Z80_CF : 0) | (h ? Z80_HF : 0) | (v ? Z80_PF : 0);
}

static void alu_and(z80_t *c, uint8_t val) {
    c->A &= val;
    c->F = sz53p(c->A) | Z80_HF;
}

static void alu_xor(z80_t *c, uint8_t val) {
    c->A ^= val;
    c->F = sz53p(c->A);
}

static void alu_or(z80_t *c, uint8_t val) {
    c->A |= val;
    c->F = sz53p(c->A);
}

static void alu_cp(z80_t *c, uint8_t val) {
    uint16_t r = c->A - val;
    uint8_t h = (c->A ^ val ^ r) & 0x10;
    uint8_t v = ((c->A ^ val) & (c->A ^ r)) & 0x80;
    /* Note: F3 and F5 come from the operand, not the result */
    c->F = (r & 0xFF ? 0 : Z80_ZF) | ((uint8_t)r & Z80_SF) | Z80_NF |
           (val & (Z80_F5 | Z80_F3)) |
           (r & 0x100 ? Z80_CF : 0) | (h ? Z80_HF : 0) | (v ? Z80_PF : 0);
}

static void do_alu(z80_t *c, int op, uint8_t val) {
    switch (op) {
        case 0: alu_add(c, val); break;
        case 1: alu_adc(c, val); break;
        case 2: alu_sub(c, val); break;
        case 3: alu_sbc(c, val); break;
        case 4: alu_and(c, val); break;
        case 5: alu_xor(c, val); break;
        case 6: alu_or(c, val);  break;
        case 7: alu_cp(c, val);  break;
    }
}

/* ── INC/DEC 8-bit ───────────────────────────────────────────────── */

static uint8_t inc8(z80_t *c, uint8_t val) {
    uint8_t r = val + 1;
    c->F = (c->F & Z80_CF) | sz53(r) |
           (r == 0x80 ? Z80_PF : 0) |
           ((r & 0x0F) == 0 ? Z80_HF : 0);
    return r;
}

static uint8_t dec8(z80_t *c, uint8_t val) {
    uint8_t r = val - 1;
    c->F = (c->F & Z80_CF) | sz53(r) | Z80_NF |
           (val == 0x80 ? Z80_PF : 0) |
           ((val & 0x0F) == 0 ? Z80_HF : 0);
    return r;
}

/* ── 16-bit ADD HL,rr ────────────────────────────────────────────── */

static void add_hl(z80_t *c, uint16_t *hl, uint16_t val) {
    uint32_t r = *hl + val;
    uint16_t h = (*hl ^ val ^ r) & 0x1000;
    c->F = (c->F & (Z80_SF | Z80_ZF | Z80_PF)) |
           ((r >> 8) & (Z80_F5 | Z80_F3)) |
           (r & 0x10000 ? Z80_CF : 0) |
           (h ? Z80_HF : 0);
    *hl = (uint16_t)r;
}

/* ── Rotate/shift helpers (CB prefix) ────────────────────────────── */

static uint8_t rlc(z80_t *c, uint8_t val) {
    uint8_t carry = val >> 7;
    uint8_t r = (val << 1) | carry;
    c->F = sz53p(r) | carry;
    return r;
}

static uint8_t rrc(z80_t *c, uint8_t val) {
    uint8_t carry = val & 1;
    uint8_t r = (val >> 1) | (carry << 7);
    c->F = sz53p(r) | carry;
    return r;
}

static uint8_t rl(z80_t *c, uint8_t val) {
    uint8_t carry = val >> 7;
    uint8_t r = (val << 1) | (c->F & Z80_CF);
    c->F = sz53p(r) | carry;
    return r;
}

static uint8_t rr(z80_t *c, uint8_t val) {
    uint8_t carry = val & 1;
    uint8_t r = (val >> 1) | ((c->F & Z80_CF) << 7);
    c->F = sz53p(r) | carry;
    return r;
}

static uint8_t sla(z80_t *c, uint8_t val) {
    uint8_t carry = val >> 7;
    uint8_t r = val << 1;
    c->F = sz53p(r) | carry;
    return r;
}

static uint8_t sra(z80_t *c, uint8_t val) {
    uint8_t carry = val & 1;
    uint8_t r = (val >> 1) | (val & 0x80);
    c->F = sz53p(r) | carry;
    return r;
}

static uint8_t sll(z80_t *c, uint8_t val) {
    /* Undocumented: shifts left, bit 0 = 1 */
    uint8_t carry = val >> 7;
    uint8_t r = (val << 1) | 1;
    c->F = sz53p(r) | carry;
    return r;
}

static uint8_t srl(z80_t *c, uint8_t val) {
    uint8_t carry = val & 1;
    uint8_t r = val >> 1;
    c->F = sz53p(r) | carry;
    return r;
}

static uint8_t do_rot(z80_t *c, int op, uint8_t val) {
    switch (op) {
        case 0: return rlc(c, val);
        case 1: return rrc(c, val);
        case 2: return rl(c, val);
        case 3: return rr(c, val);
        case 4: return sla(c, val);
        case 5: return sra(c, val);
        case 6: return sll(c, val);
        case 7: return srl(c, val);
    }
    return val;
}

/* ── Increment R register (lower 7 bits only) ────────────────────── */

static inline void inc_r(z80_t *c) {
    c->R = (c->R & 0x80) | ((c->R + 1) & 0x7F);
}

/* ── DAA ─────────────────────────────────────────────────────────── */

static void daa(z80_t *c) {
    uint8_t a = c->A;
    uint8_t correction = 0;
    uint8_t carry = c->F & Z80_CF;

    if ((c->F & Z80_HF) || (a & 0x0F) > 9)
        correction |= 0x06;
    if (carry || a > 0x99) {
        correction |= 0x60;
        carry = Z80_CF;
    }

    if (c->F & Z80_NF)
        c->A -= correction;
    else
        c->A += correction;

    c->F = sz53p(c->A) | carry | (c->F & Z80_NF) |
           ((a ^ c->A) & Z80_HF);
}

/* ── Main instruction execution ──────────────────────────────────── */

/* T-state tables for unprefixed opcodes */
static const int t_states_main[256] = {
/*       x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xA  xB  xC  xD  xE  xF */
/* 0x */  4, 10,  7,  6,  4,  4,  7,  4,  4, 11,  7,  6,  4,  4,  7,  4,
/* 1x */  8, 10,  7,  6,  4,  4,  7,  4, 12, 11,  7,  6,  4,  4,  7,  4,
/* 2x */  7, 10, 16,  6,  4,  4,  7,  4,  7, 11, 16,  6,  4,  4,  7,  4,
/* 3x */  7, 10, 13,  6, 11, 11, 10,  4,  7, 11, 13,  6,  4,  4,  7,  4,
/* 4x */  4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
/* 5x */  4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
/* 6x */  4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
/* 7x */  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  7,  4,
/* 8x */  4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
/* 9x */  4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
/* Ax */  4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
/* Bx */  4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4,
/* Cx */  5, 10, 10, 10, 10, 11,  7, 11,  5, 10, 10,  4, 10, 17,  7, 11,
/* Dx */  5, 10, 10, 11, 10, 11,  7, 11,  5,  4, 10, 11, 10,  4,  7, 11,
/* Ex */  5, 10, 10, 19, 10, 11,  7, 11,  5,  4, 10,  4, 10,  4,  7, 11,
/* Fx */  5, 10, 10,  4, 10, 11,  7, 11,  5,  6, 10,  4, 10,  4,  7, 11,
};

static int exec_cb(z80_t *c) {
    uint8_t op = fetch8(c);
    int x = op >> 6;
    int y = (op >> 3) & 7;
    int z = op & 7;
    int t = (z == 6) ? 15 : 8;

    uint8_t val = get_reg8(c, z);

    switch (x) {
        case 0: /* Rotate/shift */
            val = do_rot(c, y, val);
            set_reg8(c, z, val);
            break;
        case 1: /* BIT y, r[z] */
            t = (z == 6) ? 12 : 8;
            {
                uint8_t result = val & (1 << y);
                c->F = (c->F & Z80_CF) | Z80_HF | (result ? 0 : (Z80_ZF | Z80_PF));
                if (result & Z80_SF) c->F |= Z80_SF;
                if (z == 6) {
                    /* Bits 3,5 come from high byte of address for (HL) */
                    /* For normal CB prefix this is just H */
                } else {
                    c->F = (c->F & ~(Z80_F3 | Z80_F5)) | (val & (Z80_F3 | Z80_F5));
                }
            }
            break;
        case 2: /* RES y, r[z] */
            set_reg8(c, z, val & ~(1 << y));
            break;
        case 3: /* SET y, r[z] */
            set_reg8(c, z, val | (1 << y));
            break;
    }

    return t;
}

static int exec_ddfd_cb(z80_t *c, uint16_t ixiy) {
    int8_t d = (int8_t)fetch8(c);
    uint8_t op = fetch8(c);
    int x = op >> 6;
    int y = (op >> 3) & 7;
    int z = op & 7;

    uint16_t addr = ixiy + d;
    uint8_t val = rb(c, addr);

    switch (x) {
        case 0: /* Rotate/shift (IX+d)/(IY+d) */
            val = do_rot(c, y, val);
            wb(c, addr, val);
            if (z != 6) set_reg8(c, z, val); /* Undocumented: also copy to register */
            return 23;
        case 1: /* BIT y, (IX+d)/(IY+d) */
            {
                uint8_t result = val & (1 << y);
                c->F = (c->F & Z80_CF) | Z80_HF | (result ? 0 : (Z80_ZF | Z80_PF));
                if (result & Z80_SF) c->F |= Z80_SF;
                /* Bits 3,5 come from high byte of (IX+d) address */
                c->F = (c->F & ~(Z80_F3 | Z80_F5)) | ((addr >> 8) & (Z80_F3 | Z80_F5));
            }
            return 20;
        case 2: /* RES y, (IX+d)/(IY+d) */
            val &= ~(1 << y);
            wb(c, addr, val);
            if (z != 6) set_reg8(c, z, val); /* Undocumented */
            return 23;
        case 3: /* SET y, (IX+d)/(IY+d) */
            val |= (1 << y);
            wb(c, addr, val);
            if (z != 6) set_reg8(c, z, val); /* Undocumented */
            return 23;
    }
    return 23;
}

static int exec_ed(z80_t *c) {
    uint8_t op = fetch8(c);
    int x = op >> 6;
    int y = (op >> 3) & 7;
    int z = op & 7;
    int p = y >> 1;
    int q = y & 1;

    if (x == 1) {
        switch (z) {
            case 0: /* IN r[y], (C) / IN (C) if y==6 */
                {
                    uint16_t port = ((uint16_t)c->B << 8) | c->C;
                    uint8_t val = io_in(c, port);
                    if (y != 6) set_reg8(c, y, val);
                    c->F = (c->F & Z80_CF) | sz53p(val);
                }
                return 12;
            case 1: /* OUT (C), r[y] / OUT (C),0 if y==6 */
                {
                    uint16_t port = ((uint16_t)c->B << 8) | c->C;
                    uint8_t val = (y == 6) ? 0 : get_reg8(c, y);
                    io_out(c, port, val);
                }
                return 12;
            case 2: /* SBC/ADC HL, rp[p] */
                {
                    uint16_t hl = rp_hl(c);
                    uint16_t val = get_rp(c, p);
                    if (q == 0) {
                        /* SBC HL, rp */
                        uint8_t carry = c->F & Z80_CF ? 1 : 0;
                        uint32_t r = (uint32_t)hl - val - carry;
                        uint16_t h = (hl ^ val ^ r) & 0x1000;
                        uint8_t v = ((hl ^ val) & (hl ^ r) & 0x8000) ? Z80_PF : 0;
                        uint16_t result = (uint16_t)r;
                        c->F = ((result >> 8) & (Z80_SF | Z80_F5 | Z80_F3)) |
                               (result == 0 ? Z80_ZF : 0) | Z80_NF |
                               (r & 0x10000 ? Z80_CF : 0) |
                               (h ? Z80_HF : 0) | v;
                        set_hl(c, result);
                    } else {
                        /* ADC HL, rp */
                        uint8_t carry = c->F & Z80_CF ? 1 : 0;
                        uint32_t r = (uint32_t)hl + val + carry;
                        uint16_t h = (hl ^ val ^ r) & 0x1000;
                        uint8_t v = ((hl ^ val ^ 0x8000) & (hl ^ r) & 0x8000) ? Z80_PF : 0;
                        uint16_t result = (uint16_t)r;
                        c->F = ((result >> 8) & (Z80_SF | Z80_F5 | Z80_F3)) |
                               (result == 0 ? Z80_ZF : 0) |
                               (r & 0x10000 ? Z80_CF : 0) |
                               (h ? Z80_HF : 0) | v;
                        set_hl(c, result);
                    }
                }
                return 15;
            case 3: /* LD (nn), rp[p] / LD rp[p], (nn) */
                {
                    uint16_t addr = fetch16(c);
                    if (q == 0)
                        ww(c, addr, get_rp(c, p));
                    else
                        set_rp(c, p, rw(c, addr));
                }
                return 20;
            case 4: /* NEG */
                {
                    uint8_t a = c->A;
                    c->A = 0;
                    alu_sub(c, a);
                }
                return 8;
            case 5: /* RETN / RETI */
                c->IFF1 = c->IFF2;
                c->PC = pop16(c);
                return 14;
            case 6: /* IM y */
                switch (y) {
                    case 0: case 4: c->IM = 0; break;
                    case 1: case 5: c->IM = 0; break;
                    case 2: case 6: c->IM = 1; break;
                    case 3: case 7: c->IM = 2; break;
                }
                return 8;
            case 7: /* Misc: LD I,A / LD R,A / LD A,I / LD A,R / RRD / RLD / NOP / NOP */
                switch (y) {
                    case 0: c->I = c->A; return 9;
                    case 1: c->R = c->A; return 9;
                    case 2: /* LD A, I */
                        c->A = c->I;
                        c->F = (c->F & Z80_CF) | sz53(c->A) | (c->IFF2 ? Z80_PF : 0);
                        return 9;
                    case 3: /* LD A, R */
                        c->A = c->R;
                        c->F = (c->F & Z80_CF) | sz53(c->A) | (c->IFF2 ? Z80_PF : 0);
                        return 9;
                    case 4: /* RRD */
                        {
                            uint8_t m = rb(c, rp_hl(c));
                            uint8_t lo_a = c->A & 0x0F;
                            c->A = (c->A & 0xF0) | (m & 0x0F);
                            m = (m >> 4) | (lo_a << 4);
                            wb(c, rp_hl(c), m);
                            c->F = (c->F & Z80_CF) | sz53p(c->A);
                        }
                        return 18;
                    case 5: /* RLD */
                        {
                            uint8_t m = rb(c, rp_hl(c));
                            uint8_t lo_a = c->A & 0x0F;
                            c->A = (c->A & 0xF0) | (m >> 4);
                            m = (m << 4) | lo_a;
                            wb(c, rp_hl(c), m);
                            c->F = (c->F & Z80_CF) | sz53p(c->A);
                        }
                        return 18;
                    default: return 8; /* NOP (ED-prefixed) */
                }
        }
    } else if (x == 2 && z <= 3 && y >= 4) {
        /* Block instructions */
        int repeat = 0;
        switch (z) {
            case 0: /* LDI/LDD/LDIR/LDDR */
                {
                    uint8_t val = rb(c, rp_hl(c));
                    wb(c, rp_de(c), val);
                    if (y == 4 || y == 6) {
                        /* LDI / LDIR: increment */
                        set_hl(c, rp_hl(c) + 1);
                        set_de(c, rp_de(c) + 1);
                    } else {
                        /* LDD / LDDR: decrement */
                        set_hl(c, rp_hl(c) - 1);
                        set_de(c, rp_de(c) - 1);
                    }
                    set_bc(c, rp_bc(c) - 1);
                    uint8_t n = val + c->A;
                    c->F = (c->F & (Z80_SF | Z80_ZF | Z80_CF)) |
                           (rp_bc(c) != 0 ? Z80_PF : 0) |
                           (n & Z80_F3) |
                           ((n & 0x02) ? Z80_F5 : 0);
                    if (y >= 6 && rp_bc(c) != 0) {
                        c->PC -= 2;
                        repeat = 1;
                    }
                }
                return repeat ? 21 : 16;

            case 1: /* CPI/CPD/CPIR/CPDR */
                {
                    uint8_t val = rb(c, rp_hl(c));
                    uint8_t result = c->A - val;
                    uint8_t hf = (c->A ^ val ^ result) & 0x10;
                    if (y == 4 || y == 6)
                        set_hl(c, rp_hl(c) + 1);
                    else
                        set_hl(c, rp_hl(c) - 1);
                    set_bc(c, rp_bc(c) - 1);
                    uint8_t n = result - (hf ? 1 : 0);
                    c->F = (c->F & Z80_CF) | Z80_NF |
                           (result & Z80_SF) |
                           (result == 0 ? Z80_ZF : 0) |
                           (hf ? Z80_HF : 0) |
                           (rp_bc(c) != 0 ? Z80_PF : 0) |
                           (n & Z80_F3) |
                           ((n & 0x02) ? Z80_F5 : 0);
                    if (y >= 6 && rp_bc(c) != 0 && result != 0) {
                        c->PC -= 2;
                        repeat = 1;
                    }
                }
                return repeat ? 21 : 16;

            case 2: /* INI/IND/INIR/INDR */
                {
                    uint16_t port = ((uint16_t)c->B << 8) | c->C;
                    uint8_t val = io_in(c, port);
                    wb(c, rp_hl(c), val);
                    c->B--;
                    if (y == 4 || y == 6)
                        set_hl(c, rp_hl(c) + 1);
                    else
                        set_hl(c, rp_hl(c) - 1);
                    c->F = (c->F & ~(Z80_ZF | Z80_NF)) |
                           (c->B == 0 ? Z80_ZF : 0) | Z80_NF |
                           (c->B & (Z80_SF | Z80_F5 | Z80_F3));
                    if (y >= 6 && c->B != 0) {
                        c->PC -= 2;
                        repeat = 1;
                    }
                }
                return repeat ? 21 : 16;

            case 3: /* OUTI/OUTD/OTIR/OTDR */
                {
                    uint8_t val = rb(c, rp_hl(c));
                    c->B--;
                    uint16_t port = ((uint16_t)c->B << 8) | c->C;
                    io_out(c, port, val);
                    if (y == 4 || y == 6)
                        set_hl(c, rp_hl(c) + 1);
                    else
                        set_hl(c, rp_hl(c) - 1);
                    c->F = (c->F & ~(Z80_ZF | Z80_NF)) |
                           (c->B == 0 ? Z80_ZF : 0) | Z80_NF |
                           (c->B & (Z80_SF | Z80_F5 | Z80_F3));
                    if (y >= 6 && c->B != 0) {
                        c->PC -= 2;
                        repeat = 1;
                    }
                }
                return repeat ? 21 : 16;
        }
    }

    /* Unrecognized ED-prefixed opcodes act as NOP */
    return 8;
}

static int exec_ddfd(z80_t *c, uint16_t *ixiy) {
    uint8_t op = fetch8(c);
    int x = op >> 6;
    int y = (op >> 3) & 7;
    int z = op & 7;
    int p = y >> 1;
    int q = y & 1;
    int8_t d = 0;

    /* DDCB/FDCB prefix */
    if (op == 0xCB) {
        return exec_ddfd_cb(c, *ixiy);
    }

    /* DD DD / FD FD / DD FD / FD DD just restarts prefix parsing.
       For simplicity, we treat another DD/FD as consuming 4 T-states and
       re-entering. But the simplest approach: re-process. */
    if (op == 0xDD || op == 0xFD) {
        /* Another prefix; PC already points past it */
        c->R = (c->R & 0x80) | ((c->R + 1) & 0x7F);
        uint16_t *new_ixiy = (op == 0xDD) ? &c->IX : &c->IY;
        return 4 + exec_ddfd(c, new_ixiy);
    }

    /* ED after DD/FD: the DD/FD is ignored */
    if (op == 0xED) {
        inc_r(c);
        return 4 + exec_ed(c);
    }

    /* For opcodes that reference (HL), read displacement now */
    /* But we must be careful: not all opcodes use it */
    /* Handle each opcode group directly */

    /* x=0 group */
    if (x == 0) {
        switch (z) {
            case 0: /* NOP, EX AF, DJNZ, JR, JR cc - none use HL indexing */
                goto normal;
            case 1:
                if (q == 0) {
                    /* LD rp, nn -- replace HL with IX/IY */
                    if (p == 2) {
                        *ixiy = fetch16(c);
                        return 14;
                    }
                    goto normal;
                } else {
                    /* ADD HL, rp */
                    if (p == 2) {
                        add_hl(c, ixiy, *ixiy);
                    } else {
                        add_hl(c, ixiy, get_rp(c, p));
                    }
                    return 15;
                }
            case 2:
                if (p == 2 && q == 0) {
                    /* LD (nn), HL -> LD (nn), IX/IY */
                    uint16_t addr = fetch16(c);
                    ww(c, addr, *ixiy);
                    return 20;
                } else if (p == 2 && q == 1) {
                    /* LD HL, (nn) -> LD IX/IY, (nn) */
                    uint16_t addr = fetch16(c);
                    *ixiy = rw(c, addr);
                    return 20;
                }
                goto normal;
            case 3:
                if (p == 2) {
                    if (q == 0)
                        (*ixiy)++;
                    else
                        (*ixiy)--;
                    return 10;
                }
                goto normal;
            case 4: /* INC r */
                if (y == 4) { /* INC IXH/IYH */
                    *ixiy = ((uint16_t)inc8(c, *ixiy >> 8) << 8) | (*ixiy & 0xFF);
                    return 8;
                } else if (y == 5) { /* INC IXL/IYL */
                    *ixiy = (*ixiy & 0xFF00) | inc8(c, *ixiy & 0xFF);
                    return 8;
                } else if (y == 6) { /* INC (IX+d) */
                    d = (int8_t)fetch8(c);
                    uint16_t addr = *ixiy + d;
                    wb(c, addr, inc8(c, rb(c, addr)));
                    return 23;
                }
                goto normal;
            case 5: /* DEC r */
                if (y == 4) { /* DEC IXH/IYH */
                    *ixiy = ((uint16_t)dec8(c, *ixiy >> 8) << 8) | (*ixiy & 0xFF);
                    return 8;
                } else if (y == 5) { /* DEC IXL/IYL */
                    *ixiy = (*ixiy & 0xFF00) | dec8(c, *ixiy & 0xFF);
                    return 8;
                } else if (y == 6) { /* DEC (IX+d) */
                    d = (int8_t)fetch8(c);
                    uint16_t addr = *ixiy + d;
                    wb(c, addr, dec8(c, rb(c, addr)));
                    return 23;
                }
                goto normal;
            case 6: /* LD r, n */
                if (y == 4) { /* LD IXH, n */
                    uint8_t n = fetch8(c);
                    *ixiy = ((uint16_t)n << 8) | (*ixiy & 0xFF);
                    return 11;
                } else if (y == 5) { /* LD IXL, n */
                    uint8_t n = fetch8(c);
                    *ixiy = (*ixiy & 0xFF00) | n;
                    return 11;
                } else if (y == 6) { /* LD (IX+d), n */
                    d = (int8_t)fetch8(c);
                    uint8_t n = fetch8(c);
                    wb(c, *ixiy + d, n);
                    return 19;
                }
                goto normal;
            case 7: goto normal;
        }
    }

    /* x=1: LD r, r' -- with IX/IY substitutions */
    if (x == 1) {
        if (y == 6 && z == 6) {
            /* LD (HL),(HL) = HALT - not affected by DD/FD? Actually it is still HALT */
            goto normal;
        }
        if (y == 6) {
            /* LD (IX+d), r */
            d = (int8_t)fetch8(c);
            uint8_t val = get_reg8(c, z); /* source is NOT IX-substituted for (IX+d) dest */
            wb(c, *ixiy + d, val);
            return 19;
        }
        if (z == 6) {
            /* LD r, (IX+d) */
            d = (int8_t)fetch8(c);
            uint8_t val = rb(c, *ixiy + d);
            set_reg8(c, y, val); /* dest is NOT IX-substituted for (IX+d) source */
            return 19;
        }
        /* LD r, r' -- H/L replaced with IXH/IXL/IYH/IYL */
        {
            uint8_t val;
            if (z == 4) val = *ixiy >> 8;
            else if (z == 5) val = *ixiy & 0xFF;
            else val = get_reg8(c, z);

            if (y == 4) *ixiy = ((uint16_t)val << 8) | (*ixiy & 0xFF);
            else if (y == 5) *ixiy = (*ixiy & 0xFF00) | val;
            else set_reg8(c, y, val);
        }
        return 8;
    }

    /* x=2: ALU A, r -- with IX/IY substitutions */
    if (x == 2) {
        uint8_t val;
        if (z == 6) {
            d = (int8_t)fetch8(c);
            val = rb(c, *ixiy + d);
            do_alu(c, y, val);
            return 19;
        } else if (z == 4) {
            val = *ixiy >> 8;
            do_alu(c, y, val);
            return 8;
        } else if (z == 5) {
            val = *ixiy & 0xFF;
            do_alu(c, y, val);
            return 8;
        }
        goto normal;
    }

    /* x=3 group -- mostly not affected by DD/FD */
    if (x == 3) {
        switch (z) {
            case 1:
                if (p == 2 && q == 0) {
                    /* POP HL -> POP IX/IY */
                    *ixiy = pop16(c);
                    return 14;
                }
                if (p == 2 && q == 1) {
                    /* Various -- JP (HL) replaced by JP (IX/IY) at z=1,p=2,q=1 -> opcode E9 */
                    /* Actually E9 is z=1,p=2,q=1? No, E9 = 11 101 001 -> x=3,z=1,y=5,p=2,q=1 */
                    c->PC = *ixiy;
                    return 8;
                }
                goto normal;
            case 3:
                if (op == 0xE3) {
                    /* EX (SP), HL -> EX (SP), IX/IY */
                    uint16_t val = rw(c, c->SP);
                    ww(c, c->SP, *ixiy);
                    *ixiy = val;
                    return 23;
                }
                goto normal;
            case 5:
                if (p == 2 && q == 0) {
                    /* PUSH HL -> PUSH IX/IY */
                    push16(c, *ixiy);
                    return 15;
                }
                goto normal;
            case 7: /* RST - not affected */
                goto normal;
            default:
                goto normal;
        }
    }

normal:
    /* This opcode is not affected by DD/FD prefix, execute normally */
    /* But we already consumed the opcode byte. We need to execute it. */
    /* Fall through to the main decoder but with the already-fetched opcode */
    return exec_main_op(c, op) + 4; /* 4 T-states for the DD/FD prefix fetch */
}

static int exec_main_op(z80_t *c, uint8_t op) {
    int x = op >> 6;
    int y = (op >> 3) & 7;
    int z = op & 7;
    int p = y >> 1;
    int q = y & 1;
    int t = t_states_main[op];

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: /* NOP */
                break;
            case 1: /* EX AF, AF' */
                { uint8_t ta = c->A; c->A = c->A_; c->A_ = ta; }
                { uint8_t tf = c->F; c->F = c->F_; c->F_ = tf; }
                break;
            case 2: /* DJNZ d */
                {
                    int8_t d = (int8_t)fetch8(c);
                    c->B--;
                    if (c->B != 0) {
                        c->PC += d;
                        t = 13;
                    }
                    /* else t = 8 from table */
                }
                break;
            case 3: /* JR d */
                {
                    int8_t d = (int8_t)fetch8(c);
                    c->PC += d;
                }
                break;
            case 4: case 5: case 6: case 7: /* JR cc, d */
                {
                    int8_t d = (int8_t)fetch8(c);
                    if (eval_cc(c, y - 4)) {
                        c->PC += d;
                        t = 12;
                    }
                    /* else t = 7 from table */
                }
                break;
            }
            break;

        case 1:
            if (q == 0) {
                /* LD rp[p], nn */
                set_rp(c, p, fetch16(c));
            } else {
                /* ADD HL, rp[p] */
                uint16_t hl = rp_hl(c);
                add_hl(c, &hl, get_rp(c, p));
                set_hl(c, hl);
            }
            break;

        case 2:
            switch (p) {
            case 0:
                if (q == 0) wb(c, rp_bc(c), c->A);      /* LD (BC), A */
                else c->A = rb(c, rp_bc(c));              /* LD A, (BC) */
                break;
            case 1:
                if (q == 0) wb(c, rp_de(c), c->A);       /* LD (DE), A */
                else c->A = rb(c, rp_de(c));              /* LD A, (DE) */
                break;
            case 2:
                if (q == 0) { uint16_t a = fetch16(c); ww(c, a, rp_hl(c)); }  /* LD (nn), HL */
                else { uint16_t a = fetch16(c); set_hl(c, rw(c, a)); }         /* LD HL, (nn) */
                break;
            case 3:
                if (q == 0) { uint16_t a = fetch16(c); wb(c, a, c->A); }      /* LD (nn), A */
                else { uint16_t a = fetch16(c); c->A = rb(c, a); }            /* LD A, (nn) */
                break;
            }
            break;

        case 3:
            if (q == 0) set_rp(c, p, get_rp(c, p) + 1);  /* INC rp */
            else        set_rp(c, p, get_rp(c, p) - 1);   /* DEC rp */
            break;

        case 4: /* INC r[y] */
            set_reg8(c, y, inc8(c, get_reg8(c, y)));
            break;

        case 5: /* DEC r[y] */
            set_reg8(c, y, dec8(c, get_reg8(c, y)));
            break;

        case 6: /* LD r[y], n */
            set_reg8(c, y, fetch8(c));
            break;

        case 7:
            switch (y) {
            case 0: /* RLCA */
                {
                    uint8_t carry = c->A >> 7;
                    c->A = (c->A << 1) | carry;
                    c->F = (c->F & (Z80_SF | Z80_ZF | Z80_PF)) |
                           (c->A & (Z80_F5 | Z80_F3)) | carry;
                }
                break;
            case 1: /* RRCA */
                {
                    uint8_t carry = c->A & 1;
                    c->A = (c->A >> 1) | (carry << 7);
                    c->F = (c->F & (Z80_SF | Z80_ZF | Z80_PF)) |
                           (c->A & (Z80_F5 | Z80_F3)) | carry;
                }
                break;
            case 2: /* RLA */
                {
                    uint8_t carry = c->A >> 7;
                    c->A = (c->A << 1) | (c->F & Z80_CF);
                    c->F = (c->F & (Z80_SF | Z80_ZF | Z80_PF)) |
                           (c->A & (Z80_F5 | Z80_F3)) | carry;
                }
                break;
            case 3: /* RRA */
                {
                    uint8_t carry = c->A & 1;
                    c->A = (c->A >> 1) | ((c->F & Z80_CF) << 7);
                    c->F = (c->F & (Z80_SF | Z80_ZF | Z80_PF)) |
                           (c->A & (Z80_F5 | Z80_F3)) | carry;
                }
                break;
            case 4: /* DAA */
                daa(c);
                break;
            case 5: /* CPL */
                c->A = ~c->A;
                c->F = (c->F & (Z80_SF | Z80_ZF | Z80_PF | Z80_CF)) |
                       (c->A & (Z80_F5 | Z80_F3)) | Z80_HF | Z80_NF;
                break;
            case 6: /* SCF */
                c->F = (c->F & (Z80_SF | Z80_ZF | Z80_PF)) |
                       (c->A & (Z80_F5 | Z80_F3)) | Z80_CF;
                break;
            case 7: /* CCF */
                {
                    uint8_t hf = (c->F & Z80_CF) ? Z80_HF : 0;
                    c->F = (c->F & (Z80_SF | Z80_ZF | Z80_PF)) |
                           (c->A & (Z80_F5 | Z80_F3)) |
                           hf | ((c->F & Z80_CF) ^ Z80_CF);
                }
                break;
            }
            break;
        }
        break;

    case 1:
        if (y == 6 && z == 6) {
            /* HALT */
            c->halted = 1;
            c->PC--; /* Keep executing HALT */
        } else {
            /* LD r[y], r[z] */
            set_reg8(c, y, get_reg8(c, z));
        }
        break;

    case 2:
        /* ALU A, r[z] */
        do_alu(c, y, get_reg8(c, z));
        break;

    case 3:
        switch (z) {
        case 0: /* RET cc[y] */
            if (eval_cc(c, y)) {
                c->PC = pop16(c);
                t = 11;
            }
            /* else t = 5 from table */
            break;

        case 1:
            if (q == 0) {
                /* POP rp2[p] */
                set_rp2(c, p, pop16(c));
            } else {
                switch (p) {
                case 0: /* RET */
                    c->PC = pop16(c);
                    break;
                case 1: /* EXX */
                    { uint8_t tmp;
                      tmp = c->B; c->B = c->B_; c->B_ = tmp;
                      tmp = c->C; c->C = c->C_; c->C_ = tmp;
                      tmp = c->D; c->D = c->D_; c->D_ = tmp;
                      tmp = c->E; c->E = c->E_; c->E_ = tmp;
                      tmp = c->H; c->H = c->H_; c->H_ = tmp;
                      tmp = c->L; c->L = c->L_; c->L_ = tmp;
                    }
                    break;
                case 2: /* JP (HL) */
                    c->PC = rp_hl(c);
                    break;
                case 3: /* LD SP, HL */
                    c->SP = rp_hl(c);
                    break;
                }
            }
            break;

        case 2: /* JP cc[y], nn */
            {
                uint16_t addr = fetch16(c);
                if (eval_cc(c, y))
                    c->PC = addr;
            }
            break;

        case 3:
            switch (y) {
            case 0: /* JP nn */
                c->PC = fetch16(c);
                break;
            case 1: /* CB prefix */
                t = exec_cb(c);
                break;
            case 2: /* OUT (n), A */
                {
                    uint8_t port = fetch8(c);
                    io_out(c, ((uint16_t)c->A << 8) | port, c->A);
                }
                break;
            case 3: /* IN A, (n) */
                {
                    uint8_t port = fetch8(c);
                    c->A = io_in(c, ((uint16_t)c->A << 8) | port);
                }
                break;
            case 4: /* EX (SP), HL */
                {
                    uint16_t val = rw(c, c->SP);
                    ww(c, c->SP, rp_hl(c));
                    set_hl(c, val);
                }
                break;
            case 5: /* EX DE, HL */
                {
                    uint16_t tmp = rp_de(c);
                    set_de(c, rp_hl(c));
                    set_hl(c, tmp);
                }
                break;
            case 6: /* DI */
                c->IFF1 = 0;
                c->IFF2 = 0;
                break;
            case 7: /* EI */
                c->IFF1 = 1;
                c->IFF2 = 1;
                c->ei_delay = 1;
                break;
            }
            break;

        case 4: /* CALL cc[y], nn */
            {
                uint16_t addr = fetch16(c);
                if (eval_cc(c, y)) {
                    push16(c, c->PC);
                    c->PC = addr;
                    t = 17;
                }
                /* else t = 10 from table */
            }
            break;

        case 5:
            if (q == 0) {
                /* PUSH rp2[p] */
                push16(c, get_rp2(c, p));
            } else {
                switch (p) {
                case 0: /* CALL nn */
                    {
                        uint16_t addr = fetch16(c);
                        push16(c, c->PC);
                        c->PC = addr;
                    }
                    break;
                case 1: /* DD prefix */
                    inc_r(c);
                    t = exec_ddfd(c, &c->IX);
                    break;
                case 2: /* ED prefix */
                    t = exec_ed(c);
                    break;
                case 3: /* FD prefix */
                    inc_r(c);
                    t = exec_ddfd(c, &c->IY);
                    break;
                }
            }
            break;

        case 6: /* ALU A, n */
            do_alu(c, y, fetch8(c));
            break;

        case 7: /* RST y*8 */
            push16(c, c->PC);
            c->PC = y * 8;
            break;
        }
        break;
    }

    return t;
}

/* ── Public API ──────────────────────────────────────────────────── */

void z80_init(z80_t *cpu) {
    init_parity();
    memset(cpu, 0, sizeof(*cpu));
    cpu->PC = 0x0000;
    cpu->SP = 0xFFFF;
    cpu->A = 0xFF;
    cpu->F = 0xFF;
}

int z80_step(z80_t *cpu) {
    if (cpu->ei_delay) {
        cpu->ei_delay = 0;
    }

    if (cpu->halted) {
        inc_r(cpu);
        cpu->t_states += 4;
        return 4;
    }

    inc_r(cpu);
    uint8_t op = fetch8(cpu);
    int t = exec_main_op(cpu, op);
    cpu->t_states += t;
    return t;
}

void z80_interrupt(z80_t *cpu, uint8_t data) {
    if (!cpu->IFF1 || cpu->ei_delay) return;

    cpu->halted = 0;
    cpu->IFF1 = 0;
    cpu->IFF2 = 0;

    switch (cpu->IM) {
    case 0:
        /* Execute instruction on data bus (typically RST 38h = 0xFF) */
        push16(cpu, cpu->PC);
        cpu->PC = data & 0x38; /* For RST instructions */
        cpu->t_states += 13;
        break;
    case 1:
        push16(cpu, cpu->PC);
        cpu->PC = 0x0038;
        cpu->t_states += 13;
        break;
    case 2:
        push16(cpu, cpu->PC);
        {
            uint16_t vector_addr = ((uint16_t)cpu->I << 8) | (data & 0xFE);
            cpu->PC = rw(cpu, vector_addr);
        }
        cpu->t_states += 19;
        break;
    }
}

void z80_nmi(z80_t *cpu) {
    cpu->halted = 0;
    cpu->IFF2 = cpu->IFF1;
    cpu->IFF1 = 0;
    push16(cpu, cpu->PC);
    cpu->PC = 0x0066;
    cpu->t_states += 11;
}
