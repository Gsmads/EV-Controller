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
#include "../firmware/util_cobs.h"
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

/** Собрать корректный кадр v3: COBS(cmd|payload|crc) и разделитель. */
static uint16_t build(uint8_t *out, uint8_t cmd, const uint8_t *payload, uint8_t plen) {
    uint8_t packet[PROTO_MAX_PACKET];
    uint8_t n = 0;
    packet[n++] = cmd;
    for (uint8_t i = 0; i < plen; i++) packet[n++] = payload[i];

    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < n; i++) crc = util_crc16_update(crc, packet[i]);
    packet[n++] = (uint8_t)(crc & 0xFF);
    packet[n++] = (uint8_t)(crc >> 8);

    int16_t enc = util_cobs_encode(packet, n, out, PROTO_MAX_ENCODED);
    if (enc < 0) return 0;
    out[enc] = PROTO_DELIMITER;
    return (uint16_t)(enc + 1);
}

/**
 * @brief Разобрать ответ контроллера и найти кадр с указанной командой
 *
 * Ответ разбирается ровно так же, как его разбирал бы веб: поток режется
 * по разделителю, каждая единица раскодируется и проверяется по CRC.
 * Тест не подглядывает в сырые байты — иначе он проверял бы не протокол,
 * а собственные представления о нём.
 *
 * @param cmd     Искомая команда ответа
 * @param out     Куда положить полезную нагрузку (может быть NULL)
 * @param out_len Куда положить её длину (может быть NULL)
 * @return 1 если найден
 */
static int find_response_ex(uint8_t cmd, uint8_t *out, uint8_t *out_len) {
    uint16_t start = 0;
    for (uint16_t i = 0; i < tx_len; i++) {
        if (tx[i] != PROTO_DELIMITER) continue;

        uint16_t unit_len = i - start;
        if (unit_len > 0 && unit_len <= PROTO_MAX_ENCODED) {
            uint8_t dec[PROTO_MAX_PACKET];
            int16_t n = util_cobs_decode(&tx[start], (uint8_t)unit_len, dec, sizeof(dec));
            if (n >= 3) {
                uint8_t body = (uint8_t)n - 2;
                uint16_t crc = 0xFFFF;
                for (uint8_t k = 0; k < body; k++) crc = util_crc16_update(crc, dec[k]);
                uint16_t got = (uint16_t)dec[body] | ((uint16_t)dec[body + 1] << 8);
                if (crc == got && dec[0] == cmd) {
                    if (out_len) *out_len = (uint8_t)(body - 1);
                    if (out) for (uint8_t k = 1; k < body; k++) out[k - 1] = dec[k];
                    return 1;
                }
            }
        }
        start = i + 1;
    }
    return 0;
}

static int find_response(uint8_t cmd) {
    return find_response_ex(cmd, 0, 0) ? 1 : -1;
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
    uint8_t pl[8]; uint8_t pl_len = 0;
    ASSERT_EQ(1, find_response_ex(RSP_NACK, pl, &pl_len),
              "неизвестная команда получила NACK, а не молчание");
    ASSERT_EQ(2, pl_len, "в отказе две байта: команда и код");
    ASSERT_EQ(0x77, pl[0], "отказ ссылается на исходную команду");
    ASSERT_EQ(ERR_UNKNOWN_CMD, pl[1], "код отказа — «неизвестная команда»");
}

/* ====================================================================
 *  Мусор и битые кадры
 * ==================================================================== */

void test_garbage_before_sync(void) {
    printf("--- test_garbage_before_sync ---\n");
    /* Мусор без разделителя — незакрытая единица обмена. Кадр,
       пришедший следом, закрывается своим разделителем и разбирается. */
    reset_all();
    uint8_t noise[5] = { 0x13, 0xFF, 0x7E, 0x01, 0x00 };
    feed(noise, 5);
    uint8_t f[8];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0, "помеха перед кадром не мешает разобрать кадр");
}

void test_bad_crc_dropped_silently(void) {
    printf("--- test_bad_crc_dropped_silently ---\n");
    reset_all();
    uint8_t f[PROTO_MAX_FRAME];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    /* Портим байт внутри кадра, не трогая разделитель: он на месте,
       поэтому граница кадра известна и повреждение локально. */
    f[1] ^= 0x55;
    if (f[1] == PROTO_DELIMITER) f[1] = 0x5A;
    feed(f, n);
    ASSERT_EQ(0, tx_len, "кадр с несошедшимся CRC отброшен без ответа");

    /* И парсер не заклинило */
    tx_len = 0;
    n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0, "следующий кадр разобран нормально");
}

