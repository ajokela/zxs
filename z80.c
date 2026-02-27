/*
 * z80.c — Clean room Z80 CPU emulator implementation
 *
 * Decodes instructions using the standard x/y/z/p/q bit field scheme.
 * All official and undocumented instructions are implemented.
 */

#include "z80.h"
#include <string.h>

/* ── Helper macros ─────────────────────────────────────────────────── */

/* Opcode bit field extraction */
#define X(op) ((op) >> 6)
#define Y(op) (((op) >> 3) & 7)
#define Z(op) ((op) & 7)
#define P(op) (((op) >> 4) & 3)
#define Q(op) (((op) >> 3) & 1)

/* Memory access through callbacks */
#define RB(addr)      cpu->mem_read(cpu->ctx, (addr))
#define WB(addr, val) cpu->mem_write(cpu->ctx, (addr), (val))

/* 16-bit memory read/write (little-endian) */
#define RW(addr) ((uint16_t)RB(addr) | ((uint16_t)RB((addr) + 1) << 8))
#define WW(addr, val) do { \
    WB((addr), (uint8_t)(val));       \
    WB((addr) + 1, (uint8_t)((val) >> 8)); \
} while (0)

/* I/O access through callbacks */
#define IN(port)       cpu->io_read(cpu->ctx, (port))
#define OUT(port, val) cpu->io_write(cpu->ctx, (port), (val))

/* Fetch byte at PC and advance */
#define FETCH() RB(cpu->pc++)

/* Fetch 16-bit word at PC and advance */
static inline uint16_t fetch16(z80 *cpu) {
    uint16_t lo = FETCH();
    uint16_t hi = FETCH();
    return lo | (hi << 8);
}

/* Register pair accessors */
#define AF() ((uint16_t)(cpu->a << 8) | cpu->f)
#define BC() ((uint16_t)(cpu->b << 8) | cpu->c)
#define DE() ((uint16_t)(cpu->d << 8) | cpu->e)
#define HL() ((uint16_t)(cpu->h << 8) | cpu->l)

#define SET_AF(v) do { cpu->a = (v) >> 8; cpu->f = (v) & 0xFF; } while (0)
#define SET_BC(v) do { cpu->b = (v) >> 8; cpu->c = (v) & 0xFF; } while (0)
#define SET_DE(v) do { cpu->d = (v) >> 8; cpu->e = (v) & 0xFF; } while (0)
#define SET_HL(v) do { cpu->h = (v) >> 8; cpu->l = (v) & 0xFF; } while (0)

/* Push/pop */
#define PUSH(val) do { \
    cpu->sp -= 2;       \
    WW(cpu->sp, (val)); \
} while (0)

#define POP() (cpu->sp += 2, RW(cpu->sp - 2))

/* Increment R register (lower 7 bits, bit 7 preserved) */
#define INC_R() (cpu->r = (cpu->r & 0x80) | ((cpu->r + 1) & 0x7F))

/* ── Flag lookup tables ────────────────────────────────────────────── */

/* SZ53P table: indexed by result byte, gives S, Z, Y, X, and P flags */
static uint8_t sz53p[256];

/* Parity table: 1 if even parity */
static uint8_t parity[256];

static void init_tables(void) {
    for (int i = 0; i < 256; i++) {
        int p = 0;
        for (int b = 0; b < 8; b++)
            p ^= (i >> b) & 1;
        parity[i] = p ? 0 : Z80_PF; /* PF set if even parity */

        sz53p[i] = (i & (Z80_SF | Z80_YF | Z80_XF)) | parity[i];
        if (i == 0) sz53p[i] |= Z80_ZF;
    }
}

static int tables_initialized = 0;

/* ── ALU functions ─────────────────────────────────────────────────── */

static inline void alu_add8(z80 *cpu, uint8_t val) {
    uint16_t r = cpu->a + val;
    uint8_t halfr = (cpu->a & 0x0F) + (val & 0x0F);
    cpu->f = sz53p[r & 0xFF]
           | (halfr & Z80_HF)
           | ((r >> 8) & Z80_CF)
           | ((((cpu->a ^ ~val) & (cpu->a ^ r)) >> 5) & Z80_PF);
    cpu->a = r & 0xFF;
}

static inline void alu_adc8(z80 *cpu, uint8_t val) {
    uint8_t c = cpu->f & Z80_CF;
    uint16_t r = cpu->a + val + c;
    uint8_t halfr = (cpu->a & 0x0F) + (val & 0x0F) + c;
    cpu->f = sz53p[r & 0xFF]
           | (halfr & Z80_HF)
           | ((r >> 8) & Z80_CF)
           | ((((cpu->a ^ ~val) & (cpu->a ^ r)) >> 5) & Z80_PF);
    cpu->a = r & 0xFF;
}

static inline void alu_sub8(z80 *cpu, uint8_t val) {
    uint16_t r = cpu->a - val;
    uint8_t halfr = (cpu->a & 0x0F) - (val & 0x0F);
    cpu->f = sz53p[r & 0xFF]
           | Z80_NF
           | (halfr & Z80_HF)
           | ((r >> 8) & Z80_CF)
           | ((((cpu->a ^ val) & (cpu->a ^ r)) >> 5) & Z80_PF);
    cpu->a = r & 0xFF;
}

static inline void alu_sbc8(z80 *cpu, uint8_t val) {
    uint8_t c = cpu->f & Z80_CF;
    uint16_t r = cpu->a - val - c;
    uint8_t halfr = (cpu->a & 0x0F) - (val & 0x0F) - c;
    cpu->f = sz53p[r & 0xFF]
           | Z80_NF
           | (halfr & Z80_HF)
           | ((r >> 8) & Z80_CF)
           | ((((cpu->a ^ val) & (cpu->a ^ r)) >> 5) & Z80_PF);
    cpu->a = r & 0xFF;
}

static inline void alu_and(z80 *cpu, uint8_t val) {
    cpu->a &= val;
    cpu->f = sz53p[cpu->a] | Z80_HF;
}

static inline void alu_xor(z80 *cpu, uint8_t val) {
    cpu->a ^= val;
    cpu->f = sz53p[cpu->a];
}

static inline void alu_or(z80 *cpu, uint8_t val) {
    cpu->a |= val;
    cpu->f = sz53p[cpu->a];
}

static inline void alu_cp(z80 *cpu, uint8_t val) {
    uint16_t r = cpu->a - val;
    uint8_t halfr = (cpu->a & 0x0F) - (val & 0x0F);
    /* CP sets Y and X flags from the operand, not the result */
    cpu->f = (sz53p[r & 0xFF] & ~(Z80_YF | Z80_XF))
           | (val & (Z80_YF | Z80_XF))
           | Z80_NF
           | (halfr & Z80_HF)
           | ((r >> 8) & Z80_CF)
           | ((((cpu->a ^ val) & (cpu->a ^ r)) >> 5) & Z80_PF);
}

static inline uint8_t alu_inc8(z80 *cpu, uint8_t val) {
    uint8_t r = val + 1;
    cpu->f = (cpu->f & Z80_CF)
           | sz53p[r]
           | ((val & 0x0F) == 0x0F ? Z80_HF : 0)
           | (val == 0x7F ? Z80_PF : 0);
    return r;
}

