/**
 * @file test_svc_speed.c
 * @brief Юнит-тест расчёта скорости по времени между импульсами (ADR-0023)
 *
 * Модуль переписан: счёт импульсов за окно как способ измерения скорости
 * удалён, остался только период между импульсами. Тесты переписаны вместе
 * с ним, но эталонные точки те же самые — физика не изменилась. Точка
 * «300 об/мин = 11,3 км/ч», которой ловился дефект B-3, проверяется и
 * здесь, просто подаётся она теперь периодом, а не количеством импульсов.
 *
 * Все ожидаемые значения посчитаны вручную от физики:
 *   об/мин  = 60 000 000 / (период_мкс × импульсов_на_оборот)
 *   км/ч    = об/мин × π × D_мм × 60 / 1 000 000
 *
 * Компиляция: см. tests/Makefile
 */
#include <stdio.h>
#include <string.h>
#include "../firmware/svc_speed.h"
#include "../firmware/cfg_board.h"
#include "../firmware/hal_encoder.h"
#include "../firmware/cfg_settings.h"

static int pass = 0, fail = 0;
#define ASSERT_EQ(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%d got=%d)\n", msg, (int)(a), (int)(b)); fail++; } } while(0)
#define ASSERT_EQ32(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%lu got=%lu)\n", msg, (unsigned long)(a), (unsigned long)(b)); fail++; } } while(0)

/* ==== Моки HAL ==== */

static uint32_t         mock_now_us;
static encoder_sample_t mock_enc[ENCODER_COUNT];
static uint16_t         mock_glitch[ENCODER_COUNT];

uint32_t hal_system_micros(void) { return mock_now_us; }
void     hal_encoder_init(void)  {}

void hal_encoder_take(encoder_channel_t ch, encoder_sample_t *out) {
    if (ch >= ENCODER_COUNT) { memset(out, 0, sizeof(*out)); return; }
    *out = mock_enc[ch];
    mock_enc[ch].pulses = 0;       /* как в железе: счётчик обнуляется чтением */
}

uint16_t hal_encoder_glitch_count(encoder_channel_t ch) {
    return (ch < ENCODER_COUNT) ? mock_glitch[ch] : 0;
}

/* Мок EEPROM — cfg_settings нужен: геометрия колеса живёт в настройках */
static uint8_t mock_eeprom[1024];
void hal_nvm_read(uint16_t addr, uint8_t *buf, uint16_t len) {
    if (addr + len <= sizeof(mock_eeprom)) memcpy(buf, mock_eeprom + addr, len);
}
void hal_nvm_write(uint16_t addr, const uint8_t *buf, uint16_t len) {
    if (addr + len <= sizeof(mock_eeprom)) memcpy(mock_eeprom + addr, buf, len);
}
uint8_t hal_nvm_read_byte(uint16_t addr) {
    return (addr < sizeof(mock_eeprom)) ? mock_eeprom[addr] : 0xFF;
}
void hal_nvm_write_byte(uint16_t addr, uint8_t d) {
    if (addr < sizeof(mock_eeprom)) mock_eeprom[addr] = d;
}

/* ==== Помощники ==== */

static encoder_channel_t chan(speed_wheel_t w) {
    return (w == SPEED_WHEEL_LEFT) ? ENCODER_LEFT : ENCODER_RIGHT;
}

static void setup_clean(void) {
    memset(mock_enc, 0, sizeof(mock_enc));
    memset(mock_glitch, 0, sizeof(mock_glitch));
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));
    mock_now_us = 1000000;
    cfg_settings_init();
    svc_speed_init();
}

/** Колесо крутится с заданным периодом, последний импульс только что. */
static void wheel_spins(speed_wheel_t w, uint32_t period_us, uint16_t pulses) {
    encoder_sample_t *e = &mock_enc[chan(w)];
    e->period_us     = period_us;
    e->last_pulse_us = mock_now_us;
    e->pulses        = pulses;
    e->has_period    = 1;
}

