#include <avr/pgmspace.h>
#include "swd.h"
#include "dp.h"
#include "ap.h"
#include "cortex.h"
#include "flash.h"
#include "fpb.h"
#include "dwt.h"
#include "rtt.h"
#include "target.h"
#include "uart.h"
#include "xmodem.h"
#include "shell.h"

#define LINE_MAX 40

static const char help_text[] PROGMEM =
    "c              connect\r\n"
    "cr             connect holding NRST, for firmware that steals the pins\r\n"
    "force          run flash commands on an unrecognised part anyway\r\n"
    "i              ids: DPIDR, AP IDR, CPUID, DBGMCU\r\n"
    "r <addr> [sz]  read, sz 1 2 or 4 (default 4)\r\n"
    "d <addr> [n]   dump n words (default 8)\r\n"
    "w <a> <v> [sz] write, sz 1 2 or 4 (default 4)\r\n"
    "h              halt\r\n"
    "g              resume\r\n"
    "t              reset and halt\r\n"
    "q              reset and run\r\n"
    "s              status (DHCSR)\r\n"
    "u              unlock flash\r\n"
    "f              flash size and sector map\r\n"
    "e <sect|addr>  erase sector, or the one holding addr\r\n"
    "p <addr> <val> program flash word\r\n"
    "o              option bytes and readout protection\r\n"
    "z              mass erase (confirms first)\r\n"
    "v              drop readout protection (confirms first)\r\n"
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
    "a <addr> [rwb] [len] watch: r read, w write, b both\r\n"
    "j [slot]       clear one watchpoint, or all\r\n"
    "rtt [base] [n] stream RTT output from target RAM\r\n"
    "?              this help\r\n";

/* Keeps string literals in flash. The AVR has 2KB of RAM and .data was eating it. */
#define P(str) do { static const char _lit[] PROGMEM = (str); puts_P(_lit); } while (0)

static void puts_P(const char *s)
{
    char c;
    while ((c = pgm_read_byte(s++)))
        uart_putc(c);
}

static void nl(void)
{
    P("\r\n");
}

static void put_hex32(uint32_t v)
{
    P("0x");
    uart_print_hex32(v);
}

static void report(uint8_t ack)
{
    if (ack == SWD_ACK_OK) {
        P("ok");
    } else {
        P("failed, ack=0x");
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
            P("\b \b");
            n--;
            continue;
        }

        if (c >= ' ' && n < LINE_MAX - 1) {
            uart_putc(c);
            buf[n++] = c;
        }
    }
}

static uint32_t rtt_cb;

/*
 * Flash operations need the core stopped: an erase never completes while the
 * core fetches from the flash being erased. Report rather than press on, since
 * the failure otherwise shows up as a stalled erase with no explanation.
 */
static uint8_t halt_for_flash(void)
{
    uint8_t ack = cortex_halt();

    if (ack != SWD_ACK_OK)
        P("could not halt the core, flash operations need it stopped\r\n");

    return ack;
}

static void report_target(void)
{
    char name[TARGET_NAME_MAX];

    P("DEV    ");
    put_hex32(target_dev_id());
    P("  rev ");
    put_hex32(target_rev_id());
    P("  ");

    if (target_name(name)) {
        P("STM32");
        uart_puts(name);
    } else {
        P("unrecognised");
    }

    if (target_flash_ok()) {
        if (target_forced())
            P("  flash: forced\r\n");
        else
            P("  flash: ok\r\n");
    } else {
        P("  flash: refused, driver is F4 only\r\n");
    }
}

static void cmd_connect(void)
{
    uint32_t idcode = 0;
    uint8_t ack = dp_connect(&idcode);

    P("DPIDR ");
    put_hex32(idcode);
    P("  ");

    if (ack != SWD_ACK_OK) {
        report(ack);
        return;
    }

    dp_power_up();
    dp_clear_errors();

    rtt_cb = 0;

    ack = mem_ap_init();
    if (ack == SWD_ACK_OK) {
        cortex_init();
        fpb_init();
        dwt_init();
        target_identify();
        flash_probe();
    }

    report(ack);

    if (ack == SWD_ACK_OK)
        report_target();
}