static inline uint8_t alu_dec8(z80 *cpu, uint8_t val) {
    uint8_t r = val - 1;
    cpu->f = (cpu->f & Z80_CF)
           | sz53p[r]
           | Z80_NF
           | ((val & 0x0F) == 0x00 ? Z80_HF : 0)
           | (val == 0x80 ? Z80_PF : 0);
    return r;
}

static inline void alu_add16(z80 *cpu, uint16_t *dest, uint16_t val) {
    uint32_t r = *dest + val;
    uint16_t halfr = (*dest & 0x0FFF) + (val & 0x0FFF);
    cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
           | ((r >> 16) & Z80_CF)
           | ((halfr >> 8) & Z80_HF)
           | ((r >> 8) & (Z80_YF | Z80_XF));
    *dest = r & 0xFFFF;
}

static inline void alu_adc16(z80 *cpu, uint16_t val) {
    uint8_t c = cpu->f & Z80_CF;
    uint16_t hl = HL();
    uint32_t r = hl + val + c;
    uint16_t halfr = (hl & 0x0FFF) + (val & 0x0FFF) + c;
    cpu->f = ((r >> 8) & (Z80_SF | Z80_YF | Z80_XF))
           | ((r & 0xFFFF) == 0 ? Z80_ZF : 0)
           | ((halfr >> 8) & Z80_HF)
           | ((r >> 16) & Z80_CF)
           | ((((hl ^ ~val) & (hl ^ r)) >> 13) & Z80_PF);
    SET_HL(r & 0xFFFF);
}

static inline void alu_sbc16(z80 *cpu, uint16_t val) {
    uint8_t c = cpu->f & Z80_CF;
    uint16_t hl = HL();
    uint32_t r = hl - val - c;
    uint16_t halfr = (hl & 0x0FFF) - (val & 0x0FFF) - c;
    cpu->f = ((r >> 8) & (Z80_SF | Z80_YF | Z80_XF))
           | ((r & 0xFFFF) == 0 ? Z80_ZF : 0)
           | ((halfr >> 8) & Z80_HF)
           | Z80_NF
           | ((r >> 16) & Z80_CF)
           | ((((hl ^ val) & (hl ^ r)) >> 13) & Z80_PF);
    SET_HL(r & 0xFFFF);
}

/* ── Rotate/shift helpers ──────────────────────────────────────────── */

static inline uint8_t rot_rlc(z80 *cpu, uint8_t val) {
    uint8_t c = val >> 7;
    uint8_t r = (val << 1) | c;
    cpu->f = sz53p[r] | c;
    return r;
}

static inline uint8_t rot_rrc(z80 *cpu, uint8_t val) {
    uint8_t c = val & 1;
    uint8_t r = (val >> 1) | (c << 7);
    cpu->f = sz53p[r] | c;
    return r;
}

static inline uint8_t rot_rl(z80 *cpu, uint8_t val) {
    uint8_t c = val >> 7;
    uint8_t r = (val << 1) | (cpu->f & Z80_CF);
    cpu->f = sz53p[r] | c;
    return r;
}

static inline uint8_t rot_rr(z80 *cpu, uint8_t val) {
    uint8_t c = val & 1;
    uint8_t r = (val >> 1) | ((cpu->f & Z80_CF) << 7);
    cpu->f = sz53p[r] | c;
    return r;
}

static inline uint8_t rot_sla(z80 *cpu, uint8_t val) {
    uint8_t c = val >> 7;
    uint8_t r = val << 1;
    cpu->f = sz53p[r] | c;
    return r;
}

static inline uint8_t rot_sra(z80 *cpu, uint8_t val) {
    uint8_t c = val & 1;
    uint8_t r = (val >> 1) | (val & 0x80);
    cpu->f = sz53p[r] | c;
    return r;
}

/* Undocumented: SLL shifts left and sets bit 0 */
static inline uint8_t rot_sll(z80 *cpu, uint8_t val) {
    uint8_t c = val >> 7;
    uint8_t r = (val << 1) | 1;
    cpu->f = sz53p[r] | c;
    return r;
}

static inline uint8_t rot_srl(z80 *cpu, uint8_t val) {
    uint8_t c = val & 1;
    uint8_t r = val >> 1;
    cpu->f = sz53p[r] | c;
    return r;
}

/* ── BIT/RES/SET helpers ───────────────────────────────────────────── */

static inline void op_bit(z80 *cpu, int bit, uint8_t val) {
    uint8_t r = val & (1 << bit);
    cpu->f = (cpu->f & Z80_CF)
           | Z80_HF
           | (r ? (r & Z80_SF) : (Z80_ZF | Z80_PF))
           | (val & (Z80_YF | Z80_XF));
}

/* For BIT n,(IX/IY+d) the undocumented Y/X flags come from the high
 * byte of the address, not the tested value */
static inline void op_bit_idx(z80 *cpu, int bit, uint8_t val, uint8_t addrhi) {
    uint8_t r = val & (1 << bit);
    cpu->f = (cpu->f & Z80_CF)
           | Z80_HF
           | (r ? (r & Z80_SF) : (Z80_ZF | Z80_PF))
           | (addrhi & (Z80_YF | Z80_XF));
}

/* ── Condition code evaluation ─────────────────────────────────────── */

static inline int eval_cc(z80 *cpu, int cc) {
    switch (cc) {
        case 0: return !(cpu->f & Z80_ZF); /* NZ */
        case 1: return  (cpu->f & Z80_ZF); /* Z  */
        case 2: return !(cpu->f & Z80_CF); /* NC */
        case 3: return  (cpu->f & Z80_CF); /* C  */
        case 4: return !(cpu->f & Z80_PF); /* PO */
        case 5: return  (cpu->f & Z80_PF); /* PE */
        case 6: return !(cpu->f & Z80_SF); /* P  */
        case 7: return  (cpu->f & Z80_SF); /* M  */
    }
    return 0;
}

/* ── Register read/write by index ──────────────────────────────────── */

/* r[i]: B=0, C=1, D=2, E=3, H=4, L=5, (HL)=6, A=7 */
static inline uint8_t get_reg(z80 *cpu, int idx) {
    switch (idx) {
        case 0: return cpu->b;
        case 1: return cpu->c;
        case 2: return cpu->d;
        case 3: return cpu->e;
        case 4: return cpu->h;
        case 5: return cpu->l;
        case 6: return RB(HL());
        case 7: return cpu->a;
    }
    return 0;
}

static inline void set_reg(z80 *cpu, int idx, uint8_t val) {
    switch (idx) {
        case 0: cpu->b = val; break;
        case 1: cpu->c = val; break;
        case 2: cpu->d = val; break;
        case 3: cpu->e = val; break;
        case 4: cpu->h = val; break;
        case 5: cpu->l = val; break;
        case 6: WB(HL(), val); break;
        case 7: cpu->a = val; break;
    }
}

/* Register pair read/write by index.
 * rp[i]: BC=0, DE=1, HL=2, SP=3 */
static inline uint16_t get_rp(z80 *cpu, int idx) {
    switch (idx) {
        case 0: return BC();
        case 1: return DE();
        case 2: return HL();
        case 3: return cpu->sp;
    }
    return 0;
}

