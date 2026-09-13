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

/* Профиль Failsafe из умолчаний: разгон запрещён, замедление мягкое */
static drive_profile_t failsafe_profile = {
    .max_pwm = 0, .accel_rate = 0, .decel_rate = 200,
    .brake_rate_min = 200, .brake_rate_max = 200,
    .pwm_freq_hz = 7812, .pwm_resolution = 10,
    .direction = 0, .pedal_curve = 0xFF, ._pad = 0
};

/* Скорость ниже одного шага за тик: 50/с при 100 Гц = 0,5 PWM/тик */
static drive_profile_t subtick_profile = {
    .max_pwm = 1023, .accel_rate = 50, .decel_rate = 50,
    .brake_rate_min = 50, .brake_rate_max = 50,
    .pwm_freq_hz = 7812, .pwm_resolution = 10,
    .direction = 0, .pedal_curve = 0xFF, ._pad = 0
};

/* Скорость, не кратная частоте: 250/с при 100 Гц = 2,5 PWM/тик */
static drive_profile_t fractional_profile = {
    .max_pwm = 1023, .accel_rate = 250, .decel_rate = 250,
    .brake_rate_min = 250, .brake_rate_max = 250,
    .pwm_freq_hz = 7812, .pwm_resolution = 10,
    .direction = 0, .pedal_curve = 0xFF, ._pad = 0
};

/** Разогнать рампу ровно до 300 PWM в eco и оставить аккумулятор пустым.
 *  Eco: 200/с при 100 Гц = ровно 2 PWM/тик, остатка не возникает. */
static void climb_eco_to_300(void) {
    svc_ramp_init(100);
    for (int i = 0; i < 150; i++) svc_ramp_update(400, 0, &eco_profile);
}

/** Сколько тиков до полной остановки при заданных цели и нажатии тормоза.
 *  Предел в 10000 тиков - страховка от зацикливания, а не ожидаемый результат. */
static int ticks_to_stop(uint16_t target, uint16_t brake,
                         const drive_profile_t *p) {
    int t = 0;
    while (svc_ramp_get_current() > 0 && t < 10000) {
        svc_ramp_update(target, brake, p);
        t++;
    }
    return t;
}

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

/* ====================================================================
 *  Аккумулятор: дробные скорости
 *
 *  Рампа задаётся в PWM в секунду, а вызывается 100 раз в секунду.
 *  Всё, что не кратно 100, должно накапливаться, а не теряться.
 * ==================================================================== */

void test_accel_below_one_step_per_tick(void) {
    printf("--- test_accel_below_one_step_per_tick ---\n");
    svc_ramp_init(100);
    /* 50 PWM/с при 100 Гц = 0,5 шага за тик. Наивная целочисленная
       реализация стояла бы на нуле вечно. */
    svc_ramp_update(1023, 0, &subtick_profile);
    ASSERT_EQ(0, svc_ramp_get_current(), "первый тик: полшага ещё не шаг");
    svc_ramp_update(1023, 0, &subtick_profile);
    ASSERT_EQ(1, svc_ramp_get_current(), "второй тик: половинки сложились в шаг");

    for (int i = 2; i < 100; i++) svc_ramp_update(1023, 0, &subtick_profile);
    ASSERT_EQ(50, svc_ramp_get_current(), "за секунду ровно 50 - скорость соблюдена");
}

void test_accel_fraction_does_not_drift(void) {
    printf("--- test_accel_fraction_does_not_drift ---\n");
    svc_ramp_init(100);
    /* 250 PWM/с = 2,5 шага за тик: чередование 2 и 3 без накопления ошибки. */
    for (int i = 0; i < 4; i++) svc_ramp_update(1023, 0, &fractional_profile);
    ASSERT_EQ(10, svc_ramp_get_current(), "4 тика по 2,5 = ровно 10");

    for (int i = 4; i < 100; i++) svc_ramp_update(1023, 0, &fractional_profile);
    ASSERT_EQ(250, svc_ramp_get_current(), "за секунду ровно 250, дрейфа нет");

    for (int i = 0; i < 100; i++) svc_ramp_update(1023, 0, &fractional_profile);
    ASSERT_EQ(500, svc_ramp_get_current(), "за вторую секунду ещё 250");
}

/* ====================================================================
 *  Тормоз: интерполяция скорости по силе нажатия
 * ==================================================================== */

void test_brake_rate_interpolates_between_min_and_max(void) {
    printf("--- test_brake_rate_interpolates_between_min_and_max ---\n");
    /* Eco: brake_rate_min=300, brake_rate_max=1500, диапазон 1200.
       Нажатие 512: 300 + 1200*512/1023 = 300 + 600 = 900/с = 9 PWM/тик.
       С 300 PWM: 33 тика дают 297, остаётся 3; 34-й тик добирает до нуля. */
    climb_eco_to_300();
    ASSERT_EQ(300, svc_ramp_get_current(), "исходная точка 300 PWM");
    for (int i = 0; i < 33; i++) svc_ramp_update(400, 512, &eco_profile);
    ASSERT_EQ(3, svc_ramp_get_current(), "полунажатие: 33 тика по 9 = 297");
    svc_ramp_update(400, 512, &eco_profile);
    ASSERT_EQ(0, svc_ramp_get_current(), "34-й тик добирает остаток");

    /* Нажатие в пол: 1500/с = 15 PWM/тик, 300/15 = ровно 20 тиков. */
    climb_eco_to_300();
    ASSERT_EQ(20, ticks_to_stop(400, 1023, &eco_profile), "в пол: 20 тиков = 0,20 с");

    /* Едва коснулись: 300 + 1200*1/1023 = 301/с. */
    climb_eco_to_300();
    ASSERT_EQ(100, ticks_to_stop(400, 1, &eco_profile), "касание: 100 тиков = 1,00 с");
}

