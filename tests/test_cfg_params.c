/**
 * @file test_cfg_params.c
 * @brief Реестр параметров: одна охраняемая дверь для записи настроек
 *
 * Дефект S-3 docs/AUDIT.md. Раньше cfg_settings_set_field проверяла только
 * выход за границы структуры, поэтому brake_rate_max = 0 отключал тормоз
 * и сохранялся в EEPROM.
 *
 * Границы в реестре следуют из устройства системы, а не из вкуса: АЦП
 * десятибитный, кривых отклика три, каналов АЦП восемь, датчик тока меряет
 * до 20 А, темп торможения ноль означает отсутствие торможения.
 *
 * Компиляция: см. tests/Makefile
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "../firmware/cfg_settings.h"
#include "../firmware/cfg_params.h"
#include "../firmware/svc_pedals.h"

static int pass = 0, fail = 0;
#define ASSERT_EQ(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%d got=%d)\n", msg, (int)(a), (int)(b)); fail++; } } while(0)

/* ==== Мок долговременной памяти ==== */
static uint8_t mock_nvm[1024];
void hal_nvm_read(uint16_t addr, uint8_t *buf, uint16_t len) {
    if (addr + len <= sizeof(mock_nvm)) memcpy(buf, mock_nvm + addr, len);
}
void hal_nvm_write(uint16_t addr, const uint8_t *buf, uint16_t len) {
    if (addr + len <= sizeof(mock_nvm)) memcpy(mock_nvm + addr, buf, len);
}
uint8_t hal_nvm_read_byte(uint16_t addr) {
    return (addr < sizeof(mock_nvm)) ? mock_nvm[addr] : 0xFF;
}
void hal_nvm_write_byte(uint16_t addr, uint8_t d) {
    if (addr < sizeof(mock_nvm)) mock_nvm[addr] = d;
}

static void fresh(void) {
    memset(mock_nvm, 0xFF, sizeof(mock_nvm));
    cfg_settings_init();
}

/** Записать u16 по смещению. */
static uint8_t set_u16(uint16_t offset, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    return cfg_settings_set_field(offset, b, 2);
}
/** Записать u8 по смещению. */
static uint8_t set_u8(uint16_t offset, uint8_t v) {
    return cfg_settings_set_field(offset, &v, 1);
}

#define OFF_PROFILE(i, field) \
    ((uint16_t)(offsetof(settings_t, profiles) + (i) * sizeof(drive_profile_t) \
                + offsetof(drive_profile_t, field)))

/* ====================================================================
 *  S-3: значения, ломающие поведение
 * ==================================================================== */

void test_zero_brake_rate_rejected(void) {
    printf("--- test_zero_brake_rate_rejected ---\n");
    fresh();
    uint16_t off = OFF_PROFILE(DRIVE_MODE_ECO, brake_rate_max);
    uint16_t before = cfg_settings_get_profile(DRIVE_MODE_ECO)->brake_rate_max;

    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u16(off, 0),
              "S-3: brake_rate_max = 0 отвергнут — это отключение тормоза");
    ASSERT_EQ(before, cfg_settings_get_profile(DRIVE_MODE_ECO)->brake_rate_max,
              "S-3: настройки не изменились");

    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u16(OFF_PROFILE(DRIVE_MODE_ECO, brake_rate_min), 0),
              "S-3: brake_rate_min = 0 тоже отвергнут");
    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u16(OFF_PROFILE(DRIVE_MODE_ECO, decel_rate), 0),
              "decel_rate = 0 — машина не замедляется при отпускании газа");
}

void test_zero_ema_alpha_rejected(void) {
    printf("--- test_zero_ema_alpha_rejected ---\n");
    fresh();
    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u8(offsetof(settings_t, pedal_ema_alpha), 0),
              "alpha = 0 останавливает фильтр: педаль замирает навсегда");
    ASSERT_EQ(PARAM_OK, set_u8(offsetof(settings_t, pedal_ema_alpha), 1),
              "минимальное допустимое значение проходит");
}

void test_adc_raw_bounds(void) {
    printf("--- test_adc_raw_bounds ---\n");
    fresh();
    ASSERT_EQ(PARAM_OK, set_u16(offsetof(settings_t, pedal_gas_max), 1023),
              "верхняя точка калибровки 1023 — предел десятибитного АЦП");
    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u16(offsetof(settings_t, pedal_gas_max), 1024),
              "1024 отвергнуто: АЦП столько не выдаёт");
    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u16(offsetof(settings_t, pedal_gas_max), 65535),
              "и тем более 65535");
}

void test_enum_bounds(void) {
    printf("--- test_enum_bounds ---\n");
    fresh();
    ASSERT_EQ(PARAM_OK, set_u8(offsetof(settings_t, pedal_gas_curve), 2),
              "кривая 2 (s-curve) допустима");
    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u8(offsetof(settings_t, pedal_gas_curve), 3),
              "кривой 3 не существует");
    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u8(offsetof(settings_t, gas_combinator), PEDAL_COMBINE_COUNT),
              "комбинатора за пределом перечисления не существует");
    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u8(offsetof(settings_t, adc_ch_pedal_gas), ADC_CHANNEL_COUNT),
              "канала АЦП за пределом не существует");
    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u8(offsetof(settings_t, uart_baud_code), UART_BAUD_CODE_COUNT),
              "кода скорости за пределом таблицы не существует");
}

void test_current_limit_bounded_by_sensor(void) {
    printf("--- test_current_limit_bounded_by_sensor ---\n");
    fresh();
    ASSERT_EQ(PARAM_OK, set_u16(offsetof(settings_t, current_limit_hard_ma), 20000),
              "20 А — предел датчика ACS712-20A");
    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u16(offsetof(settings_t, current_limit_hard_ma), 20001),
              "выше предела датчика порог бессмыслен: он никогда не сработает");
}

