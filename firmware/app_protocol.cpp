/**
 * @file app_protocol.cpp
 * @brief Реализация бинарного протокола (v2 — layered pedals)
 *
 * Парсер больше не конечный автомат. Кадры разделяются байтом 0x00,
 * которого внутри кадра не бывает по построению (COBS, ADR-0024), поэтому
 * разбор сводится к «копить до разделителя, раскодировать, проверить CRC».
 * Состояния «жду длину» и «жду N байт данных» исчезли, а вместе с ними и
 * дефект T-4: потеря части потока больше не уносит следующий кадр.
 *
 * Все UART-команды управления педалями идут через svc_pedals.
 * svc_pedals сам управляет watchdog'ом — если команды не приходят,
 * виртуальные педали сами сбрасываются в 0.
 *
 * @version 3.0.0
 */
#include "app_protocol.h"
#include "hal_uart.h"
#include "hal_system.h"
#include "cfg_settings.h"
#include "svc_pedals.h"
#include "svc_speed.h"
#include "util_crc.h"
#include "util_cobs.h"

#include <stddef.h>  /* NULL */

/* ====================================================================
 *  Состояния парсера
 * ==================================================================== */

/* Парсер накапливает байты между разделителями. Состояния не нужны:
   единственное решение принимается на разделителе. */

/* ====================================================================
 *  Внутренние данные
 * ==================================================================== */

/* Приёмный буфер: закодированный кадр без разделителя.
   Раскодирование идёт прямо в нём — выход COBS всегда короче входа,
   а записи попадают в уже прочитанные байты (util_cobs.h). Это экономит
   отдельный буфер на 63 байта, что на 2 КБ ОЗУ заметно. */
static uint8_t  rx_buf[PROTO_MAX_ENCODED];
static uint8_t  rx_len;
static uint8_t  rx_too_long;      /* единица обмена уже длиннее кадра */
static uint16_t rx_bad_frames;

/* ====================================================================
 *  Отправка пакетов
 * ==================================================================== */

static void send_packet(uint8_t cmd, const uint8_t *payload, uint8_t payload_len)
{
    if (payload_len > PROTO_MAX_PAYLOAD) {
        return;                 /* кадр такой длины протоколом не предусмотрен */
    }

    /* Буферы статические, а не на стеке: вместе это 128 байт, заметная
       доля стека AVR. Функция не реентерантна и из обработчиков прерываний
       не вызывается, поэтому один комплект на всех безопасен. */
    static uint8_t packet[PROTO_MAX_PACKET];
    static uint8_t frame[PROTO_MAX_FRAME];

    uint8_t n = 0;
    packet[n++] = cmd;
    for (uint8_t i = 0; i < payload_len; i++) {
        packet[n++] = payload[i];
    }

    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < n; i++) {
        crc = util_crc16_update(crc, packet[i]);
    }
    packet[n++] = (uint8_t)(crc & 0xFF);
    packet[n++] = (uint8_t)(crc >> 8);

    int16_t enc = util_cobs_encode(packet, n, frame, PROTO_MAX_ENCODED);
    if (enc < 0) {
        /* Недостижимо: PROTO_MAX_ENCODED посчитан из PROTO_MAX_PACKET с
           учётом прибавки COBS. Молчать всё равно нельзя. */
        rx_bad_frames++;
        return;
    }
    frame[enc] = PROTO_DELIMITER;

    /* Кадр уходит одним блоком: решение об отбрасывании принимается там,
       где известна его длина (ADR-0016). Побайтовая отправка при нехватке
       места оставила бы в линии обрывок. */
    (void)hal_uart_write_buf(frame, (uint8_t)(enc + 1));
}

static void send_ack(uint8_t original_cmd)
{
    send_packet(RSP_ACK, &original_cmd, 1);
}

static void send_nack(uint8_t original_cmd, uint8_t error_code)
{
    uint8_t payload[2] = { original_cmd, error_code };
    send_packet(RSP_NACK, payload, 2);
}

