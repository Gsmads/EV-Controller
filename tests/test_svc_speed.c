/**
 * @file test_svc_speed.c
 * @brief Юнит-тест расчёта скорости (дефект B-3 docs/AUDIT.md)
 *
 * Модуль до этого теста не был покрыт ничем — и содержал формулу,
 * занижавшую скорость в 3,65 раза. Комментарий над ней содержал
 * «проверку», подтверждавшую неверный результат совпадением с числом π.
 * Поэтому здесь только эталонные точки, посчитанные вручную от физики,
 * а не от кода.
 *
 * Физика: km/h = RPM × 60 × π × D_мм / 1 000 000
 *
 * Компиляция: см. tests/Makefile
 */
#include <stdio.h>
#include <string.h>
#include "../firmware/svc_speed.h"
#include "../firmware/cfg_board.h"
#include "../firmware/hal_encoder.h"

static int pass = 0, fail = 0;
#define ASSERT_EQ(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%d got=%d)\n", msg, (int)(a), (int)(b)); fail++; } } while(0)

/* ==== Мок энкодеров ==== */
static uint16_t mock_pulses[ENCODER_COUNT];

uint16_t hal_encoder_read_and_reset(encoder_channel_t ch) {
    if (ch >= ENCODER_COUNT) return 0;
    uint16_t v = mock_pulses[ch];
    mock_pulses[ch] = 0;
    return v;
}
uint16_t hal_encoder_get_count(encoder_channel_t ch) {
    return (ch < ENCODER_COUNT) ? mock_pulses[ch] : 0;
}
void hal_encoder_init(void) {}

/** Подать одинаковое число импульсов на оба колеса и обновить расчёт. */
static void feed(uint16_t pulses, uint8_t freq_hz) {
    mock_pulses[ENCODER_RIGHT] = pulses;
    mock_pulses[ENCODER_LEFT]  = pulses;
    svc_speed_update(freq_hz);
}

/* ====================================================================
 *  RPM
 * ==================================================================== */

void test_rpm_reference_points(void) {
    printf("--- test_rpm_reference_points ---\n");
    /* RPM = импульсы × частота × 60 / импульсов_на_оборот
       При ENCODER_PULSES_PER_REV = 12 и частоте 10 Гц: RPM = импульсы × 50 */
    svc_speed_init();

    feed(0, 10);
    ASSERT_EQ(0, svc_speed_get_rpm(SPEED_WHEEL_RIGHT), "нет импульсов — нет оборотов");

    feed(6, 10);
    ASSERT_EQ(300, svc_speed_get_rpm(SPEED_WHEEL_RIGHT), "6 имп/тик при 10 Гц и 12 имп/об = 300 RPM");
    ASSERT_EQ(300, svc_speed_get_rpm(SPEED_WHEEL_LEFT),  "второе колесо так же");

    feed(12, 10);
    ASSERT_EQ(600, svc_speed_get_rpm(SPEED_WHEEL_RIGHT), "12 импульсов — вдвое больше");

    feed(6, 20);
    ASSERT_EQ(600, svc_speed_get_rpm(SPEED_WHEEL_RIGHT), "та же частота импульсов при 20 Гц — вдвое больше RPM");
}

/* ====================================================================
 *  B-3: километры в час
 * ==================================================================== */