static void cmd_connect_reset(void)
{
    uint32_t idcode = 0;
    uint8_t ack = cortex_connect_under_reset(&idcode);

    P("DPIDR ");
    put_hex32(idcode);
    P("  ");

    rtt_cb = 0;

    if (ack == SWD_ACK_OK) {
        fpb_init();
        dwt_init();
        target_identify();
        flash_probe();
    }

    report(ack);

    if (ack == SWD_ACK_OK)
        report_target();
}

static void cmd_ids(void)
{
    uint32_t v = 0;

    dp_read(DP_DPIDR, &v);
    P("DPIDR  ");
    put_hex32(v);
    nl();

    ap_select(0, 0xF);
    ap_read(AP_IDR, &v);
    P("AP IDR ");
    put_hex32(v);
    nl();
    ap_select(0, 0x0);

    mem_ap_read_word(0xE000ED00UL, &v);
    P("CPUID  ");
    put_hex32(v);
    nl();

    mem_ap_read_word(DBGMCU_IDCODE, &v);
    P("DBGMCU ");
    put_hex32(v);
    nl();

    report_target();
}

static void cmd_dump(uint32_t addr, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = addr + 4UL * i;

        if ((i & 3) == 0) {
            if (i)
                nl();
            put_hex32(a);
            P(":");
        }

        uint32_t v = 0;
        uint8_t ack = mem_ap_read_word(a, &v);

        P(" ");
        if (ack == SWD_ACK_OK)
            uart_print_hex32(v);
        else
            P("--------");
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

    P("DHCSR ");
    put_hex32(dhcsr);
    P("  halted=");
    uart_putc((dhcsr & DHCSR_S_HALT) ? 'y' : 'n');
    P(" lockup=");
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
        P("sp ");
    } else if (i == REG_LR) {
        P("lr ");
    } else if (i == REG_PC) {
        P("pc ");
    } else {
        P("psr");
    }
}

static void cmd_regs(void)
{
    uint32_t dhcsr = 0;
    if (cortex_read_dhcsr(&dhcsr) != SWD_ACK_OK) {
        P("cannot read DHCSR\r\n");
        return;
    }

    if (!(dhcsr & DHCSR_S_HALT)) {
        P("core is running, halt first\r\n");
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
            P("--------");

        if ((i & 3) == 3)
            nl();
        else
            P("  ");
    }
    nl();
}

static void show_pc(void)
{
    uint32_t pc = 0, psr = 0;

    if (cortex_read_reg(REG_PC, &pc) != SWD_ACK_OK) {
        P("pc unreadable\r\n");
        return;
    }
    cortex_read_reg(REG_XPSR, &psr);

    P("pc ");
    uart_print_hex32(pc);
    P("  psr ");
    uart_print_hex32(psr);
    P("  exc ");
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
            P("core is running, halt first\r\n");
            return;
        }
        if (ack != SWD_ACK_OK) {
            report(ack);
            return;
        }
    }

    show_pc();
}

static uint8_t confirmed(const char *what)
{
    char line[LINE_MAX];

    uart_puts(what);
    P("\r\ntype yes to confirm: ");
    read_line(line);

    return line[0] == 'y' && line[1] == 'e' && line[2] == 's' && line[3] == 0;
}

static void cmd_options(void)
{
    uint32_t optcr = 0;
    uint8_t ack = flash_read_optcr(&optcr);

    if (ack != SWD_ACK_OK) {
        report(ack);
        return;
    }

    P("OPTCR ");
    put_hex32(optcr);
    P("  optlock=");
    uart_putc((optcr & FLASH_OPTCR_OPTLOCK) ? 'y' : 'n');
    P("  nWRP 0x");
    /* nWRP is bits [27:16]: twelve sectors on the larger F4 parts. */
    uart_print_hex8((uint8_t)((optcr >> 24) & 0x0F));
    uart_print_hex8((uint8_t)(optcr >> 16));
    nl();

    uint8_t level = 0;
    if (flash_rdp_level(&level) != SWD_ACK_OK)
        return;

    P("readout protection: level ");
    uart_print_dec(level);
    if (level == 0)
        P(", flash readable\r\n");
    else if (level == 1)
        P(", flash blocked, removable by mass erase\r\n");
    else
        P(", debug permanently disabled\r\n");
}

