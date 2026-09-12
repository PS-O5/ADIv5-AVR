#include <avr/pgmspace.h>
#include "swd.h"
#include "dp.h"
#include "ap.h"
#include "cortex.h"
#include "flash.h"
#include "fpb.h"
#include "dwt.h"
#include "uart.h"
#include "xmodem.h"
#include "shell.h"

#define LINE_MAX 40

static const char help_text[] PROGMEM =
    "c              connect\r\n"
    "i              ids: DPIDR, AP IDR, CPUID, DBGMCU\r\n"
    "r <addr> [sz]  read, sz 1 2 or 4 (default 4)\r\n"
    "d <addr> [n]   dump n words (default 8)\r\n"
    "w <a> <v> [sz] write, sz 1 2 or 4 (default 4)\r\n"
    "h              halt\r\n"
    "g              resume\r\n"
    "t              reset and halt\r\n"
    "s              status (DHCSR)\r\n"
    "u              unlock flash\r\n"
    "f              flash size and sector map\r\n"
    "e <sect|addr>  erase sector, or the one holding addr\r\n"
    "p <addr> <val> program flash word\r\n"
    "l <addr>       load binary via xmodem\r\n"
    "y <addr> <len> save memory via xmodem\r\n"
    "x              core registers (halted only)\r\n"
    "x <reg> <val>  write reg: r0-r12, sp, lr, pc, psr\r\n"
    "n [count]      step one instruction\r\n"
    "m [count]      step with interrupts masked\r\n"
    "b              list breakpoints\r\n"
    "b <addr>       set hardware breakpoint\r\n"
    "k [slot]       clear one breakpoint, or all\r\n"
    "a              list watchpoints\r\n"
    "a <addr> [rwb] watch: r read, w write, b both\r\n"
    "j [slot]       clear one watchpoint, or all\r\n"
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

    /* More than eight digits silently means a different address than intended. */
    if (digits == 0 || digits > 8)
        return 0;

    /* Only on success, so a caller's default survives a missing argument. */
    *out = value;
    return 1;
}

static uint8_t parse_dec(const char **p, uint32_t *out)
{
    while (**p == ' ')
        (*p)++;

    uint32_t value = 0;
    uint8_t digits = 0;

    while (**p >= '0' && **p <= '9') {
        value = value * 10 + (uint32_t)(**p - '0');
        (*p)++;
        digits++;
    }

    if (digits == 0)
        return 0;

    *out = value;
    return 1;
}

