#ifndef Z80_H
#define Z80_H

#include <stdint.h>

typedef uint8_t  (*z80_read_fn)(void *ctx, uint16_t addr);
typedef void     (*z80_write_fn)(void *ctx, uint16_t addr, uint8_t val);
typedef uint8_t  (*z80_in_fn)(void *ctx, uint16_t port);
typedef void     (*z80_out_fn)(void *ctx, uint16_t port, uint8_t val);

typedef struct {
    /* Main registers */
    uint8_t  A, F;
    uint8_t  B, C;
    uint8_t  D, E;
    uint8_t  H, L;

    /* Shadow registers */
    uint8_t  A_, F_;
    uint8_t  B_, C_;
    uint8_t  D_, E_;
    uint8_t  H_, L_;

    /* Index registers */
    uint16_t IX, IY;

    /* Stack pointer and program counter */
    uint16_t SP, PC;

    /* Interrupt and refresh registers */
    uint8_t  I, R;

    /* Interrupt state */
    uint8_t  IFF1, IFF2;
    uint8_t  IM;          /* Interrupt mode: 0, 1, or 2 */
    uint8_t  halted;
    uint8_t  ei_delay;    /* EI takes effect after next instruction */

    /* Cycle counter */
    unsigned long t_states;

    /* Memory callbacks */
    z80_read_fn  mem_read;
    z80_write_fn mem_write;

    /* I/O callbacks */
    z80_in_fn    io_in;
    z80_out_fn   io_out;

    /* Opaque context passed to callbacks */
    void *ctx;
} z80_t;

/* Flag bit positions */
#define Z80_CF  0x01  /* Carry */
#define Z80_NF  0x02  /* Add/Subtract */
#define Z80_PF  0x04  /* Parity/Overflow */
#define Z80_F3  0x08  /* Undocumented bit 3 */
#define Z80_HF  0x10  /* Half Carry */
#define Z80_F5  0x20  /* Undocumented bit 5 */
#define Z80_ZF  0x40  /* Zero */
#define Z80_SF  0x80  /* Sign */

void z80_init(z80_t *cpu);
int  z80_step(z80_t *cpu);    /* Execute one instruction, return T-states used */
void z80_interrupt(z80_t *cpu, uint8_t data);  /* Request maskable interrupt */
void z80_nmi(z80_t *cpu);     /* Request non-maskable interrupt */

#endif /* Z80_H */
