#include <avr/pgmspace.h>
#include "swd.h"
#include "dp.h"
#include "ap.h"
#include "cortex.h"
#include "flash.h"
#include "uart.h"
#include "xmodem.h"
#include "shell.h"

#define LINE_MAX 40

static const char help_text[] PROGMEM =
    "c              connect\r\n"
    "i              ids: DPIDR, AP IDR, CPUID, DBGMCU\r\n"
    "r <addr>       read word\r\n"
    "d <addr> [n]   dump n words (default 8)\r\n"
    "w <addr> <val> write word\r\n"
    "h              halt\r\n"
    "g              resume\r\n"
    "t              reset and halt\r\n"
    "s              status (DHCSR)\r\n"
    "u              unlock flash\r\n"
    "e <sector>     erase flash sector\r\n"
    "p <addr> <val> program flash word\r\n"
    "l <addr>       load binary via xmodem\r\n"
    "?              this help\r\n";

static void puts_P(const char *s)
{
    char c;
    while ((c = pgm_read_byte(s++)))
        uart_putc(c);
}

static void nl(void)
{
    uart_puts("\r\n");
}

static void put_hex32(uint32_t v)
{
    uart_puts("0x");
    uart_print_hex32(v);
}

static void report(uint8_t ack)
{
    if (ack == SWD_ACK_OK) {
        uart_puts("ok");
    } else {
        uart_puts("failed, ack=0x");
        uart_print_hex8(ack);
    }
    nl();
}

static uint8_t hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 0xFF;
}

static uint8_t parse_hex(const char **p, uint32_t *out)
{
    while (**p == ' ')
        (*p)++;

    if ((*p)[0] == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X'))
        *p += 2;

    uint32_t value = 0;
    uint8_t digits = 0;

    while (hex_digit(**p) != 0xFF) {
        value = (value << 4) | hex_digit(**p);
        (*p)++;
        digits++;
    }

    *out = value;
    return digits != 0;
}

static void read_line(char *buf)
{
    uint8_t n = 0;

    for (;;) {
        char c = uart_getc();

        if (c == '\r' || c == '\n') {
            nl();
            buf[n] = 0;
            return;
        }

        if ((c == 0x08 || c == 0x7F) && n) {
            uart_puts("\b \b");
            n--;
            continue;
        }

        if (c >= ' ' && n < LINE_MAX - 1) {
            uart_putc(c);
            buf[n++] = c;
        }
    }
}

static void cmd_connect(void)
{
    uint32_t idcode = 0;
    uint8_t ack = dp_connect(&idcode);

    uart_puts("DPIDR ");
    put_hex32(idcode);
    uart_puts("  ");

    if (ack != SWD_ACK_OK) {
        report(ack);
        return;
    }

    dp_power_up();
    dp_clear_errors();
    report(mem_ap_init());
}

static void cmd_ids(void)
{
    uint32_t v = 0;

    dp_read(DP_DPIDR, &v);
    uart_puts("DPIDR  ");
    put_hex32(v);
    nl();

    ap_select(0, 0xF);
    ap_read(AP_IDR, &v);
    uart_puts("AP IDR ");
    put_hex32(v);
    nl();
    ap_select(0, 0x0);

    mem_ap_read_word(0xE000ED00UL, &v);
    uart_puts("CPUID  ");
    put_hex32(v);
    nl();

    mem_ap_read_word(0xE0042000UL, &v);
    uart_puts("DBGMCU ");
    put_hex32(v);
    nl();
}

static void cmd_dump(uint32_t addr, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = addr + 4UL * i;

        if ((i & 3) == 0) {
            if (i)
                nl();
            put_hex32(a);
            uart_puts(":");
        }

        uint32_t v = 0;
        uint8_t ack = mem_ap_read_word(a, &v);

        uart_puts(" ");
        if (ack == SWD_ACK_OK)
            uart_print_hex32(v);
        else
            uart_puts("--------");
    }
    nl();
}

static void cmd_status(void)
{
    uint32_t dhcsr = 0;
    uint8_t ack = cortex_read_dhcsr(&dhcsr);

    if (ack != SWD_ACK_OK) {
        report(ack);
        return;
    }

    uart_puts("DHCSR ");
    put_hex32(dhcsr);
    uart_puts("  halted=");
    uart_putc((dhcsr & DHCSR_S_HALT) ? 'y' : 'n');
    uart_puts(" lockup=");
    uart_putc((dhcsr & DHCSR_S_LOCKUP) ? 'y' : 'n');
    nl();
}

