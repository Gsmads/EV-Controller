/**
 * @file app_protocol.h
 * @brief Бинарный протокол управления (UART / RS485)
 *
 * Формат пакета:
 *   [0xAA] [LEN] [CMD] [PAYLOAD...] [CRC16_L] [CRC16_H]
 *
 *   SYNC:    0xAA
 *   LEN:     длина CMD + PAYLOAD (1..60)
 *   CMD:     идентификатор команды
 *   PAYLOAD: 0..59 байт
 *   CRC16:   CRC16-CCITT по LEN + CMD + PAYLOAD, little-endian
 *
 * МОДЕЛЬ УПРАВЛЕНИЯ (v2 — layered pedals):
 *   svc_pedals хранит physical (АЦП) И uart (виртуальные) значения.
 *   Комбинатор (cfg_settings.gas/brake_combinator) объединяет их.
 *   По умолчанию MAX → родитель может «доложить» поверх ребёнка.
 *
 *   SET_GAS_VIRTUAL / SET_BRAKE_VIRTUAL — основные команды управления.
 *   Watchdog: если команда не приходит uart_pedal_timeout_ms (200мс) —
 *   виртуальная педаль сбрасывается в 0 автоматически.
 *
 * @version 2.0.0
 */
#pragma once
#include <stdint.h>

/* ====================================================================
 *  Константы
 * ==================================================================== */

#define PROTO_SYNC          0xAA
#define PROTO_MAX_PAYLOAD   60

/* --- Команды Host → Controller --- */
#define CMD_SET_GAS_VIRTUAL    0x01    /* uint16: виртуальный газ 0–1023 */
#define CMD_SET_BRAKE_VIRTUAL  0x02    /* uint16: виртуальный тормоз 0–1023 */
#define CMD_SET_MODE           0x03    /* uint8:  drive_mode_id_t */
#define CMD_EMERGENCY_STOP     0x04    /* —: мягкая аварийная остановка */
#define CMD_RELEASE_CTRL       0x05    /* —: сбросить обе UART-педали в 0 */

#define CMD_SET_PARAM          0x10    /* u16 offset + u8 size + data */
#define CMD_SAVE_SETTINGS      0x11    /* —: сохранить в EEPROM */
#define CMD_RESET_DEFAULTS     0x12    /* —: сброс к заводским */
#define CMD_RESET_ODOMETER     0x13    /* —: сброс одометра */

#define CMD_GET_TELEMETRY      0x20    /* —: запрос одного пакета */
#define CMD_SET_TELEM_RATE     0x21    /* uint8: авто-телеметрия (0=off, 1–50 Гц) */
#define CMD_PING               0xFE    /* —: keepalive */

/* --- Ответы Controller → Host --- */
#define RSP_ACK             0x80
#define RSP_NACK            0x81
#define RSP_TELEMETRY       0x82
#define RSP_PONG            0x83

/* --- Коды ошибок --- */
#define ERR_UNKNOWN_CMD     0x01
#define ERR_BAD_PAYLOAD     0x02
#define ERR_BAD_MODE        0x03
#define ERR_PARAM_RANGE     0x04

/* ====================================================================
 *  Структура телеметрии (расширенная v2)
 *
 *  Размер: 36 байт
 * ==================================================================== */

typedef struct __attribute__((packed)) {
    /* Педали — раздельно по источникам */
    uint16_t gas_physical;       /* Физическая педаль газа (АЦП), 0–1023 */
    uint16_t gas_uart;           /* Виртуальный газ (UART), 0–1023 */
    uint16_t gas_effective;      /* После комбинатора, 0–1023 */
    uint16_t brake_physical;
    uint16_t brake_uart;
    uint16_t brake_effective;

    /* PWM */
    uint16_t target_pwm;         /* Целевой PWM (0–1023) */
    uint16_t current_pwm;        /* Текущий PWM после рампы (0–1023) */
    uint16_t pwm_freq_hz;        /* Частота ШИМ */

    /* Скорость */
    uint16_t speed_rpm_l;        /* RPM левого колеса */
    uint16_t speed_rpm_r;        /* RPM правого колеса */
    uint16_t speed_kmh_x10;      /* км/ч × 10 (XX.X) */

    /* Токи (заглушка до MVP-3) */
    uint16_t current_ma_l;
    uint16_t current_ma_r;

    /* Статус */
    uint8_t  drive_mode;         /* drive_mode_id_t */
    uint8_t  uart_active_flags;  /* bit 0: gas_uart>0, bit 1: brake_uart>0 */
    uint16_t faults;             /* Битовое поле ошибок */
    uint32_t uptime_ms;          /* Время работы */
} telemetry_packet_t;

/* ====================================================================
 *  API
 * ==================================================================== */

/** Инициализация парсера */
void app_protocol_init(void);

/**
 * @brief Обработка входящих байтов (вызывать 100 Гц)
 * Неблокирующий: до 16 байт за вызов.
 */
void app_protocol_update(void);

/** Отправить пакет телеметрии */
void app_protocol_send_telemetry(const telemetry_packet_t *telem);
