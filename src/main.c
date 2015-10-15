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

// TODO: this is app-specific, but a good candidate for genericizing
// This pertains to the pattern of reusable tasks using a 'next task' value.
typedef enum {
    TASK_NORMALIZABLE,
    TASK_REDUCE,
    // Not all tasks listed because only some are used as next task
} task_t;

struct msg_mult {
    CHAN_FIELD_ARRAY(uint16_t, A, NUM_DIGITS);
    CHAN_FIELD_ARRAY(uint16_t, B, NUM_DIGITS);
};

struct msg_modulus {
    CHAN_FIELD_ARRAY(uint16_t, M, NUM_DIGITS);
};

struct msg_digit {
    CHAN_FIELD(unsigned, digit);
    CHAN_FIELD(unsigned, carry);
};

struct msg_product {
    CHAN_FIELD_ARRAY(uint16_t, product, NUM_DIGITS * 2);
};

struct msg_next_task {
    CHAN_FIELD(task_t, next_task);
};

TASK(0, task_init)
TASK(1, task_mult)
TASK(2, task_normalizable)
TASK(3, task_normalize)
TASK(4, task_reduce)
TASK(5, task_print_product)

CHANNEL(task_init, task_mult, msg_digit);
MULTICAST_CHANNEL(msg_modulus, ch_modulus, task_init, task_normalizable, task_normalize);
MULTICAST_CHANNEL(msg_mult, ch_mult_args, task_init, task_mult, task_print_product);
SELF_CHANNEL(task_mult, msg_digit);
MULTICAST_CHANNEL(msg_product, ch_product, task_mult,
                  task_normalizable, task_normalize, task_print_product);
MULTICAST_CHANNEL(msg_product, ch_normalized_product, task_normalize,
                  task_reduce, task_print_product);
CHANNEL(task_mult, task_print_product, msg_next_task);
CHANNEL(task_normalizable, task_print_product, msg_next_task);
CHANNEL(task_normalize, task_print_product, msg_next_task);

// Test input
static const uint8_t A[] = { 0x40, 0x30, 0x20, 0x10 };
static const uint8_t B[] = { 0xB0, 0xA0, 0x90, 0x80 };
static const uint8_t M[] = { 0x0D, 0x49, 0x60, 0x01 };

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

    printf("init\r\n");

    blink(1, SEC_TO_CYCLES * 2, LED1 | LED2);

    // test values
    printf("init: A=");
    for (i = NUM_DIGITS - 1; i >= 0; --i) {
        CHAN_OUT(A[i], A[NUM_DIGITS - 1 - i], MC_OUT_CH(ch_mult_args, task_init, task_mult, task_print));
        printf("%x ", A[i]);
    }
    printf("\r\n");
    printf("init: B=");
    for (i = NUM_DIGITS - 1; i >= 0; --i) {
        CHAN_OUT(B[i], B[NUM_DIGITS - 1 - i], MC_OUT_CH(ch_mult_args, task_init, task_mult, task_print));
        printf("%x ", B[i]);
    }
    printf("\r\n");
    printf("init: M=");
    for (i = NUM_DIGITS - 1; i >= 0; --i) {
        CHAN_OUT(M[i], M[NUM_DIGITS - 1 - i], MC_OUT_CH(ch_modulus, task_init, task_normalizable, task_normalize));
        printf("%x ", M[i]);
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

    blink(1, SEC_TO_CYCLES / 4, LED1);

    digit = *CHAN_IN2(digit, CH(task_init, task_mult), SELF_IN_CH(task_mult));
    carry = *CHAN_IN2(carry, CH(task_init, task_mult), SELF_IN_CH(task_mult));

    printf("mult: digit=%u carry=%x\r\n", digit, carry);

    p = carry;
    for (i = 0; i < NUM_DIGITS; ++i) {
        if (digit - i >= 0 && digit - i < NUM_DIGITS) {
            a = *CHAN_IN1(A[digit - i], MC_IN_CH(ch_mult_args, task_init, task_mult));
            b = *CHAN_IN1(B[i], MC_IN_CH(ch_mult_args, task_init, task_mult));
            p += a * b;
            printf("mult: i=%u a=%x b=%x p=%x\r\n", i, a, b, p);
        }
    }

    c = p >> REG_SIZE;
    p &= REG_MASK;

    printf("mult: c=%x p=%x\r\n", c, p);

    CHAN_OUT(product[digit], p, MC_OUT_CH(ch_product, task_mult,
             task_normalizable, task_normalize, task_print_product));

    digit++;

    if (digit < NUM_DIGITS * 2) {
        CHAN_OUT(carry, c, SELF_OUT_CH(task_mult));
        CHAN_OUT(digit, digit, SELF_OUT_CH(task_mult));
        TRANSITION_TO(task_mult);
    } else {
        CHAN_OUT(next_task, TASK_NORMALIZABLE, CH(task_mult, task_print_product));
        TRANSITION_TO(task_print_product);
    }
}