/* Names or decimal numbers. Hex here would read "15" as 21 and pick the wrong one. */
static uint8_t parse_reg(const char **p, uint8_t *reg)
{
    while (**p == ' ')
        (*p)++;

    const char *s = *p;

    if (s[0] == 's' && s[1] == 'p') { *p += 2; *reg = REG_SP;   return 1; }
    if (s[0] == 'l' && s[1] == 'r') { *p += 2; *reg = REG_LR;   return 1; }
    if (s[0] == 'p' && s[1] == 'c') { *p += 2; *reg = REG_PC;   return 1; }
    if (s[0] == 'p' && s[1] == 's' && s[2] == 'r') { *p += 3; *reg = REG_XPSR; return 1; }

    uint32_t n = 0;

    if (s[0] == 'r' && s[1] >= '0' && s[1] <= '9') {
        (*p)++;
        if (!parse_dec(p, &n) || n > 12)
            return 0;
        *reg = (uint8_t)n;
        return 1;
    }

    if (parse_dec(p, &n) && n <= REG_LAST) {
        *reg = (uint8_t)n;
        return 1;
    }

    return 0;
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

    ack = mem_ap_init();
    if (ack == SWD_ACK_OK) {
        fpb_init();
        dwt_init();
        flash_probe();
    }

    report(ack);
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

static void reg_label(uint8_t i)
{
    if (i < 13) {
        uart_putc('r');
        uart_print_dec(i);
        if (i < 10)
            uart_putc(' ');
    } else if (i == REG_SP) {
        uart_puts("sp ");
    } else if (i == REG_LR) {
        uart_puts("lr ");
    } else if (i == REG_PC) {
        uart_puts("pc ");
    } else {
        uart_puts("psr");
    }
}

static void cmd_regs(void)
{
    uint32_t dhcsr = 0;
    if (cortex_read_dhcsr(&dhcsr) != SWD_ACK_OK) {
        uart_puts("cannot read DHCSR\r\n");
        return;
    }

    if (!(dhcsr & DHCSR_S_HALT)) {
        uart_puts("core is running, halt first\r\n");
        return;
    }

    for (uint8_t i = 0; i <= REG_LAST; i++) {
        uint32_t v = 0;
        uint8_t ack = cortex_read_reg(i, &v);

        reg_label(i);
        uart_putc(' ');
        if (ack == SWD_ACK_OK)
            uart_print_hex32(v);
        else
            uart_puts("--------");

        if ((i & 3) == 3)
            nl();
        else
            uart_puts("  ");
    }
    nl();
}

static void show_pc(void)
{
    uint32_t pc = 0, psr = 0;

    if (cortex_read_reg(REG_PC, &pc) != SWD_ACK_OK) {
        uart_puts("pc unreadable\r\n");
        return;
    }
    cortex_read_reg(REG_XPSR, &psr);

    uart_puts("pc ");
    uart_print_hex32(pc);
    uart_puts("  psr ");
    uart_print_hex32(psr);
    uart_puts("  exc ");
    uart_print_dec(psr & 0x1FF);
    nl();
}

static void cmd_step(uint32_t count, uint8_t mask_interrupts)
{
    if (count == 0)
        count = 1;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t ack = cortex_step(mask_interrupts);
        if (ack == CORTEX_NOT_HALTED) {
            uart_puts("core is running, halt first\r\n");
            return;
        }
        if (ack != SWD_ACK_OK) {
            report(ack);
            return;
        }
    }

    show_pc();
}

static void cmd_flash_info(void)
{
    if (flash_probe() != SWD_ACK_OK) {
        uart_puts("cannot read the flash size register\r\n");
        return;
    }

    uart_print_dec(flash_size_kb());
    uart_puts("KB, ");
    uart_print_dec(flash_sectors());
    uart_puts(" sectors\r\n");

    for (uint8_t i = 0; i < flash_sectors(); i++) {
        uart_puts("  ");
        uart_print_dec(i);
        uart_puts("  ");
        put_hex32(flash_sector_base(i));
        uart_puts("  ");
        uart_print_dec(flash_sector_size(i) / 1024);
        uart_puts("KB\r\n");
    }
}

/* A flash address erases the sector holding it, anything else is a sector number. */
static void cmd_erase(uint32_t arg)
{
    uint8_t sector = (uint8_t)arg;

    if (arg >= FLASH_BASE_ADDR) {
        if (flash_sector_of(arg, &sector) != SWD_ACK_OK) {
            uart_puts("address is not in flash\r\n");
            return;
        }
        uart_puts("sector ");
        uart_print_dec(sector);
        uart_puts(" ");
    }

    /* Do not erase underneath a core that may be fetching from flash. */
    cortex_halt();

    uint8_t ack = flash_erase_sector(sector);
    report(ack);

    if (ack != SWD_ACK_OK) {
        uint32_t sr = 0, cr = 0;
        mem_ap_read_word(FLASH_SR, &sr);
        mem_ap_read_word(FLASH_CR, &cr);
        uart_puts("  FLASH_SR ");
        put_hex32(sr);
        uart_puts("  FLASH_CR ");
        put_hex32(cr);
        nl();
    }
}