/** Оба колеса с одним периодом. */
static void both_spin(uint32_t period_us) {
    wheel_spins(SPEED_WHEEL_LEFT,  period_us, 0);
    wheel_spins(SPEED_WHEEL_RIGHT, period_us, 0);
    svc_speed_update();
}

/* ====================================================================
 *  Три состояния вместо одного нуля
 * ==================================================================== */

void test_no_data_before_any_pulse(void) {
    printf("--- test_no_data_before_any_pulse ---\n");
    setup_clean();
    svc_speed_update();
    ASSERT_EQ(SPEED_NO_DATA, svc_speed_get_state(SPEED_WHEEL_LEFT),
              "до первого импульса состояние NO_DATA, а не STOPPED");
    ASSERT_EQ(SPEED_NO_DATA, svc_speed_get_state(SPEED_WHEEL_RIGHT), "и второе колесо");
    ASSERT_EQ(0, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "оборотов нет");
    ASSERT_EQ(0, svc_speed_get_kmh_x10(), "скорости нет");
    ASSERT_EQ(1, svc_speed_is_stopped(), "машина считается стоящей");
}

void test_stopped_is_not_no_data(void) {
    printf("--- test_stopped_is_not_no_data ---\n");
    setup_clean();
    both_spin(12500);
    ASSERT_EQ(SPEED_MOVING, svc_speed_get_state(SPEED_WHEEL_LEFT), "поехали");

    /* Импульсы кончились: прошло больше порога остановки */
    mock_now_us += ENCODER_STOP_TIMEOUT_US + 1;
    svc_speed_update();
    ASSERT_EQ(SPEED_STOPPED, svc_speed_get_state(SPEED_WHEEL_LEFT),
              "колесо встало — это STOPPED, а не NO_DATA: измерения были");
    ASSERT_EQ(0, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "оборотов нет");
    ASSERT_EQ(0, svc_speed_get_kmh_x10(), "скорость ноль");
    ASSERT_EQ(1, svc_speed_is_stopped(), "машина стоит");
}

/* ====================================================================
 *  Эталонные точки: период -> обороты
 * ==================================================================== */

void test_rpm_reference_points(void) {
    printf("--- test_rpm_reference_points ---\n");
    setup_clean();

    /* 60 000 000 / (12500 × 12) = 400 ровно */
    both_spin(12500);
    ASSERT_EQ(400, svc_speed_get_rpm(SPEED_WHEEL_LEFT),  "период 12500 мкс = 400 об/мин");
    ASSERT_EQ(400, svc_speed_get_rpm(SPEED_WHEEL_RIGHT), "второе колесо так же");

    /* вдвое длиннее период — вдвое меньше оборотов */
    both_spin(25000);
    ASSERT_EQ(200, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "период 25000 мкс = 200 об/мин");

    /* вдвое короче — вдвое больше */
    both_spin(6250);
    ASSERT_EQ(800, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "период 6250 мкс = 800 об/мин");

    /* 60 000 000 / (100000 × 12) = 50 */
    both_spin(100000);
    ASSERT_EQ(50, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "период 100 мс = 50 об/мин");
}

/* ====================================================================
 *  Эталонные точки: период -> км/ч
 * ==================================================================== */

void test_kmh_reference_points(void) {
    printf("--- test_kmh_reference_points ---\n");
    setup_clean();

    /* 400 об/мин × π × 200 мм × 60 / 1e6 = 15,0796 км/ч */
    both_spin(12500);
    ASSERT_EQ(151, svc_speed_get_kmh_x10(), "400 об/мин при D=200 = 15,1 км/ч");

    /* 200 об/мин = 7,5398 км/ч */
    both_spin(25000);
    ASSERT_EQ(75, svc_speed_get_kmh_x10(), "200 об/мин = 7,5 км/ч");

    /* 800 об/мин = 30,1593 км/ч */
    both_spin(6250);
    ASSERT_EQ(302, svc_speed_get_kmh_x10(), "800 об/мин = 30,2 км/ч");
}

