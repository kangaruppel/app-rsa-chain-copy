#include <msp430.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// #define CONFIG_LIBEDB_PRINTF
#define CONFIG_LIBMSPCONSOLE_PRINTF

#include <wisp-base.h>
#include <libchain/chain.h>

#ifdef CONFIG_LIBEDB_PRINTF
#include <libedb/edb.h>
#endif

#include "pins.h"

#ifdef CONFIG_LIBEDB_PRINTF
#define printf(...) BARE_PRINTF(__VA_ARGS__)
#else // CONFIG_LIBEDB_PRINTF
#ifndef CONFIG_LIBMSPCONSOLE_PRINTF
#define printf(...)
#endif // CONFIG_LIBMSPCONSOLE_PRINTF
#endif

#define NUM_DIGITS       4 // 4 * 8 = 32-bit blocks
#define REG_SIZE        8 // arithmetic ops take 8-bit args produce 16-bit result
#define REG_MASK        0x00ff

#define LED1 (1 << 0)
#define LED2 (1 << 1)

#define SEC_TO_CYCLES 4000000 /* 4 MHz */

// If you link-in wisp-base, then you have to define some symbols.
uint8_t usrBank[USRBANK_SIZE];

struct msg_mult {
    CHAN_FIELD_ARRAY(uint16_t, A, NUM_DIGITS);
    CHAN_FIELD_ARRAY(uint16_t, B, NUM_DIGITS);
};

struct msg_digit {
    CHAN_FIELD(unsigned, digit);
    CHAN_FIELD(unsigned, carry);
};

struct msg_product {
    CHAN_FIELD_ARRAY(uint16_t, product, NUM_DIGITS * 2);
};

TASK(0, task_init)
TASK(1, task_mult)
TASK(2, task_print)

CHANNEL(task_init, task_mult, msg_digit);
MULTICAST_CHANNEL(msg_mult, mult_args, task_init, task_mult, task_print);
SELF_CHANNEL(task_mult, msg_digit);
CHANNEL(task_mult, task_print, msg_product);

void init()
{
    WISP_init();

    GPIO(PORT_LED_1, DIR) |= BIT(PIN_LED_1);
    GPIO(PORT_LED_2, DIR) |= BIT(PIN_LED_2);
#if defined(PORT_LED_3)
    GPIO(PORT_LED_3, DIR) |= BIT(PIN_LED_3);
#endif

#if defined(CONFIG_LIBEDB_PRINTF)
    BARE_PRINTF_ENABLE();
#elif defined(CONFIG_LIBMSPCONSOLE_PRINTF)
    UART_init();
#endif

    __enable_interrupt();

#if defined(PORT_LED_3) // when available, this LED indicates power-on
    GPIO(PORT_LED_3, OUT) |= BIT(PIN_LED_3);
#endif

    printf("rsa app booted\r\n");
}

static void delay(uint32_t cycles)
{
    unsigned i;
    for (i = 0; i < cycles / (1U << 15); ++i)
        __delay_cycles(1U << 15);
}

static void blink(unsigned count, uint32_t duration, unsigned leds)
{
    unsigned i;
    for (i = 0; i < count; ++i) {
        GPIO(PORT_LED_1, OUT) |= (leds & LED1) ? BIT(PIN_LED_1) : 0x0;
        GPIO(PORT_LED_2, OUT) |= (leds & LED2) ? BIT(PIN_LED_2) : 0x0;
        delay(duration / 2);
        GPIO(PORT_LED_1, OUT) &= (leds & LED1) ? ~BIT(PIN_LED_1) : ~0x0;
        GPIO(PORT_LED_2, OUT) &= (leds & LED2) ? ~BIT(PIN_LED_2) : ~0x0;
        delay(duration / 2);
    }
}

void task_init()
{
    int i;
    uint16_t a, b;

    printf("init\r\n");

    blink(1, SEC_TO_CYCLES * 2, LED1 | LED2);

    // test values
    printf("init: A=");
    for (i = NUM_DIGITS - 1; i >= 0; --i) {
        a = (0x10 + (0x10 * i)) & REG_MASK;
        CHAN_OUT(A[i], a, MC_OUT_CH(mult_args, task_init, task_mult, task_print));
        printf("%x ", a);
    }
    printf("\r\n");
    printf("init: B=");
    for (i = NUM_DIGITS - 1; i >= 0; --i) {
        b = (0x80 + (0x10 * i)) & REG_MASK;
        CHAN_OUT(B[i], b, MC_OUT_CH(mult_args, task_init, task_mult, task_print));
        printf("%x ", b);
    }
    printf("\r\n");

    CHAN_OUT(digit, 0, CH(task_init, task_mult));
    CHAN_OUT(carry, 0, CH(task_init, task_mult));

    TRANSITION_TO(task_mult);
}

void task_mult()
{
    int i;
    uint16_t a, b, c;
    uint16_t p, carry;
    int digit;

    blink(1, SEC_TO_CYCLES, LED1);

    digit = *CHAN_IN2(digit, CH(task_init, task_mult), SELF_IN_CH(task_mult));
    carry = *CHAN_IN2(carry, CH(task_init, task_mult), SELF_IN_CH(task_mult));

    printf("mult: digit=%u carry=%x\r\n", digit, carry);

    p = carry;
    for (i = 0; i < NUM_DIGITS; ++i) {
        if (digit - i >= 0 && digit - i < NUM_DIGITS) {
            a = *CHAN_IN1(A[digit - i], MC_IN_CH(mult_args, task_init, task_mult));
            b = *CHAN_IN1(B[i], MC_IN_CH(mult_args, task_init, task_mult));
            p += a * b;
            printf("mult: i=%u a=%x b=%x p=%x\r\n", i, a, b, p);
        }
    }

    c = p >> REG_SIZE;
    p &= REG_MASK;

    printf("mult: c=%x p=%x\r\n", c, p);

    CHAN_OUT(product[digit], p, CH(task_mult, task_print));

    digit++;

    if (digit < NUM_DIGITS * 2) {
        CHAN_OUT(carry, c, SELF_OUT_CH(task_mult));
        CHAN_OUT(digit, digit, SELF_OUT_CH(task_mult));
        TRANSITION_TO(task_mult);
    } else {
        TRANSITION_TO(task_print);
    }
}

void task_print()
{
    int i;
    uint16_t p;

    blink(1, SEC_TO_CYCLES, LED2);

    printf("print: P=");
    for (i = (NUM_DIGITS * 2) - 1; i >= 0; --i) {
        p = *CHAN_IN1(product[i], CH(task_mult, task_print));
        printf("%x ", p);
    }
    printf("\r\n");

    TRANSITION_TO(task_init);
}

ENTRY_TASK(task_init)
INIT_FUNC(init)