static void cmd_save(uint32_t addr, uint32_t length)
{
    uart_puts("start the receiver now\r\n");

    uint32_t sent = 0;
    uint8_t result = xmodem_send_memory(addr, length, &sent);

    nl();
    uart_print_dec(sent);
    uart_puts(" bytes from ");
    put_hex32(addr);
    uart_puts(": ");

    switch (result) {
    case XMODEM_OK:       uart_puts("ok");              break;
    case XMODEM_TIMEOUT:  uart_puts("timed out");       break;
    case XMODEM_CANCELED: uart_puts("canceled");        break;
    default:              uart_puts("target read failed"); break;
    }
    nl();
}

static void watch_mode(uint8_t function)
{
    if (function == DWT_FUNC_READ)
        uart_puts("read ");
    else if (function == DWT_FUNC_WRITE)
        uart_puts("write");
    else if (function == DWT_FUNC_RW)
        uart_puts("both ");
    else
        uart_puts("off  ");
}

static void cmd_watch_list(void)
{
    uart_print_dec(dwt_slots());
    uart_puts(" watchpoint slots\r\n");

    for (uint8_t i = 0; i < dwt_slots(); i++) {
        uint32_t addr = 0;
        uint8_t function = 0, matched = 0;

        if (dwt_get(i, &addr, &function, &matched) != SWD_ACK_OK)
            continue;

        uart_puts("  ");
        uart_print_dec(i);
        uart_puts("  ");
        watch_mode(function);
        if (function != DWT_FUNC_DISABLED) {
            uart_puts("  ");
            put_hex32(addr);
            if (matched)
                uart_puts("  matched");
        }
        nl();
    }
}

static void cmd_watch_set(uint32_t addr, char mode)
{
    uint8_t function = DWT_FUNC_RW;

    if (mode == 'r')
        function = DWT_FUNC_READ;
    else if (mode == 'w')
        function = DWT_FUNC_WRITE;

    for (uint8_t i = 0; i < dwt_slots(); i++) {
        uint32_t cur = 0;
        uint8_t cur_func = 0, matched = 0;

        uint8_t ack = dwt_get(i, &cur, &cur_func, &matched);
        if (ack != SWD_ACK_OK) {
            report(ack);
            return;
        }
        if (cur_func != DWT_FUNC_DISABLED)
            continue;

        ack = dwt_set(i, addr, function);
        if (ack != SWD_ACK_OK) {
            report(ack);
            return;
        }

        uart_puts("watchpoint ");
        uart_print_dec(i);
        uart_puts(" on ");
        watch_mode(function);
        uart_puts(" at ");
        put_hex32(addr);
        nl();
        return;
    }

    uart_puts("no free slots\r\n");
}

static void cmd_break_list(void)
{
    uart_puts("fpb rev ");
    uart_print_dec(fpb_revision());
    uart_puts(", ");
    uart_print_dec(fpb_slots());
    uart_puts(" slots\r\n");

    for (uint8_t i = 0; i < fpb_slots(); i++) {
        uint32_t comp = 0, addr = 0;
        uint8_t enabled = 0;

        if (fpb_get(i, &comp, &addr, &enabled) != SWD_ACK_OK)
            continue;

        uart_puts("  ");
        uart_print_dec(i);
        uart_puts(enabled ? " enabled  " : " free     ");
        if (enabled)
            put_hex32(addr);
        nl();
    }
}

