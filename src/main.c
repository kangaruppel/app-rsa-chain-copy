#include <msp430.h>
#undef N // conflicts with us

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

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

struct msg_mult {
    CHAN_FIELD_ARRAY(digit_t, A, NUM_DIGITS);
    CHAN_FIELD_ARRAY(digit_t, B, NUM_DIGITS);
    CHAN_FIELD(unsigned, digit);
    CHAN_FIELD(unsigned, carry);
};

struct msg_modulus {
    CHAN_FIELD_ARRAY(digit_t, N, NUM_DIGITS);
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
    CHAN_FIELD(digit_t, n_div);
};

struct msg_digit {
    CHAN_FIELD(unsigned, digit);
};

struct msg_quotient {
    CHAN_FIELD(digit_t, quotient);
};

struct msg_print {
    CHAN_FIELD_ARRAY(digit_t, product, NUM_DIGITS * 2);
    CHAN_FIELD(const task_t*, next_task);
};

TASK(1, task_init)
TASK(2, task_mult)
TASK(3, task_reduce_normalizable)
TASK(4, task_reduce_normalize)
TASK(6, task_reduce_n_divisor)
TASK(6, task_reduce_quotient)
TASK(7, task_reduce_multiply)
TASK(8, task_reduce_compare)
TASK(9, task_reduce_add)
TASK(10, task_reduce_subtract)
TASK(11, task_print_product)

CHANNEL(task_init, task_mult, msg_mult);
MULTICAST_CHANNEL(msg_modulus, ch_modulus, task_init,
                  task_reduce_normalizable, task_reduce_normalize,
                  task_reduce_n_divisor, task_reduce_quotient, task_reduce_multiply);
SELF_CHANNEL(task_mult, msg_mult_digit);
MULTICAST_CHANNEL(msg_product, ch_product, task_mult,
                  task_reduce_normalizable, task_reduce_normalize,
                  task_reduce_quotient, task_reduce_compare,
                  task_reduce_add, task_reduce_subtract);
MULTICAST_CHANNEL(msg_product, ch_normalized_product, task_reduce_normalize,
                  task_reduce_quotient, task_reduce_compare,
                  task_reduce_add, task_reduce_subtract);
CHANNEL(task_reduce_add, task_reduce_subtract, msg_product);
MULTICAST_CHANNEL(msg_product, ch_reduce_subtract_product, task_reduce_subtract,
                  task_reduce_quotient, task_reduce_compare, task_reduce_add);
SELF_CHANNEL(task_reduce_subtract, msg_product);
CHANNEL(task_reduce_n_divisor, task_reduce_quotient, msg_divisor);
SELF_CHANNEL(task_reduce_quotient, msg_digit);
MULTICAST_CHANNEL(msg_digit, ch_reduce_digit, task_reduce_quotient,
                  task_reduce_multiply, task_reduce_compare,
                  task_reduce_add, task_reduce_subtract);
CHANNEL(task_reduce_quotient, task_reduce_multiply, msg_quotient);
MULTICAST_CHANNEL(msg_product, ch_qn, task_reduce_multiply,
                  task_reduce_compare, task_reduce_subtract);
CALL_CHANNEL(ch_print_product, msg_print);

// Test input
static const uint8_t A[] = { 0x40, 0x30, 0x20, 0x10 };
static const uint8_t B[] = { 0xB0, 0xA0, 0x90, 0x80 };
static const uint8_t N[] = { 0x80, 0x49, 0x60, 0x01 }; // see note below

