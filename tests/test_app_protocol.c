/**
 * @file test_app_protocol.c
 * @brief Парсер бинарного протокола: устойчивость к мусору и битым кадрам
 *
 * Пункт 11 очереди 2 docs/AUDIT.md. Парсер — единственная дверь, через
 * которую в контроллер попадают команды извне, и до сих пор он не был
 * покрыт ничем.
 *
 * Проверяется то, что случается на настоящей линии RS485 рядом с моторами:
 * помеха перед кадром, оборванный кадр, сошедшийся не тот CRC, байт 0xFF
 * в данных (дефект B-2 жил именно здесь).
 *
 * Компиляция: см. tests/Makefile
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "../firmware/app_protocol.h"
#include "../firmware/cfg_settings.h"
#include "../firmware/cfg_params.h"
#include "../firmware/util_crc.h"
#include "../firmware/hal_adc.h"
#include "../firmware/hal_encoder.h"
#include "../firmware/svc_pedals.h"
#include "../firmware/svc_speed.h"

static int pass = 0, fail = 0;
#define ASSERT_EQ(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%d got=%d)\n", msg, (int)(a), (int)(b)); fail++; } } while(0)

/* ====================================================================
 *  Моки HAL
 * ==================================================================== */

/* Вход: что «пришло» по линии */
static uint8_t  rx[256];
static uint16_t rx_len, rx_pos;

/* Выход: что контроллер ответил */
static uint8_t  tx[256];
static uint16_t tx_len;

int16_t hal_uart_read(void) {
    if (rx_pos >= rx_len) return -1;
    return (int16_t)rx[rx_pos++];
}
uint8_t hal_uart_write_buf(const uint8_t *buf, uint8_t len) {
    for (uint8_t i = 0; i < len && tx_len < sizeof(tx); i++) tx[tx_len++] = buf[i];
    return 1;
}
void    hal_uart_write(uint8_t d) { (void)hal_uart_write_buf(&d, 1); }
void    hal_uart_init(uint32_t b) { (void)b; }
uint8_t hal_uart_available(void) { return (uint8_t)(rx_len - rx_pos); }
void    hal_uart_flush_rx(void) { rx_pos = rx_len; }
void    hal_uart_set_rs485_tx(uint8_t m) { (void)m; }
uint16_t hal_uart_tx_dropped(void) { return 0; }

static uint32_t mock_millis_value;
uint32_t hal_system_millis(void) { return mock_millis_value; }

static uint16_t signal_value[ANALOG_SIGNAL_COUNT];
uint16_t hal_adc_read_signal(analog_signal_t s) {
    return (s < ANALOG_SIGNAL_COUNT) ? signal_value[s] : 0;
}
uint16_t hal_adc_read(uint8_t c) { (void)c; return 0; }
void     hal_adc_init(void) {}
void     hal_encoder_init(void) {}
void     hal_encoder_take(encoder_channel_t c, encoder_sample_t *out) {
    (void)c;
    out->period_us = 0; out->last_pulse_us = 0; out->pulses = 0; out->has_period = 0;
}
uint16_t hal_encoder_glitch_count(encoder_channel_t c) { (void)c; return 0; }
uint32_t hal_system_micros(void) { return mock_millis_value * 1000UL; }

static uint8_t mock_nvm[1024];
void hal_nvm_read(uint16_t a, uint8_t *b, uint16_t l) {
    if (a + l <= sizeof(mock_nvm)) memcpy(b, mock_nvm + a, l);
}
void hal_nvm_write(uint16_t a, const uint8_t *b, uint16_t l) {
    if (a + l <= sizeof(mock_nvm)) memcpy(mock_nvm + a, b, l);
}
uint8_t hal_nvm_read_byte(uint16_t a) { return (a < sizeof(mock_nvm)) ? mock_nvm[a] : 0xFF; }
void    hal_nvm_write_byte(uint16_t a, uint8_t d) { if (a < sizeof(mock_nvm)) mock_nvm[a] = d; }

/* ====================================================================
 *  Помощники
 * ==================================================================== */

static void reset_all(void) {
    memset(mock_nvm, 0xFF, sizeof(mock_nvm));
    memset(signal_value, 0, sizeof(signal_value));
    rx_len = rx_pos = tx_len = 0;
    mock_millis_value = 1000;
    cfg_settings_init();
    svc_pedals_init();
    svc_speed_init();
    app_protocol_init();
}

static void feed(const uint8_t *bytes, uint16_t n) {
    for (uint16_t i = 0; i < n && rx_len < sizeof(rx); i++) rx[rx_len++] = bytes[i];
    /* Парсер берёт до 16 байт за вызов — гоняем, пока не кончатся */
    while (rx_pos < rx_len) app_protocol_update();
}

