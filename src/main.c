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
#define DIGIT_BITS       8 // arithmetic ops take 8-bit args produce 16-bit result
#define DIGIT_MASK       0x00ff

/** @brief Type large enough to store a product of two digits */
typedef uint16_t digit_t;

#if NUM_DIGITS < 2
#error The modular reduction implementation requires at least 2 digits
#endif

#define LED1 (1 << 0)
#define LED2 (1 << 1)

#define SEC_TO_CYCLES 4000000 /* 4 MHz */

#define BLINK_DURATION_TASK SEC_TO_CYCLES

// If you link-in wisp-base, then you have to define some symbols.
uint8_t usrBank[USRBANK_SIZE];

// TODO: this is app-specific, but a good candidate for genericizing
// This pertains to the pattern of reusable tasks using a 'next task' value.
typedef enum {
    TASK_INIT,
    TASK_NORMALIZABLE,
    TASK_REDUCE_M_DIVISOR,
    TASK_REDUCE_QUOTIENT,
    TASK_REDUCE_COMPARE,
    TASK_REDUCE_SUBTRACT,
    // Not all tasks listed because only some are used as next task
} task_t;

struct msg_mult {
    CHAN_FIELD_ARRAY(digit_t, A, NUM_DIGITS);
    CHAN_FIELD_ARRAY(digit_t, B, NUM_DIGITS);
};

struct msg_modulus {
    CHAN_FIELD_ARRAY(digit_t, M, NUM_DIGITS);
};

struct msg_mult_digit {
    CHAN_FIELD(unsigned, digit);
    CHAN_FIELD(unsigned, carry);
};

struct msg_product {
    CHAN_FIELD_ARRAY(digit_t, product, NUM_DIGITS * 2);
};

struct msg_divisor {
    CHAN_FIELD(unsigned, digit);
    CHAN_FIELD(digit_t, m_div);
};

struct msg_digit {
    CHAN_FIELD(unsigned, digit);
};

struct msg_quotient {
    CHAN_FIELD(digit_t, quotient);
};

struct msg_next_task {
    CHAN_FIELD(task_t, next_task);
};

TASK(0, task_init)
TASK(1, task_mult)
TASK(2, task_normalizable)
TASK(3, task_normalize)
TASK(4, task_reduce_m_divisor)
TASK(5, task_reduce_quotient)
TASK(6, task_reduce_multiply)
TASK(7, task_reduce_compare)
TASK(8, task_reduce_add)
TASK(9, task_reduce_subtract)
TASK(10, task_print_product)

CHANNEL(task_init, task_mult, msg_mult_digit);
MULTICAST_CHANNEL(msg_modulus, ch_modulus, task_init,
                  task_normalizable, task_normalize,
                  task_reduce_m_divisor, task_reduce_quotient, task_reduce_multiply);
MULTICAST_CHANNEL(msg_mult, ch_mult_args, task_init, task_mult, task_print_product);
SELF_CHANNEL(task_mult, msg_mult_digit);
MULTICAST_CHANNEL(msg_product, ch_product, task_mult,
                  task_normalizable, task_normalize,
                  task_reduce_quotient, task_reduce_compare,
                  task_reduce_add, task_reduce_subtract,
                  task_print_product);
MULTICAST_CHANNEL(msg_product, ch_normalized_product, task_normalize,
                  task_reduce_quotient, task_reduce_compare,
                  task_reduce_add, task_reduce_subtract,
                  task_print_product);
MULTICAST_CHANNEL(msg_product, ch_reduced_product, task_reduce_subtract,
                  task_reduce_quotient,
                  task_print_product);
MULTICAST_CHANNEL(msg_product, ch_reduce_add_product, task_reduce_add,
                  task_reduce_subtract,
                  task_print_product);
MULTICAST_CHANNEL(msg_product, ch_reduce_subtract_product, task_reduce_subtract,
                  task_reduce_quotient,
                  task_print_product);
CHANNEL(task_mult, task_print_product, msg_next_task);
CHANNEL(task_normalizable, task_print_product, msg_next_task);
CHANNEL(task_normalize, task_print_product, msg_next_task);
CHANNEL(task_reduce_m_divisor, task_reduce_quotient, msg_divisor);
SELF_CHANNEL(task_reduce_quotient, msg_digit);
MULTICAST_CHANNEL(msg_digit, ch_reduce_digit, task_reduce_quotient,
                  task_reduce_multiply, task_reduce_compare,
                  task_reduce_add, task_reduce_subtract);