// NOTE: Restriction: M >= 0x80000000 (i.e. MSB set). To lift restriction need
// to implement normalization: left shift until MSB is set, to reverse, right
// shift the remainder.

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
        CHAN_OUT(A[NUM_DIGITS - 1 - i], A[i], CH(task_init, task_mult));
        printf("%x ", A[i]);
    }
    printf("\r\n");
    printf("init: B=");
    for (i = 0; i < NUM_DIGITS; ++i) {
        CHAN_OUT(B[NUM_DIGITS - 1 - i], B[i], CH(task_init, task_mult));
        printf("%x ", B[i]);
    }
    printf("\r\n");
    printf("init: N=");
    for (i = 0; i < NUM_DIGITS; ++i) {
        CHAN_OUT(N[NUM_DIGITS - 1 - i], N[i], MC_OUT_CH(ch_modulus, task_init,
                 task_reduce_normalizable, task_reduce_normalize,
                 task_reduce_n_divisor, task_reduce_quotient,
                 task_reduce_multiply, task_reduce_add));
        printf("%x ", N[i]);
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
            a = *CHAN_IN1(A[digit - i], CH(task_init, task_mult));
            b = *CHAN_IN1(B[i], CH(task_init, task_mult));
            p += a * b;
            printf("mult: i=%u a=%x b=%x p=%x\r\n", i, a, b, p);
        }
    }

    c = p >> DIGIT_BITS;
    p &= DIGIT_MASK;

    printf("mult: c=%x p=%x\r\n", c, p);

    CHAN_OUT(product[digit], p, MC_OUT_CH(ch_product, task_mult,
             task_reduce_normalizable, task_reduce_normalize));

    CHAN_OUT(product[digit], p, CALL_CH(ch_print_product));

    digit++;

    if (digit < NUM_DIGITS * 2) {
        CHAN_OUT(carry, c, SELF_OUT_CH(task_mult));
        CHAN_OUT(digit, digit, SELF_OUT_CH(task_mult));
        TRANSITION_TO(task_mult);
    } else {
        CHAN_OUT(next_task, TASK_REF(task_reduce_normalizable), CALL_CH(ch_print_product));
        TRANSITION_TO(task_print_product);
    }
}

void task_reduce_normalizable()
{
    int i;
    unsigned m, n;
    bool normalizable = true;

    printf("normalizable\r\n");

    // Variables:
    //   m: message
    //   n: modulus
    //   b: digit base (2**8)
    //   l: number of digits in the product (2 * NUM_DIGITS)
    //   k: number of digits in the modulus (NUM_DIGITS)
    //
    // if (m > n b^(l-k)
    //     m = m - n b^(l-k)
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
        m = *CHAN_IN1(product[NUM_DIGITS + i],
                      MC_IN_CH(ch_product, task_mult, task_reduce_normalizable));
        n = *CHAN_IN1(N[i], MC_IN_CH(ch_modulus, task_init, task_reduce_normalizable));

        printf("normalizable: m[%u]=%x n[%u]=%x\r\n", NUM_DIGITS + i, m, i, n);

        if (m > n) {
            break;
        } else if (m < n) {
            normalizable = false;
            break;
        }
    }

    printf("normalizable: %u\r\n", normalizable);

    if (normalizable) {
        TRANSITION_TO(task_reduce_normalize);
    } else {
        TRANSITION_TO(task_reduce_n_divisor);
    }
}

// TODO: consider decomposing into subtasks
void task_reduce_normalize()
{
    int i;
    digit_t m, n, d, s;
    unsigned borrow;

    printf("normalize\r\n");

    // To call the print task, we need to proxy the values we don't touch
    for (i = 0; i < NUM_DIGITS; ++i) {
        m = *CHAN_IN1(product[i], MC_IN_CH(ch_product, task_mult, task_reduce_normalize));
        CHAN_OUT(product[i], m, CALL_CH(ch_print_product));
    }

    borrow = 0;
    for (i = 0; i < NUM_DIGITS; ++i) {
        m = *CHAN_IN1(product[NUM_DIGITS + i],
                      MC_IN_CH(ch_product, task_mult, task_reduce_normalize));
        n = *CHAN_IN1(N[i], MC_IN_CH(ch_modulus, task_init, task_reduce_normalize));

        s = n + borrow;
        if (m < s) {
            m += 1 << DIGIT_BITS;
            borrow = 1;
        } else {
            borrow = 0;
        }
        d = m - s;

        printf("normalize: m[%u]=%x n[%u]=%x b=%u d=%x\r\n",
                NUM_DIGITS + i, m, i, n, borrow, d);

        CHAN_OUT(product[NUM_DIGITS + i], d,
                 MC_OUT_CH(ch_normalized_product, task_reduce_normalize,
                           task_reduce_quotient, task_reduce_compare,
                           task_reduce_add, task_reduce_subtract));

        CHAN_OUT(product[NUM_DIGITS + i], d, CALL_CH(ch_print_product));
    }

    CHAN_OUT(next_task, TASK_REF(task_reduce_n_divisor), CALL_CH(ch_print_product));
    TRANSITION_TO(task_print_product);
}

