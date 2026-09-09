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

static int pass = 0, fail = 0;
#define ASSERT(cond, msg) do { if (cond) pass++; else { printf("  FAIL: %s\n", msg); fail++; } } while(0)
#define ASSERT_EQ(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%d got=%d)\n", msg, (int)(a), (int)(b)); fail++; } } while(0)

/* ==== Mock EEPROM ==== */
static uint8_t mock_eeprom[1024];

void hal_eeprom_read(uint16_t addr, uint8_t *buf, uint16_t len) {
    if (addr + len <= sizeof(mock_eeprom))
        memcpy(buf, mock_eeprom + addr, len);
}

void hal_eeprom_write(uint16_t addr, const uint8_t *buf, uint16_t len) {
    if (addr + len <= sizeof(mock_eeprom))
        memcpy(mock_eeprom + addr, buf, len);
}

uint8_t hal_eeprom_read_byte(uint16_t addr) {
    return (addr < sizeof(mock_eeprom)) ? mock_eeprom[addr] : 0xFF;
}

void hal_eeprom_write_byte(uint16_t addr, uint8_t data) {
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
    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