static void cmd_mass_erase(void)
{
    if (!confirmed("this erases the entire flash")) {
        P("cancelled\r\n");
        return;
    }

    if (halt_for_flash() != SWD_ACK_OK)
        return;

    P("erasing, this takes several seconds\r\n");
    report(flash_mass_erase());
}

static void cmd_unprotect(void)
{
    uint8_t level = 0;
    if (flash_rdp_level(&level) != SWD_ACK_OK) {
        P("cannot read the protection level\r\n");
        return;
    }

    if (level == 0) {
        P("already level 0, nothing to do\r\n");
        return;
    }

    if (level == 2) {
        P("level 2 cannot be undone\r\n");
        return;
    }

    if (!confirmed("dropping protection mass erases the flash")) {
        P("cancelled\r\n");
        return;
    }

    report(flash_remove_readout_protection());
}

static uint8_t word_is(const char *line, const char *word)
{
    while (*word) {
        if (*line != *word)
            return 0;
        line++;
        word++;
    }

    return *line == 0 || *line == ' ';
}

static void rtt_sink(char c)
{
    uart_putc(c);
}

static void cmd_rtt(const char *args)
{
    uint32_t base = 0x20000000UL;
    uint32_t length = 128UL * 1024;

    parse_hex(&args, &base);
    parse_hex(&args, &length);

    if (!rtt_cb) {
        P("searching RAM for the control block\r\n");
        uint8_t ack = rtt_find(base, length, &rtt_cb);

        if (ack == RTT_NOT_FOUND) {
            P("no control block: is RTT linked into the target?\r\n");
            return;
        }
        if (ack != SWD_ACK_OK) {
            report(ack);
            return;
        }
    }

    P("control block at ");
    put_hex32(rtt_cb);

    uint32_t up = 0;
    if (rtt_up_count(rtt_cb, &up) == SWD_ACK_OK) {
        P(", ");
        uart_print_dec(up);
        P(" up buffers");
    }
    P("\r\npress a key to stop\r\n");

    while (!uart_available()) {
        uint16_t got = 0;
        uint8_t ack = rtt_drain(rtt_cb, 0, rtt_sink, &got);

        if (ack != SWD_ACK_OK) {
            nl();
            report(ack);
            return;
        }
    }

    uart_getc();
    P("\r\nstopped\r\n");
}

static void cmd_flash_info(void)
{
    if (flash_probe() != SWD_ACK_OK) {
        P("cannot read the flash size register\r\n");
        return;
    }

    uart_print_dec(flash_size_kb());
    P("KB, ");
    uart_print_dec(flash_sectors());
    P(" sectors\r\n");

    for (uint8_t i = 0; i < flash_sectors(); i++) {
        P("  ");
        uart_print_dec(i);
        P("  ");
        put_hex32(flash_sector_base(i));
        P("  ");
        uart_print_dec(flash_sector_size(i) / 1024);
        P("KB\r\n");
    }
}

/* A flash address erases the sector holding it, anything else is a sector number. */
static void cmd_erase(uint32_t arg)
{
    uint8_t sector = (uint8_t)arg;

    if (arg >= FLASH_BASE_ADDR) {
        if (flash_sector_of(arg, &sector) != SWD_ACK_OK) {
            P("address is not in flash\r\n");
            return;
        }
        P("sector ");
        uart_print_dec(sector);
        P(" ");
    }

    if (halt_for_flash() != SWD_ACK_OK)
        return;

    uint8_t ack = flash_erase_sector(sector);
    report(ack);

    if (ack != SWD_ACK_OK) {
        uint32_t sr = 0, cr = 0;
        mem_ap_read_word(FLASH_SR, &sr);
        mem_ap_read_word(FLASH_CR, &cr);
        P("  FLASH_SR ");
        put_hex32(sr);
        P("  FLASH_CR ");
        put_hex32(cr);
        nl();
    }
}

static void cmd_save(uint32_t addr, uint32_t length)
{
    P("start the receiver now\r\n");

    uint32_t sent = 0;
    uint8_t result = xmodem_send_memory(addr, length, &sent);

    nl();
    uart_print_dec(sent);
    P(" bytes from ");
    put_hex32(addr);
    P(": ");

    switch (result) {
    case XMODEM_OK:       P("ok");              break;
    case XMODEM_TIMEOUT:  P("timed out");       break;
    case XMODEM_CANCELED: P("canceled");        break;
    default:              P("target read failed"); break;
    }
    nl();
}