void task_normalizable()
{
    int i;
    unsigned p, m;
    bool normalizable = true;

    printf("normalizable\r\n");

    // Variables:
    //   l: number of digits in the product (2 * NUM_DIGITS)
    //   k: number of digits in the modulus (NUM_DIGITS)
    //
    // if (x > m b^(l-k)
    //     x = x - m b^(l-k)
    //
    // NOTE: It's temptimg to do the subtraction opportunistically, and if
    // the result is negative, then the condition must have been false.
    // However, we can't do that because under this approach, we must
    // write to the output channel zero digits for digits that turn
    // out to be equal, but if a later digit pair is such that condition
    // is false (p < m), then those rights are invalid, but we have no
    // good way of exluding them from being picked up by the later
    // task. One possiblity is to transmit a flag to that task that
    // tells it whether to include our output channel into its input sync
    // statement. However, this seems less elegant than splitting the
    // normalization into two tasks: the condition and the subtraction.
    //
    // Multiplication by a power of radix (b) is a shift, so when we do the
    // comparison/subtraction of the digits, we offset the index into the
    // product digits by (l-k) = NUM_DIGITS.

    for (i = NUM_DIGITS - 1; i >= 0; --i) {
        p = *CHAN_IN1(product[NUM_DIGITS + i], MC_IN_CH(ch_product, task_mult, task_normalizable));
        m = *CHAN_IN1(M[i], MC_IN_CH(ch_modulus, task_init, task_normalizable));

        printf("normalizable: p[%u]=%x m[%u]=%x\r\n", NUM_DIGITS + i, p, i, m);

        if (p > m) {
            break;
        } else if (p < m) {
            normalizable = false;
            break;
        }
    }

    printf("normalizable: %u\r\n", normalizable);

    if (normalizable) {
        TRANSITION_TO(task_normalize);
    } else {
        TRANSITION_TO(task_reduce);
    }
}

// TODO: consider decomposing into subtasks
void task_normalize()
{
    int i;
    uint16_t p, m, d, s;
    unsigned borrow;

    printf("normalize\r\n");

    borrow = 0;
    for (i = 0; i < NUM_DIGITS; ++i) {
        p = *CHAN_IN1(product[NUM_DIGITS + i], MC_IN_CH(ch_product, task_mult, task_normalize));
        m = *CHAN_IN1(M[i], MC_IN_CH(ch_modulus, task_init, task_normalize));

        s = m + borrow;
        if (p < s) {
            p += 1 << REG_SIZE;
            borrow = 1;
        } else {
            borrow = 0;
        }
        d = p - s;

        printf("normalize: p[%u]=%x m[%u]=%x b=%u d=%x\r\n", NUM_DIGITS + i, p, i, m, borrow, d);

        CHAN_OUT(product[NUM_DIGITS + i], d, MC_OUT_CH(ch_normalized_product, task_normalize,
                                              task_reduce, task_print_product));
    }

#if 0 // NOTE: we don't need to do that because: sync! it grabs the latest
    // Copy the digits unaffected by the normalization
    for (i = NUM_DIGITS - 1; i >= 0; --i) {
        d = *CHAN_IN1(product[i], MC_IN_CH(task_mult, task_normalize));
        CHAN_OUT(product[i], d, CH(task_normalize, task_reduce));
    }
#endif

    CHAN_OUT(next_task, TASK_REDUCE, CH(task_normalize, task_print_product));
    TRANSITION_TO(task_print_product);
}

void task_reduce()
{
    blink(1, SEC_TO_CYCLES, LED2);

    printf("reduce\r\n");

    // TODO: CHAN_IN2(product[], MC_IN_CH(ch_product, task_mult),
    //                           MC_IN_CH(ch_normalized_product, task_normalize));

    TRANSITION_TO(task_init);
}

void task_print_product()
{
    int i;
    uint16_t p;
    task_t next_task;

    printf("print: P=");
    for (i = (NUM_DIGITS * 2) - 1; i >= 0; --i) {
        p = *CHAN_IN2(product[i], MC_IN_CH(ch_product, task_mult, task_print_product),
                                  MC_IN_CH(ch_normalized_product, task_normalize, task_print_product));
        printf("%x ", p);
    }
    printf("\r\n");

    next_task = *CHAN_IN2(next_task, CH(task_mult, task_print_product),
                                     CH(task_normalize, task_print_product));

    // TODO: this is where first-class tasks would come in useful. Basically,
    // any re-usable task would have this control flow branching.
    switch (next_task) {
        case TASK_NORMALIZABLE:
            TRANSITION_TO(task_normalizable);
            break;
        case TASK_REDUCE:
            TRANSITION_TO(task_reduce);
            break;
        default:
            // TODO: ABORT
            break;
    }
}

ENTRY_TASK(task_init)
INIT_FUNC(init)