static inline void set_rp(z80 *cpu, int idx, uint16_t val) {
    switch (idx) {
        case 0: SET_BC(val); break;
        case 1: SET_DE(val); break;
        case 2: SET_HL(val); break;
        case 3: cpu->sp = val; break;
    }
}

/* rp2[i]: BC=0, DE=1, HL=2, AF=3 */
static inline uint16_t get_rp2(z80 *cpu, int idx) {
    switch (idx) {
        case 0: return BC();
        case 1: return DE();
        case 2: return HL();
        case 3: return AF();
    }
    return 0;
}

static inline void set_rp2(z80 *cpu, int idx, uint16_t val) {
    switch (idx) {
        case 0: SET_BC(val); break;
        case 1: SET_DE(val); break;
        case 2: SET_HL(val); break;
        case 3: SET_AF(val); break;
    }
}

/* ── IX/IY register helpers for DD/FD prefixed instructions ────────── */

/* Read register by index, but substitute IX/IY halves for H/L */
static inline uint8_t get_reg_idx(z80 *cpu, int idx, uint16_t ixiy) {
    switch (idx) {
        case 0: return cpu->b;
        case 1: return cpu->c;
        case 2: return cpu->d;
        case 3: return cpu->e;
        case 4: return (ixiy >> 8) & 0xFF;   /* IXH/IYH */
        case 5: return ixiy & 0xFF;           /* IXL/IYL */
        case 6: return 0; /* handled separately */
        case 7: return cpu->a;
    }
    return 0;
}

static inline void set_reg_idx(z80 *cpu, int idx, uint16_t *ixiy, uint8_t val) {
    switch (idx) {
        case 0: cpu->b = val; break;
        case 1: cpu->c = val; break;
        case 2: cpu->d = val; break;
        case 3: cpu->e = val; break;
        case 4: *ixiy = (*ixiy & 0x00FF) | ((uint16_t)val << 8); break;
        case 5: *ixiy = (*ixiy & 0xFF00) | val; break;
        case 6: break; /* handled separately */
        case 7: cpu->a = val; break;
    }
}

/* ── Initialization ────────────────────────────────────────────────── */

void z80_init(z80 *cpu) {
    if (!tables_initialized) {
        init_tables();
        tables_initialized = 1;
    }
    memset(cpu, 0, sizeof(*cpu));
    cpu->a = cpu->f = 0xFF;
    cpu->sp = 0xFFFF;
    cpu->pc = 0x0000;
}

/* ── CB prefix handler ─────────────────────────────────────────────── */

static int exec_cb(z80 *cpu) {
    uint8_t op = FETCH();
    INC_R();
    int x = X(op), y = Y(op), z = Z(op);
    int clk = (z == 6) ? 15 : 8;

    if (x == 0) {
        /* Rotate/shift operations */
        uint8_t val = get_reg(cpu, z);
        uint8_t r;
        switch (y) {
            case 0: r = rot_rlc(cpu, val); break;
            case 1: r = rot_rrc(cpu, val); break;
            case 2: r = rot_rl(cpu, val);  break;
            case 3: r = rot_rr(cpu, val);  break;
            case 4: r = rot_sla(cpu, val); break;
            case 5: r = rot_sra(cpu, val); break;
            case 6: r = rot_sll(cpu, val); break; /* undocumented */
            case 7: r = rot_srl(cpu, val); break;
            default: r = val; break;
        }
        set_reg(cpu, z, r);
    } else if (x == 1) {
        /* BIT y, r[z] */
        uint8_t val = get_reg(cpu, z);
        op_bit(cpu, y, val);
        if (z == 6) clk = 12;
    } else if (x == 2) {
        /* RES y, r[z] */
        uint8_t val = get_reg(cpu, z);
        set_reg(cpu, z, val & ~(1 << y));
    } else {
        /* SET y, r[z] */
        uint8_t val = get_reg(cpu, z);
        set_reg(cpu, z, val | (1 << y));
    }
    return clk;
}

/* ── DDCB/FDCB indexed bit operations ─────────────────────────────── */

static int exec_ddfdcb(z80 *cpu, uint16_t ixiy) {
    /* Format: DD CB d op (d already fetched before this call) */
    int8_t d = (int8_t)FETCH();
    uint8_t op = FETCH();
    uint16_t addr = ixiy + d;
    uint8_t val = RB(addr);
    int x = X(op), y = Y(op), z = Z(op);

    if (x == 0) {
        /* Rotate/shift with indexed addressing */
        uint8_t r;
        switch (y) {
            case 0: r = rot_rlc(cpu, val); break;
            case 1: r = rot_rrc(cpu, val); break;
            case 2: r = rot_rl(cpu, val);  break;
            case 3: r = rot_rr(cpu, val);  break;
            case 4: r = rot_sla(cpu, val); break;
            case 5: r = rot_sra(cpu, val); break;
            case 6: r = rot_sll(cpu, val); break;
            case 7: r = rot_srl(cpu, val); break;
            default: r = val; break;
        }
        WB(addr, r);
        /* Undocumented: result also stored in register z (if z != 6) */
        if (z != 6) set_reg(cpu, z, r);
        return 23;
    } else if (x == 1) {
        /* BIT y, (IX/IY+d) */
        op_bit_idx(cpu, y, val, (addr >> 8) & 0xFF);
        return 20;
    } else if (x == 2) {
        /* RES y, (IX/IY+d) — undocumented: also store in r[z] */
        uint8_t r = val & ~(1 << y);
        WB(addr, r);
        if (z != 6) set_reg(cpu, z, r);
        return 23;
    } else {
        /* SET y, (IX/IY+d) — undocumented: also store in r[z] */
        uint8_t r = val | (1 << y);
        WB(addr, r);
        if (z != 6) set_reg(cpu, z, r);
        return 23;
    }
}

/* ── ED prefix handler ─────────────────────────────────────────────── */