static void cmd_break_set(uint32_t addr)
{
    for (uint8_t i = 0; i < fpb_slots(); i++) {
        uint32_t comp = 0, cur = 0;
        uint8_t enabled = 0;

        uint8_t ack = fpb_get(i, &comp, &cur, &enabled);
        if (ack != SWD_ACK_OK) {
            report(ack);
            return;
        }
        if (enabled)
            continue;

        ack = fpb_set(i, addr);
        if (ack == FPB_OUT_OF_RANGE) {
            uart_puts("this fpb only breaks below 0x20000000\r\n");
            return;
        }
        if (ack != SWD_ACK_OK) {
            report(ack);
            return;
        }

        uart_puts("breakpoint ");
        uart_print_dec(i);
        uart_puts(" at ");
        put_hex32(addr);
        nl();
        return;
    }

    uart_puts("no free slots\r\n");
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

    /*
     * The command is one character. Without this, "pc 20000000" parses as p
     * with an argument of c, which is a valid hex digit, and programs flash at
     * address 0xC.
     */
    if (*line && *line != ' ') {
        uart_puts("unknown command, ? for help\r\n");
        return;
    }

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
        if (!parse_dec(&line, &b))
            b = 4;
        {
            uint32_t v = 0;
            uint8_t ack;
            uint8_t v8 = 0;
            uint16_t v16 = 0;

            if (b == 1) {
                ack = mem_ap_read8(a, &v8);
                v = v8;
            } else if (b == 2) {
                ack = mem_ap_read16(a, &v16);
                v = v16;
            } else if (b == 4) {
                ack = mem_ap_read_word(a, &v);
            } else {
                uart_puts("size must be 1, 2 or 4\r\n");
                break;
            }

            if (ack != SWD_ACK_OK) {
                report(ack);
                break;
            }

            put_hex32(a);
            uart_puts(": ");
            if (b == 1) {
                uart_print_hex8((uint8_t)v);
            } else if (b == 2) {
                uart_print_hex8((uint8_t)(v >> 8));
                uart_print_hex8((uint8_t)v);
            } else {
                uart_print_hex32(v);
            }
            nl();
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
        {
            uint32_t size = 4;
            parse_dec(&line, &size);

            if (size == 1)
                report(mem_ap_write8(a, (uint8_t)b));
            else if (size == 2)
                report(mem_ap_write16(a, (uint16_t)b));
            else if (size == 4)
                report(mem_ap_write_word(a, b));
            else
                uart_puts("size must be 1, 2 or 4\r\n");
        }
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

    case 'f':
        cmd_flash_info();
        break;

    case 'e':
        if (!parse_hex(&line, &a)) {
            uart_puts("need a sector number or a flash address\r\n");
            break;
        }
        cmd_erase(a);
        break;

    case 'p':
        if (!parse_hex(&line, &a) || !parse_hex(&line, &b)) {
            uart_puts("need an address and a value\r\n");
            break;
        }
        report(flash_program_word(a, b));
        break;

    case 'y':
        if (!parse_hex(&line, &a) || !parse_hex(&line, &b)) {
            uart_puts("need an address and a length\r\n");
            break;
        }
        cmd_save(a, b);
        break;

    case 'b':
        if (parse_hex(&line, &a))
            cmd_break_set(a);
        else
            cmd_break_list();
        break;

    case 'a':
        if (parse_hex(&line, &a)) {
            while (*line == ' ')
                line++;
            cmd_watch_set(a, *line);
        } else {
            cmd_watch_list();
        }
        break;

    case 'j':
        if (parse_dec(&line, &a)) {
            report(dwt_clear((uint8_t)a));
        } else {
            for (uint8_t i = 0; i < dwt_slots(); i++)
                dwt_clear(i);
            uart_puts("all cleared\r\n");
        }
        break;

    case 'k':
        if (parse_dec(&line, &a)) {
            report(fpb_clear((uint8_t)a));
        } else {
            for (uint8_t i = 0; i < fpb_slots(); i++)
                fpb_clear(i);
            uart_puts("all cleared\r\n");
        }
        break;

    case 'n':
        parse_hex(&line, &a);
        cmd_step(a, 0);
        break;

    case 'm':
        parse_hex(&line, &a);
        cmd_step(a, 1);
        break;

    case 'x':
        {
            uint8_t reg = 0;
            if (!parse_reg(&line, &reg)) {
                cmd_regs();
                break;
            }
            if (!parse_hex(&line, &b)) {
                uart_puts("need a value to write\r\n");
                break;
            }

            uint8_t ack = cortex_write_reg(reg, b);
            if (ack == CORTEX_NOT_HALTED)
                uart_puts("core is running, halt first\r\n");
            else
                report(ack);
        }
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
