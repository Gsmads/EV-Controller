/**
 * @file test_cfg_settings.c
 * @brief Юнит-тест cfg_settings с мок-EEPROM
 *
 * Компиляция:
 *   gcc -std=c11 -o test_cfg_settings test_cfg_settings.c \
 *       ../cfg/cfg_settings.c ../util/util_crc.c
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "../firmware/cfg_settings.h"
#include "../firmware/util_crc.h"

static int pass = 0, fail = 0;
#define ASSERT(cond, msg) do { if (cond) pass++; else { printf("  FAIL: %s\n", msg); fail++; } } while(0)
#define ASSERT_EQ(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%d got=%d)\n", msg, (int)(a), (int)(b)); fail++; } } while(0)

/* ==== Mock EEPROM ==== */
static uint8_t mock_eeprom[1024];

void hal_nvm_read(uint16_t addr, uint8_t *buf, uint16_t len) {
    if (addr + len <= sizeof(mock_eeprom))
        memcpy(buf, mock_eeprom + addr, len);
}

void hal_nvm_write(uint16_t addr, const uint8_t *buf, uint16_t len) {
    if (addr + len <= sizeof(mock_eeprom))
        memcpy(mock_eeprom + addr, buf, len);
}

uint8_t hal_nvm_read_byte(uint16_t addr) {
    return (addr < sizeof(mock_eeprom)) ? mock_eeprom[addr] : 0xFF;
}

void hal_nvm_write_byte(uint16_t addr, uint8_t data) {
    if (addr < sizeof(mock_eeprom)) mock_eeprom[addr] = data;
}

/* ==== Tests ==== */