static int exec_ed(z80 *cpu) {
    uint8_t op = FETCH();
    INC_R();
    int x = X(op), y = Y(op), z = Z(op);
    int p = P(op), q = Q(op);

    if (x == 1) {
        switch (z) {
            case 0: {
                /* IN r, (C) / IN (C) if y==6 */
                uint8_t val = IN(BC());
                if (y != 6)
                    set_reg(cpu, y, val);
                cpu->f = (cpu->f & Z80_CF) | sz53p[val];
                return 12;
            }
            case 1: {
                /* OUT (C), r / OUT (C), 0 if y==6 */
                uint8_t val = (y == 6) ? 0 : get_reg(cpu, y);
                OUT(BC(), val);
                return 12;
            }
            case 2:
                /* SBC HL, rp[p] / ADC HL, rp[p] */
                if (q == 0) {
                    alu_sbc16(cpu, get_rp(cpu, p));
                } else {
                    alu_adc16(cpu, get_rp(cpu, p));
                }
                return 15;
            case 3:
                /* LD (nn), rp[p] / LD rp[p], (nn) */
                if (q == 0) {
                    uint16_t addr = fetch16(cpu);
                    WW(addr, get_rp(cpu, p));
                } else {
                    uint16_t addr = fetch16(cpu);
                    set_rp(cpu, p, RW(addr));
                }
                return 20;
            case 4:
                /* NEG */
                {
                    uint8_t a = cpu->a;
                    cpu->a = 0;
                    alu_sub8(cpu, a);
                }
                return 8;
            case 5:
                /* RETN / RETI */
                cpu->pc = POP();
                cpu->iff1 = cpu->iff2;
                return 14;
            case 6:
                /* IM y */
                switch (y) {
                    case 0: case 4: cpu->im = 0; break;
                    case 1: case 5: cpu->im = 0; break;
                    case 2: case 6: cpu->im = 1; break;
                    case 3: case 7: cpu->im = 2; break;
                }
                return 8;
            case 7:
                switch (y) {
                    case 0: /* LD I, A */
                        cpu->i = cpu->a;
                        return 9;
                    case 1: /* LD R, A */
                        cpu->r = cpu->a;
                        return 9;
                    case 2: /* LD A, I */
                        cpu->a = cpu->i;
                        cpu->f = (cpu->f & Z80_CF)
                               | sz53p[cpu->a]
                               | (cpu->iff2 ? Z80_PF : 0);
                        /* Clear parity from sz53p table, use IFF2 instead */
                        cpu->f = (cpu->f & ~Z80_PF)
                               | (cpu->iff2 ? Z80_PF : 0);
                        return 9;
                    case 3: /* LD A, R */
                        cpu->a = cpu->r;
                        cpu->f = (cpu->f & Z80_CF)
                               | sz53p[cpu->a]
                               | (cpu->iff2 ? Z80_PF : 0);
                        cpu->f = (cpu->f & ~Z80_PF)
                               | (cpu->iff2 ? Z80_PF : 0);
                        return 9;
                    case 4: { /* RRD */
                        uint8_t val = RB(HL());
                        uint8_t lo_a = cpu->a & 0x0F;
                        cpu->a = (cpu->a & 0xF0) | (val & 0x0F);
                        val = (val >> 4) | (lo_a << 4);
                        WB(HL(), val);
                        cpu->f = (cpu->f & Z80_CF) | sz53p[cpu->a];
                        return 18;
                    }
                    case 5: { /* RLD */
                        uint8_t val = RB(HL());
                        uint8_t lo_a = cpu->a & 0x0F;
                        cpu->a = (cpu->a & 0xF0) | (val >> 4);
                        val = (val << 4) | lo_a;
                        WB(HL(), val);
                        cpu->f = (cpu->f & Z80_CF) | sz53p[cpu->a];
                        return 18;
                    }
                    default: /* NOP (ED xx undefined) */
                        return 8;
                }
        }
    } else if (x == 2 && y >= 4 && z <= 3) {
        /* Block instructions */
        switch (z) {
            case 0:
                /* LDI/LDD/LDIR/LDDR */
                switch (y) {
                    case 4: { /* LDI */
                        uint8_t val = RB(HL());
                        WB(DE(), val);
                        SET_HL(HL() + 1);
                        SET_DE(DE() + 1);
                        SET_BC(BC() - 1);
                        uint8_t n = val + cpu->a;
                        cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_CF))
                               | (BC() ? Z80_PF : 0)
                               | (n & Z80_XF)
                               | ((n << 4) & Z80_YF);
                        return 16;
                    }
                    case 5: { /* LDD */
                        uint8_t val = RB(HL());
                        WB(DE(), val);
                        SET_HL(HL() - 1);
                        SET_DE(DE() - 1);
                        SET_BC(BC() - 1);
                        uint8_t n = val + cpu->a;
                        cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_CF))
                               | (BC() ? Z80_PF : 0)
                               | (n & Z80_XF)
                               | ((n << 4) & Z80_YF);
                        return 16;
                    }
                    case 6: { /* LDIR */
                        uint8_t val = RB(HL());
                        WB(DE(), val);
                        SET_HL(HL() + 1);
                        SET_DE(DE() + 1);
                        SET_BC(BC() - 1);
                        uint8_t n = val + cpu->a;
                        cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_CF))
                               | (BC() ? Z80_PF : 0)
                               | (n & Z80_XF)
                               | ((n << 4) & Z80_YF);
                        if (BC()) {
                            cpu->pc -= 2;
                            return 21;
                        }
                        return 16;
                    }
                    case 7: { /* LDDR */
                        uint8_t val = RB(HL());
                        WB(DE(), val);
                        SET_HL(HL() - 1);
                        SET_DE(DE() - 1);
                        SET_BC(BC() - 1);
                        uint8_t n = val + cpu->a;
                        cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_CF))
                               | (BC() ? Z80_PF : 0)
                               | (n & Z80_XF)
                               | ((n << 4) & Z80_YF);
                        if (BC()) {
                            cpu->pc -= 2;
                            return 21;
                        }
                        return 16;
                    }
                }
                break;
            case 1:
                /* CPI/CPD/CPIR/CPDR */
                switch (y) {
                    case 4: { /* CPI */
                        uint8_t val = RB(HL());
                        uint8_t r = cpu->a - val;
                        uint8_t hc = (cpu->a & 0x0F) - (val & 0x0F);
                        SET_HL(HL() + 1);
                        SET_BC(BC() - 1);
                        uint8_t n = r - ((hc & Z80_HF) ? 1 : 0);
                        cpu->f = (cpu->f & Z80_CF)
                               | (sz53p[r] & ~(Z80_YF | Z80_XF | Z80_PF))
                               | (hc & Z80_HF)
                               | Z80_NF
                               | (BC() ? Z80_PF : 0)
                               | (n & Z80_XF)
                               | ((n << 4) & Z80_YF);
                        return 16;
                    }
                    case 5: { /* CPD */
                        uint8_t val = RB(HL());
                        uint8_t r = cpu->a - val;
                        uint8_t hc = (cpu->a & 0x0F) - (val & 0x0F);
                        SET_HL(HL() - 1);
                        SET_BC(BC() - 1);
                        uint8_t n = r - ((hc & Z80_HF) ? 1 : 0);
                        cpu->f = (cpu->f & Z80_CF)
                               | (sz53p[r] & ~(Z80_YF | Z80_XF | Z80_PF))
                               | (hc & Z80_HF)
                               | Z80_NF
                               | (BC() ? Z80_PF : 0)
                               | (n & Z80_XF)
                               | ((n << 4) & Z80_YF);
                        return 16;
                    }
                    case 6: { /* CPIR */
                        uint8_t val = RB(HL());
                        uint8_t r = cpu->a - val;
                        uint8_t hc = (cpu->a & 0x0F) - (val & 0x0F);
                        SET_HL(HL() + 1);
                        SET_BC(BC() - 1);
                        uint8_t n = r - ((hc & Z80_HF) ? 1 : 0);
                        cpu->f = (cpu->f & Z80_CF)
                               | (sz53p[r] & ~(Z80_YF | Z80_XF | Z80_PF))
                               | (hc & Z80_HF)
                               | Z80_NF
                               | (BC() ? Z80_PF : 0)
                               | (n & Z80_XF)
                               | ((n << 4) & Z80_YF);
                        if (BC() && r != 0) {
                            cpu->pc -= 2;
                            return 21;
                        }
                        return 16;
                    }
                    case 7: { /* CPDR */
                        uint8_t val = RB(HL());
                        uint8_t r = cpu->a - val;
                        uint8_t hc = (cpu->a & 0x0F) - (val & 0x0F);
                        SET_HL(HL() - 1);
                        SET_BC(BC() - 1);
                        uint8_t n = r - ((hc & Z80_HF) ? 1 : 0);
                        cpu->f = (cpu->f & Z80_CF)
                               | (sz53p[r] & ~(Z80_YF | Z80_XF | Z80_PF))
                               | (hc & Z80_HF)
                               | Z80_NF
                               | (BC() ? Z80_PF : 0)
                               | (n & Z80_XF)
                               | ((n << 4) & Z80_YF);
                        if (BC() && r != 0) {
                            cpu->pc -= 2;
                            return 21;
                        }
                        return 16;
                    }
                }
                break;
            case 2:
                /* INI/IND/INIR/INDR */
                switch (y) {
                    case 4: { /* INI */
                        uint8_t val = IN(BC());
                        WB(HL(), val);
                        SET_HL(HL() + 1);
                        cpu->b = alu_dec8(cpu, cpu->b);
                        /* N flag set from bit 7 of input */
                        cpu->f |= (val >> 6) & Z80_NF;
                        return 16;
                    }
                    case 5: { /* IND */
                        uint8_t val = IN(BC());
                        WB(HL(), val);
                        SET_HL(HL() - 1);
                        cpu->b = alu_dec8(cpu, cpu->b);
                        cpu->f |= (val >> 6) & Z80_NF;
                        return 16;
                    }
                    case 6: { /* INIR */
                        uint8_t val = IN(BC());
                        WB(HL(), val);
                        SET_HL(HL() + 1);
                        cpu->b = alu_dec8(cpu, cpu->b);
                        cpu->f |= (val >> 6) & Z80_NF;
                        if (cpu->b) {
                            cpu->pc -= 2;
                            return 21;
                        }
                        return 16;
                    }
                    case 7: { /* INDR */
                        uint8_t val = IN(BC());
                        WB(HL(), val);
                        SET_HL(HL() - 1);
                        cpu->b = alu_dec8(cpu, cpu->b);
                        cpu->f |= (val >> 6) & Z80_NF;
                        if (cpu->b) {
                            cpu->pc -= 2;
                            return 21;
                        }
                        return 16;
                    }
                }
                break;
            case 3:
                /* OUTI/OUTD/OTIR/OTDR */
                switch (y) {
                    case 4: { /* OUTI */
                        uint8_t val = RB(HL());
                        cpu->b = alu_dec8(cpu, cpu->b);
                        OUT(BC(), val);
                        SET_HL(HL() + 1);
                        cpu->f |= (val >> 6) & Z80_NF;
                        return 16;
                    }
                    case 5: { /* OUTD */
                        uint8_t val = RB(HL());
                        cpu->b = alu_dec8(cpu, cpu->b);
                        OUT(BC(), val);
                        SET_HL(HL() - 1);
                        cpu->f |= (val >> 6) & Z80_NF;
                        return 16;
                    }
                    case 6: { /* OTIR */
                        uint8_t val = RB(HL());
                        cpu->b = alu_dec8(cpu, cpu->b);
                        OUT(BC(), val);
                        SET_HL(HL() + 1);
                        cpu->f |= (val >> 6) & Z80_NF;
                        if (cpu->b) {
                            cpu->pc -= 2;
                            return 21;
                        }
                        return 16;
                    }
                    case 7: { /* OTDR */
                        uint8_t val = RB(HL());
                        cpu->b = alu_dec8(cpu, cpu->b);
                        OUT(BC(), val);
                        SET_HL(HL() - 1);
                        cpu->f |= (val >> 6) & Z80_NF;
                        if (cpu->b) {
                            cpu->pc -= 2;
                            return 21;
                        }
                        return 16;
                    }
                }
                break;
        }
    }

    /* All other ED opcodes are NOPs */
    return 8;
}