void test_corruption_costs_only_its_own_frame(void) {
    printf("--- test_corruption_costs_only_its_own_frame ---\n");
    /*
     * Суть закрытия T-4 (ADR-0024). Пока разделитель кадра доходит,
     * повреждение остаётся внутри своего кадра: следующий разбирается
     * сразу, без потерь.
     *
     * Прежний формат этого не обеспечивал. Там длина кадра бралась из
     * поля LEN, и испорченный LEN заставлял парсер съесть до 61 чужого
     * байта — то есть повреждение одного байта уносило соседний кадр.
     * Теперь длины в кадре нет вовсе, границу задаёт разделитель,
     * которого внутри кадра не бывает.
     */
    reset_all();
    uint8_t f[PROTO_MAX_FRAME];

    /* Кадр с испорченной серединой: разделитель на месте */
    uint16_t n = build(f, CMD_PING, NULL, 0);
    f[1] ^= 0x7F;
    if (f[1] == PROTO_DELIMITER) f[1] = 0x3C;
    feed(f, n);
    ASSERT_EQ(0, tx_len, "испорченный кадр отброшен");

    /* Следующий кадр — сразу же, без промежуточного потерянного */
    n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0,
              "T-4: соседний кадр цел — повреждение не вышло за свои границы");
}

void test_lost_delimiter_costs_exactly_one_more_frame(void) {
    printf("--- test_lost_delimiter_costs_exactly_one_more_frame ---\n");
    /*
     * Честная граница возможностей COBS. Если передатчик оборвался на
     * середине кадра, разделитель в линию не ушёл — и сказать, что кадр
     * кончился, попросту нечему. Байты обрывка склеиваются со следующим
     * кадром, и он теряется вместе с ними.
     *
     * Что COBS всё же гарантирует: потеря ограничена ровно одним кадром,
     * и рассинхронизация не может продлиться дольше. Второй кадр после
     * обрыва разбирается всегда, при любых данных.
     *
     * Закрыть и этот случай может межкадровый таймаут поверх COBS —
     * молчание дольше времени передачи кадра означает конец кадра. Это
     * отдельное решение, оно не принято.
     */
    reset_all();
    uint8_t f[PROTO_MAX_FRAME];

    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, (uint16_t)(n - 1));             /* всё, кроме разделителя */
    ASSERT_EQ(0, tx_len, "оборванный кадр ответа не вызвал");

    n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(-1, find_response(RSP_PONG),
              "кадр, склеенный с обрывком, теряется — разделителя у обрывка не было");

    n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0,
              "но ровно через один кадр связь восстанавливается, и не позже");
}

void test_recovers_from_noise_in_the_middle_of_a_frame(void) {
    printf("--- test_recovers_from_noise_in_the_middle_of_a_frame ---\n");
    /* Половина кадра, помеха с байтом старой синхронизации 0xAA, затем
       разделитель. Прежний парсер мог принять 0xAA за начало кадра и
       уехать по ложной длине; здесь ложное начало невозможно в принципе. */
    reset_all();
    uint8_t f[PROTO_MAX_FRAME];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, (uint16_t)(n / 2));

    uint8_t noise[4] = { 0xAA, 0x3C, 0x7F, PROTO_DELIMITER };
    feed(noise, 4);

    n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0,
              "после помехи с разделителем следующий кадр разобран сразу");
}

void test_too_short_unit_rejected(void) {
    printf("--- test_too_short_unit_rejected ---\n");
    /* Поля длины в протоколе больше нет — длина следует из границ кадра.
       Осмысленный минимум: команда и два байта CRC. Всё короче — не кадр. */
    reset_all();
    uint8_t tiny[] = { 0x03, 0x11, 0x22, PROTO_DELIMITER };   /* два байта после разбора */
    feed(tiny, 4);
    ASSERT_EQ(0, tx_len, "кадр короче минимума отброшен без ответа");

    uint8_t f[PROTO_MAX_FRAME];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0, "и парсер остался рабочим");
}

void test_empty_unit_is_not_an_error(void) {
    printf("--- test_empty_unit_is_not_an_error ---\n");
    /* Два разделителя подряд — пустая единица. Считать её испорченным
       кадром не за что: в линии просто ничего не было. */
    reset_all();
    uint8_t delims[4] = { PROTO_DELIMITER, PROTO_DELIMITER, PROTO_DELIMITER, PROTO_DELIMITER };
    feed(delims, 4);
    ASSERT_EQ(0, tx_len, "пустые единицы не вызывают ответа");

    uint8_t f[PROTO_MAX_FRAME];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0, "кадр после них разобран");
}