void test_b3_reference_point_survives_method_change(void) {
    printf("--- test_b3_reference_point_survives_method_change ---\n");
    /* Точка, которой ловился дефект B-3: 300 об/мин при D = 200 мм —
       это 11,3 км/ч, а прежняя формула давала 3,1. Способ измерения
       сменился, физика нет: подаём те же 300 об/мин периодом.
       60 000 000 / (300 × 12) = 16666,7 мкс. */
    setup_clean();
    both_spin(16667);
    ASSERT_EQ(300, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "период 16667 мкс = 300 об/мин");
    ASSERT_EQ(113, svc_speed_get_kmh_x10(),
              "300 об/мин при D=200 = 11,3 км/ч — та же точка, что ловила B-3");
}

void test_resolution_beats_pulse_counting(void) {
    printf("--- test_resolution_beats_pulse_counting ---\n");
    /* Прежний способ при 12 имп/об и такте 10 Гц имел квант 50 об/мин:
       400 и 410 об/мин он показывал одинаково. Разница 10 об/мин — это
       0,38 км/ч, и она обязана быть видна. */
    setup_clean();
    both_spin(12500);                       /* 400 об/мин */
    uint16_t at_400 = svc_speed_get_kmh_x10();
    both_spin(12195);                       /* 410 об/мин */
    uint16_t at_410 = svc_speed_get_kmh_x10();

    ASSERT_EQ(400, svc_speed_get_rpm(SPEED_WHEEL_LEFT) - 10, "второе измерение — 410 об/мин");
    ASSERT_EQ(151, at_400, "400 об/мин = 15,1 км/ч");
    ASSERT_EQ(155, at_410, "410 об/мин = 15,5 км/ч");
    ASSERT_EQ(1, at_410 > at_400, "десять оборотов разницы видны, а не съедены квантом");
}

/* ====================================================================
 *  Возраст измерения: показание не должно замирать
 * ==================================================================== */

void test_speed_decays_as_pulses_stop_coming(void) {
    printf("--- test_speed_decays_as_pulses_stop_coming ---\n");
    setup_clean();
    both_spin(12500);
    ASSERT_EQ(151, svc_speed_get_kmh_x10(), "едем 15,1 км/ч");

    /* Импульсов больше нет. Раз последний был 50 мс назад, настоящий
       период уже не меньше 50 мс — значит не быстрее 100 об/мин. */
    mock_now_us += 50000;
    svc_speed_update();
    ASSERT_EQ(100, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "возраст 50 мс = не быстрее 100 об/мин");
    ASSERT_EQ(38, svc_speed_get_kmh_x10(), "показание упало до 3,8 км/ч, а не замерло на 15,1");

    mock_now_us += 150000;                   /* всего 200 мс */
    svc_speed_update();
    ASSERT_EQ(25, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "возраст 200 мс = 25 об/мин");
    ASSERT_EQ(9, svc_speed_get_kmh_x10(), "0,9 км/ч");
}

void test_stop_timeout_is_the_measurement_floor(void) {
    printf("--- test_stop_timeout_is_the_measurement_floor ---\n");
    setup_clean();
    both_spin(12500);

    /* На один такт до порога ещё меряем: 60e6/(499999×12) = 10 об/мин */
    mock_now_us += ENCODER_STOP_TIMEOUT_US - 1;
    svc_speed_update();
    ASSERT_EQ(SPEED_MOVING, svc_speed_get_state(SPEED_WHEEL_LEFT), "до порога ещё движение");
    ASSERT_EQ(10, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "нижняя измеримая граница — 10 об/мин");
    ASSERT_EQ(4, svc_speed_get_kmh_x10(), "то есть 0,4 км/ч");

    /* Ровно на пороге — уже остановка */
    setup_clean();
    both_spin(12500);
    mock_now_us += ENCODER_STOP_TIMEOUT_US;
    svc_speed_update();
    ASSERT_EQ(SPEED_STOPPED, svc_speed_get_state(SPEED_WHEEL_LEFT),
              "на пороге показываем остановку, а не выдуманное маленькое число");
}

