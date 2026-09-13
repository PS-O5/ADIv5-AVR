#include <stdint.h>

#define RCC_AHB1ENR (*(volatile uint32_t *)0x40023830UL)
#define GPIOA_MODER (*(volatile uint32_t *)0x40020000UL)
#define GPIOC_MODER (*(volatile uint32_t *)0x40020800UL)
#define GPIOC_ODR   (*(volatile uint32_t *)0x40020814UL)

#define SWDIO 13
#define SWCLK 14
#define LED   13

static void delay(volatile uint32_t count)
{
    while (count--)
        __asm__ volatile ("");
}

/*
 * Takes PA13 and PA14 away from the debug port within a few instructions of
 * reset, which is what firmware doing anything useful with those pins would do.
 * Once this runs there is no SWD to attach to, so a plain connect has nothing
 * to talk to and only connect under reset can get back in.
 */
static void start(void)
{
    RCC_AHB1ENR |= (1UL << 0) | (1UL << 2);   /* GPIOA and GPIOC clocks */

    GPIOA_MODER &= ~((3UL << (SWDIO * 2)) | (3UL << (SWCLK * 2)));
    GPIOA_MODER |= (1UL << (SWDIO * 2)) | (1UL << (SWCLK * 2));

    GPIOC_MODER &= ~(3UL << (LED * 2));
    GPIOC_MODER |= (1UL << (LED * 2));

    /* Blinks so there is a sign of life once the pins are gone. */
    for (;;) {
        GPIOC_ODR ^= (1UL << LED);
        delay(400000);
    }
}

extern uint32_t _estack;

__attribute__((section(".vectors"), used))
static void *const vectors[] = {
    &_estack,
    (void *)start,
};