/** Собрать корректный кадр. */
static uint16_t build(uint8_t *out, uint8_t cmd, const uint8_t *payload, uint8_t plen) {
    uint8_t len = (uint8_t)(1 + plen);
    uint16_t crc = 0xFFFF;
    crc = util_crc16_update(crc, len);
    crc = util_crc16_update(crc, cmd);
    for (uint8_t i = 0; i < plen; i++) crc = util_crc16_update(crc, payload[i]);

    uint16_t n = 0;
    out[n++] = PROTO_SYNC;
    out[n++] = len;
    out[n++] = cmd;
    for (uint8_t i = 0; i < plen; i++) out[n++] = payload[i];
    out[n++] = (uint8_t)(crc & 0xFF);
    out[n++] = (uint8_t)(crc >> 8);
    return n;
}

/** Найти в ответе кадр с указанной командой. Возвращает индекс или -1. */
static int find_response(uint8_t cmd) {
    for (uint16_t i = 0; i + 2 < tx_len; i++) {
        if (tx[i] == PROTO_SYNC && tx[i + 2] == cmd) return (int)i;
    }
    return -1;
}

/* ====================================================================
 *  Корректный обмен
 * ==================================================================== */

void test_ping_answered(void) {
    printf("--- test_ping_answered ---\n");
    reset_all();
    uint8_t f[8];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0, "на PING пришёл PONG");
}

void test_unknown_command_nacked(void) {
    printf("--- test_unknown_command_nacked ---\n");
    reset_all();
    uint8_t f[8];
    uint16_t n = build(f, 0x77, NULL, 0);
    feed(f, n);
    int at = find_response(RSP_NACK);
    ASSERT_EQ(1, at >= 0, "неизвестная команда получила NACK, а не молчание");
    ASSERT_EQ(ERR_UNKNOWN_CMD, tx[at + 4], "код отказа — «неизвестная команда»");
}

/* ====================================================================
 *  Мусор и битые кадры
 * ==================================================================== */

void test_garbage_before_sync(void) {
    printf("--- test_garbage_before_sync ---\n");
    reset_all();
    uint8_t noise[5] = { 0x00, 0x13, 0xFF, 0x7E, 0x01 };
    feed(noise, 5);
    uint8_t f[8];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0, "помеха перед кадром не мешает разобрать кадр");
}

void test_bad_crc_dropped_silently(void) {
    printf("--- test_bad_crc_dropped_silently ---\n");
    reset_all();
    uint8_t f[8];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    f[n - 1] ^= 0xFF;                       /* портим старший байт CRC */
    feed(f, n);
    ASSERT_EQ(0, tx_len, "кадр с несошедшимся CRC отброшен без ответа");

    /* И парсер не заклинило */
    tx_len = 0;
    n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0, "следующий кадр разобран нормально");
}

void test_truncated_frame_costs_the_next_one(void) {
    printf("--- test_truncated_frame_costs_the_next_one ---\n");
    /*
     * Оборванный кадр съедает начало следующего, и это не ошибка теста,
     * а свойство кадрирования без межкадрового таймаута.
     *
     * Кадр оборвался в состоянии «жду младший байт CRC». Следующий кадр
     * начинается с 0xAA — парсер принимает его за младший байт CRC,
     * затем байт длины за старший, CRC не сходится, парсер сбрасывается.
     * К этому моменту голова второго кадра уже проглочена.
     *
     * Записано в docs/AUDIT.md как T-4. Лечится таймаутом: молчание
     * дольше времени передачи кадра означает конец кадра, что бы ни
     * говорило состояние разбора.
     */
    reset_all();
    uint8_t f[8];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, (uint16_t)(n - 2));             /* кадр оборвался на CRC */
    ASSERT_EQ(0, tx_len, "оборванный кадр ответа не вызвал");

    uint16_t n2 = build(f, CMD_PING, NULL, 0);
    feed(f, n2);
    ASSERT_EQ(-1, find_response(RSP_PONG),
              "T-4: следующий кадр потерян — его голову съел хвост оборванного");

    uint16_t n3 = build(f, CMD_PING, NULL, 0);
    feed(f, n3);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0,
              "но через один кадр связь восстанавливается сама");
}

void test_zero_length_rejected(void) {
    printf("--- test_zero_length_rejected ---\n");
    reset_all();
    uint8_t bad[4] = { PROTO_SYNC, 0x00, 0x00, 0x00 };
    feed(bad, 4);
    ASSERT_EQ(0, tx_len, "длина 0 не образует кадра");
    uint8_t f[8];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0, "и парсер остался рабочим");
}