/* ── DD/FD prefix handler (IX/IY substitution) ────────────────────── */

static int exec_ddfd(z80 *cpu, uint16_t *ixiy) {
    uint8_t op = FETCH();
    INC_R();

    /* DD CB / FD CB — indexed bit operations */
    if (op == 0xCB) {
        return exec_ddfdcb(cpu, *ixiy);
    }

    /* DD DD or DD FD — treat as another prefix (just add 4 clocks) */
    if (op == 0xDD || op == 0xFD) {
        cpu->pc--;
        return 4;
    }

    /* ED after DD/FD: ED ignores DD/FD prefix */
    if (op == 0xED) {
        return exec_ed(cpu) + 4;
    }

    int x = X(op), y = Y(op), z = Z(op);
    int p = P(op), q = Q(op);

    switch (x) {
        case 0:
            switch (z) {
                case 0:
                    /* Relative jumps and DJNZ — not affected by DD/FD */
                    goto unprefixed;
                case 1:
                    if (q == 0) {
                        if (p == 2) {
                            /* LD IX/IY, nn */
                            *ixiy = fetch16(cpu);
                            return 14;
                        }
                        goto unprefixed;
                    } else {
                        if (p == 2) {
                            /* ADD IX/IY, rp */
                            /* rp[2] is IX/IY itself in this context */
                            uint16_t val;
                            switch (P(op)) {
                                case 0: val = BC(); break;
                                case 1: val = DE(); break;
                                case 2: val = *ixiy; break;
                                case 3: val = cpu->sp; break;
                                default: val = 0; break;
                            }
                            alu_add16(cpu, ixiy, val);
                            return 15;
                        }
                        goto unprefixed;
                    }
                case 2:
                    if (p == 2) {
                        if (q == 0) {
                            /* LD (nn), IX/IY */
                            uint16_t addr = fetch16(cpu);
                            WW(addr, *ixiy);
                            return 20;
                        } else {
                            /* LD IX/IY, (nn) */
                            uint16_t addr = fetch16(cpu);
                            *ixiy = RW(addr);
                            return 20;
                        }
                    }
                    goto unprefixed;
                case 3:
                    if (p == 2) {
                        if (q == 0) {
                            /* INC IX/IY */
                            (*ixiy)++;
                            return 10;
                        } else {
                            /* DEC IX/IY */
                            (*ixiy)--;
                            return 10;
                        }
                    }
                    goto unprefixed;
                case 4:
                    /* INC r (with IX/IY half-register substitution) */
                    if (y == 4) {
                        /* INC IXH/IYH */
                        uint8_t val = (*ixiy >> 8) & 0xFF;
                        val = alu_inc8(cpu, val);
                        *ixiy = (*ixiy & 0x00FF) | ((uint16_t)val << 8);
                        return 8;
                    } else if (y == 5) {
                        /* INC IXL/IYL */
                        uint8_t val = *ixiy & 0xFF;
                        val = alu_inc8(cpu, val);
                        *ixiy = (*ixiy & 0xFF00) | val;
                        return 8;
                    } else if (y == 6) {
                        /* INC (IX/IY+d) */
                        int8_t d = (int8_t)FETCH();
                        uint16_t addr = *ixiy + d;
                        uint8_t val = RB(addr);
                        val = alu_inc8(cpu, val);
                        WB(addr, val);
                        return 23;
                    }
                    goto unprefixed;
                case 5:
                    /* DEC r (with IX/IY half-register substitution) */
                    if (y == 4) {
                        uint8_t val = (*ixiy >> 8) & 0xFF;
                        val = alu_dec8(cpu, val);
                        *ixiy = (*ixiy & 0x00FF) | ((uint16_t)val << 8);
                        return 8;
                    } else if (y == 5) {
                        uint8_t val = *ixiy & 0xFF;
                        val = alu_dec8(cpu, val);
                        *ixiy = (*ixiy & 0xFF00) | val;
                        return 8;
                    } else if (y == 6) {
                        int8_t d = (int8_t)FETCH();
                        uint16_t addr = *ixiy + d;
                        uint8_t val = RB(addr);
                        val = alu_dec8(cpu, val);
                        WB(addr, val);
                        return 23;
                    }
                    goto unprefixed;
                case 6:
                    /* LD r, n (with IX/IY half-register substitution) */
                    if (y == 4) {
                        uint8_t n = FETCH();
                        *ixiy = (*ixiy & 0x00FF) | ((uint16_t)n << 8);
                        return 11;
                    } else if (y == 5) {
                        uint8_t n = FETCH();
                        *ixiy = (*ixiy & 0xFF00) | n;
                        return 11;
                    } else if (y == 6) {
                        int8_t d = (int8_t)FETCH();
                        uint8_t n = FETCH();
                        WB(*ixiy + d, n);
                        return 19;
                    }
                    goto unprefixed;
                case 7:
                    goto unprefixed;
            }
            break;

        case 1:
            /* LD r, r' with IX/IY substitution */
            if (y == 6 && z == 6) {
                /* LD (HL),(HL) = HALT, not affected */
                goto unprefixed;
            }
            if (y == 6) {
                /* LD (IX/IY+d), r */
                int8_t d = (int8_t)FETCH();
                uint8_t val = get_reg(cpu, z); /* source uses normal regs */
                WB(*ixiy + d, val);
                return 19;
            }
            if (z == 6) {
                /* LD r, (IX/IY+d) */
                int8_t d = (int8_t)FETCH();
                uint8_t val = RB(*ixiy + d);
                set_reg(cpu, y, val);
                return 19;
            }
            /* LD with IXH/IXL/IYH/IYL substitution (undocumented) */
            {
                uint8_t val = get_reg_idx(cpu, z, *ixiy);
                set_reg_idx(cpu, y, ixiy, val);
                return 8;
            }

        case 2:
            /* ALU A, r with IX/IY substitution */
            {
                uint8_t val;
                int clk = 8;
                if (z == 6) {
                    int8_t d = (int8_t)FETCH();
                    val = RB(*ixiy + d);
                    clk = 19;
                } else {
                    val = get_reg_idx(cpu, z, *ixiy);
                }
                switch (y) {
                    case 0: alu_add8(cpu, val); break;
                    case 1: alu_adc8(cpu, val); break;
                    case 2: alu_sub8(cpu, val); break;
                    case 3: alu_sbc8(cpu, val); break;
                    case 4: alu_and(cpu, val);  break;
                    case 5: alu_xor(cpu, val);  break;
                    case 6: alu_or(cpu, val);   break;
                    case 7: alu_cp(cpu, val);   break;
                }
                return clk;
            }

        case 3:
            switch (z) {
                case 1:
                    if (q == 0 && p == 2) {
                        /* POP IX/IY */
                        *ixiy = POP();
                        return 14;
                    }
                    goto unprefixed;
                case 3:
                    if (op == 0xE3) {
                        /* EX (SP), IX/IY */
                        uint16_t val = RW(cpu->sp);
                        WW(cpu->sp, *ixiy);
                        *ixiy = val;
                        return 23;
                    }
                    goto unprefixed;
                case 5:
                    if (q == 0 && p == 2) {
                        /* PUSH IX/IY */
                        PUSH(*ixiy);
                        return 15;
                    }
                    goto unprefixed;
                case 7:
                    goto unprefixed;
                case 9: /* JP (IX/IY) — note: opcode E9 */
                    goto unprefixed;
                default:
                    if (op == 0xE9) {
                        /* JP (IX/IY) */
                        cpu->pc = *ixiy;
                        return 8;
                    }
                    if (op == 0xF9) {
                        /* LD SP, IX/IY */
                        cpu->sp = *ixiy;
                        return 10;
                    }
                    goto unprefixed;
            }
    }

unprefixed:
    /* Instruction not affected by DD/FD prefix — re-execute as unprefixed.
     * We've already fetched the opcode and incremented R, so we need to
     * execute it directly. Rather than duplicating the main decoder, we
     * push PC back and let the main loop handle it, but we've already
     * consumed 4 T-states for the prefix. */
    cpu->pc--;
    INC_R(); /* Undo the R increment — main decoder will do it. Actually,
                the prefix itself does count as an R increment, so the
                INC_R in this function is for the prefix. We need to undo
                the second INC_R we did at the top of this function,
                since the main decoder will increment R again. */
    /* Actually, the R was incremented once for the prefix fetch (in exec_ddfd's
     * caller if we track it) and once here for the opcode fetch. If the opcode
     * is re-executed by the main loop, it will increment R again. So we need to
     * decrement R to compensate. */
    cpu->r = (cpu->r & 0x80) | ((cpu->r - 1) & 0x7F);
    return 4; /* 4 T-states for the prefix byte */
}