void test_first_pulse_after_a_stop_is_not_a_speed(void) {
    printf("--- test_first_pulse_after_a_stop_is_not_a_speed ---\n");
    /* Машина постояла и тронулась. Первый период охватывает весь простой,
       и скоростью он не является. Отдельного правила для этого нет —
       работает то же сравнение с порогом. */
    setup_clean();
    both_spin(12500);
    mock_now_us += 3000000;                  /* три секунды стоим */
    svc_speed_update();
    ASSERT_EQ(SPEED_STOPPED, svc_speed_get_state(SPEED_WHEEL_LEFT), "стоим");

    /* Пришёл импульс: период между ним и предыдущим — все три секунды */
    wheel_spins(SPEED_WHEEL_LEFT,  3000000, 1);
    wheel_spins(SPEED_WHEEL_RIGHT, 3000000, 1);
    svc_speed_update();
    ASSERT_EQ(SPEED_STOPPED, svc_speed_get_state(SPEED_WHEEL_LEFT),
              "первый импульс после простоя скоростью не считается");

    /* А вот второй уже даёт настоящий период */
    both_spin(12500);
    ASSERT_EQ(SPEED_MOVING, svc_speed_get_state(SPEED_WHEEL_LEFT), "второй импульс — поехали");
    ASSERT_EQ(151, svc_speed_get_kmh_x10(), "15,1 км/ч");
}

/* ====================================================================
 *  Два колеса
 * ==================================================================== */

void test_turning_averages_wheel_speeds(void) {
    printf("--- test_turning_averages_wheel_speeds ---\n");
    setup_clean();
    /* Внешнее колесо 15,08 км/ч, внутреннее 7,54 — среднее 11,31 */
    wheel_spins(SPEED_WHEEL_LEFT,  12500, 0);
    wheel_spins(SPEED_WHEEL_RIGHT, 25000, 0);
    svc_speed_update();
    ASSERT_EQ(400, svc_speed_get_rpm(SPEED_WHEEL_LEFT),  "внешнее 400 об/мин");
    ASSERT_EQ(200, svc_speed_get_rpm(SPEED_WHEEL_RIGHT), "внутреннее 200 об/мин");
    ASSERT_EQ(113, svc_speed_get_kmh_x10(),
              "усредняются скорости, а не периоды: (15,08 + 7,54)/2 = 11,3");
}

void test_one_silent_wheel_does_not_halve_speed(void) {
    printf("--- test_one_silent_wheel_does_not_halve_speed ---\n");
    setup_clean();
    /* Правый датчик не подключён, левое колесо едет 15,1 км/ч */
    wheel_spins(SPEED_WHEEL_LEFT, 12500, 0);
    svc_speed_update();
    ASSERT_EQ(SPEED_MOVING,  svc_speed_get_state(SPEED_WHEEL_LEFT),  "левое едет");
    ASSERT_EQ(SPEED_NO_DATA, svc_speed_get_state(SPEED_WHEEL_RIGHT), "правое молчит");
    ASSERT_EQ(151, svc_speed_get_kmh_x10(),
              "молчащее колесо в среднее не входит — иначе скорость занизилась бы вдвое");
    ASSERT_EQ(0, svc_speed_is_stopped(), "машина не стоит: одно колесо крутится");
}

/* ====================================================================
 *  Одометр: путь считается импульсами
 * ==================================================================== */