static void cmd_load(uint32_t addr)
{
    /* Halt first so the target is not running while its flash changes. */
    cortex_halt();

    uart_puts("send binary now (erase the sectors first)\r\n");

    uint32_t written = 0;
    uint8_t result = xmodem_receive_to_flash(addr, &written);

    /* Read the sticky state before clearing it, so a failure is not hidden. */
    uint32_t ctrlstat = 0;
    uint8_t cs_ack = dp_read(DP_CTRLSTAT, &ctrlstat);

    /* Programming can leave a sticky error behind; do not hand back a dead link. */
    dp_clear_errors();

    nl();
    uart_print_dec(written);
    uart_puts(" bytes to ");
    put_hex32(addr);
    uart_puts(": ");

    switch (result) {
    case XMODEM_OK:       uart_puts("ok");             break;
    case XMODEM_TIMEOUT:  uart_puts("timed out");      break;
    case XMODEM_CANCELED: uart_puts("canceled");       break;
    default:              uart_puts("flash write failed"); break;
    }
    nl();

    uart_puts("naks=");
    uart_print_dec(xm_stats.naks_sent);
    uart_puts(" ok=");
    uart_print_dec(xm_stats.blocks_ok);
    uart_puts(" badsum=");
    uart_print_dec(xm_stats.bad_checksum);
    uart_puts(" badblk=");
    uart_print_dec(xm_stats.bad_blocknum);
    uart_puts(" resync=");
    uart_print_dec(xm_stats.resyncs);
    uart_puts(" first=0x");
    uart_print_hex32((uint32_t)(uint16_t)xm_stats.first_byte);
    uart_puts(" blk=0x");
    uart_print_hex32((uint32_t)(uint16_t)xm_stats.last_blk);
    uart_puts(" inv=0x");
    uart_print_hex32((uint32_t)(uint16_t)xm_stats.last_inv);
    nl();

    uart_puts("CTRL/STAT ");
    if (cs_ack == SWD_ACK_OK) {
        put_hex32(ctrlstat);
        if (ctrlstat & (1UL << 5))
            uart_puts(" STICKYERR");
        if (ctrlstat & (1UL << 1))
            uart_puts(" STICKYORUN");
        if (ctrlstat & (1UL << 7))
            uart_puts(" WDATAERR");
    } else {
        uart_puts("unreadable, ack=0x");
        uart_print_hex8(cs_ack);
    }
    nl();
}

static void dispatch(const char *line)
{
    while (*line == ' ')
        line++;

    char cmd = *line;
    if (!cmd)
        return;

    line++;

    uint32_t a = 0, b = 0;

    switch (cmd) {
    case 'c':
        cmd_connect();
        break;

    case 'i':
        cmd_ids();
        break;

    case 'r':
        if (!parse_hex(&line, &a)) {
            uart_puts("need an address\r\n");
            break;
        }
        {
            uint32_t v = 0;
            uint8_t ack = mem_ap_read_word(a, &v);
            if (ack == SWD_ACK_OK) {
                put_hex32(a);
                uart_puts(": ");
                put_hex32(v);
                nl();
            } else {
                report(ack);
            }
        }
        break;

    case 'd':
        if (!parse_hex(&line, &a)) {
            uart_puts("need an address\r\n");
            break;
        }
        if (!parse_hex(&line, &b))
            b = 8;
        cmd_dump(a, b);
        break;

    case 'w':
        if (!parse_hex(&line, &a) || !parse_hex(&line, &b)) {
            uart_puts("need an address and a value\r\n");
            break;
        }
        report(mem_ap_write_word(a, b));
        break;

    case 'h':
        report(cortex_halt());
        break;

    case 'g':
        report(cortex_resume());
        break;

    case 't':
        report(cortex_reset_halt());
        break;

    case 's':
        cmd_status();
        break;

    case 'u':
        report(flash_unlock());
        break;

    case 'e':
        if (!parse_hex(&line, &a)) {
            uart_puts("need a sector number\r\n");
            break;
        }
        report(flash_erase_sector((uint8_t)a));
        break;

    case 'p':
        if (!parse_hex(&line, &a) || !parse_hex(&line, &b)) {
            uart_puts("need an address and a value\r\n");
            break;
        }
        report(flash_program_word(a, b));
        break;

    case 'l':
        if (!parse_hex(&line, &a)) {
            uart_puts("need an address\r\n");
            break;
        }
        cmd_load(a);
        break;

    case '?':
        puts_P(help_text);
        break;

    default:
        uart_puts("unknown command, ? for help\r\n");
        break;
    }
}

void shell_run(void)
{
    char line[LINE_MAX];

    uart_puts("\r\nADIv5-AVR, ? for help\r\n");
    cmd_connect();

    for (;;) {
        uart_puts("> ");
        read_line(line);
        dispatch(line);
    }
}
