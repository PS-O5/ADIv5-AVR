#include <avr/pgmspace.h>
#include <string.h>
#include "swd.h"
#include "ap.h"
#include "target.h"

/*
 * flash.c implements the F4 sector erase algorithm and nothing else. Pointed at
 * a part with a different flash controller it would write plausible values into
 * registers that mean something else, so the device id decides whether the
 * flash commands are allowed to run at all.
 *
 * The names of parts it cannot drive are here too. Refusing is more useful when
 * it can say what it found.
 */
struct entry {
    uint16_t id;
    uint8_t supported;
    char name[TARGET_NAME_MAX];
};

static const struct entry devices[] PROGMEM = {
    { 0x413, 1, "F405/407" },
    { 0x419, 1, "F42x/43x" },
    { 0x423, 1, "F401xB/C" },
    { 0x433, 1, "F401xD/E" },
    { 0x431, 1, "F411" },
    { 0x441, 1, "F412" },
    { 0x421, 1, "F446" },
    { 0x434, 1, "F469/479" },
    { 0x458, 1, "F410" },
    { 0x463, 1, "F413/423" },

    { 0x410, 0, "F103 md" },
    { 0x414, 0, "F103 hd" },
    { 0x430, 0, "F103 xl" },
    { 0x418, 0, "F105/107" },
    { 0x440, 0, "F030/051" },
    { 0x415, 0, "L475/476" },
    { 0x435, 0, "L43x/44x" },
    { 0x450, 0, "H743/753" },
    { 0x451, 0, "F76x/77x" },
    { 0x449, 0, "F74x/75x" },
    { 0x452, 0, "F72x/73x" },
    { 0x422, 0, "F302/303" },
    { 0x468, 0, "G431/441" },
    { 0x460, 0, "G071/081" },
};

#define DEVICE_COUNT (sizeof(devices) / sizeof(devices[0]))

static uint16_t dev_id;
static uint16_t rev_id;
static uint8_t known;
static uint8_t supported;
static uint8_t forced;

static uint8_t find(uint16_t id, struct entry *out)
{
    for (uint8_t i = 0; i < DEVICE_COUNT; i++) {
        if (pgm_read_word(&devices[i].id) == id) {
            memcpy_P(out, &devices[i], sizeof(*out));
            return 1;
        }
    }
    return 0;
}

uint8_t target_identify(void)
{
    uint32_t idcode = 0;

    dev_id = 0;
    rev_id = 0;
    known = 0;
    supported = 0;

    uint8_t ack = mem_ap_read_word(DBGMCU_IDCODE, &idcode);
    if (ack != SWD_ACK_OK)
        return ack;

    dev_id = (uint16_t)(idcode & 0xFFF);
    rev_id = (uint16_t)(idcode >> 16);

    struct entry e;
    if (find(dev_id, &e)) {
        known = 1;
        supported = e.supported;
    }

    return SWD_ACK_OK;
}

uint16_t target_dev_id(void)
{
    return dev_id;
}

uint16_t target_rev_id(void)
{
    return rev_id;
}

uint8_t target_name(char *buf)
{
    struct entry e;

    if (!known || !find(dev_id, &e))
        return 0;

    memcpy(buf, e.name, TARGET_NAME_MAX);
    buf[TARGET_NAME_MAX - 1] = 0;
    return 1;
}

uint8_t target_flash_ok(void)
{
    return supported || forced;
}

void target_force(void)
{
    forced = 1;
}

uint8_t target_forced(void)
{
    return forced;
}