/* ── Main instruction decoder ──────────────────────────────────────── */

int z80_step(z80 *cpu) {
    if (!tables_initialized) {
        init_tables();
        tables_initialized = 1;
    }

    /* Handle EI delay: after EI, interrupts aren't enabled until after
     * the NEXT instruction */
    if (cpu->ei_delay) {
        cpu->ei_delay--;
        if (cpu->ei_delay == 0) {
            cpu->iff1 = cpu->iff2 = 1;
        }
    }

    if (cpu->halted) {
        /* In HALT state, the CPU continuously executes NOPs */
        INC_R();
        cpu->clocks += 4;
        return 4;
    }

    uint8_t op = FETCH();
    INC_R();

    int clk = 0;

    /* Handle prefix bytes */
    if (op == 0xCB) {
        clk = exec_cb(cpu);
        cpu->clocks += clk;
        return clk;
    }
    if (op == 0xED) {
        clk = exec_ed(cpu);
        cpu->clocks += clk;
        return clk;
    }
    if (op == 0xDD) {
        clk = exec_ddfd(cpu, &cpu->ix);
        cpu->clocks += clk;
        return clk;
    }
    if (op == 0xFD) {
        clk = exec_ddfd(cpu, &cpu->iy);
        cpu->clocks += clk;
        return clk;
    }

    /* Unprefixed instruction decode using x/y/z fields */
    int x = X(op), y = Y(op), z = Z(op);
    int p = P(op), q = Q(op);

    switch (x) {
        case 0:
            switch (z) {
                case 0:
                    switch (y) {
                        case 0: /* NOP */
                            clk = 4;
                            break;
                        case 1: /* EX AF, AF' */
                            { uint8_t t;
                              t = cpu->a; cpu->a = cpu->a_; cpu->a_ = t;
                              t = cpu->f; cpu->f = cpu->f_; cpu->f_ = t;
                            }
                            clk = 4;
                            break;
                        case 2: { /* DJNZ d */
                            int8_t d = (int8_t)FETCH();
                            cpu->b--;
                            if (cpu->b) {
                                cpu->pc += d;
                                clk = 13;
                            } else {
                                clk = 8;
                            }
                            break;
                        }
                        case 3: { /* JR d */
                            int8_t d = (int8_t)FETCH();
                            cpu->pc += d;
                            clk = 12;
                            break;
                        }
                        default: { /* JR cc, d (y=4..7 → cc=0..3) */
                            int8_t d = (int8_t)FETCH();
                            if (eval_cc(cpu, y - 4)) {
                                cpu->pc += d;
                                clk = 12;
                            } else {
                                clk = 7;
                            }
                            break;
                        }
                    }
                    break;

                case 1:
                    if (q == 0) {
                        /* LD rp[p], nn */
                        set_rp(cpu, p, fetch16(cpu));
                        clk = 10;
                    } else {
                        /* ADD HL, rp[p] */
                        uint16_t hl = HL();
                        alu_add16(cpu, &hl, get_rp(cpu, p));
                        SET_HL(hl);
                        clk = 11;
                    }
                    break;

                case 2:
                    switch (y) {
                        case 0: /* LD (BC), A */
                            WB(BC(), cpu->a);
                            clk = 7;
                            break;
                        case 1: /* LD A, (BC) */
                            cpu->a = RB(BC());
                            clk = 7;
                            break;
                        case 2: /* LD (DE), A */
                            WB(DE(), cpu->a);
                            clk = 7;
                            break;
                        case 3: /* LD A, (DE) */
                            cpu->a = RB(DE());
                            clk = 7;
                            break;
                        case 4: { /* LD (nn), HL */
                            uint16_t addr = fetch16(cpu);
                            WW(addr, HL());
                            clk = 16;
                            break;
                        }
                        case 5: { /* LD HL, (nn) */
                            uint16_t addr = fetch16(cpu);
                            SET_HL(RW(addr));
                            clk = 16;
                            break;
                        }
                        case 6: { /* LD (nn), A */
                            uint16_t addr = fetch16(cpu);
                            WB(addr, cpu->a);
                            clk = 13;
                            break;
                        }
                        case 7: { /* LD A, (nn) */
                            uint16_t addr = fetch16(cpu);
                            cpu->a = RB(addr);
                            clk = 13;
                            break;
                        }
                    }
                    break;

                case 3:
                    if (q == 0) {
                        /* INC rp[p] */
                        set_rp(cpu, p, get_rp(cpu, p) + 1);
                        clk = 6;
                    } else {
                        /* DEC rp[p] */
                        set_rp(cpu, p, get_rp(cpu, p) - 1);
                        clk = 6;
                    }
                    break;

                case 4: {
                    /* INC r[y] */
                    if (y == 6) {
                        uint16_t addr = HL();
                        WB(addr, alu_inc8(cpu, RB(addr)));
                        clk = 11;
                    } else {
                        uint8_t val = get_reg(cpu, y);
                        set_reg(cpu, y, alu_inc8(cpu, val));
                        clk = 4;
                    }
                    break;
                }

                case 5: {
                    /* DEC r[y] */
                    if (y == 6) {
                        uint16_t addr = HL();
                        WB(addr, alu_dec8(cpu, RB(addr)));
                        clk = 11;
                    } else {
                        uint8_t val = get_reg(cpu, y);
                        set_reg(cpu, y, alu_dec8(cpu, val));
                        clk = 4;
                    }
                    break;
                }

                case 6: {
                    /* LD r[y], n */
                    uint8_t n = FETCH();
                    if (y == 6) {
                        WB(HL(), n);
                        clk = 10;
                    } else {
                        set_reg(cpu, y, n);
                        clk = 7;
                    }
                    break;
                }

                case 7:
                    switch (y) {
                        case 0: { /* RLCA */
                            uint8_t c = cpu->a >> 7;
                            cpu->a = (cpu->a << 1) | c;
                            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
                                   | (cpu->a & (Z80_YF | Z80_XF))
                                   | c;
                            clk = 4;
                            break;
                        }
                        case 1: { /* RRCA */
                            uint8_t c = cpu->a & 1;
                            cpu->a = (cpu->a >> 1) | (c << 7);
                            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
                                   | (cpu->a & (Z80_YF | Z80_XF))
                                   | c;
                            clk = 4;
                            break;
                        }
                        case 2: { /* RLA */
                            uint8_t c = cpu->a >> 7;
                            cpu->a = (cpu->a << 1) | (cpu->f & Z80_CF);
                            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
                                   | (cpu->a & (Z80_YF | Z80_XF))
                                   | c;
                            clk = 4;
                            break;
                        }
                        case 3: { /* RRA */
                            uint8_t c = cpu->a & 1;
                            cpu->a = (cpu->a >> 1) | ((cpu->f & Z80_CF) << 7);
                            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
                                   | (cpu->a & (Z80_YF | Z80_XF))
                                   | c;
                            clk = 4;
                            break;
                        }
                        case 4: { /* DAA */
                            uint8_t a = cpu->a;
                            uint8_t correction = 0;
                            uint8_t carry = 0;
                            if ((cpu->f & Z80_HF) || (a & 0x0F) > 9)
                                correction |= 0x06;
                            if ((cpu->f & Z80_CF) || a > 0x99) {
                                correction |= 0x60;
                                carry = Z80_CF;
                            }
                            if (cpu->f & Z80_NF) {
                                cpu->a -= correction;
                                cpu->f = sz53p[cpu->a]
                                       | (cpu->f & Z80_NF)
                                       | carry
                                       | ((a ^ cpu->a) & Z80_HF);
                            } else {
                                cpu->a += correction;
                                cpu->f = sz53p[cpu->a]
                                       | carry
                                       | ((a ^ cpu->a) & Z80_HF);
                            }
                            clk = 4;
                            break;
                        }
                        case 5: /* CPL */
                            cpu->a = ~cpu->a;
                            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF | Z80_CF))
                                   | (cpu->a & (Z80_YF | Z80_XF))
                                   | Z80_HF | Z80_NF;
                            clk = 4;
                            break;
                        case 6: /* SCF */
                            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
                                   | (cpu->a & (Z80_YF | Z80_XF))
                                   | Z80_CF;
                            clk = 4;
                            break;
                        case 7: /* CCF */
                            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
                                   | (cpu->a & (Z80_YF | Z80_XF))
                                   | ((cpu->f & Z80_CF) ? Z80_HF : 0)
                                   | ((cpu->f & Z80_CF) ^ Z80_CF);
                            clk = 4;
                            break;
                    }
                    break;
            }
            break;

        case 1:
            if (y == 6 && z == 6) {
                /* HALT */
                cpu->halted = 1;
                cpu->pc--; /* PC stays on HALT instruction */
                clk = 4;
            } else {
                /* LD r[y], r[z] */
                set_reg(cpu, y, get_reg(cpu, z));
                clk = (y == 6 || z == 6) ? 7 : 4;
            }
            break;

        case 2: {
            /* ALU[y] A, r[z] */
            uint8_t val = get_reg(cpu, z);
            switch (y) {
                case 0: alu_add8(cpu, val); break;
                case 1: alu_adc8(cpu, val); break;
                case 2: alu_sub8(cpu, val); break;
                case 3: alu_sbc8(cpu, val); break;
                case 4: alu_and(cpu, val);  break;
                case 5: alu_xor(cpu, val);  break;
                case 6: alu_or(cpu, val);   break;
                case 7: alu_cp(cpu, val);   break;
            }
            clk = (z == 6) ? 7 : 4;
            break;
        }

        case 3:
            switch (z) {
                case 0:
                    /* RET cc[y] */
                    if (eval_cc(cpu, y)) {
                        cpu->pc = POP();
                        clk = 11;
                    } else {
                        clk = 5;
                    }
                    break;

                case 1:
                    if (q == 0) {
                        /* POP rp2[p] */
                        set_rp2(cpu, p, POP());
                        clk = 10;
                    } else {
                        switch (p) {
                            case 0: /* RET */
                                cpu->pc = POP();
                                clk = 10;
                                break;
                            case 1: /* EXX */
                                { uint8_t t;
                                  t = cpu->b; cpu->b = cpu->b_; cpu->b_ = t;
                                  t = cpu->c; cpu->c = cpu->c_; cpu->c_ = t;
                                  t = cpu->d; cpu->d = cpu->d_; cpu->d_ = t;
                                  t = cpu->e; cpu->e = cpu->e_; cpu->e_ = t;
                                  t = cpu->h; cpu->h = cpu->h_; cpu->h_ = t;
                                  t = cpu->l; cpu->l = cpu->l_; cpu->l_ = t;
                                }
                                clk = 4;
                                break;
                            case 2: /* JP (HL) */
                                cpu->pc = HL();
                                clk = 4;
                                break;
                            case 3: /* LD SP, HL */
                                cpu->sp = HL();
                                clk = 6;
                                break;
                        }
                    }
                    break;

                case 2: {
                    /* JP cc[y], nn */
                    uint16_t addr = fetch16(cpu);
                    if (eval_cc(cpu, y))
                        cpu->pc = addr;
                    clk = 10;
                    break;
                }

                case 3:
                    switch (y) {
                        case 0: { /* JP nn */
                            cpu->pc = fetch16(cpu);
                            clk = 10;
                            break;
                        }
                        case 1: /* CB prefix — already handled above */
                            break;
                        case 2: { /* OUT (n), A */
                            uint8_t port = FETCH();
                            OUT((uint16_t)(cpu->a << 8) | port, cpu->a);
                            clk = 11;
                            break;
                        }
                        case 3: { /* IN A, (n) */
                            uint8_t port = FETCH();
                            cpu->a = IN((uint16_t)(cpu->a << 8) | port);
                            clk = 11;
                            break;
                        }
                        case 4: { /* EX (SP), HL */
                            uint16_t val = RW(cpu->sp);
                            WW(cpu->sp, HL());
                            SET_HL(val);
                            clk = 19;
                            break;
                        }
                        case 5: { /* EX DE, HL */
                            uint8_t t;
                            t = cpu->d; cpu->d = cpu->h; cpu->h = t;
                            t = cpu->e; cpu->e = cpu->l; cpu->l = t;
                            clk = 4;
                            break;
                        }
                        case 6: /* DI */
                            cpu->iff1 = cpu->iff2 = 0;
                            clk = 4;
                            break;
                        case 7: /* EI */
                            /* Interrupts are enabled after the NEXT instruction */
                            cpu->ei_delay = 1;
                            clk = 4;
                            break;
                    }
                    break;

                case 4: {
                    /* CALL cc[y], nn */
                    uint16_t addr = fetch16(cpu);
                    if (eval_cc(cpu, y)) {
                        PUSH(cpu->pc);
                        cpu->pc = addr;
                        clk = 17;
                    } else {
                        clk = 10;
                    }
                    break;
                }

                case 5:
                    if (q == 0) {
                        /* PUSH rp2[p] */
                        PUSH(get_rp2(cpu, p));
                        clk = 11;
                    } else {
                        if (p == 0) {
                            /* CALL nn */
                            uint16_t addr = fetch16(cpu);
                            PUSH(cpu->pc);
                            cpu->pc = addr;
                            clk = 17;
                        }
                        /* p=1: DD prefix, p=2: ED prefix, p=3: FD prefix
                         * These are handled above, should not reach here */
                    }
                    break;

                case 6: {
                    /* ALU[y] A, n */
                    uint8_t n = FETCH();
                    switch (y) {
                        case 0: alu_add8(cpu, n); break;
                        case 1: alu_adc8(cpu, n); break;
                        case 2: alu_sub8(cpu, n); break;
                        case 3: alu_sbc8(cpu, n); break;
                        case 4: alu_and(cpu, n);  break;
                        case 5: alu_xor(cpu, n);  break;
                        case 6: alu_or(cpu, n);   break;
                        case 7: alu_cp(cpu, n);   break;
                    }
                    clk = 7;
                    break;
                }

                case 7: {
                    /* RST y*8 */
                    PUSH(cpu->pc);
                    cpu->pc = y * 8;
                    clk = 11;
                    break;
                }
            }
            break;
    }

    cpu->clocks += clk;
    return clk;
}