void test_oversized_length_rejected(void) {
    printf("--- test_oversized_length_rejected ---\n");
    reset_all();
    uint8_t bad[4] = { PROTO_SYNC, (uint8_t)(PROTO_MAX_PAYLOAD + 2), 0x00, 0x00 };
    feed(bad, 4);
    uint8_t f[8];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0,
              "заявленная длина сверх допустимой не переполняет буфер и не ломает парсер");
}

/* ====================================================================
 *  B-2 на уровне протокола: байт 0xFF в данных
 * ==================================================================== */

void test_ff_byte_in_payload(void) {
    printf("--- test_ff_byte_in_payload ---\n");
    reset_all();
    /* Полный газ по UART: 1023 = 0xFF 0x03 — команда, которая раньше
       никогда не доходила, потому что 0xFF означал «буфер пуст». */
    uint8_t payload[2] = { 0xFF, 0x03 };
    uint8_t f[16];
    uint16_t n = build(f, CMD_SET_GAS_VIRTUAL, payload, 2);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_ACK) >= 0, "B-2: команда с байтом 0xFF дошла");
    ASSERT_EQ(1023, svc_pedals_get_gas_uart(), "B-2: и значение 1023 применилось целиком");
}

void test_ff_everywhere_in_payload(void) {
    printf("--- test_ff_everywhere_in_payload ---\n");
    reset_all();
    uint8_t payload[2] = { 0xFF, 0xFF };    /* 65535 — заведомо вне диапазона */
    uint8_t f[16];
    uint16_t n = build(f, CMD_SET_BRAKE_VIRTUAL, payload, 2);
    feed(f, n);
    ASSERT_EQ(1, tx_len > 0, "кадр из одних 0xFF в данных всё равно разобран");
}

/* ====================================================================
 *  Связка с реестром параметров
 * ==================================================================== */

void test_set_param_out_of_range_nacked(void) {
    printf("--- test_set_param_out_of_range_nacked ---\n");
    reset_all();
    /* brake_rate_max профиля ECO = 0 — отключение тормоза */
    uint16_t off = (uint16_t)(offsetof(settings_t, profiles)
                              + DRIVE_MODE_ECO * sizeof(drive_profile_t)
                              + offsetof(drive_profile_t, brake_rate_max));
    uint16_t before = cfg_settings_get_profile(DRIVE_MODE_ECO)->brake_rate_max;

    uint8_t payload[5] = { (uint8_t)(off & 0xFF), (uint8_t)(off >> 8), 2, 0x00, 0x00 };
    uint8_t f[16];
    uint16_t n = build(f, CMD_SET_PARAM, payload, 5);
    feed(f, n);

    int at = find_response(RSP_NACK);
    ASSERT_EQ(1, at >= 0, "S-3 через протокол: попытка обнулить тормоз получила NACK");
    ASSERT_EQ(before, cfg_settings_get_profile(DRIVE_MODE_ECO)->brake_rate_max,
              "S-3 через протокол: настройки не изменились");
}

void test_set_param_valid_acked(void) {
    printf("--- test_set_param_valid_acked ---\n");
    reset_all();
    uint16_t off = (uint16_t)offsetof(settings_t, pedal_gas_min);
    uint8_t payload[5] = { (uint8_t)(off & 0xFF), (uint8_t)(off >> 8), 2, 77, 0 };
    uint8_t f[16];
    uint16_t n = build(f, CMD_SET_PARAM, payload, 5);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_ACK) >= 0, "допустимое значение принято");
    ASSERT_EQ(77, cfg_settings_get()->pedal_gas_min, "и записано");
}

/* ====================================================================
 *  Два кадра подряд
 * ==================================================================== */

void test_two_frames_back_to_back(void) {
    printf("--- test_two_frames_back_to_back ---\n");
    reset_all();
    uint8_t stream[32];
    uint16_t n = build(stream, CMD_PING, NULL, 0);
    n += build(stream + n, CMD_PING, NULL, 0);
    feed(stream, n);

    int count = 0;
    for (uint16_t i = 0; i + 2 < tx_len; i++) {
        if (tx[i] == PROTO_SYNC && tx[i + 2] == RSP_PONG) count++;
    }
    ASSERT_EQ(2, count, "два кадра подряд дали два ответа");
}

int main(void) {
    printf("=========================================\n");
    printf("  app_protocol Unit Tests\n");
    printf("=========================================\n\n");

    test_ping_answered();
    test_unknown_command_nacked();
    test_garbage_before_sync();
    test_bad_crc_dropped_silently();
    test_truncated_frame_costs_the_next_one();
    test_zero_length_rejected();
    test_oversized_length_rejected();
    test_ff_byte_in_payload();
    test_ff_everywhere_in_payload();
    test_set_param_out_of_range_nacked();
    test_set_param_valid_acked();
    test_two_frames_back_to_back();

    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