/* ====================================================================
 *  Обработчики команд
 * ==================================================================== */

static void handle_set_gas_virtual(const uint8_t *payload, uint8_t len)
{
    if (len < 2) { send_nack(CMD_SET_GAS_VIRTUAL, ERR_BAD_PAYLOAD); return; }
    uint16_t v = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    svc_pedals_set_uart_gas(v);
    send_ack(CMD_SET_GAS_VIRTUAL);
}

static void handle_set_brake_virtual(const uint8_t *payload, uint8_t len)
{
    if (len < 2) { send_nack(CMD_SET_BRAKE_VIRTUAL, ERR_BAD_PAYLOAD); return; }
    uint16_t v = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    svc_pedals_set_uart_brake(v);
    send_ack(CMD_SET_BRAKE_VIRTUAL);
}

static void handle_set_mode(const uint8_t *payload, uint8_t len)
{
    if (len < 1) { send_nack(CMD_SET_MODE, ERR_BAD_PAYLOAD); return; }
    if (payload[0] >= DRIVE_MODE_COUNT) {
        send_nack(CMD_SET_MODE, ERR_BAD_MODE);
        return;
    }
    /* TODO: интеграция с svc_drive_mode в MVP-3+ */
    send_ack(CMD_SET_MODE);
}

static void handle_emergency_stop(void)
{
    /* Мягкая аварийная остановка:
     *   gas_uart  = 0 (снять любой газ)
     *   brake_uart= 1023 (полный тормоз)
     * Физическая педаль газа всё ещё может быть нажата, но тормоз через
     * комбинатор MAX переопределит её. Рампа сама плавно сбросит PWM. */
    svc_pedals_set_uart_gas(0);
    svc_pedals_set_uart_brake(1023);
    send_ack(CMD_EMERGENCY_STOP);
}

static void handle_release_control(void)
{
    /* Сброс обеих UART-педалей в 0 → управление возвращается к физическим */
    svc_pedals_release_uart();
    send_ack(CMD_RELEASE_CTRL);
}

static void handle_set_param(const uint8_t *payload, uint8_t len)
{
    if (len < 4) { send_nack(CMD_SET_PARAM, ERR_BAD_PAYLOAD); return; }

    uint16_t offset = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    uint8_t  size   = payload[2];

    if (size == 0 || size > 4 || (3 + size) > len) {
        send_nack(CMD_SET_PARAM, ERR_BAD_PAYLOAD);
        return;
    }

    uint8_t result = cfg_settings_set_field(offset, &payload[3], size);
    if (result != 0) {
        send_nack(CMD_SET_PARAM, ERR_PARAM_RANGE);
        return;
    }

    send_ack(CMD_SET_PARAM);
}

static void handle_save_settings(void)
{
    cfg_settings_save();
    send_ack(CMD_SAVE_SETTINGS);
}

static void handle_reset_defaults(void)
{
    cfg_settings_reset_defaults();
    send_ack(CMD_RESET_DEFAULTS);
}

static void handle_reset_odometer(void)
{
    svc_speed_reset_odometer();
    send_ack(CMD_RESET_ODOMETER);
}

/* ====================================================================
 *  Диспетчер команд
 * ==================================================================== */

static void dispatch_packet(const uint8_t *packet, uint8_t len)
{
    uint8_t cmd = packet[0];
    const uint8_t *payload = &packet[1];
    uint8_t payload_len = len - 1;

    switch (cmd) {
        case CMD_SET_GAS_VIRTUAL:   handle_set_gas_virtual(payload, payload_len);   break;
        case CMD_SET_BRAKE_VIRTUAL: handle_set_brake_virtual(payload, payload_len); break;
        case CMD_SET_MODE:          handle_set_mode(payload, payload_len);          break;
        case CMD_EMERGENCY_STOP:    handle_emergency_stop();                         break;
        case CMD_RELEASE_CTRL:      handle_release_control();                        break;
        case CMD_SET_PARAM:         handle_set_param(payload, payload_len);         break;
        case CMD_SAVE_SETTINGS:     handle_save_settings();                          break;
        case CMD_RESET_DEFAULTS:    handle_reset_defaults();                         break;
        case CMD_RESET_ODOMETER:    handle_reset_odometer();                         break;
        case CMD_GET_TELEMETRY:     send_ack(CMD_GET_TELEMETRY);                    break;
        case CMD_PING:              send_packet(RSP_PONG, NULL, 0);                 break;
        default:                    send_nack(cmd, ERR_UNKNOWN_CMD);                break;
    }
}