void test_light_brake_is_weaker_than_coasting(void) {
    printf("--- test_light_brake_is_weaker_than_coasting ---\n");
    /* Дефект R-1 (docs/AUDIT.md): во всех ходовых профилях
       brake_rate_min < decel_rate, поэтому лёгкое касание тормоза
       останавливает машину МЕДЛЕННЕЕ, чем если бы ребёнок просто
       отпустил газ. Тест фиксирует поведение как оно есть; решение
       о правке - за владельцем (CLAUDE.md §5). */
    climb_eco_to_300();
    int coasting = ticks_to_stop(0, 0, &eco_profile);    /* газ отпущен, decel_rate = 400 */
    ASSERT_EQ(75, coasting, "отпустить газ: 400/с = 4 PWM/тик, 300/4 = 75 тиков");

    climb_eco_to_300();
    int light_brake = ticks_to_stop(400, 1, &eco_profile);    /* rate = 301 */
    ASSERT_EQ(100, light_brake, "коснуться тормоза: 301/с, 100 тиков");

    ASSERT_TRUE(light_brake > coasting,
                "R-1: нажатый тормоз тормозит слабее отпущенного газа");
}

void test_brake_overrides_target(void) {
    printf("--- test_brake_overrides_target ---\n");
    /* Газ в полу и тормоз одновременно: тормоз главнее, цель становится нулём. */
    climb_eco_to_300();
    for (int i = 0; i < 5; i++) svc_ramp_update(1023, 1023, &eco_profile);
    ASSERT_EQ(225, svc_ramp_get_current(), "5 тиков по 15 вниз, несмотря на газ 1023");
    ASSERT_EQ(0, svc_ramp_is_at_target(),
              "цель под тормозом - ноль, и она ещё не достигнута");

    for (int i = 0; i < 15; i++) svc_ramp_update(1023, 1023, &eco_profile);
    ASSERT_EQ(0, svc_ramp_get_current(), "доехали до нуля");
    ASSERT_EQ(1, svc_ramp_is_stopped(), "остановлено");
    ASSERT_EQ(1, svc_ramp_is_at_target(), "и это и есть цель при нажатом тормозе");
}

/* ====================================================================
 *  Границы ответственности и состояния
 * ==================================================================== */

void test_ramp_does_not_clamp_to_max_pwm(void) {
    printf("--- test_ramp_does_not_clamp_to_max_pwm ---\n");
    /* Контракт: max_pwm применяет svc_pedals_get_target_pwm() до рампы
       (EV_Controller.ino, шаг 3). Рампа обязана доехать до того, что ей
       дали, иначе ограничение окажется в двух местах сразу. */
    svc_ramp_init(100);
    for (int i = 0; i < 500; i++) svc_ramp_update(900, 0, &eco_profile);
    ASSERT_EQ(900, svc_ramp_get_current(),
              "рампа едет до заданной цели, max_pwm профиля ей не указ");
    ASSERT_EQ(400, eco_profile.max_pwm, "при этом max_pwm профиля меньше цели");
}

void test_is_at_target(void) {
    printf("--- test_is_at_target ---\n");
    svc_ramp_init(100);
    ASSERT_EQ(1, svc_ramp_is_at_target(), "после init ноль равен нулевой цели");

    svc_ramp_update(400, 0, &eco_profile);
    ASSERT_EQ(0, svc_ramp_is_at_target(), "поехали - цель ещё впереди");

    for (int i = 1; i < 200; i++) svc_ramp_update(400, 0, &eco_profile);
    ASSERT_EQ(400, svc_ramp_get_current(), "доехали");
    ASSERT_EQ(1, svc_ramp_is_at_target(), "цель достигнута");
    ASSERT_EQ(0, svc_ramp_is_stopped(), "но это не остановка");
}

void test_failsafe_cannot_accelerate(void) {
    printf("--- test_failsafe_cannot_accelerate ---\n");
    /* accel_rate = 0 в профиле Failsafe - это обещание: при потере связи
       машина не тронется, сколько бы газа ни просили. */
    svc_ramp_init(100);
    for (int i = 0; i < 1000; i++) svc_ramp_update(1023, 0, &failsafe_profile);
    ASSERT_EQ(0, svc_ramp_get_current(), "10 секунд полного газа - ни одного шага");
    ASSERT_EQ(1, svc_ramp_is_stopped(), "стоит");
}

void test_failsafe_decel_is_soft(void) {
    printf("--- test_failsafe_decel_is_soft ---\n");
    /* Машина уже едет, связь потеряна: 200/с = 2 PWM/тик, с 300 это 150
       тиков = 1,5 с мягкого замедления вместо рывка. */
    climb_eco_to_300();
    int t = 0;
    while (svc_ramp_get_current() > 0 && t < 10000) {
        svc_ramp_update(0, 0, &failsafe_profile);
        t++;
    }
    ASSERT_EQ(150, t, "замедление до нуля за 150 тиков = 1,5 с");
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
    test_accel_below_one_step_per_tick();
    test_accel_fraction_does_not_drift();
    test_brake_rate_interpolates_between_min_and_max();
    test_light_brake_is_weaker_than_coasting();
    test_brake_overrides_target();
    test_ramp_does_not_clamp_to_max_pwm();
    test_is_at_target();
    test_failsafe_cannot_accelerate();
    test_failsafe_decel_is_soft();
    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
