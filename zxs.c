#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

/* ── System types ────────────────────────────────────────────────── */

enum system_type { SYS_AUTO, SYS_BASIC, SYS_CPM };

/* ── Emulator state ──────────────────────────────────────────────── */

static uint8_t memory[65536];
static z80_t cpu;

/* ACIA state */
static uint8_t acia_rx_data;
static int     acia_rx_ready;
static int     acia_irq_enabled;
static uint16_t serial_base = 0x80; /* Status port; data port = base+1 */

/* Terminal state */
static struct termios orig_termios;
static int raw_mode = 0;
static volatile sig_atomic_t quit_flag = 0;

/* ── Terminal helpers ────────────────────────────────────────────── */

static void restore_terminal(void) {
    if (raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode = 0;
    }
}

static void set_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_terminal);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);  /* Raw output: ROM sends its own \r\n */
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode = 1;
}

static int char_available(void) {
    unsigned char ch;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    if (n == 1) {
        if (ch == 0x1D) { /* Ctrl+] exits emulator */
            quit_flag = 1;
            return 0;
        }
        acia_rx_data = ch;
        return 1;
    }
    return 0;
}

/* ── Signal handler ──────────────────────────────────────────────── */

static void sig_handler(int sig) {
    (void)sig;
    quit_flag = 1;
}

/* ── Memory callbacks ────────────────────────────────────────────── */

static uint8_t mem_read(void *ctx, uint16_t addr) {
    (void)ctx;
    return memory[addr];
}

static void mem_write(void *ctx, uint16_t addr, uint8_t val) {
    (void)ctx;
    memory[addr] = val;
}

/* ── BASIC SBC I/O callbacks ─────────────────────────────────────── */

static uint8_t basic_io_in(void *ctx, uint16_t port) {
    (void)ctx;
    uint8_t p = port & 0xFF;
    if (p == serial_base) {
        /* ACIA status register */
        uint8_t status = 0x02; /* TDRE always ready */
        if (acia_rx_ready)
            status |= 0x01; /* RDRF */
        return status;
    }
    if (p == (uint8_t)(serial_base + 1)) {
        /* ACIA data register */
        acia_rx_ready = 0;
        return acia_rx_data;
    }
    return 0xFF;
}

static void basic_io_out(void *ctx, uint16_t port, uint8_t val) {
    (void)ctx;
    uint8_t p = port & 0xFF;
    if (p == serial_base) {
        /* ACIA control register */
        if (val == 0x03) {
            /* Master reset */
            acia_rx_ready = 0;
            acia_irq_enabled = 0;
        } else {
            /* Check if receive interrupt enabled (bit 7) */
            acia_irq_enabled = (val & 0x80) ? 1 : 0;
        }
        return;
    }
    if (p == (uint8_t)(serial_base + 1)) {
        /* ACIA data register - transmit */
        if (val == '\r') {
            write(STDOUT_FILENO, "\r\n", 2);
        } else {
            char ch = val;
            write(STDOUT_FILENO, &ch, 1);
        }
        return;
    }
}

/* ── CP/M I/O callbacks ──────────────────────────────────────────── */

static uint8_t cpm_io_in(void *ctx, uint16_t port) {
    (void)ctx; (void)port;
    return 0xFF;
}

static void cpm_io_out(void *ctx, uint16_t port, uint8_t val) {
    (void)ctx; (void)port; (void)val;
}

/* ── File loading ────────────────────────────────────────────────── */

static int load_binary(const char *path, uint16_t addr) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 65536 - addr) sz = 65536 - addr;
    fread(&memory[addr], 1, sz, f);
    fclose(f);
    return (int)sz;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hex_byte(const char *s) {
    int hi = hex_nibble(s[0]);
    int lo = hex_nibble(s[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

static int load_hex(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }

    char line[600];
    int total = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Skip lines that don't start with : */
        if (line[0] != ':') continue;

        int len = hex_byte(&line[1]);
        if (len < 0) continue;

        int addr_hi = hex_byte(&line[3]);
        int addr_lo = hex_byte(&line[5]);
        if (addr_hi < 0 || addr_lo < 0) continue;
        uint16_t addr = (addr_hi << 8) | addr_lo;

        int type = hex_byte(&line[7]);
        if (type < 0) continue;

        if (type == 0x01) break; /* EOF record */
        if (type != 0x00) continue; /* Only process data records */

        for (int i = 0; i < len; i++) {
            int b = hex_byte(&line[9 + i * 2]);
            if (b < 0) break;
            memory[addr + i] = (uint8_t)b;
            total++;
        }
    }
    fclose(f);
    return total;
}

static int is_hex_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    if (strcasecmp(ext, ".hex") == 0) return 1;

    /* Check if file starts with : (Intel HEX format) */
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ch = fgetc(f);
    fclose(f);
    return ch == ':';
}

/* ── Serial port auto-detection ──────────────────────────────────── */

static uint16_t detect_serial_port(int rom_size) {
    /* Scan ROM for IN A,(n) [DB xx] and OUT (n),A [D3 xx] patterns */
    int in_count[256] = {0};
    int out_count[256] = {0};

    for (int i = 0; i < rom_size - 1; i++) {
        if (memory[i] == 0xDB)
            in_count[memory[i + 1]]++;
        else if (memory[i] == 0xD3)
            out_count[memory[i + 1]]++;
    }

    /* Look for adjacent port pairs where both IN and OUT appear.
       ACIA pattern: IN on status port + OUT on both ports.
       Score pairs that have both IN and OUT activity. */
    int best_port = 0x80;
    int best_score = 0;

    for (int p = 0; p < 255; p++) {
        /* Need both IN and OUT on the pair to be a serial port */
        int has_in  = in_count[p] + in_count[p + 1];
        int has_out = out_count[p] + out_count[p + 1];
        if (has_in == 0 || has_out == 0) continue;

        int score = has_in + has_out;
        if (score > best_score) {
            best_score = score;
            best_port = p;
        }
    }

    if (best_score > 0)
        return (uint16_t)best_port;
    return 0x80; /* Default */
}