void task_reduce_n_divisor()
{
    digit_t n[2]; // [1]=N[msd], [0]=N[msd-1]
    digit_t n_div;

    blink(1, SEC_TO_CYCLES, LED2);

    printf("reduce: n divisor\r\n");

    n[1]  = *CHAN_IN1(N[NUM_DIGITS - 1],
                      MC_IN_CH(ch_modulus, task_init, task_reduce_n_divisor));
    n[0] = *CHAN_IN1(N[NUM_DIGITS - 2], MC_IN_CH(ch_modulus, task_init, task_n_divisor));

    // Divisor, derived from modulus, for refining quotient guess into exact value
    n_div = ((n[1]<< DIGIT_BITS) + n[0]);

    printf("reduce: n divisor: n[1]=%x n[0]=%x n_div=%x\r\n", n[1], n[0], n_div);

    CHAN_OUT(n_div, n_div, CH(task_reduce_n_divisor, task_reduce_quotient));

    // Start reduction loop at most significant digit
    CHAN_OUT(digit, NUM_DIGITS * 2 - 1, CH(task_reduce_n_divisor, task_reduce_quotient));

    TRANSITION_TO(task_reduce_quotient);
}

void task_reduce_quotient()
{
    unsigned d;
    digit_t m[3]; // [2]=m[d], [1]=m[d-1], [0]=m[d-2]
    digit_t m_n, n_div, q;
    uint32_t qn, n_q; // must hold at least 3 digits

    blink(1, BLINK_DURATION_TASK, LED2);

    d = *CHAN_IN2(digit, CH(task_reduce_n_divisor, task_reduce_quotient),
                         SELF_IN_CH(task_reduce_quotient));

    printf("reduce: quotient: d=%x\r\n", d);

    m[2] = *CHAN_IN3(product[d],
                     MC_IN_CH(ch_product, task_mult, task_reduce_quotient),
                     MC_IN_CH(ch_normalized_product, task_reduce_normalize, task_reduce_quotient),
                     MC_IN_CH(ch_reduce_subtract_product, task_reduce_subtract,
                              task_reduce_quotient));
    m[1] = *CHAN_IN3(product[d - 1],
                     MC_IN_CH(ch_product, task_mult, task_reduce_quotient),
                     MC_IN_CH(ch_normalized_product, task_reduce_normalize, task_reduce_quotient),
                     MC_IN_CH(ch_reduce_subtract_product, task_reduce_subtract,
                              task_reduce_quotient));
    m[0] = *CHAN_IN3(product[d - 2],
                     MC_IN_CH(ch_product, task_mult, task_reduce_quotient),
                     MC_IN_CH(ch_normalized_product, task_reduce_normalize, task_reduce_quotient),
                     MC_IN_CH(ch_reduce_subtract_product, task_reduce_subtract,
                              task_reduce_quotient));
    // NOTE: we asserted that NUM_DIGITS >= 2, so p[d-2] is safe

    m_n = *CHAN_IN1(N[NUM_DIGITS - 1],
                    MC_IN_CH(ch_modulus, task_init, task_reduce_quotient));

    printf("reduce: quotient: m_n=%x m[d]=%x\r\n", m_n, m[2]);

    // Choose an initial guess for quotient
    if (m[2] == m_n) {
        q = (1 << DIGIT_BITS) - 1;
    } else {
        q = ((m[2] << DIGIT_BITS) + m[1]) / m_n;
    }

    printf("reduce: quotient: q0=%x\r\n", q);

    // Refine quotient guess

    // NOTE: An alternative to composing the digits into one variable, is to
    // have a loop that does the comparison digit by digit to implement the
    // condition of the while loop below.
    n_q = ((uint32_t)m[2] << (2 * DIGIT_BITS)) + (m[1] << DIGIT_BITS) + m[0];

    printf("reduce: quotient: m[d]=%x m[d-1]=%x m[d-2]=%x n_q=%x%x\r\n",
           m[2], m[1], m[0], (uint16_t)((n_q >> 16) & 0xffff), (uint16_t)(n_q & 0xffff));

    n_div = *CHAN_IN1(n_div, CH(task_reduce_n_divisor, task_reduce_quotient));

    printf("reduce: quotient: n_div=%x q0=%x\r\n", n_div, q);

    q++;
    do {
        q--;
        qn = (uint32_t)n_div * q;
        printf("reduce: quotient: q=%x qn=%x%x\r\n", q,
              (uint16_t)((qn >> 16) & 0xffff), (uint16_t)(qn & 0xffff));
    } while (qn > n_q);

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
    digit_t m, q, n;
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

    // For calling the print task we need to proxy to it values that
    // we do not modify
    for (i = 0; i < offset; ++i) {
        CHAN_OUT(product[i], 0, CALL_CH(ch_print_product));
    }

    // TODO: could convert the loop into a self-edge
    c = 0;
    for (i = offset; i < 2 * NUM_DIGITS; ++i) {

        // This condition creates the left-shifted zeros.
        // TODO: consider adding number of digits to go along with the 'product' field,
        // then we would not have to zero out the MSDs
        m = c;
        if (i < offset + NUM_DIGITS) {
            n = *CHAN_IN1(N[i - offset],
                          MC_IN_CH(ch_modulus, task_init, task_reduce_multiply));
            m += q * n;
        } else {
            n = 0;
            // TODO: could break out of the loop  in this case (after CHAN_OUT)
        }

        printf("reduce: multiply: n[%u]=%x q=%x c=%x m[%u]=%x\r\n",
               i - offset, n, q, c, i, m);

        c = m >> DIGIT_BITS;
        m &= DIGIT_MASK;

        CHAN_OUT(product[i], m, MC_OUT_CH(ch_qn, task_reduce_multiply,
                                          task_reduce_compare, task_reduce_subtract));

        CHAN_OUT(product[i], m, CALL_CH(ch_print_product));
    }

    CHAN_OUT(next_task, TASK_REF(task_reduce_compare), CALL_CH(ch_print_product));
    TRANSITION_TO(task_print_product);
}