static void watch_mode(uint8_t function)
{
    if (function == DWT_FUNC_READ)
        P("read ");
    else if (function == DWT_FUNC_WRITE)
        P("write");
    else if (function == DWT_FUNC_RW)
        P("both ");
    else
        P("off  ");
}

static void cmd_watch_list(void)
{
    uart_print_dec(dwt_slots());
    P(" watchpoint slots\r\n");

    for (uint8_t i = 0; i < dwt_slots(); i++) {
        uint32_t addr = 0;
        uint8_t function = 0, matched = 0;

        if (dwt_get(i, &addr, &function, &matched) != SWD_ACK_OK)
            continue;

        P("  ");
        uart_print_dec(i);
        P("  ");
        watch_mode(function);
        if (function != DWT_FUNC_DISABLED) {
            P("  ");
            put_hex32(addr);
            if (matched)
                P("  matched");
        }
        nl();
    }
}

static void cmd_watch_set(uint32_t addr, char mode, uint32_t len)
{
    uint8_t function = DWT_FUNC_RW;

    if (mode == 'r')
        function = DWT_FUNC_READ;
    else if (mode == 'w')
        function = DWT_FUNC_WRITE;

    /* Length to MASK: 1 gives 0, 2 gives 1, 4 gives 2, 8 gives 3. */
    uint8_t mask = 0;
    while (mask < 16 && (1UL << mask) < len)
        mask++;

    if ((1UL << mask) != len || (addr & (len - 1))) {
        P("length must be a power of two, address aligned to it\r\n");
        return;
    }

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

        ack = dwt_set(i, addr, function, mask);
        if (ack != SWD_ACK_OK) {
            report(ack);
            return;
        }

        P("watchpoint ");
        uart_print_dec(i);
        P(" on ");
        watch_mode(function);
        P(" at ");
        put_hex32(addr);
        nl();
        return;
    }

    P("no free slots\r\n");
}

static void cmd_break_list(void)
{
    P("fpb rev ");
    uart_print_dec(fpb_revision());
    P(", ");
    uart_print_dec(fpb_slots());
    P(" slots\r\n");

    for (uint8_t i = 0; i < fpb_slots(); i++) {
        uint32_t comp = 0, addr = 0;
        uint8_t enabled = 0;

        if (fpb_get(i, &comp, &addr, &enabled) != SWD_ACK_OK)
            continue;

        P("  ");
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
            P("this fpb only breaks below 0x20000000\r\n");
            return;
        }
        if (ack != SWD_ACK_OK) {
            report(ack);
            return;
        }

        P("breakpoint ");
        uart_print_dec(i);
        P(" at ");
        put_hex32(addr);
        nl();
        return;
    }

    P("no free slots\r\n");
}