CHANNEL(task_reduce_quotient, task_reduce_multiply, msg_quotient);
CHANNEL(task_reduce_multiply, task_print_product, msg_next_task);
MULTICAST_CHANNEL(msg_product, ch_qm, task_reduce_multiply,
                  task_reduce_compare, task_reduce_subtract,
                  task_print_product);
CHANNEL(task_reduce_add, task_print_product, msg_next_task);
CHANNEL(task_reduce_subtract, task_print_product, msg_next_task);

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
    for (i = 0; i < NUM_DIGITS; ++i) {
        CHAN_OUT(A[NUM_DIGITS - 1 - i], A[i], MC_OUT_CH(ch_mult_args, task_init, task_mult, task_print));
        printf("%x ", A[i]);
    }
    printf("\r\n");
    printf("init: B=");
    for (i = 0; i < NUM_DIGITS; ++i) {
        CHAN_OUT(B[NUM_DIGITS - 1 - i], B[i], MC_OUT_CH(ch_mult_args, task_init, task_mult, task_print));
        printf("%x ", B[i]);
    }
    printf("\r\n");
    printf("init: M=");
    for (i = 0; i < NUM_DIGITS; ++i) {
        CHAN_OUT(M[NUM_DIGITS - 1 - i], M[i],
                MC_OUT_CH(ch_modulus, task_init,
                          task_normalizable, task_normalize, task_reduce));
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
    digit_t a, b, c;
    digit_t p, carry;
    int digit;

    blink(1, BLINK_DURATION_TASK / 4, LED1);

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

    c = p >> DIGIT_BITS;
    p &= DIGIT_MASK;

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
        TRANSITION_TO(task_reduce_m_divisor);
    }
}