void task_reduce_compare()
{
    int i;
    digit_t m, d, qn;
    char relation = '=';

    blink(1, BLINK_DURATION_TASK, LED2);

    d = *CHAN_IN1(digit, MC_IN_CH(ch_reduce_digit,
                                  task_reduce_quotient, task_reduce_compare));

    printf("reduce: compare: d=%u\r\n", d);

    // TODO: could transform this loop into a self-edge
    // TODO: this loop might not have to go down to zero, but to NUM_DIGITS
    // TODO: consider adding number of digits to go along with the 'product' field
    for (i = NUM_DIGITS * 2 - 1; i >= 0; --i) {
        m = *CHAN_IN3(product[i],
                      MC_IN_CH(ch_product, task_mult, task_reduce_compare),
                      MC_IN_CH(ch_normalized_product, task_reduce_normalize, task_reduce_compare),
                      // TODO: do we need 'ch_reduce_add_product' here? We do not if
                      // the 'task_reduce_subtract' overwrites all values written by
                      // 'task_reduce_add', which, I think, is the case.
                      MC_IN_CH(ch_reduce_subtract_product, task_reduce_subtract,
                               task_reduce_compare));
        qn = *CHAN_IN1(product[i],
                       MC_IN_CH(ch_qn, task_reduce_multiply, task_reduce_compare));

        printf("reduce: compare: m[%u]=%x qn[%u]=%x\r\n", i, m, i, qn);

        if (m > qn) {
            relation = '>';
            break;
        } else if (m < qn) {
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
    int i, j;
    digit_t m, n, c, r;
    unsigned d, offset;

    blink(1, BLINK_DURATION_TASK, LED2);

    d = *CHAN_IN1(digit, MC_IN_CH(ch_reduce_digit,
                                  task_reduce_quotient, task_reduce_compare));

    printf("reduce: add: d=%u\r\n", d);

    // Part of this task is to shift modulus by radix^(digit - NUM_DIGITS)
    offset = d - NUM_DIGITS;

    // For calling the print task we need to proxy to it values that
    // we do not modify
    for (i = 0; i < offset; ++i) {
        CHAN_OUT(product[i], 0, CALL_CH(ch_print_product));
    }

    // For calling the print task we need to proxy to it values that
    // we do not modify
    for (i = 0; i < offset; ++i) {
        CHAN_OUT(product[i], 0, CALL_CH(ch_print_product));
    }

    // TODO: coult transform this loop into a self-edge
    c = 0;
    for (i = offset; i < 2 * NUM_DIGITS; ++i) {
        m = *CHAN_IN3(product[i],
                      MC_IN_CH(ch_product, task_mult, task_reduce_add),
                      MC_IN_CH(ch_normalized_product, task_reduce_normalize, task_reduce_add),
                      MC_IN_CH(ch_reduce_subtract_product,
                               task_reduce_subtract, task_reduce_add));

        // Shifted index of the modulus digit
        j = i - offset;

        if (i < offset + NUM_DIGITS) {
            n = *CHAN_IN1(N[j], MC_IN_CH(ch_modulus, task_init, task_reduce_add));
        } else {
            n = 0;
            j = 0; // a bit ugly, we want 'nan', but ok, since for output only
            // TODO: could break out of the loop in this case (after CHAN_OUT)
        }

        r = c + m + n;

        printf("reduce: add: m[%u]=%x n[%u]=%x c=%x r=%x\r\n", i, m, j, n, c, r);

        c = r >> DIGIT_BITS;
        r &= DIGIT_MASK;

        CHAN_OUT(product[i], r, CH(task_reduce_add, task_reduce_subtract));
        CHAN_OUT(product[i], r, CALL_CH(ch_print_product));
    }

    CHAN_OUT(next_task, TASK_REF(task_reduce_subtract), CALL_CH(ch_print_product));
    TRANSITION_TO(task_print_product);
}

// TODO: re-use task_reduce_normalize?
void task_reduce_subtract()
{
    int i;
    digit_t m, s, r, qn;
    unsigned d, borrow, offset;

    blink(1, BLINK_DURATION_TASK, LED2);

    d = *CHAN_IN1(digit, MC_IN_CH(ch_reduce_digit, task_reduce_quotient,
                                  task_reduce_subtract));

    printf("reduce: subtract: d=%u\r\n", d);


    // The qn product had been shifted by this offset, no need to subtract the zeros
    offset = d - NUM_DIGITS;

    // For calling the print task we need to proxy to it values that
    // we do not modify
    for (i = 0; i < offset; ++i) {
        CHAN_OUT(product[i], 0, CALL_CH(ch_print_product));
    }

    // TODO: could transform this loop into a self-edge
    borrow = 0;
    for (i = offset; i < 2 * NUM_DIGITS; ++i) {
        m = *CHAN_IN4(product[i],
                      MC_IN_CH(ch_product, task_mult, task_reduce_subtract),
                      MC_IN_CH(ch_normalized_product, task_reduce_normalize, task_reduce_subtract),
                      CH(task_reduce_add, task_reduce_subtract),
                      SELF_IN_CH(task_reduce_subtract));
        qn = *CHAN_IN1(product[i],
                       MC_IN_CH(ch_qn, task_reduce_multiply, task_reduce_subtract));

        s = qn + borrow;
        if (m < s) {
            m += 1 << DIGIT_BITS;
            borrow = 1;
        } else {
            borrow = 0;
        }
        r = m - s;

        printf("reduce: subtract: m[%u]=%x qn[%u]=%x b=%u r=%x\r\n",
               i, m, i, qn, borrow, r);

        CHAN_OUT(product[i], r, MC_OUT_CH(ch_reduce_subtract_product, task_reduce_subtract,
                                          task_reduce_quotient, task_reduce_compare));
        CHAN_OUT(product[i], r, SELF_OUT_CH(task_reduce_subtract));
        CHAN_OUT(product[i], r, CALL_CH(ch_print_product));
    }

    if (d > NUM_DIGITS) {
        CHAN_OUT(next_task, TASK_REF(task_reduce_quotient), CALL_CH(ch_print_product));
    } else { // reduction finished
        CHAN_OUT(next_task, TASK_REF(task_init), CALL_CH(ch_print_product));
    }

    TRANSITION_TO(task_print_product);
}

void task_print_product()
{
    int i;
    digit_t m;
    const task_t* next_task;

    printf("print: P=");
    for (i = (NUM_DIGITS * 2) - 1; i >= 0; --i) {
        m = *CHAN_IN1(product[i], CALL_CH(ch_print_product));
        printf("%x ", m);
    }
    printf("\r\n");

    next_task = *CHAN_IN1(next_task, CALL_CH(ch_print_product));
    transition_to(next_task);
}

ENTRY_TASK(task_init)
INIT_FUNC(init)
