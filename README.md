# ZXS — Clean Room Z80 Emulator

A complete Z80 CPU emulator written under clean room constraints: all implementation derived from the [Zilog Z80 CPU User Manual](https://www.zilog.com/docs/z80/um0080.pdf) and community hardware documentation — no existing emulator source code referenced.

The CPU core was implemented by [Claude](https://claude.ai/claude-code) (LLM) from a detailed architectural plan and specification. See the [blog post](https://tinycomputers.io/posts/clean-room-z80-emulator.html) for the full write-up.

## Features

**Z80 CPU Core**
- All official instructions: unprefixed, CB, ED, DD/FD, DDCB/FDCB prefix groups
- Undocumented behaviors: F3/F5 flag bits, IXH/IXL/IYH/IYL half-index registers, DDCB/FDCB register copy side effects
- Accurate T-state cycle counting per instruction
- Interrupt modes 0, 1, and 2 with EI delay
- Non-maskable interrupts (NMI)
- R register with correct bit 7 preservation
- Callback-based memory and I/O for system independence

**BASIC SBC Mode**
- MC6850 ACIA serial emulation (status + data register pair)
- Automatic serial port detection by scanning ROM for IN/OUT patterns
- RST 38h interrupt delivery for received data
- Boots Grant Searle and RC2014 BASIC ROMs

**CP/M Mode**
- Loads .COM/.CIM files at 0x0100
- BDOS shim: console output (fn 2), string output (fn 9), program termination (fn 0)
- Clean exit on HALT or return to 0x0000

**General**
- Intel HEX file loading
- System auto-detection from file extension
- 117 unit tests covering all instruction groups
- Builds with zero warnings under `-Wall -Wextra`

## Building

```
make
```

Requires a C compiler (cc/gcc/clang). Produces two binaries:
- `zxs` — the emulator
- `z80_test` — the CPU test suite

## Usage

```
./zxs <file>                       # auto-detect system type
./zxs --system cpm <file>          # force CP/M mode
./zxs --system basic <file>        # force BASIC SBC mode
./zxs --port 0x80 <file>           # override serial port base address
```

### Examples

```
# Boot Grant Searle BASIC ROM
./zxs basic.rom

# Boot RC2014 BASIC from Intel HEX (ACIA at port 0x80)
./zxs --port 0x80 rc2014_56k.hex

# Run a CP/M .COM program
./zxs program.com
```

Press **Ctrl+]** to exit the emulator.

### System Auto-Detection

| Extension | Mode |
|-----------|------|
| `.com`, `.cim` | CP/M |
| Everything else | BASIC SBC |

Intel HEX files (`.hex` or any file starting with `:`) are parsed automatically regardless of extension.

### Serial Port Auto-Detection

In BASIC SBC mode, the emulator scans the loaded ROM for `IN A,(n)` and `OUT (n),A` instruction patterns to find the ACIA port pair. Use `--port` to override if the auto-detection picks the wrong address.

## Running Tests

```
make test
```

Or directly:

```
./z80_test
```

```
Z80 CPU Test Suite
==================
test_nop                                                    PASS
test_ld_reg_imm                                             PASS
...
test_r_bit7_preserved                                       PASS

==================
Results: 117/117 passed
```

## Project Structure

| File | Lines | Description |
|------|------:|-------------|
| `z80.h` | 69 | CPU state struct, flag constants, public API |
| `z80.c` | 1,328 | Full Z80 CPU emulation core |
| `z80_test.c` | 1,981 | 117 unit tests |
| `zxs.c` | 439 | Emulator binary (ACIA, CP/M, CLI) |
| `Makefile` | 18 | Build system |

## Clean Room Methodology

This emulator was developed under clean room constraints to ensure the implementation is derived from specifications rather than existing emulator source code.

**Permitted sources:**
- [Zilog Z80 CPU User Manual](https://www.zilog.com/docs/z80/um0080.pdf) (official instruction set, timing, architecture)
- [The Undocumented Z80 Documented](http://www.z80.info/zip/z80-documented.pdf) by Sean Young (undocumented flag behavior, half-index registers)
- Hardware documentation for the MC6850 ACIA
- Intel HEX format specification

**Not referenced:**
- MAME, FUSE, or any other Z80 emulator source code
- Existing open-source Z80 implementations on GitHub or elsewhere

The CPU core was implemented by Claude (Anthropic's LLM) from a detailed architectural plan specifying the bit field decoding strategy, callback interfaces, and test structure. This approach was inspired by [antirez's Z80 emulator experiment](https://antirez.com/news/160).

## API

The CPU core can be embedded in other projects. The interface is four functions:

```c
void z80_init(z80_t *cpu);                      // Initialize CPU state
int  z80_step(z80_t *cpu);                      // Execute one instruction, return T-states
void z80_interrupt(z80_t *cpu, uint8_t data);   // Request maskable interrupt
void z80_nmi(z80_t *cpu);                       // Request non-maskable interrupt
```

Set the callback fields on the `z80_t` struct before calling `z80_step`:

```c
z80_t cpu;
z80_init(&cpu);
cpu.mem_read  = my_read;
cpu.mem_write = my_write;
cpu.io_in     = my_in;
cpu.io_out    = my_out;
cpu.ctx       = &my_system;  // passed to all callbacks

while (running) {
    int cycles = z80_step(&cpu);
    // ... poll peripherals, deliver interrupts ...
}
```

## License

BSD 3-Clause. See [LICENSE](LICENSE).