/* ====================================================================
 *  Парсер
 *
 *  Кадры разделяются байтом 0x00. Внутри кадра его не бывает: COBS
 *  убирает нули из данных (ADR-0024). Поэтому разбор состоит из двух
 *  правил, и ни одно из них ничего не угадывает:
 *    - не разделитель -> дописать в буфер;
 *    - разделитель    -> попытаться разобрать накопленное.
 *
 *  Отсюда и восстановление после сбоя: что бы ни пришло в линию, первый
 *  же 0x00 закрывает испорченную единицу, и следующий кадр разбирается
 *  с чистого места. Прежний парсер искал начало по байту 0xAA, который
 *  встречается в данных, и после оборванного кадра терял следующий
 *  (дефект T-4).
 * ==================================================================== */

static void parser_reset(void)
{
    rx_len = 0;
    rx_too_long = 0;
}

/**
 * @brief Разобрать накопленную единицу обмена
 *
 * Раскодирование идёт на месте, в rx_buf: выход COBS всегда короче входа.
 */
static void parser_take_unit(void)
{
    int16_t n = util_cobs_decode(rx_buf, rx_len, rx_buf, sizeof(rx_buf));

    /* Минимум осмысленного кадра — команда и две байта CRC */
    if (n < 3) {
        rx_bad_frames++;
        return;
    }

    uint8_t body = (uint8_t)n - 2;          /* команда и полезная нагрузка */

    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < body; i++) {
        crc = util_crc16_update(crc, rx_buf[i]);
    }
    uint16_t crc_received = (uint16_t)rx_buf[body] |
                            ((uint16_t)rx_buf[body + 1] << 8);

    if (crc != crc_received) {
        rx_bad_frames++;
        return;
    }

    dispatch_packet(rx_buf, body);
}

static void parser_feed(uint8_t byte)
{
    if (byte == PROTO_DELIMITER) {
        if (rx_too_long) {
            /* Единица обмена не могла быть кадром: она длиннее любого
               возможного. Разделитель её закрывает — следующий кадр
               начинается с чистого места. */
            rx_bad_frames++;
        } else if (rx_len > 0) {
            parser_take_unit();
        }
        /* rx_len == 0 означает два разделителя подряд: пустая единица,
           считать её испорченным кадром не за что. */
        parser_reset();
        return;
    }

    if (rx_too_long) return;                /* ждём разделителя */

    if (rx_len >= sizeof(rx_buf)) {
        rx_too_long = 1;
        return;
    }
    rx_buf[rx_len++] = byte;
}

/* ====================================================================
 *  Публичный API
 * ==================================================================== */

void app_protocol_init(void)
{
    parser_reset();
    rx_bad_frames = 0;
}

uint16_t app_protocol_bad_frames(void)
{
    return rx_bad_frames;
}

void app_protocol_update(void)
{
    /* До 16 байт за вызов, чтобы не блокировать */
    uint8_t max_bytes = 16;
    while (max_bytes-- > 0) {
        int16_t byte = hal_uart_read();
        if (byte < 0) break;    /* B-2: пустота непредставима в данных */
        parser_feed((uint8_t)byte);
    }
}

void app_protocol_send_telemetry(const telemetry_packet_t *telem)
{
    send_packet(RSP_TELEMETRY, (const uint8_t *)telem, sizeof(telemetry_packet_t));
}
