/**
 * @file test_util_math.c
 * @brief Юнит-тест утилит util_math
 *
 * Компиляция: gcc -std=c11 -o test_util_math test_util_math.c ../util/util_math.c
 * Запуск:     ./test_util_math
 */
#include <stdio.h>
#include <stdlib.h>
#include "../firmware/util_math.h"

static int pass = 0, fail = 0;
#define ASSERT(cond, msg) do { if (cond) pass++; else { printf("  FAIL: %s\n", msg); fail++; } } while(0)
#define ASSERT_EQ(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%d got=%d)\n", msg, (int)(a), (int)(b)); fail++; } } while(0)
#define ASSERT_RANGE(v, lo, hi, msg) do { if ((v)>=(lo)&&(v)<=(hi)) pass++; else { printf("  FAIL: %s (%d not in [%d..%d])\n", msg,(int)(v),(int)(lo),(int)(hi)); fail++; } } while(0)

/* ==== map / clamp ==== */
void test_map(void) {
    printf("--- test_map ---\n");
    ASSERT_EQ(511, util_map_u16(500, 0, 1000, 0, 1023), "midpoint map (integer: 500/1000*1023=511)");
    ASSERT_EQ(0,   util_map_u16(0, 10, 1000, 0, 1023), "below min clamps to out_min");
    ASSERT_EQ(1023,util_map_u16(1023, 10, 1000, 0, 1023), "above max clamps to out_max");
    ASSERT_EQ(0,   util_map_u16(500, 500, 500, 0, 1023), "zero range → out_min");
}

void test_clamp(void) {
    printf("--- test_clamp ---\n");
    ASSERT_EQ(50,  util_clamp_u16(50, 0, 100), "in range");
    ASSERT_EQ(0,   util_clamp_u16(0, 0, 100), "at min");
    ASSERT_EQ(100, util_clamp_u16(200, 0, 100), "above max");
    ASSERT_EQ(-50, util_clamp_i16(-50, -100, 100), "signed in range");
    ASSERT_EQ(-100,util_clamp_i16(-200, -100, 100), "signed below min");
}

/* ==== EMA filter ==== */
void test_ema(void) {
    printf("--- test_ema ---\n");
    int32_t f = util_ema_init(0);
    /* Подаём постоянный 1000 */
    for (int i = 0; i < 300; i++) f = util_ema_update(f, 1000, 40);
    ASSERT_RANGE(util_ema_extract(f), 998, 1002, "EMA converges to constant");

    /* Шаг от 0 к 500: после 1 итерации ~78 (alpha=40 → ~15.6%) */
    f = util_ema_init(0);
    f = util_ema_update(f, 500, 40);
    uint16_t v = util_ema_extract(f);
    ASSERT_RANGE(v, 70, 90, "EMA first step ~15% of input");
}

/* ==== Response curves ==== */
void test_curves(void) {
    printf("--- test_curves ---\n");
    /* Linear */
    ASSERT_EQ(512, util_curve_apply(512, CURVE_LINEAR), "linear passthrough");
    ASSERT_EQ(0,   util_curve_apply(0, CURVE_LINEAR), "linear zero");
    ASSERT_EQ(1023,util_curve_apply(1023, CURVE_LINEAR), "linear max");

    /* Quadratic: at half input, output ≈ 25% */
    uint16_t q = util_curve_apply(512, CURVE_QUADRATIC);
    ASSERT_RANGE(q, 245, 265, "quadratic half → ~25%");
    ASSERT_EQ(1023, util_curve_apply(1023, CURVE_QUADRATIC), "quadratic max=max");
    ASSERT_EQ(0,    util_curve_apply(0, CURVE_QUADRATIC), "quadratic zero=zero");

    /* S-curve: at midpoint, output ≈ 50% (inflection point) */
    uint16_t s = util_curve_apply(512, CURVE_S_CURVE);
    ASSERT_RANGE(s, 490, 530, "s-curve midpoint ≈ 50%");
    ASSERT_EQ(1023, util_curve_apply(1023, CURVE_S_CURVE), "s-curve max=max");
    ASSERT_EQ(0,    util_curve_apply(0, CURVE_S_CURVE), "s-curve zero=zero");

    /* S-curve quarter: should be < 50% (slow start) */
    uint16_t sq = util_curve_apply(256, CURVE_S_CURVE);
    ASSERT(sq < 256, "s-curve quarter < linear (slow start)");
}