/* ── Interrupt handling ────────────────────────────────────────────── */

void z80_interrupt(z80 *cpu, uint8_t data) {
    if (!cpu->iff1)
        return;

    cpu->halted = 0;
    cpu->iff1 = cpu->iff2 = 0;
    INC_R();

    switch (cpu->im) {
        case 0:
            /* Execute instruction from data bus. Most commonly RST 38h (0xFF) */
            if (data == 0xFF) {
                /* RST 38h */
                PUSH(cpu->pc);
                cpu->pc = 0x0038;
                cpu->clocks += 13;
            } else {
                /* General case: push current opcode back and execute */
                /* For simplicity, handle common RST instructions */
                if ((data & 0xC7) == 0xC7) {
                    /* RST instruction */
                    PUSH(cpu->pc);
                    cpu->pc = data & 0x38;
                    cpu->clocks += 13;
                }
                /* Other instructions on data bus are rare */
            }
            break;
        case 1:
            /* Always RST 38h */
            PUSH(cpu->pc);
            cpu->pc = 0x0038;
            cpu->clocks += 13;
            break;
        case 2: {
            /* Vectored: jump via table at (I<<8 | data) */
            PUSH(cpu->pc);
            uint16_t addr = ((uint16_t)cpu->i << 8) | (data & 0xFE);
            cpu->pc = RW(addr);
            cpu->clocks += 19;
            break;
        }
    }
}

void z80_nmi(z80 *cpu) {
    cpu->halted = 0;
    cpu->iff2 = cpu->iff1;
    cpu->iff1 = 0;
    cpu->ei_delay = 0; /* Clear any pending EI */
    INC_R();
    PUSH(cpu->pc);
    cpu->pc = 0x0066;
    cpu->clocks += 11;
}