void test_odometer_counts_pulses(void) {
    printf("--- test_odometer_counts_pulses ---\n");
    setup_clean();
    /* π × 200 мм = 628,3 мм за оборот; 16 оборотов = 10053 мм = 10 м */
    for (int rev = 0; rev < 16; rev++) {
        wheel_spins(SPEED_WHEEL_LEFT,  12500, ENCODER_PULSES_PER_REV);
        wheel_spins(SPEED_WHEEL_RIGHT, 12500, ENCODER_PULSES_PER_REV);
        svc_speed_update();
    }
    ASSERT_EQ32(10UL, svc_speed_get_odometer_m(),
                "16 оборотов колеса D=200 мм = 10,05 м");
}

void test_odometer_keeps_the_remainder(void) {
    printf("--- test_odometer_keeps_the_remainder ---\n");
    /* Один импульс — это 52,360 мм. Три импульса на колесо дают 157,08 мм.
       Если остаток от деления терять на каждом вызове, получится 156. */
    setup_clean();
    for (int i = 0; i < 3; i++) {
        wheel_spins(SPEED_WHEEL_LEFT,  12500, 1);
        wheel_spins(SPEED_WHEEL_RIGHT, 12500, 1);
        svc_speed_update();
    }
    /* Метры ещё не набежали, но миллиметры внутри — проверяем через метры
       после добора до целого: доводим до 20 импульсов, это 1047,2 мм */
    for (int i = 3; i < 20; i++) {
        wheel_spins(SPEED_WHEEL_LEFT,  12500, 1);
        wheel_spins(SPEED_WHEEL_RIGHT, 12500, 1);
        svc_speed_update();
    }
    ASSERT_EQ32(1UL, svc_speed_get_odometer_m(),
                "20 импульсов = 1047 мм = 1 м; при потере остатка было бы 1040 мм");
}

void test_odometer_reset(void) {
    printf("--- test_odometer_reset ---\n");
    setup_clean();
    for (int rev = 0; rev < 16; rev++) {
        wheel_spins(SPEED_WHEEL_LEFT,  12500, ENCODER_PULSES_PER_REV);
        wheel_spins(SPEED_WHEEL_RIGHT, 12500, ENCODER_PULSES_PER_REV);
        svc_speed_update();
    }
    ASSERT_EQ(1, svc_speed_get_odometer_m() > 0, "путь накоплен");
    svc_speed_reset_odometer();
    ASSERT_EQ32(0UL, svc_speed_get_odometer_m(), "сброшен");
}

/* ====================================================================
 *  Край: переполнение микросекундного счётчика
 * ==================================================================== */

void test_survives_micros_overflow(void) {
    printf("--- test_survives_micros_overflow ---\n");
    /* micros() переполняется примерно раз в 71,6 минуты. Разность двух
       беззнаковых меток через переполнение остаётся верной, и на этом
       всё держится. Последний импульс до переполнения, обновление после. */
    setup_clean();
    mock_now_us = 0xFFFFF000UL;
    wheel_spins(SPEED_WHEEL_LEFT,  12500, 0);
    wheel_spins(SPEED_WHEEL_RIGHT, 12500, 0);

    mock_now_us = 0x00001000UL;          /* 8192 мкс спустя, счётчик перевернулся */
    svc_speed_update();
    ASSERT_EQ(SPEED_MOVING, svc_speed_get_state(SPEED_WHEEL_LEFT),
              "переполнение не выглядит как остановка");
    ASSERT_EQ(400, svc_speed_get_rpm(SPEED_WHEEL_LEFT),
              "возраст 8192 мкс меньше периода 12500 — скорость прежняя");
}

/* ====================================================================
 *  Отброшенные фронты видны
 * ==================================================================== */

void test_glitch_count_is_visible(void) {
    printf("--- test_glitch_count_is_visible ---\n");
    setup_clean();
    ASSERT_EQ(0, svc_speed_get_glitch_count(SPEED_WHEEL_LEFT), "наводок не было");

    mock_glitch[ENCODER_LEFT] = 7;
    ASSERT_EQ(7, svc_speed_get_glitch_count(SPEED_WHEEL_LEFT),
              "отброшенные фронты доступны наверх, а не теряются молча");
    ASSERT_EQ(0, svc_speed_get_glitch_count(SPEED_WHEEL_RIGHT), "счётчики раздельные");
}

