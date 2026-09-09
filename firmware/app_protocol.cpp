/**
 * @file app_protocol.cpp
 * @brief Реализация бинарного протокола (v2 — layered pedals)
 *
 * Парсер — конечный автомат:
 *   WAIT_SYNC → WAIT_LEN → WAIT_DATA → WAIT_CRC_L → WAIT_CRC_H → DISPATCH
 *
 * Все UART-команды управления педалями идут через svc_pedals.
 * svc_pedals сам управляет watchdog'ом — если команды не приходят,
 * виртуальные педали сами сбрасываются в 0.
 *
 * @version 2.0.0
 */
#include "app_protocol.h"
#include "hal_uart.h"
#include "hal_system.h"
#include "cfg_settings.h"
#include "svc_pedals.h"
#include "svc_speed.h"
#include "util_crc.h"

/* ====================================================================
 *  Состояния парсера
 * ==================================================================== */

typedef enum {
    PS_WAIT_SYNC,
    PS_WAIT_LEN,
    PS_WAIT_DATA,
    PS_WAIT_CRC_L,
    PS_WAIT_CRC_H
} parser_state_t;

/* ====================================================================
 *  Внутренние данные
 * ==================================================================== */

static parser_state_t parser_state;
static uint8_t  pkt_buf[PROTO_MAX_PAYLOAD + 2];
static uint8_t  pkt_len;
static uint8_t  pkt_idx;
static uint16_t pkt_crc_received;

/* ====================================================================
 *  Отправка пакетов
 * ==================================================================== */

static void send_packet(uint8_t cmd, const uint8_t *payload, uint8_t payload_len)
{
    uint8_t len = 1 + payload_len;

    uint16_t crc = 0xFFFF;
    crc = util_crc16_update(crc, len);
    crc = util_crc16_update(crc, cmd);
    for (uint8_t i = 0; i < payload_len; i++) {
        crc = util_crc16_update(crc, payload[i]);
    }

    hal_uart_write(PROTO_SYNC);
    hal_uart_write(len);
    hal_uart_write(cmd);
    for (uint8_t i = 0; i < payload_len; i++) {
        hal_uart_write(payload[i]);
    }
    hal_uart_write((uint8_t)(crc & 0xFF));
    hal_uart_write((uint8_t)(crc >> 8));
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

static void dispatch_packet(void)
{
    uint8_t cmd = pkt_buf[0];
    const uint8_t *payload = &pkt_buf[1];
    uint8_t payload_len = pkt_len - 1;

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
 *  Парсер — конечный автомат
 * ==================================================================== */

static void parser_reset(void)
{
    parser_state = PS_WAIT_SYNC;
    pkt_len = 0;
    pkt_idx = 0;
}

static void parser_feed(uint8_t byte)
{
    switch (parser_state) {
        case PS_WAIT_SYNC:
            if (byte == PROTO_SYNC) parser_state = PS_WAIT_LEN;
            break;

        case PS_WAIT_LEN:
            if (byte == 0 || byte > PROTO_MAX_PAYLOAD + 1) {
                parser_reset();
            } else {
                pkt_len = byte;
                pkt_idx = 0;
                parser_state = PS_WAIT_DATA;
            }
            break;

        case PS_WAIT_DATA:
            pkt_buf[pkt_idx++] = byte;
            if (pkt_idx >= pkt_len) parser_state = PS_WAIT_CRC_L;
            break;

        case PS_WAIT_CRC_L:
            pkt_crc_received = byte;
            parser_state = PS_WAIT_CRC_H;
            break;

        case PS_WAIT_CRC_H: {
            pkt_crc_received |= ((uint16_t)byte << 8);

            uint16_t crc = 0xFFFF;
            crc = util_crc16_update(crc, pkt_len);
            for (uint8_t i = 0; i < pkt_len; i++) {
                crc = util_crc16_update(crc, pkt_buf[i]);
            }

            if (crc == pkt_crc_received) {
                dispatch_packet();
            }
            /* CRC mismatch → молча отбрасываем */

            parser_reset();
            break;
        }
    }
}

/* ====================================================================
 *  Публичный API
 * ==================================================================== */

void app_protocol_init(void)
{
    parser_reset();
}

void app_protocol_update(void)
{
    /* До 16 байт за вызов, чтобы не блокировать */
    uint8_t max_bytes = 16;
    while (max_bytes-- > 0) {
        uint8_t byte = hal_uart_read();
        if (byte == HAL_UART_NO_DATA) break;
        parser_feed(byte);
    }
}

void app_protocol_send_telemetry(const telemetry_packet_t *telem)
{
    send_packet(RSP_TELEMETRY, (const uint8_t *)telem, sizeof(telemetry_packet_t));
}