void test_defaults_on_empty_eeprom(void) {
    printf("--- test_defaults_on_empty_eeprom ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));

    cfg_settings_init();

    ASSERT_EQ(0, cfg_settings_is_loaded_from_eeprom(), "should use defaults");

    const settings_t *s = cfg_settings_get();
    ASSERT_EQ(10,  s->pedal_gas_min, "default gas_min");
    ASSERT_EQ(1000,s->pedal_gas_max, "default gas_max");
    ASSERT_EQ(40,  s->pedal_ema_alpha, "default ema_alpha");
    ASSERT_EQ(200, s->eps_kp, "default eps_kp");
    ASSERT_EQ(512, s->eps_center, "default eps_center");
}

void test_save_and_reload(void) {
    printf("--- test_save_and_reload ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));

    /* Init defaults and modify */
    cfg_settings_init();
    settings_t *s = cfg_settings_get_mutable();
    s->pedal_gas_min = 42;
    s->pedal_gas_max = 999;
    s->eps_kp = 350;
    cfg_settings_save();

    /* Reinitialize — should load from EEPROM */
    cfg_settings_init();
    ASSERT_EQ(1, cfg_settings_is_loaded_from_eeprom(), "should load from EEPROM");

    const settings_t *s2 = cfg_settings_get();
    ASSERT_EQ(42,  s2->pedal_gas_min, "saved gas_min");
    ASSERT_EQ(999, s2->pedal_gas_max, "saved gas_max");
    ASSERT_EQ(350, s2->eps_kp, "saved eps_kp");
}

void test_crc_corruption(void) {
    printf("--- test_crc_corruption ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));

    /* Save valid settings */
    cfg_settings_init();
    settings_t *s = cfg_settings_get_mutable();
    s->pedal_gas_min = 77;
    cfg_settings_save();

    /* Corrupt one byte in data area */
    mock_eeprom[10] ^= 0x55;

    /* Reload — should fall back to defaults */
    cfg_settings_init();
    ASSERT_EQ(0, cfg_settings_is_loaded_from_eeprom(), "corrupted → defaults");

    const settings_t *s2 = cfg_settings_get();
    ASSERT_EQ(10, s2->pedal_gas_min, "back to default gas_min");
}

void test_reset_defaults(void) {
    printf("--- test_reset_defaults ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));

    cfg_settings_init();
    settings_t *s = cfg_settings_get_mutable();
    s->pedal_gas_min = 123;

    cfg_settings_reset_defaults();

    const settings_t *s2 = cfg_settings_get();
    ASSERT_EQ(10, s2->pedal_gas_min, "reset to default");
}

void test_drive_profiles(void) {
    printf("--- test_drive_profiles ---\n");
    cfg_settings_reset_defaults();

    const drive_profile_t *eco = cfg_settings_get_profile(DRIVE_MODE_ECO);
    ASSERT_EQ(400, eco->max_pwm, "Eco max_pwm = 400");
    ASSERT_EQ(200, eco->accel_rate, "Eco accel = 200");
    ASSERT_EQ(0,   eco->direction, "Eco direction = forward");

    const drive_profile_t *sport = cfg_settings_get_profile(DRIVE_MODE_SPORT);
    ASSERT_EQ(1023, sport->max_pwm, "Sport max_pwm = 1023");
    ASSERT_EQ(9,    sport->pwm_resolution, "Sport = 9-bit");

    const drive_profile_t *rev = cfg_settings_get_profile(DRIVE_MODE_REVERSE);
    ASSERT_EQ(1, rev->direction, "Reverse direction = 1");
    ASSERT_EQ(300, rev->max_pwm, "Reverse max_pwm = 300");

    const drive_profile_t *fs = cfg_settings_get_profile(DRIVE_MODE_FAILSAFE);
    ASSERT_EQ(0, fs->max_pwm, "Failsafe max_pwm = 0");
    ASSERT(fs->decel_rate > 0, "Failsafe has decel_rate for soft stop");
}

void test_set_field(void) {
    printf("--- test_set_field ---\n");
    cfg_settings_reset_defaults();

    uint16_t new_val = 555;
    uint16_t offset = offsetof(settings_t, pedal_gas_max);
    uint8_t result = cfg_settings_set_field(offset, &new_val, sizeof(new_val));
    ASSERT_EQ(0, result, "set_field returns 0 on success");
    ASSERT_EQ(555, cfg_settings_get()->pedal_gas_max, "field updated");

    /* Out of bounds */
    result = cfg_settings_set_field(sizeof(settings_t) - 1, &new_val, sizeof(new_val));
    ASSERT_EQ(1, result, "set_field returns 1 on overflow");
}

void test_settings_size(void) {
    printf("--- test_settings_size ---\n");
    uint16_t sz = cfg_settings_get_size();
    printf("  INFO: settings_t size = %u bytes\n", sz);
    /* Must fit in EEPROM with header + CRC */
    ASSERT(sz + 4 + 2 <= 1024, "settings + header + crc fits in 1KB EEPROM");
    /* Reasonable size check */
    ASSERT(sz > 50, "settings has reasonable content");
    ASSERT(sz < 800, "settings leaves room for future expansion");
}


/* ==== Параметры UART (v3): ADR-0016, ADR-0019 ==== */

void test_uart_defaults(void) {
    printf("--- test_uart_defaults ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));
    cfg_settings_init();
    const settings_t *s = cfg_settings_get();

    ASSERT_EQ(UART_BAUD_CODE_250000, s->uart_baud_code,
              "ADR-0015: умолчание скорости — 250000");
    ASSERT_EQ(TX_OVERFLOW_DROP_PACKET, s->uart_tx_policy,
              "ADR-0016: умолчание политики — отбрасывать пакет, а не ждать");
    ASSERT_EQ(10000, s->uart_baud_probation_ms,
              "ADR-0019: испытательный период 10 секунд");
}

void test_baud_table(void) {
    printf("--- test_baud_table ---\n");
    ASSERT_EQ(9600,   cfg_settings_baud_from_code(UART_BAUD_CODE_9600),   "код 0 -> 9600");
    ASSERT_EQ(115200, cfg_settings_baud_from_code(UART_BAUD_CODE_115200), "код 4 -> 115200");
    ASSERT_EQ(250000, cfg_settings_baud_from_code(UART_BAUD_CODE_250000), "код 5 -> 250000");
    ASSERT_EQ(500000, cfg_settings_baud_from_code(UART_BAUD_CODE_500000), "код 6 -> 500000");
    ASSERT_EQ(0, cfg_settings_baud_from_code(UART_BAUD_CODE_COUNT),
              "код вне таблицы даёт 0, а не подстановку умолчания молча");
    ASSERT_EQ(0, cfg_settings_baud_from_code(255), "заведомо чужой код тоже 0");
}

/* Собираем в мок-EEPROM блок настроек версии 2 и проверяем, что миграция
   сохраняет калибровку педалей. Прежний код на несовпадении версии просто
   сбрасывал всё к умолчаниям — а калибровку добывают замерами на железе. */
void test_migration_from_v2(void) {
    printf("--- test_migration_from_v2 ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));

    const uint16_t V2_SIZE = 182;
    const uint16_t DATA_OFF = 4;

    /* Данные версии 2: узнаваемый образец плюс калибровка в известных местах */
    uint8_t v2[182];
    for (uint16_t i = 0; i < V2_SIZE; i++) v2[i] = (uint8_t)(i & 0xFF);
    /* pedal_gas_min = 123, pedal_gas_max = 987 — первые два поля структуры */
    v2[0] = 123 & 0xFF; v2[1] = (123 >> 8) & 0xFF;
    v2[2] = 987 & 0xFF; v2[3] = (987 >> 8) & 0xFF;

    /* Заголовок: magic, version = 2 */
    mock_eeprom[0] = SETTINGS_MAGIC & 0xFF;
    mock_eeprom[1] = (SETTINGS_MAGIC >> 8) & 0xFF;
    mock_eeprom[2] = 2;
    mock_eeprom[3] = 0;
    memcpy(mock_eeprom + DATA_OFF, v2, V2_SIZE);

    /* CRC по правилам версии 2: magic + version(2) + reserved + 182 байта */
    uint16_t crc = 0xFFFF;
    crc = util_crc16_update(crc, (uint8_t)(SETTINGS_MAGIC & 0xFF));
    crc = util_crc16_update(crc, (uint8_t)(SETTINGS_MAGIC >> 8));
    crc = util_crc16_update(crc, 2);
    crc = util_crc16_update(crc, 0);
    for (uint16_t i = 0; i < V2_SIZE; i++) crc = util_crc16_update(crc, v2[i]);
    mock_eeprom[DATA_OFF + V2_SIZE]     = (uint8_t)(crc & 0xFF);
    mock_eeprom[DATA_OFF + V2_SIZE + 1] = (uint8_t)(crc >> 8);

    cfg_settings_init();
    const settings_t *s = cfg_settings_get();

    ASSERT_EQ(1, cfg_settings_is_loaded_from_eeprom(),
              "миграция: настройки взяты из EEPROM, а не сброшены");
    ASSERT_EQ(123, s->pedal_gas_min, "миграция: калибровка педали газа сохранена");
    ASSERT_EQ(987, s->pedal_gas_max, "миграция: верхняя точка сохранена");
    ASSERT_EQ(UART_BAUD_CODE_250000, s->uart_baud_code,
              "миграция: новое поле получило умолчание");
    ASSERT_EQ(TX_OVERFLOW_DROP_PACKET, s->uart_tx_policy,
              "миграция: политика получила умолчание");
}

void test_migration_rejects_corrupt_v2(void) {
    printf("--- test_migration_rejects_corrupt_v2 ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));
    mock_eeprom[0] = SETTINGS_MAGIC & 0xFF;
    mock_eeprom[1] = (SETTINGS_MAGIC >> 8) & 0xFF;
    mock_eeprom[2] = 2;
    mock_eeprom[3] = 0;
    /* Данные есть, CRC мусорный */
    cfg_settings_init();
    ASSERT_EQ(0, cfg_settings_is_loaded_from_eeprom(),
              "повреждённый блок версии 2 не мигрируется, берутся умолчания");
}

void test_migration_unknown_version(void) {
    printf("--- test_migration_unknown_version ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));
    mock_eeprom[0] = SETTINGS_MAGIC & 0xFF;
    mock_eeprom[1] = (SETTINGS_MAGIC >> 8) & 0xFF;
    mock_eeprom[2] = 99;                    /* версия из будущего */
    mock_eeprom[3] = 0;
    cfg_settings_init();
    ASSERT_EQ(0, cfg_settings_is_loaded_from_eeprom(),
              "незнакомая версия — умолчания, а не попытка угадать раскладку");
}


/* ==== Каналы АЦП (v4): ADR-0009 ==== */

void test_adc_defaults(void) {
    printf("--- test_adc_defaults ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));
    cfg_settings_init();
    const settings_t *s = cfg_settings_get();
    /* Та же карта, что была зашита в cfg_board.h: A0..A3 и A6 */
    ASSERT_EQ(0, s->adc_ch_pedal_gas,     "газ — канал 0");
    ASSERT_EQ(1, s->adc_ch_pedal_brake,   "тормоз — канал 1");
    ASSERT_EQ(2, s->adc_ch_current_right, "ток правого — канал 2");
    ASSERT_EQ(3, s->adc_ch_steering_pos,  "руль — канал 3");
    ASSERT_EQ(6, s->adc_ch_current_left,  "ток левого — канал 6");
    ASSERT_EQ(ADC_MAP_OK, cfg_settings_validate_adc(s), "умолчания проходят проверку");
}

void test_adc_validation(void) {
    printf("--- test_adc_validation ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));
    cfg_settings_init();
    settings_t *s = cfg_settings_get_mutable();

    s->adc_ch_pedal_gas = 8;
    ASSERT_EQ(ADC_MAP_OUT_OF_RANGE, cfg_settings_validate_adc(s),
              "канал 8 вне диапазона: у ATmega328P их восемь, 0..7");
    s->adc_ch_pedal_gas = 255;
    ASSERT_EQ(ADC_MAP_OUT_OF_RANGE, cfg_settings_validate_adc(s), "255 тоже вне диапазона");

    s->adc_ch_pedal_gas = 0;
    ASSERT_EQ(ADC_MAP_OK, cfg_settings_validate_adc(s), "возврат в диапазон — снова годно");

    s->adc_ch_pedal_brake = 0;
    ASSERT_EQ(ADC_MAP_DUPLICATE, cfg_settings_validate_adc(s),
              "ADR-0009: газ и тормоз на одном канале — педаль читала бы чужой датчик");
    s->adc_ch_current_left = 3;
    s->adc_ch_pedal_brake = 1;
    ASSERT_EQ(ADC_MAP_DUPLICATE, cfg_settings_validate_adc(s),
              "совпадение любых двух каналов, не только соседних полей");
}

void test_adc_bad_map_in_eeprom_rejected(void) {
    printf("--- test_adc_bad_map_in_eeprom_rejected ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));
    cfg_settings_init();
    settings_t *m = cfg_settings_get_mutable();
    m->pedal_gas_min = 111;
    m->adc_ch_pedal_brake = m->adc_ch_pedal_gas;   /* негодная карта */
    cfg_settings_save();                            /* CRC сойдётся! */

    cfg_settings_init();
    ASSERT_EQ(0, cfg_settings_is_loaded_from_eeprom(),
              "целый по CRC, но бессмысленный блок не принимается");
    ASSERT_EQ(10, cfg_settings_get()->pedal_gas_min, "взяты умолчания");
}

/* Блок версии 3: v2 плюс четыре байта параметров UART. Проверяем, что
   обобщённая миграция тянет и его, а не только версию 2. */
void test_migration_from_v3(void) {
    printf("--- test_migration_from_v3 ---\n");
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));

    const uint16_t V3_SIZE = 186;
    const uint16_t DATA_OFF = 4;

    uint8_t v3[186];
    for (uint16_t i = 0; i < V3_SIZE; i++) v3[i] = (uint8_t)(i & 0xFF);
    v3[0] = 55;  v3[1] = 0;          /* pedal_gas_min = 55 */
    v3[2] = 200; v3[3] = 3;          /* pedal_gas_max = 968 */
    v3[182] = UART_BAUD_CODE_115200; /* скорость, выставленная пользователем */
    v3[183] = TX_OVERFLOW_BLOCK;     /* политика, выставленная пользователем */
    v3[184] = 0x88; v3[185] = 0x13;  /* probation = 5000 */

    mock_eeprom[0] = SETTINGS_MAGIC & 0xFF;
    mock_eeprom[1] = (SETTINGS_MAGIC >> 8) & 0xFF;
    mock_eeprom[2] = 3;
    mock_eeprom[3] = 0;
    memcpy(mock_eeprom + DATA_OFF, v3, V3_SIZE);

    uint16_t crc = 0xFFFF;
    crc = util_crc16_update(crc, (uint8_t)(SETTINGS_MAGIC & 0xFF));
    crc = util_crc16_update(crc, (uint8_t)(SETTINGS_MAGIC >> 8));
    crc = util_crc16_update(crc, 3);
    crc = util_crc16_update(crc, 0);
    for (uint16_t i = 0; i < V3_SIZE; i++) crc = util_crc16_update(crc, v3[i]);
    mock_eeprom[DATA_OFF + V3_SIZE]     = (uint8_t)(crc & 0xFF);
    mock_eeprom[DATA_OFF + V3_SIZE + 1] = (uint8_t)(crc >> 8);

    cfg_settings_init();
    const settings_t *s = cfg_settings_get();

    ASSERT_EQ(1, cfg_settings_is_loaded_from_eeprom(), "версия 3 мигрирована");
    ASSERT_EQ(55,  s->pedal_gas_min, "калибровка сохранена");
    ASSERT_EQ(968, s->pedal_gas_max, "верхняя точка сохранена");
    ASSERT_EQ(UART_BAUD_CODE_115200, s->uart_baud_code,
              "выбранная пользователем скорость сохранена");
    ASSERT_EQ(TX_OVERFLOW_BLOCK, s->uart_tx_policy, "выбранная политика сохранена");
    ASSERT_EQ(5000, s->uart_baud_probation_ms, "испытательный период сохранён");
    ASSERT_EQ(0, s->adc_ch_pedal_gas, "новое поле версии 4 получило умолчание");
    ASSERT_EQ(6, s->adc_ch_current_left, "и последнее новое поле тоже");
}

int main(void) {
    printf("=========================================\n");
    printf("  cfg_settings Unit Tests\n");
    printf("=========================================\n\n");
    test_defaults_on_empty_eeprom();
    test_save_and_reload();
    test_crc_corruption();
    test_reset_defaults();
    test_drive_profiles();
    test_set_field();
    test_settings_size();
    test_uart_defaults();
    test_baud_table();
    test_migration_from_v2();
    test_migration_rejects_corrupt_v2();
    test_migration_unknown_version();
    test_adc_defaults();
    test_adc_validation();
    test_adc_bad_map_in_eeprom_rejected();
    test_migration_from_v3();
    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