/* ── System detection ────────────────────────────────────────────── */

static enum system_type detect_system(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return SYS_BASIC;

    if (strcasecmp(ext, ".com") == 0) return SYS_CPM;
    if (strcasecmp(ext, ".cim") == 0) return SYS_CPM;
    return SYS_BASIC;
}

/* ── BDOS emulation ──────────────────────────────────────────────── */

static int handle_bdos(void) {
    uint8_t fn = cpu.C;
    switch (fn) {
        case 2: /* C_WRITE: output character in E */
            {
                char ch = cpu.E;
                write(STDOUT_FILENO, &ch, 1);
            }
            break;
        case 9: /* C_WRITESTR: output $-terminated string at DE */
            {
                uint16_t addr = ((uint16_t)cpu.D << 8) | cpu.E;
                while (1) {
                    uint8_t ch = memory[addr++];
                    if (ch == '$') break;
                    write(STDOUT_FILENO, &ch, 1);
                    if (addr == 0) break; /* Wrapped */
                }
            }
            break;
        case 0: /* P_TERMCPM: terminate */
            return 1;
        default:
            break;
    }
    /* Execute RET to return from CALL 5 */
    cpu.PC = memory[cpu.SP] | ((uint16_t)memory[cpu.SP + 1] << 8);
    cpu.SP += 2;
    return 0;
}

/* ── Run modes ───────────────────────────────────────────────────── */

static void run_basic(void) {
    set_raw_mode();
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    while (!quit_flag) {
        /* Run ~7373 cycles (approximately 2ms at 3.6864 MHz) */
        unsigned long target = cpu.t_states + 7373;
        while (cpu.t_states < target) {
            z80_step(&cpu);
        }

        /* Poll for input */
        if (char_available()) {
            acia_rx_ready = 1;
            /* Deliver interrupt if enabled */
            if (acia_irq_enabled && cpu.IFF1) {
                z80_interrupt(&cpu, 0xFF); /* RST 38h */
            }
        }
    }

    restore_terminal();
}

static void run_cpm(void) {
    while (1) {
        /* Check for exit conditions */
        if (cpu.PC == 0x0000) break;
        if (cpu.halted) break;

        /* BDOS intercept at address 0x0005 */
        if (cpu.PC == 0x0005) {
            if (handle_bdos()) break;
            continue;
        }

        z80_step(&cpu);
    }
}

/* ── Usage ───────────────────────────────────────────────────────── */

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [options] <file>\n", argv0);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --system cpm|basic   Force system type\n");
    fprintf(stderr, "  --port <hex>         Override serial port base (e.g. 0x80)\n");
    fprintf(stderr, "\nAuto-detection:\n");
    fprintf(stderr, "  .com/.cim -> CP/M, everything else -> BASIC SBC\n");
    fprintf(stderr, "  Intel HEX files loaded by format, binary files at 0x0000\n");
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    enum system_type sys = SYS_AUTO;
    int port_override = -1;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--system") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "cpm") == 0) sys = SYS_CPM;
            else if (strcmp(argv[i], "basic") == 0) sys = SYS_BASIC;
            else { fprintf(stderr, "Unknown system: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            i++;
            port_override = (int)strtol(argv[i], NULL, 16);
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 1;
        } else {
            file = argv[i];
        }
    }

    if (!file) {
        usage(argv[0]);
        return 1;
    }

    /* Initialize CPU */
    memset(memory, 0, sizeof(memory));
    z80_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    cpu.ctx = NULL;

    /* Load file */
    int loaded;
    if (is_hex_file(file)) {
        loaded = load_hex(file);
        if (loaded < 0) return 1;
        fprintf(stderr, "Loaded %d bytes from HEX file\n", loaded);
    } else {
        /* Detect system for loading address */
        if (sys == SYS_AUTO) sys = detect_system(file);
        uint16_t load_addr = (sys == SYS_CPM) ? 0x0100 : 0x0000;
        loaded = load_binary(file, load_addr);
        if (loaded < 0) return 1;
        fprintf(stderr, "Loaded %d bytes at 0x%04X\n", loaded, load_addr);
    }

    /* Auto-detect system if needed */
    if (sys == SYS_AUTO) sys = detect_system(file);

    /* Configure system */
    if (sys == SYS_BASIC) {
        if (port_override >= 0) {
            serial_base = (uint16_t)port_override;
        } else {
            serial_base = detect_serial_port(loaded);
        }
        fprintf(stderr, "BASIC SBC mode, serial port base: 0x%02X (Ctrl+] to exit)\n", serial_base);
        cpu.io_in = basic_io_in;
        cpu.io_out = basic_io_out;
        cpu.PC = 0x0000;
        run_basic();
    } else {
        fprintf(stderr, "CP/M mode\n");
        cpu.io_in = cpm_io_in;
        cpu.io_out = cpm_io_out;
        cpu.PC = 0x0100;
        cpu.SP = 0xFFFE;
        /* Push return address 0x0000 for clean exit */
        cpu.SP -= 2;
        memory[cpu.SP] = 0x00;
        memory[cpu.SP + 1] = 0x00;
        run_cpm();
    }

    return 0;
}