void test_kmh_reference_points(void) {
    printf("--- test_kmh_reference_points ---\n");
    /*
     * Эталон посчитан от физики, не от кода. D = 200 мм.
     *
     *   RPM    точное значение         kmh_x10
     *     0    0,0000 км/ч                   0
     *    50    1,8850 км/ч = 18,850         19
     *   300   11,3097 км/ч = 113,097       113
     *   600   22,6195 км/ч = 226,195       226
     *  1200   45,2389 км/ч = 452,389       452
     *
     * Прежняя формула на RPM = 300 давала 31 — занижение в 3,65 раза.
     */
    svc_speed_init();

    feed(0, 10);
    ASSERT_EQ(0, svc_speed_get_kmh_x10(), "стоим — ноль");

    feed(1, 10);   /* RPM = 50 */
    ASSERT_EQ(19, svc_speed_get_kmh_x10(), "RPM 50 -> 1,885 км/ч -> 19 (округление, не 18)");

    feed(6, 10);   /* RPM = 300 */
    ASSERT_EQ(113, svc_speed_get_kmh_x10(), "B-3: RPM 300 -> 11,31 км/ч -> 113, а не 31");

    feed(12, 10);  /* RPM = 600 */
    ASSERT_EQ(226, svc_speed_get_kmh_x10(), "RPM 600 -> 22,62 км/ч -> 226");

    feed(24, 10);  /* RPM = 1200 */
    ASSERT_EQ(452, svc_speed_get_kmh_x10(), "RPM 1200 -> 45,24 км/ч -> 452");
}

void test_kmh_is_linear_in_rpm(void) {
    printf("--- test_kmh_is_linear_in_rpm ---\n");
    /* Удвоение оборотов удваивает скорость: проверка, что коэффициент
       не зависит от величины, то есть формула линейна, а не подогнана. */
    svc_speed_init();
    feed(3, 10);
    uint16_t a = svc_speed_get_kmh_x10();
    feed(6, 10);
    uint16_t b = svc_speed_get_kmh_x10();
    feed(12, 10);
    uint16_t c = svc_speed_get_kmh_x10();
    ASSERT_EQ(1, (b >= 2*a - 1 && b <= 2*a + 1), "удвоение RPM удваивает скорость");
    ASSERT_EQ(1, (c >= 2*b - 1 && c <= 2*b + 1), "и ещё раз");
}

/* ====================================================================
 *  Одометр
 * ==================================================================== */

void test_odometer(void) {
    printf("--- test_odometer ---\n");
    /*
     * Полоборота каждым колесом: 6 импульсов при 12 импульсах на оборот.
     * Физика: 0,5 × π × 200 = 314,159 мм.
     * Код считает в целых миллиметрах через коэффициент ×1000 и даёт 313 —
     * потеря 1 мм на усечении, 0,3 %. Для одометра это приемлемо, но
     * зафиксировано числом, а не словом «примерно».
     */
    svc_speed_init();
    svc_speed_reset_odometer();

    for (int i = 0; i < 1000; i++) feed(6, 10);   /* 500 оборотов колеса */
    /* 1000 × 313 мм = 313000 мм = 313 м */
    ASSERT_EQ(313, svc_speed_get_odometer_m(), "1000 тиков по полоборота = 313 м");

    svc_speed_reset_odometer();
    ASSERT_EQ(0, svc_speed_get_odometer_m(), "сброс обнуляет одометр");
}

void test_is_stopped(void) {
    printf("--- test_is_stopped ---\n");
    svc_speed_init();
    feed(0, 10);
    ASSERT_EQ(1, svc_speed_is_stopped(), "без импульсов — стоим");
    feed(6, 10);
    ASSERT_EQ(0, svc_speed_is_stopped(), "есть импульсы — едем");
    feed(0, 10);
    ASSERT_EQ(1, svc_speed_is_stopped(), "импульсы кончились — снова стоим");
}

void test_zero_frequency_guarded(void) {
    printf("--- test_zero_frequency_guarded ---\n");
    svc_speed_init();
    feed(6, 10);
    uint16_t before = svc_speed_get_kmh_x10();
    feed(6, 0);                                  /* деление на ноль */
    ASSERT_EQ(before, svc_speed_get_kmh_x10(), "нулевая частота не меняет расчёт и не роняет");
}

int main(void) {
    printf("=========================================\n");
    printf("  svc_speed Unit Tests\n");
    printf("=========================================\n\n");

    test_rpm_reference_points();
    test_kmh_reference_points();
    test_kmh_is_linear_in_rpm();
    test_odometer();
    test_is_stopped();
    test_zero_frequency_guarded();

    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
