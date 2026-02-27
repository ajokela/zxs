/*
 * zxs.c — Unified Z80 system emulator
 *
 * Supports two system types, auto-detected from file extension:
 *
 *   BASIC SBC — Grant Searle's minimal Z80 SBC running NASCOM BASIC 4.7
 *     8K ROM at 0x0000-0x1FFF, 56K RAM, MC6850 ACIA on ports 0x80/0x81
 *
 *   CP/M — Minimal CP/M environment for running .COM files (e.g. ZEXDOC)
 *     64K flat memory, loads .COM at 0x0100, BDOS intercept at CALL 5
 *
 * Usage:
 *   ./zxs <file>                  # auto-detect from extension
 *   ./zxs --system cpm <file>     # force CP/M mode
 *   ./zxs --system basic <file>   # force BASIC SBC mode
 */

#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <errno.h>

/* ── System types ──────────────────────────────────────────────────── */

enum system_type {
    SYS_BASIC,
    SYS_CPM,
};

/* ── Memory ────────────────────────────────────────────────────────── */

#define MEM_SIZE 0x10000 /* 64K */

static uint8_t memory[MEM_SIZE];
static uint16_t rom_size; /* 0 for CP/M, 0x2000 for BASIC */

static uint8_t mem_read(void *ctx, uint16_t addr) {
    (void)ctx;
    return memory[addr];
}

static void mem_write(void *ctx, uint16_t addr, uint8_t val) {
    (void)ctx;
    if (addr < rom_size) return; /* ROM is read-only */
    memory[addr] = val;
}

/* ── File loading helpers ──────────────────────────────────────────── */

/* Load Intel HEX file into memory */
static int load_hex(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != ':') continue;
        int count, addr, type;
        if (sscanf(line + 1, "%2x%4x%2x", &count, &addr, &type) != 3)
            continue;
        if (type == 1) break; /* EOF record */
        if (type != 0) continue; /* Only data records */
        for (int i = 0; i < count; i++) {
            int byte;
            sscanf(line + 9 + i * 2, "%2x", &byte);
            memory[addr + i] = byte;
        }
    }
    fclose(f);
    return 0;
}

/* Load raw binary file at given address, up to max_size bytes */
static int load_bin(const char *path, uint16_t base, size_t max_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(memory + base, 1, max_size, f);
    fclose(f);
    return n > 0 ? 0 : -1;
}

/* ── Terminal setup ────────────────────────────────────────────────── */

static struct termios orig_termios;
static int terminal_raw = 0;

static void terminal_restore(void) {
    if (terminal_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        terminal_raw = 0;
    }
}

static void signal_handler(int sig) {
    (void)sig;
    terminal_restore();
    _exit(0);
}