void test_bad_wheel_index(void) {
    printf("--- test_bad_wheel_index ---\n");
    setup_clean();
    ASSERT_EQ(0, svc_speed_get_rpm((speed_wheel_t)5), "неверный индекс не читает за массив");
    ASSERT_EQ(SPEED_NO_DATA, svc_speed_get_state((speed_wheel_t)5), "и состояние безопасно");
    ASSERT_EQ(0, svc_speed_get_glitch_count((speed_wheel_t)5), "и счётчик наводок");
}

/* ====================================================================
 *  Дефект T-3 в новом обличье: быстрое не должно выглядеть медленным
 * ==================================================================== */

void test_absurdly_fast_saturates_instead_of_wrapping(void) {
    printf("--- test_absurdly_fast_saturates_instead_of_wrapping ---\n");
    /* Период 287 мкс — это 17 422 об/мин, физически невозможно, но фильтр
       дребезга такое пропускает (его граница 200 мкс). Точное км/ч×100
       равно 65 679 и в uint16 не помещается: при усечении получилось бы
       143, то есть 1,4 км/ч. Колесо на 17 400 об/мин показало бы скорость
       пешехода — ровно тот молчаливый отказ, из-за которого записан T-3. */
    setup_clean();
    both_spin(287);
    ASSERT_EQ(17422, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "обороты не завёрнуты");
    ASSERT_EQ(1, svc_speed_get_kmh_x10() > 1000,
              "T-3: показание упёрлось в предел, а не завернулось в 1,4 км/ч");
    ASSERT_EQ(6554, svc_speed_get_kmh_x10(), "предел uint16 в км/ч×100 = 655,4 км/ч");
}

void test_plausible_high_speed_is_not_saturated(void) {
    printf("--- test_plausible_high_speed_is_not_saturated ---\n");
    /* Граница насыщения не должна задевать реальные скорости. 1000 мкс —
       это 5000 об/мин, уже далеко за пределами детской машины, и оно
       по-прежнему считается точно: 188,5 км/ч. */
    setup_clean();
    both_spin(1000);
    ASSERT_EQ(5000, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "5000 об/мин");
    ASSERT_EQ(1885, svc_speed_get_kmh_x10(), "188,5 км/ч — считается, а не упирается");
}

/* ====================================================================
 *  Геометрия из настроек (ADR-0017): подбор без перепрошивки
 * ==================================================================== */

void test_pulses_per_rev_comes_from_settings(void) {
    printf("--- test_pulses_per_rev_comes_from_settings ---\n");
    /* По docs/HARDWARE_BRINGUP.md настоящее число импульсов на оборот
       скорее 30-70, чем 12. Пока оно неизвестно, его подбирают - и
       подбирать надо без перепрошивки. */
    setup_clean();
    ASSERT_EQ(ENCODER_PULSES_PER_REV, cfg_settings_get()->encoder_pulses_per_rev,
              "умолчание берётся из cfg_board.h");

    both_spin(12500);
    ASSERT_EQ(400, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "при 12 имп/об это 400 об/мин");
    ASSERT_EQ(151, svc_speed_get_kmh_x10(), "и 15,1 км/ч");

    /* Вдвое больше импульсов на оборот — тот же период означает вдвое
       меньше оборотов: 60 000 000 / (12500 x 24) = 200 */
    cfg_settings_get_mutable()->encoder_pulses_per_rev = 24;
    both_spin(12500);
    ASSERT_EQ(200, svc_speed_get_rpm(SPEED_WHEEL_LEFT),
              "24 имп/об при том же периоде дают 200 об/мин");
    ASSERT_EQ(75, svc_speed_get_kmh_x10(), "и скорость вдвое меньше — 7,5 км/ч");
}