// TODO: consider decomposing into subtasks
void task_normalize()
{
    int i;
    digit_t p, m, d, s;
    unsigned borrow;

    printf("normalize\r\n");

    borrow = 0;
    for (i = 0; i < NUM_DIGITS; ++i) {
        p = *CHAN_IN1(product[NUM_DIGITS + i], MC_IN_CH(ch_product, task_mult, task_normalize));
        m = *CHAN_IN1(M[i], MC_IN_CH(ch_modulus, task_init, task_normalize));

        s = m + borrow;
        if (p < s) {
            p += 1 << DIGIT_BITS;
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

    CHAN_OUT(next_task, TASK_REDUCE_M_DIVISOR, CH(task_normalize, task_print_product));
    TRANSITION_TO(task_print_product);
}

void task_reduce_m_divisor()
{
    digit_t m[2]; // [1]=M[msd], [0]=M[msd-1]
    digit_t m_div;

    blink(1, SEC_TO_CYCLES, LED2);

    printf("reduce: m divisor\r\n");

    m[1]  = *CHAN_IN1(M[NUM_DIGITS - 1], MC_IN_CH(ch_modulus, task_init, task_reduce_m_divisor));
    m[0] = *CHAN_IN1(M[NUM_DIGITS - 2], MC_IN_CH(ch_modulus, task_init, task_m_divisor));

    // Divisor, derived from modulus, for refining quotient guess into exact value
    m_div = ((m[1]<< DIGIT_BITS) + m[0]);

    printf("reduce: m divisor: m[1]=%x m[0]=%x m_div=%x\r\n", m[1], m[0], m_div);

    CHAN_OUT(m_div, m_div, CH(task_reduce_m_divisor, task_reduce_quotient));

    // Start reduction loop at most significant digit
    CHAN_OUT(digit, NUM_DIGITS * 2 - 1, CH(task_reduce_m_divisor, task_reduce_quotient));

    TRANSITION_TO(task_reduce_quotient);
}

void task_reduce_quotient()
{
    unsigned d;
    digit_t p[3]; // [2]=p[d], [1]=p[d-1], [0]=p[d-2]
    digit_t m_n, m_div, q;
    uint32_t qm, p_q; // must hold at least 3 digits

    blink(1, BLINK_DURATION_TASK, LED2);

    d = *CHAN_IN2(digit, CH(task_reduce_m_divisor, task_reduce_quotient),
                         SELF_IN_CH(task_reduce_quotient));

    printf("reduce: quotient: d=%x\r\n", d);

    p[2] = *CHAN_IN3(product[d],
                     MC_IN_CH(ch_product, task_mult, task_reduce_quotient),
                     MC_IN_CH(ch_normalized_product, task_normalize, task_reduce_quotient),
                     MC_IN_CH(ch_reduced_product, task_reduce_subtract, task_reduce_quotient));
    p[1] = *CHAN_IN3(product[d - 1],
                     MC_IN_CH(ch_product, task_mult, task_reduce_quotient),
                     MC_IN_CH(ch_normalized_product, task_normalize, task_reduce_quotient),
                     MC_IN_CH(ch_reduced_product, task_reduce_subtract, task_reduce_quotient));
    p[0] = *CHAN_IN3(product[d - 2],
                     MC_IN_CH(ch_product, task_mult, task_reduce_quotient),
                     MC_IN_CH(ch_normalized_product, task_normalize, task_reduce_quotient),
                     MC_IN_CH(ch_reduced_product, task_reduce_subtract, task_reduce_quotient));
    // NOTE: we asserted that NUM_DIGITS >= 2, so p[d-2] is safe

    m_n = *CHAN_IN1(M[NUM_DIGITS - 1],
                    MC_IN_CH(ch_modulus, task_init, task_reduce_quotient));

    printf("reduce: quotient: m_n=%x p[d]=%x\r\n", m_n, p[2]);

    // Choose an initial guess for quotient
    if (p[2] == m_n) {
        q = (1 << DIGIT_BITS) - 1;
    } else {
        q = ((p[2] << DIGIT_BITS) + p[1]) / m_n;
    }

    printf("reduce: quotient: q0=%x\r\n", q);

    // Refine quotient guess

    // NOTE: An alternative to composing the digits into one variable, is to
    // have a loop that does the comparison digit by digit to implement the
    // condition of the while loop below.
    p_q = ((uint32_t)p[2] << (2 * DIGIT_BITS)) + (p[1] << DIGIT_BITS) + p[0];

    printf("reduce: quotient: p[d]=%x p[d-1]=%x p[d-2]=%x p_q=%x%x\r\n",
           p[2], p[1], p[0], (uint16_t)((p_q >> 16) & 0xffff), (uint16_t)(p_q & 0xffff));

    m_div = *CHAN_IN1(m_div, CH(task_reduce_m_divisor, task_reduce_quotient));

    printf("reduce: quotient: m_div=%x q0=%x\r\n", m_div, q);

    q++;
    do {
        q--;
        qm = (uint32_t)m_div * q;
        printf("reduce: quotient: q=%x qm=%x%x\r\n", q,
              (uint16_t)((qm >> 16) & 0xffff), (uint16_t)(qm & 0xffff));
    } while (qm > p_q);

    // This is still not the final quotient, it may be off by one,
    // which we determine and fix in the 'compare' and 'add' steps.
    printf("reduce: quotient: q=%x\r\n", q);

    CHAN_OUT(quotient, q, CH(task_reduce_quotient, task_reduce_multiply));

    CHAN_OUT(digit, d, MC_OUT_CH(ch_reduce_digit, task_reduce_quotient,
                                 task_reduce_multiply, task_reduce_add,
                                 task_reduce_subtract));

    d--;
    CHAN_OUT(digit, d, SELF_OUT_CH(task_reduce_quotient));

    TRANSITION_TO(task_reduce_multiply);
}

// NOTE: this is multiplication by one digit, hence not re-using mult task
void task_reduce_multiply()
{
    int i;
    digit_t p, q, m;
    unsigned c, d, offset;

    blink(1, BLINK_DURATION_TASK, LED2);

    d = *CHAN_IN1(digit, MC_IN_CH(ch_reduce_digit,
                                  task_reduce_quotient, task_reduce_multiply));
    q = *CHAN_IN1(quotient, CH(task_reduce_quotient, task_reduce_multiply));

    printf("reduce: multiply: d=%x q=%x\r\n", d, q);

    // As part of this task, we also perform the left-shifting of the q*m
    // product by radix^(digit-NUM_DIGITS), where NUM_DIGITS is the number
    // of digits in the modulus. We implement this by fetching the digits
    // of number being reduced at that offset.
    offset = d - NUM_DIGITS;
    printf("reduce: multiply: offset=%u\r\n", offset);

    // TODO: could convert the loop into a self-edge
    c = 0;
    for (i = offset; i < 2 * NUM_DIGITS; ++i) {

        // This condition creates the left-shifted zeros.
        // TODO: consider adding number of digits to go along with the 'product' field,
        // then we would not have to zero out the MSDs
        p = c;
        if (i < offset + NUM_DIGITS) {
            m = *CHAN_IN1(M[i - offset], MC_IN_CH(ch_modulus, task_init, task_reduce_multiply));
            p += q * m;
        } else {
            m = 0;
            // TODO: could break out of the loop  in this case (after CHAN_OUT)
        }

        printf("reduce: multiply: m[%u]=%x q=%x c=%x p[%u]=%x\r\n", i - offset, m, q, c, i, p);

        c = p >> DIGIT_BITS;
        p &= DIGIT_MASK;

        CHAN_OUT(product[i], p,
                 MC_OUT_CH(ch_qm, task_reduce_multiply,
                           task_reduce_compare, task_reduce_subtract,
                           task_print_product));
    }

    CHAN_OUT(next_task, TASK_REDUCE_COMPARE, CH(task_reduce_subtract, task_print_product));
    TRANSITION_TO(task_print_product);
}

void task_reduce_compare()
{
    int i;
    digit_t p, d, qm;
    char relation = '=';

    blink(1, BLINK_DURATION_TASK, LED2);

    d = *CHAN_IN1(digit, MC_IN_CH(ch_reduce_digit,
                                  task_reduce_quotient, task_reduce_compare));

    printf("reduce: compare: d=%u\r\n", d);

    // TODO: could transform this loop into a self-edge
    // TODO: this loop might not have to go down to zero, but to NUM_DIGITS
    // TODO: consider adding number of digits to go along with the 'product' field
    for (i = NUM_DIGITS * 2 - 1; i >= 0; --i) {
        p = *CHAN_IN2(product[i],
                      MC_IN_CH(ch_product, task_mult, task_reduce_compare),
                      MC_IN_CH(ch_normalized_product, task_normalize, task_reduce_compare));
        qm = *CHAN_IN1(product[i], MC_IN_CH(ch_qm, task_reduce_multiply, task_reduce_compare));

        printf("reduce: compare: p[%u]=%x qm[%u]=%x\r\n", i, p, i, qm);

        if (p > qm) {
            relation = '>';
            break;
        } else if (p < qm) {
            relation = '<';
            break;
        }
    }

    printf("reduce: compare: relation %c\r\n", relation);

    if (relation == '<') {
        TRANSITION_TO(task_reduce_add);
    } else {
        TRANSITION_TO(task_reduce_subtract);
    }
}

// TODO: this addition and subtraction can probably be collapsed
// into one loop that always subtracts the digits, but, conditionally, also
// adds depending on the result from the 'compare' task. For now,
// we keep them separate for clarity.

void task_reduce_add()
{
    int i;
    digit_t p, m, c, r;
    unsigned d, offset;

    blink(1, BLINK_DURATION_TASK, LED2);

    d = *CHAN_IN1(digit, MC_IN_CH(ch_reduce_digit,
                                  task_reduce_quotient, task_reduce_compare));

    printf("reduce: add: d=%u\r\n", d);

    // Part of this task is to shift modulus by radix^(digit - NUM_DIGITS)
    offset = d - NUM_DIGITS;

    // TODO: coult transform this loop into a self-edge
    c = 0;
    for (i = offset; i < 2 * NUM_DIGITS; ++i) {
        p = *CHAN_IN2(product[i],
                      MC_IN_CH(ch_product, task_mult, task_reduce_add),
                      MC_IN_CH(ch_normalized_product, task_normalize, task_reduce_add));

        m = *CHAN_IN1(M[i - offset], MC_IN_CH(ch_modulus, task_init, task_reduce_add));

        r = c + p + m;

        printf("reduce: add: p[%u]=%x m[%u]=%x c=%x r=%x\r\n", i, p, i - offset, m, c, r);

        c = r >> DIGIT_BITS;
        r &= DIGIT_MASK;

        CHAN_OUT(product[i], r, MC_OUT_CH(ch_reduce_add_product, task_reduce_add,
                                          task_reduce_subtract, task_print_product));
    }

    CHAN_OUT(next_task, TASK_REDUCE_SUBTRACT, CH(task_reduce_subtract, task_print_product));
    TRANSITION_TO(task_print_product);
}

// TODO: re-use task_normalize?
void task_reduce_subtract()
{
    int i;
    digit_t p, s, r, qm;
    unsigned d, borrow;
    task_t next_task;

    blink(1, BLINK_DURATION_TASK, LED2);

    d = *CHAN_IN1(digit, MC_IN_CH(ch_reduce_digit, task_reduce_quotient, task_reduce_subtract));

    printf("reduce: subtract: d=%u\r\n", d);

    // TODO: could transform this loop into a self-edge
    borrow = 0;
    for (i = d - NUM_DIGITS; i < 2 * NUM_DIGITS; ++i) {
        p = *CHAN_IN3(product[i],
                      MC_IN_CH(ch_product, task_mult, task_reduce_subtract),
                      MC_IN_CH(ch_normalized_product, task_normalize, task_reduce_subtract),
                      MC_IN_CH(ch_reduce_add_product, task_reduce_add, task_reduce_subtract));
        qm = *CHAN_IN1(product[i], MC_IN_CH(ch_qm, task_reduce_multiply, task_reduce_subtract));

        s = qm + borrow;
        if (p < s) {
            p += 1 << DIGIT_BITS;
            borrow = 1;
        } else {
            borrow = 0;
        }
        r = p - s;

        printf("reduce: subtract: p[%u]=%x qm[%u]=%x b=%u r=%x\r\n",
               i, p, i, qm, borrow, r);

        CHAN_OUT(product[i], r,
                 MC_OUT_CH(ch_reduce_subtract_product, task_reduce_subtract,
                           task_reduce_quotient, task_print_product));
    }

    if (d - NUM_DIGITS < NUM_DIGITS) {
        next_task = TASK_REDUCE_QUOTIENT;
    } else { // reduction finished
        next_task = TASK_INIT;
    }

    CHAN_OUT(next_task, next_task, CH(task_reduce_subtract, task_print_product));
    TRANSITION_TO(task_print_product);
}

void task_print_product()
{
    int i;
    digit_t p;
    task_t next_task;

    printf("print: P=");
    for (i = (NUM_DIGITS * 2) - 1; i >= 0; --i) {
        p = *CHAN_IN5(product[i],
                      MC_IN_CH(ch_product, task_mult, task_print_product),
                      MC_IN_CH(ch_normalized_product, task_normalize, task_print_product),
                      MC_IN_CH(ch_reduce_add_product, task_reduce_add, task_print_product),
                      MC_IN_CH(ch_reduce_subtract_product, task_reduce_subtract, task_print_product),
                      MC_IN_CH(ch_qm, task_reduce_multiply, task_print_product));

        printf("%x ", p);
    }
    printf("\r\n");

    next_task = *CHAN_IN5(next_task, CH(task_mult, task_print_product),
                                     CH(task_normalize, task_print_product),
                                     CH(task_reduce_multiply, task_print_product),
                                     CH(task_reduce_add, task_print_product),
                                     CH(task_reduce_subtract, task_print_product));

    // TODO: this is where first-class tasks would come in useful. Basically,
    // any re-usable task would have this control flow branching.
    // TODO: don't we already effectively support this? can't we pass a pointer
    // variable to TRANSITION_TO?
    switch (next_task) {
        case TASK_NORMALIZABLE:
            TRANSITION_TO(task_normalizable);
            break;
        case TASK_REDUCE_M_DIVISOR:
            TRANSITION_TO(task_reduce_m_divisor);
            break;
        case TASK_REDUCE_QUOTIENT:
            TRANSITION_TO(task_reduce_quotient);
            break;
        case TASK_REDUCE_COMPARE:
            TRANSITION_TO(task_reduce_compare);
            break;
        case TASK_REDUCE_SUBTRACT:
            TRANSITION_TO(task_reduce_subtract);
            break;
        case TASK_INIT:
            TRANSITION_TO(task_init);
            break;
        default:
            // TODO: ABORT
            break;
    }
}

ENTRY_TASK(task_init)
INIT_FUNC(init)
