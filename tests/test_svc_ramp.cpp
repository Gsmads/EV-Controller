/**
 * @file test_svc_ramp.cpp
 * @brief Тест svc_ramp с drive_profile_t
 *
 * g++ -std=c++11 -I.. -o test_svc_ramp test_svc_ramp.cpp ../svc_ramp.cpp ../util_math.cpp
 */
#include <stdio.h>
#include <string.h>
#include "../firmware/cfg_settings.h"
#include "../firmware/svc_ramp.h"
#include "../firmware/util_math.h"

static int pass = 0, fail = 0;
#define ASSERT_EQ(a,b,m) do{if((a)==(b))pass++;else{printf("  FAIL: %s (exp=%d got=%d)\n",m,(int)(a),(int)(b));fail++;}}while(0)
#define ASSERT_TRUE(c,m) do{if(c)pass++;else{printf("  FAIL: %s\n",m);fail++;}}while(0)

/* Мок-профили */
static drive_profile_t eco_profile = {
    .max_pwm = 400, .accel_rate = 200, .decel_rate = 400,
    .brake_rate_min = 300, .brake_rate_max = 1500,
    .pwm_freq_hz = 7812, .pwm_resolution = 10,
    .direction = 0, .pedal_curve = 0xFF, ._pad = 0
};

static drive_profile_t sport_profile = {
    .max_pwm = 1023, .accel_rate = 800, .decel_rate = 800,
    .brake_rate_min = 500, .brake_rate_max = 2500,
    .pwm_freq_hz = 15625, .pwm_resolution = 9,
    .direction = 0, .pedal_curve = 0xFF, ._pad = 0
};

void test_eco_accel(void) {
    printf("--- test_eco_accel ---\n");
    svc_ramp_init(100);
    /* Eco: accel=200 → 2 PWM/tick. За 100 тиков (1с): 200 */
    for (int i = 0; i < 100; i++) svc_ramp_update(400, 0, &eco_profile);
    ASSERT_EQ(200, svc_ramp_get_current(), "Eco 1s accel → 200");
    /* Ещё 1с → 400 (достигнет max_pwm) */
    for (int i = 0; i < 100; i++) svc_ramp_update(400, 0, &eco_profile);
    ASSERT_EQ(400, svc_ramp_get_current(), "Eco 2s accel → 400");
}

void test_sport_accel(void) {
    printf("--- test_sport_accel ---\n");
    svc_ramp_init(100);
    /* Sport: accel=800 → 8 PWM/tick. За 100 тиков: 800 */
    for (int i = 0; i < 100; i++) svc_ramp_update(1023, 0, &sport_profile);
    ASSERT_EQ(800, svc_ramp_get_current(), "Sport 1s accel → 800");
}

void test_profile_switch(void) {
    printf("--- test_profile_switch ---\n");
    svc_ramp_init(100);
    /* Разгон в eco */
    for (int i = 0; i < 100; i++) svc_ramp_update(400, 0, &eco_profile);
    ASSERT_EQ(200, svc_ramp_get_current(), "Eco 1s = 200");
    /* Торможение до 0 в sport (decel=800 → 8/tick) */
    for (int i = 0; i < 50; i++) svc_ramp_update(0, 0, &sport_profile);
    ASSERT_EQ(0, svc_ramp_get_current(), "Sport decel to 0");
}

void test_brake_with_profile(void) {
    printf("--- test_brake_with_profile ---\n");
    svc_ramp_init(100);
    /* Ставим PWM=300 вручную через update с высокой целью */
    for (int i = 0; i < 150; i++) svc_ramp_update(400, 0, &eco_profile);
    uint16_t before = svc_ramp_get_current();
    ASSERT_TRUE(before >= 300, "PWM >= 300 after 1.5s eco");

    /* Полный тормоз: brake=1023, brake_rate_max=1500 → 15/tick */
    /* 300/15 = 20 тиков до 0 */
    for (int i = 0; i < 30; i++) svc_ramp_update(400, 1023, &eco_profile);
    ASSERT_EQ(0, svc_ramp_get_current(), "Full brake stops");
}

void test_reset(void) {
    printf("--- test_reset ---\n");
    svc_ramp_init(100);
    for (int i = 0; i < 50; i++) svc_ramp_update(400, 0, &eco_profile);
    ASSERT_TRUE(svc_ramp_get_current() > 0, "Non-zero before reset");
    svc_ramp_reset();
    ASSERT_EQ(0, svc_ramp_get_current(), "Zero after reset");
    ASSERT_EQ(1, svc_ramp_is_stopped(), "Stopped after reset");
}

int main(void) {
    printf("=========================================\n");
    printf("  svc_ramp Unit Tests (with profiles)\n");
    printf("=========================================\n\n");
    test_eco_accel();
    test_sport_accel();
    test_profile_switch();
    test_brake_with_profile();
    test_reset();
    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