/* ====================================================================
 *  Механика реестра
 * ==================================================================== */

void test_readonly_padding(void) {
    printf("--- test_readonly_padding ---\n");
    fresh();
    ASSERT_EQ(PARAM_READONLY, set_u8(offsetof(settings_t, _pad1), 5),
              "байт выравнивания не предназначен для записи");
}

void test_unknown_offset(void) {
    printf("--- test_unknown_offset ---\n");
    fresh();
    /* Смещение внутрь поля, а не на его начало */
    ASSERT_EQ(PARAM_UNKNOWN, set_u8((uint16_t)(offsetof(settings_t, pedal_gas_min) + 1), 7),
              "середина поля — не поле: раньше сюда молча писался мусор");
}

void test_out_of_bounds(void) {
    printf("--- test_out_of_bounds ---\n");
    fresh();
    ASSERT_EQ(PARAM_OUT_OF_BOUNDS, set_u16((uint16_t)(sizeof(settings_t) - 1), 1),
              "запись за конец структуры отвергнута");
}

void test_size_mismatch(void) {
    printf("--- test_size_mismatch ---\n");
    fresh();
    ASSERT_EQ(PARAM_SIZE_MISMATCH, set_u8(offsetof(settings_t, pedal_gas_min), 5),
              "один байт в двухбайтовое поле — рассинхронизация схемы веба");
    ASSERT_EQ(PARAM_SIZE_MISMATCH, set_u16(offsetof(settings_t, motor_deadzone), 5),
              "два байта в однобайтовое поле — то же самое");
}

void test_allow_ff_sentinel(void) {
    printf("--- test_allow_ff_sentinel ---\n");
    fresh();
    uint16_t off = OFF_PROFILE(DRIVE_MODE_SPORT, pedal_curve);
    ASSERT_EQ(PARAM_OK, set_u8(off, 0xFF),
              "0xFF в профиле означает «брать общую кривую» и допустим");
    ASSERT_EQ(PARAM_OK, set_u8(off, 2), "обычное значение тоже");
    ASSERT_EQ(PARAM_OUT_OF_RANGE, set_u8(off, 3), "а 3 — нет");
}

void test_valid_write_lands(void) {
    printf("--- test_valid_write_lands ---\n");
    fresh();
    ASSERT_EQ(PARAM_OK, set_u16(offsetof(settings_t, pedal_gas_min), 42),
              "допустимое значение принято");
    ASSERT_EQ(42, cfg_settings_get()->pedal_gas_min, "и записано");

    uint16_t off = OFF_PROFILE(DRIVE_MODE_SPORT, max_pwm);
    ASSERT_EQ(PARAM_OK, set_u16(off, 900), "запись в профиль принята");
    ASSERT_EQ(900, cfg_settings_get_profile(DRIVE_MODE_SPORT)->max_pwm, "и попала в нужный профиль");
    ASSERT_EQ(1023, cfg_settings_get_profile(DRIVE_MODE_ECO)->max_pwm == 900 ? 900 : 1023,
              "соседний профиль не задет");
}

void test_every_profile_validated(void) {
    printf("--- test_every_profile_validated ---\n");
    fresh();
    /* Одно описание поля профиля обслуживает все девять профилей */
    int rejected = 0;
    for (int i = 0; i < DRIVE_MODE_COUNT; i++) {
        if (set_u16(OFF_PROFILE(i, brake_rate_max), 0) == PARAM_OUT_OF_RANGE) rejected++;
    }
    ASSERT_EQ(DRIVE_MODE_COUNT, rejected, "нулевой тормоз отвергнут во всех девяти профилях");
}

/* ====================================================================
 *  Таблица
 * ==================================================================== */

void test_registry_lookup(void) {
    printf("--- test_registry_lookup ---\n");
    param_desc_t d;
    ASSERT_EQ(1, cfg_params_find_by_id(0x0201, &d), "поиск по идентификатору находит поле");
    ASSERT_EQ(offsetof(settings_t, pedal_gas_min), d.offset, "смещение то самое");
    ASSERT_EQ(PARAM_U16, d.type, "тип тот самый");
    ASSERT_EQ(0, cfg_params_find_by_id(0xFFFF, &d), "несуществующий идентификатор не находится");
    ASSERT_EQ(1, cfg_params_count() > 0, "реестр не пуст");
}

void test_every_descriptor_sane(void) {
    printf("--- test_every_descriptor_sane ---\n");
    int bad_range = 0, bad_id = 0;
    for (uint8_t i = 0; i < cfg_params_count(); i++) {
        param_desc_t d;
        cfg_params_get(i, &d);
        if (d.min > d.max) bad_range++;
        if (d.id == 0) bad_id++;
        /* Идентификаторы уникальны */
        for (uint8_t j = 0; j < i; j++) {
            param_desc_t e;
            cfg_params_get(j, &e);
            if (e.id == d.id) bad_id++;
        }
    }
    ASSERT_EQ(0, bad_range, "ни у одного параметра min не больше max");
    ASSERT_EQ(0, bad_id, "идентификаторы ненулевые и не повторяются");
}

int main(void) {
    printf("=========================================\n");
    printf("  cfg_params Unit Tests\n");
    printf("=========================================\n\n");

    test_zero_brake_rate_rejected();
    test_zero_ema_alpha_rejected();
    test_adc_raw_bounds();
    test_enum_bounds();
    test_current_limit_bounded_by_sensor();
    test_readonly_padding();
    test_unknown_offset();
    test_out_of_bounds();
    test_size_mismatch();
    test_allow_ff_sentinel();
    test_valid_write_lands();
    test_every_profile_validated();
    test_registry_lookup();
    test_every_descriptor_sane();

    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
