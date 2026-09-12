#include <stdint.h>

#define RCC_AHB1ENR (*(volatile uint32_t *)0x40023830UL)
#define GPIOC_MODER (*(volatile uint32_t *)0x40020800UL)
#define GPIOC_ODR   (*(volatile uint32_t *)0x40020814UL)

#define LED 13

static void delay(volatile uint32_t count)
{
    while (count--)
        __asm__ volatile ("");
}

static void start(void)
{
    RCC_AHB1ENR |= (1UL << 2);          /* GPIOC clock */

    GPIOC_MODER &= ~(3UL << (LED * 2));
    GPIOC_MODER |= (1UL << (LED * 2));  /* general purpose output */

    /* The Black Pill's LED sits between 3V3 and PC13, so low lights it. */
    for (;;) {
        GPIOC_ODR ^= (1UL << LED);
        delay(400000);
    }
}

extern uint32_t _estack;

/*
 * No startup code: nothing here has an initialiser to copy and nothing lives in
 * .bss, so the reset vector can go straight to the loop.
 */
__attribute__((section(".vectors"), used))
static void *const vectors[] = {
    &_estack,
    (void *)start,
};