static void cmd_load(uint32_t addr)
{
    if (halt_for_flash() != SWD_ACK_OK)
        return;

    P("send binary now, sectors are erased as it goes\r\n");

    uint32_t written = 0;
    uint8_t result = xmodem_receive_to_flash(addr, &written);

    /* Read the sticky state before clearing it, so a failure is not hidden. */
    uint32_t ctrlstat = 0;
    uint8_t cs_ack = dp_read(DP_CTRLSTAT, &ctrlstat);

    /* Programming can leave a sticky error behind; do not hand back a dead link. */
    dp_clear_errors();

    nl();
    uart_print_dec(written);
    P(" bytes to ");
    put_hex32(addr);
    P(": ");

    switch (result) {
    case XMODEM_OK:       P("ok");             break;
    case XMODEM_TIMEOUT:  P("timed out");      break;
    case XMODEM_CANCELED: P("canceled");       break;
    case XMODEM_VERIFY:   P("verify failed, flash does not match"); break;
    default:              P("flash write failed"); break;
    }
    nl();

    P("sectors erased: ");
    uart_print_dec(xm_stats.sectors_erased);
    nl();

    P("naks=");
    uart_print_dec(xm_stats.naks_sent);
    P(" ok=");
    uart_print_dec(xm_stats.blocks_ok);
    P(" badsum=");
    uart_print_dec(xm_stats.bad_checksum);
    P(" badblk=");
    uart_print_dec(xm_stats.bad_blocknum);
    P(" resync=");
    uart_print_dec(xm_stats.resyncs);
    P(" first=0x");
    uart_print_hex32((uint32_t)(uint16_t)xm_stats.first_byte);
    P(" blk=0x");
    uart_print_hex32((uint32_t)(uint16_t)xm_stats.last_blk);
    P(" inv=0x");
    uart_print_hex32((uint32_t)(uint16_t)xm_stats.last_inv);
    nl();

    P("CTRL/STAT ");
    if (cs_ack == SWD_ACK_OK) {
        put_hex32(ctrlstat);
        if (ctrlstat & (1UL << 5))
            P(" STICKYERR");
        if (ctrlstat & (1UL << 1))
            P(" STICKYORUN");
        if (ctrlstat & (1UL << 7))
            P(" WDATAERR");
    } else {
        P("unreadable, ack=0x");
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
     * The single letters are the whole alphabet by now, so longer names are
     * matched whole. Matching the entire token keeps the property that
     * "pc 20000000" is an error rather than p with an argument of c, which
     * would program flash.
     */
    if (*line && *line != ' ') {
        line--;
        if (word_is(line, "rtt")) {
            cmd_rtt(line + 3);
            return;
        }
        if (word_is(line, "cr")) {
            cmd_connect_reset();
            return;
        }
        if (word_is(line, "force")) {
            target_force();
            P("flash driver forced on, it is written for the F4 only\r\n");
            return;
        }
        P("unknown command, ? for help\r\n");
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
            P("need an address\r\n");
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
                P("size must be 1, 2 or 4\r\n");
                break;
            }

            if (ack != SWD_ACK_OK) {
                report(ack);
                break;
            }

            put_hex32(a);
            P(": ");
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
            P("need an address\r\n");
            break;
        }
        if (!parse_hex(&line, &b))
            b = 8;
        cmd_dump(a, b);
        break;

    case 'w':
        if (!parse_hex(&line, &a) || !parse_hex(&line, &b)) {
            P("need an address and a value\r\n");
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
                P("size must be 1, 2 or 4\r\n");
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

    case 'o':
        cmd_options();
        break;

    case 'z':
        cmd_mass_erase();
        break;

    case 'v':
        cmd_unprotect();
        break;

    case 'q':
        P("reset and run: ");
        report(cortex_reset_run());
        break;

    case 'e':
        if (!parse_hex(&line, &a)) {
            P("need a sector number or a flash address\r\n");
            break;
        }
        cmd_erase(a);
        break;

    case 'p':
        if (!parse_hex(&line, &a) || !parse_hex(&line, &b)) {
            P("need an address and a value\r\n");
            break;
        }
        report(flash_program_word(a, b));
        break;

    case 'y':
        if (!parse_hex(&line, &a) || !parse_hex(&line, &b)) {
            P("need an address and a length\r\n");
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

            char mode = *line;
            if (*line)
                line++;

            if (!parse_dec(&line, &b))
                b = 1;

            cmd_watch_set(a, mode, b);
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
            P("all cleared\r\n");
        }
        break;

    case 'k':
        if (parse_dec(&line, &a)) {
            report(fpb_clear((uint8_t)a));
        } else {
            for (uint8_t i = 0; i < fpb_slots(); i++)
                fpb_clear(i);
            P("all cleared\r\n");
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
                P("need a value to write\r\n");
                break;
            }

            uint8_t ack = cortex_write_reg(reg, b);
            if (ack == CORTEX_NOT_HALTED)
                P("core is running, halt first\r\n");
            else
                report(ack);
        }
        break;

    case 'l':
        if (!parse_hex(&line, &a)) {
            P("need an address\r\n");
            break;
        }
        cmd_load(a);
        break;

    case '?':
        puts_P(help_text);
        break;

    default:
        P("unknown command, ? for help\r\n");
        break;
    }
}

void shell_run(void)
{
    char line[LINE_MAX];

    P("\r\nADIv5-AVR, ? for help\r\n");
    cmd_connect();

    for (;;) {
        P("> ");
        read_line(line);
        dispatch(line);
    }
}