void test_wheel_diameter_comes_from_settings(void) {
    printf("--- test_wheel_diameter_comes_from_settings ---\n");
    setup_clean();
    ASSERT_EQ(WHEEL_DIAMETER_MM, cfg_settings_get()->wheel_diameter_mm,
              "умолчание берётся из cfg_board.h");

    /* Диаметр входит в скорость линейно: вдвое больше колесо — вдвое
       больше путь за оборот, обороты те же */
    cfg_settings_get_mutable()->wheel_diameter_mm = 400;
    both_spin(12500);
    ASSERT_EQ(400, svc_speed_get_rpm(SPEED_WHEEL_LEFT), "обороты от диаметра не зависят");
    ASSERT_EQ(302, svc_speed_get_kmh_x10(), "а скорость удвоилась: 30,2 км/ч");
}

void test_odometer_follows_settings(void) {
    printf("--- test_odometer_follows_settings ---\n");
    /* Путь на импульс = pi*D/ppr. При D = 400 и 12 имп/об это 104,72 мм;
       10 оборотов на обоих колёсах дают 12566 мм = 12 м. */
    setup_clean();
    cfg_settings_get_mutable()->wheel_diameter_mm = 400;
    for (int rev = 0; rev < 10; rev++) {
        wheel_spins(SPEED_WHEEL_LEFT,  12500, ENCODER_PULSES_PER_REV);
        wheel_spins(SPEED_WHEEL_RIGHT, 12500, ENCODER_PULSES_PER_REV);
        svc_speed_update();
    }
    ASSERT_EQ32(12UL, svc_speed_get_odometer_m(),
                "10 оборотов колеса D=400 мм = 12,57 м");
}

void test_zero_geometry_falls_back_instead_of_dividing_by_zero(void) {
    printf("--- test_zero_geometry_falls_back_instead_of_dividing_by_zero ---\n");
    /* Реестр параметров нуля не пропустит, но если он всё же окажется в
       настройках (испорченный EEPROM с совпавшей CRC), деление на ноль
       на AVR не ловится. Поэтому в сервисе стоит запасной путь. */
    setup_clean();
    cfg_settings_get_mutable()->encoder_pulses_per_rev = 0;
    cfg_settings_get_mutable()->wheel_diameter_mm = 0;
    both_spin(12500);
    ASSERT_EQ(400, svc_speed_get_rpm(SPEED_WHEEL_LEFT),
              "нулевая геометрия откатывается к умолчанию, а не роняет расчёт");
    ASSERT_EQ(151, svc_speed_get_kmh_x10(), "и скорость считается по умолчанию");
}

int main(void) {
    printf("=========================================\n");
    printf("  svc_speed Unit Tests (ADR-0023)\n");
    printf("=========================================\n\n");
    test_no_data_before_any_pulse();
    test_stopped_is_not_no_data();
    test_rpm_reference_points();
    test_kmh_reference_points();
    test_b3_reference_point_survives_method_change();
    test_resolution_beats_pulse_counting();
    test_speed_decays_as_pulses_stop_coming();
    test_stop_timeout_is_the_measurement_floor();
    test_first_pulse_after_a_stop_is_not_a_speed();
    test_turning_averages_wheel_speeds();
    test_one_silent_wheel_does_not_halve_speed();
    test_odometer_counts_pulses();
    test_odometer_keeps_the_remainder();
    test_odometer_reset();
    test_survives_micros_overflow();
    test_absurdly_fast_saturates_instead_of_wrapping();
    test_plausible_high_speed_is_not_saturated();
    test_glitch_count_is_visible();
    test_bad_wheel_index();
    test_pulses_per_rev_comes_from_settings();
    test_wheel_diameter_comes_from_settings();
    test_odometer_follows_settings();
    test_zero_geometry_falls_back_instead_of_dividing_by_zero();

    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