/* ==== PID controller ==== */
void test_pid(void) {
    printf("--- test_pid ---\n");
    pid_state_t pid;
    util_pid_init(&pid, 100, 0, 0, -1023, 1023);  /* Kp=1.00, no I/D */

    /* Pure P: output = kp * error / 100 = 100 * 500 / 100 = 500 */
    int16_t out = util_pid_update(&pid, 500);
    ASSERT_EQ(500, out, "pure P: error=500 → out=500");

    /* Clamping */
    out = util_pid_update(&pid, 2000);
    ASSERT_EQ(1023, out, "P clamped to max");

    /* Negative */
    util_pid_reset(&pid);
    out = util_pid_update(&pid, -300);
    ASSERT_EQ(-300, out, "P negative");

    /* PID with I */
    util_pid_init(&pid, 100, 50, 0, -1023, 1023);  /* Kp=1.0, Ki=0.5 */
    util_pid_reset(&pid);
    out = util_pid_update(&pid, 100);  /* P=100, I=50*100/100=50 → 150 */
    ASSERT_EQ(150, out, "P+I first step");

    out = util_pid_update(&pid, 100);  /* P=100, I=50*200/100=100 → 200 */
    ASSERT_EQ(200, out, "P+I second step (integral accumulates)");

    /* Reset clears integral */
    util_pid_reset(&pid);
    out = util_pid_update(&pid, 100);
    ASSERT_EQ(150, out, "P+I after reset = first step again");

    /* PID with D */
    util_pid_init(&pid, 0, 0, 100, -1023, 1023);  /* Only Kd=1.0 */
    util_pid_reset(&pid);
    out = util_pid_update(&pid, 100);  /* D = 100*(100-0)/100 = 100 */
    ASSERT_EQ(100, out, "pure D: first step");
    out = util_pid_update(&pid, 100);  /* D = 100*(100-100)/100 = 0 */
    ASSERT_EQ(0, out, "pure D: no change → 0");
    out = util_pid_update(&pid, 50);   /* D = 100*(50-100)/100 = -50 */
    ASSERT_EQ(-50, out, "pure D: decrease → negative");
}

/* ==== Ramp accumulator ==== */
void test_ramp_accum(void) {
    printf("--- test_ramp_accum ---\n");
    ramp_accum_t ra;
    util_ramp_accum_init(&ra, 100);

    /* rate=300, freq=100 → 3 steps per tick */
    uint16_t s = util_ramp_accum_step(&ra, 300);
    ASSERT_EQ(3, s, "rate 300 @ 100Hz = 3/tick");

    /* rate=150 → alternates 1 and 2 */
    util_ramp_accum_reset(&ra);
    uint32_t total = 0;
    for (int i = 0; i < 200; i++) {
        total += util_ramp_accum_step(&ra, 150);
    }
    ASSERT_EQ(300, total, "rate 150 for 2s = exactly 300");

    /* rate=1 → 1 step every 100 ticks */
    util_ramp_accum_reset(&ra);
    total = 0;
    for (int i = 0; i < 100; i++) {
        total += util_ramp_accum_step(&ra, 1);
    }
    ASSERT_EQ(1, total, "rate 1 for 1s = 1 step");
}

int main(void) {
    printf("=========================================\n");
    printf("  util_math Unit Tests\n");
    printf("=========================================\n\n");
    test_map();
    test_clamp();
    test_ema();
    test_curves();
    test_pid();
    test_ramp_accum();
    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
