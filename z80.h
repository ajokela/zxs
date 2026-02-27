/*
 * z80.h — Clean room Z80 CPU emulator
 *
 * Instruction-at-a-time execution with memory/IO callbacks for system
 * decoupling. Accurate T-state tracking, all official and undocumented
 * instructions supported.
 */

#ifndef Z80_H
#define Z80_H

#include <stdint.h>

/* Flag bit positions in the F register */
#define Z80_SF  0x80  /* Sign */
#define Z80_ZF  0x40  /* Zero */
#define Z80_YF  0x20  /* Undocumented bit 5 (copy of result bit 5) */
#define Z80_HF  0x10  /* Half-carry */
#define Z80_XF  0x08  /* Undocumented bit 3 (copy of result bit 3) */
#define Z80_PF  0x04  /* Parity/Overflow */
#define Z80_NF  0x02  /* Subtract */
#define Z80_CF  0x01  /* Carry */

/* Forward declaration */
typedef struct z80 z80;

/* Memory and I/O callback types */
typedef uint8_t (*z80_mem_read_fn)(void *ctx, uint16_t addr);
typedef void    (*z80_mem_write_fn)(void *ctx, uint16_t addr, uint8_t val);
typedef uint8_t (*z80_io_read_fn)(void *ctx, uint16_t port);
typedef void    (*z80_io_write_fn)(void *ctx, uint16_t port, uint8_t val);

struct z80 {
    /* Main registers */
    uint8_t a, f;
    uint8_t b, c;
    uint8_t d, e;
    uint8_t h, l;

    /* Shadow registers */
    uint8_t a_, f_;
    uint8_t b_, c_;
    uint8_t d_, e_;
    uint8_t h_, l_;

    /* Index registers */
    uint16_t ix, iy;

    /* Control registers */
    uint16_t sp, pc;

    /* Special registers */
    uint8_t i;   /* Interrupt vector base */
    uint8_t r;   /* Memory refresh counter */

    /* Interrupt state */
    uint8_t iff1;   /* Interrupt flip-flop 1 */
    uint8_t iff2;   /* Interrupt flip-flop 2 */
    uint8_t im;     /* Interrupt mode (0, 1, or 2) */
    uint8_t halted; /* CPU is halted */

    /* EI delay: interrupts are not enabled until after the instruction
     * following EI, so we track this with a counter */
    uint8_t ei_delay;

    /* Timing */
    uint64_t clocks; /* Total T-states elapsed */

    /* Memory and I/O callbacks */
    z80_mem_read_fn  mem_read;
    z80_mem_write_fn mem_write;
    z80_io_read_fn   io_read;
    z80_io_write_fn  io_write;
    void *ctx; /* Opaque context passed to callbacks */
};

/* Initialize CPU to power-on state */
void z80_init(z80 *cpu);

/* Execute one instruction, return T-states consumed */
int z80_step(z80 *cpu);

/* Request a maskable interrupt with data byte from bus */
void z80_interrupt(z80 *cpu, uint8_t data);

/* Request a non-maskable interrupt */
void z80_nmi(z80 *cpu);

#endif /* Z80_H */