static void terminal_setup(void) {
    if (!isatty(STDIN_FILENO)) return;

    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(terminal_restore);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* Keep OPOST so \r\n works naturally */
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    /* Keep ISIG so Ctrl-C terminates cleanly */
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    terminal_raw = 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * BASIC SBC system
 * ═══════════════════════════════════════════════════════════════════ */

/* ── ACIA (MC6850) emulation ───────────────────────────────────────── */

/*
 * Status register bits:
 *   bit 0: RDRF — Receive Data Register Full (data available)
 *   bit 1: TDRE — Transmit Data Register Empty (always ready)
 *   bit 7: IRQ  — Interrupt pending
 */

static uint8_t acia_control; /* Last value written to control register */
static uint8_t acia_rx_data; /* Received byte waiting to be read */
static int acia_rx_full;     /* RDRF: data available in rx_data */

/* Check if stdin has data available (non-blocking) */
static int stdin_ready(void) {
    fd_set fds;
    struct timeval tv = {0, 0};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

/* Try to fill the ACIA receive register from stdin */
static void acia_poll(void) {
    if (acia_rx_full) return;
    if (stdin_ready()) {
        uint8_t ch;
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            acia_rx_data = ch;
            acia_rx_full = 1;
        }
    }
}

static uint8_t acia_read_status(void) {
    acia_poll();
    uint8_t status = 0x02; /* TDRE always set */
    if (acia_rx_full)
        status |= 0x01;    /* RDRF */
    if ((acia_control & 0x80) && acia_rx_full)
        status |= 0x80;    /* IRQ */
    return status;
}

static uint8_t acia_read_data(void) {
    acia_rx_full = 0;
    return acia_rx_data;
}

static void acia_write_control(uint8_t val) {
    acia_control = val;
    /* Master reset when CR1:CR0 = 11 */
    if ((val & 0x03) == 0x03) {
        acia_rx_full = 0;
        acia_rx_data = 0;
    }
}

static void acia_write_data(uint8_t val) {
    if (write(STDOUT_FILENO, &val, 1) < 0) { /* ignore errors */ }
}

/* ── BASIC I/O callbacks ──────────────────────────────────────────── */

static uint8_t sys_basic_io_read(void *ctx, uint16_t port) {
    (void)ctx;
    uint8_t addr = port & 0xFF;
    if (addr & 0x80) {
        if (addr & 0x01)
            return acia_read_data();
        else
            return acia_read_status();
    }
    return 0xFF;
}

static void sys_basic_io_write(void *ctx, uint16_t port, uint8_t val) {
    (void)ctx;
    uint8_t addr = port & 0xFF;
    if (addr & 0x80) {
        if (addr & 0x01)
            acia_write_data(val);
        else
            acia_write_control(val);
    }
}

/* ── BASIC init & main loop ───────────────────────────────────────── */

static int sys_basic_init(z80 *cpu, const char *path) {
    rom_size = 0x2000;

    memset(memory, 0xFF, rom_size);           /* Fill ROM area with 0xFF */
    memset(memory + rom_size, 0, MEM_SIZE - rom_size); /* Clear RAM */

    /* Detect format by extension */
    const char *ext = strrchr(path, '.');
    int rc;
    if (ext && strcasecmp(ext, ".hex") == 0)
        rc = load_hex(path);
    else
        rc = load_bin(path, 0, rom_size);

    if (rc < 0) {
        fprintf(stderr, "Error: cannot load ROM from %s\n", path);
        return -1;
    }

    if (memory[0] == 0xFF && memory[1] == 0xFF)
        fprintf(stderr, "Warning: ROM appears empty\n");

    /* Set up terminal and ACIA */
    terminal_setup();
    acia_control = 0;
    acia_rx_data = 0;
    acia_rx_full = 0;

    /* Configure CPU callbacks */
    cpu->io_read = sys_basic_io_read;
    cpu->io_write = sys_basic_io_write;

    return 0;
}

/* Number of CPU cycles between input polls (~1ms at 7.3728 MHz) */
#define CYCLES_PER_POLL 7373

static void sys_basic_run(z80 *cpu) {
    int cycles_until_poll = CYCLES_PER_POLL;

    for (;;) {
        int clk = z80_step(cpu);
        cycles_until_poll -= clk;

        if (cycles_until_poll <= 0) {
            cycles_until_poll = CYCLES_PER_POLL;
            acia_poll();
            if (acia_rx_full && (acia_control & 0x80) && cpu->iff1)
                z80_interrupt(cpu, 0xFF); /* RST 38h */
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * CP/M system
 * ═══════════════════════════════════════════════════════════════════ */

/* ── CP/M I/O callbacks ───────────────────────────────────────────── */

static uint8_t sys_cpm_io_read(void *ctx, uint16_t port) {
    (void)ctx; (void)port;
    return 0xFF;
}

static void sys_cpm_io_write(void *ctx, uint16_t port, uint8_t val) {
    (void)ctx; (void)port; (void)val;
}

/* ── CP/M BDOS ────────────────────────────────────────────────────── */

static void sys_cpm_bdos(z80 *cpu) {
    uint8_t func = cpu->c;
    switch (func) {
        case 2: /* Console output: char in E */
            putchar(cpu->e);
            fflush(stdout);
            break;
        case 9: { /* Print string: DE points to '$'-terminated string */
            uint16_t addr = (cpu->d << 8) | cpu->e;
            while (1) {
                uint8_t ch = memory[addr++];
                if (ch == '$') break;
                putchar(ch);
            }
            fflush(stdout);
            break;
        }
        default:
            break;
    }
}

/* ── CP/M init & main loop ────────────────────────────────────────── */

static int sys_cpm_init(z80 *cpu, const char *path) {
    rom_size = 0; /* No ROM protection */

    memset(memory, 0, sizeof(memory));

    /* Load .COM file at 0x0100 */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 65536 - 0x100) {
        fprintf(stderr, "Error: file too large (%ld bytes)\n", size);
        fclose(f);
        return -1;
    }
    if (fread(&memory[0x0100], 1, size, f) != (size_t)size) {
        fprintf(stderr, "Error: failed to read file\n");
        fclose(f);
        return -1;
    }
    fclose(f);

    /* BDOS intercept: RET at address 5 */
    memory[0x0005] = 0xC9;

    /* Warm boot intercept: HALT at address 0 */
    memory[0x0000] = 0x76;

    /* Configure CPU */
    cpu->io_read = sys_cpm_io_read;
    cpu->io_write = sys_cpm_io_write;
    cpu->pc = 0x0100;
    cpu->sp = 0xFFFE;

    /* Push 0x0000 as return address — RET from main → warm boot → exit */
    cpu->sp -= 2;
    memory[cpu->sp] = 0x00;
    memory[cpu->sp + 1] = 0x00;

    return 0;
}

static void sys_cpm_run(z80 *cpu, const char *filename) {
    printf("Running %s...\n", filename);

    for (;;) {
        if (cpu->pc == 0x0005)
            sys_cpm_bdos(cpu);

        if (cpu->pc == 0x0000 || cpu->halted)
            break;

        z80_step(cpu);
    }

    printf("\n\nProgram terminated. %llu T-states executed.\n",
           (unsigned long long)cpu->clocks);
}

/* ═══════════════════════════════════════════════════════════════════
 * Main — argument parsing and system dispatch
 * ═══════════════════════════════════════════════════════════════════ */

static int has_extension(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static enum system_type detect_system(const char *path) {
    if (has_extension(path, ".com") || has_extension(path, ".cim"))
        return SYS_CPM;
    return SYS_BASIC;
}

int main(int argc, char *argv[]) {
    const char *file_path = NULL;
    enum system_type sys = SYS_BASIC;
    int sys_forced = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--system") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --system requires an argument (basic or cpm)\n");
                return 1;
            }
            i++;
            if (strcmp(argv[i], "cpm") == 0) {
                sys = SYS_CPM;
                sys_forced = 1;
            } else if (strcmp(argv[i], "basic") == 0) {
                sys = SYS_BASIC;
                sys_forced = 1;
            } else {
                fprintf(stderr, "Error: unknown system type '%s' (use basic or cpm)\n",
                        argv[i]);
                return 1;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            if (file_path) {
                fprintf(stderr, "Error: multiple files specified\n");
                return 1;
            }
            file_path = argv[i];
        }
    }

    /* BASIC mode: try default ROM filenames if no file given */
    if (!file_path && (!sys_forced || sys == SYS_BASIC)) {
        static const char *defaults[] = {
            "basic.rom", "R0000000.HEX", "ROM.HEX", NULL
        };
        for (int i = 0; defaults[i]; i++) {
            if (access(defaults[i], R_OK) == 0) {
                file_path = defaults[i];
                if (!sys_forced) sys = SYS_BASIC;
                break;
            }
        }
    }

    if (!file_path) {
        fprintf(stderr, "Usage: %s [--system basic|cpm] <file>\n", argv[0]);
        return 1;
    }

    /* Auto-detect system type from extension if not forced */
    if (!sys_forced)
        sys = detect_system(file_path);

    /* Initialize CPU */
    z80 cpu;
    z80_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;

    /* Initialize and run the selected system */
    if (sys == SYS_BASIC) {
        if (sys_basic_init(&cpu, file_path) < 0)
            return 1;
        sys_basic_run(&cpu);
    } else {
        if (sys_cpm_init(&cpu, file_path) < 0)
            return 1;
        sys_cpm_run(&cpu, file_path);
    }

    return 0;
}