void test_oversized_unit_rejected(void) {
    printf("--- test_oversized_unit_rejected ---\n");
    /* Поток без разделителя длиннее любого возможного кадра. Приёмник
       обязан не переполнить буфер и восстановиться на разделителе. */
    reset_all();
    uint8_t flood[PROTO_MAX_ENCODED * 3];
    for (uint16_t i = 0; i < sizeof(flood); i++) flood[i] = (uint8_t)(i | 1);  /* без нулей */
    feed(flood, sizeof(flood));
    ASSERT_EQ(0, tx_len, "переросшая единица не породила ответа");

    uint8_t delim = PROTO_DELIMITER;
    feed(&delim, 1);

    uint8_t f[PROTO_MAX_FRAME];
    uint16_t n = build(f, CMD_PING, NULL, 0);
    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_PONG) >= 0,
              "после переросшей единицы парсер разбирает следующий кадр");
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
 *  Нулевой байт в данных: то, ради чего взят COBS
 * ==================================================================== */

void test_zero_byte_in_payload(void) {
    printf("--- test_zero_byte_in_payload ---\n");
    /* Разделитель кадра — 0x00, и он же встречается в данных постоянно:
       отпущенная педаль это 0x00 0x00. Кодирование обязано убрать его из
       кадра, иначе команда «отпустить газ» резала бы собственный кадр
       пополам. */
    reset_all();
    uint8_t payload[2] = { 0x00, 0x00 };    /* газ 0 */
    uint8_t f[16];
    uint16_t n = build(f, CMD_SET_GAS_VIRTUAL, payload, 2);

    for (uint16_t i = 0; i + 1 < n; i++) {
        ASSERT_EQ(0, f[i] == PROTO_DELIMITER ? 1 : 0,
                  "внутри кадра нет разделителя, хотя в данных одни нули");
    }
    ASSERT_EQ(PROTO_DELIMITER, f[n - 1], "разделитель стоит только в конце");

    feed(f, n);
    ASSERT_EQ(1, find_response(RSP_ACK) >= 0, "команда с нулями в данных дошла");
    ASSERT_EQ(0, svc_pedals_get_gas_uart(), "и применилась: газ отпущен");
}

void test_zero_bytes_everywhere_in_a_long_payload(void) {
    printf("--- test_zero_bytes_everywhere_in_a_long_payload ---\n");
    /* Длинный кадр, набитый нулями вперемешку со значащими байтами:
       проверяем и отсутствие разделителя внутри, и обещанную прибавку
       ровно в один байт на кадр. */
    reset_all();
    uint8_t payload[PROTO_MAX_PAYLOAD];
    for (uint8_t i = 0; i < PROTO_MAX_PAYLOAD; i++) {
        payload[i] = (uint8_t)((i % 3 == 0) ? 0x00 : i);
    }
    uint8_t f[PROTO_MAX_FRAME];
    uint16_t n = build(f, CMD_SET_PARAM, payload, PROTO_MAX_PAYLOAD);

    ASSERT_EQ(PROTO_MAX_FRAME, n,
              "максимальный кадр занимает ровно PROTO_MAX_FRAME байт");
    int zeros_inside = 0;
    for (uint16_t i = 0; i + 1 < n; i++) if (f[i] == PROTO_DELIMITER) zeros_inside++;
    ASSERT_EQ(0, zeros_inside, "ни одного разделителя внутри самого длинного кадра");

    feed(f, n);
    ASSERT_EQ(1, tx_len > 0, "контроллер ответил — кадр разобран целиком");
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

    /* Считаем ответы так же, как считал бы веб: по разделителям */
    int count = 0;
    uint16_t start = 0;
    for (uint16_t i = 0; i < tx_len; i++) {
        if (tx[i] != PROTO_DELIMITER) continue;
        uint8_t dec[PROTO_MAX_PACKET];
        int16_t d = util_cobs_decode(&tx[start], (uint8_t)(i - start), dec, sizeof(dec));
        if (d >= 3 && dec[0] == RSP_PONG) count++;
        start = i + 1;
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
    test_corruption_costs_only_its_own_frame();
    test_lost_delimiter_costs_exactly_one_more_frame();
    test_recovers_from_noise_in_the_middle_of_a_frame();
    test_too_short_unit_rejected();
    test_empty_unit_is_not_an_error();
    test_oversized_unit_rejected();
    test_ff_byte_in_payload();
    test_ff_everywhere_in_payload();
    test_zero_byte_in_payload();
    test_zero_bytes_everywhere_in_a_long_payload();
    test_set_param_out_of_range_nacked();
    test_set_param_valid_acked();
    test_two_frames_back_to_back();

    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
