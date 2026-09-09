/**
 * @file svc_pedals.h
 * @brief Сервис педалей: физические (АЦП) + виртуальные (UART) с комбинатором
 *
 * МОДЕЛЬ:
 * 
 *   physical_gas   ─┐
 *                   ├─> combinator() ─> effective_gas   ─> ramp/motor
 *   uart_gas       ─┘
 *
 *   physical_brake ─┐
 *                   ├─> combinator() ─> effective_brake ─> ramp/motor
 *   uart_brake     ─┘
 *
 * UART-педали действуют как "второй комплект педалей". Когда UART-значение
 * освобождается (или watchdog отсчитал uart_pedal_timeout_ms без команд),
 * эффективное значение возвращается к физической педали.
 *
 * КОМБИНАТОРЫ (настраиваются в cfg_settings.gas_combinator / brake_combinator):
 *   MAX             — max(physical, uart) — родитель может добавить тормоза/газа поверх ребёнка
 *   ADDITIVE_CLAMP  — clamp(physical + uart, 0, 1023)
 *   UART_PRIORITY   — uart если активен, иначе physical (старая бинарная модель)
 *   PHYSICAL_ONLY   — игнорировать uart (для отладки)
 *   UART_ONLY       — игнорировать physical (для отладки)
 *
 * @version 2.0.0 (MVP-1 + Layered Pedals)
 */
#pragma once
#include <stdint.h>

/* ====================================================================
 *  Типы
 * ==================================================================== */

typedef enum {
    PEDAL_COMBINE_MAX             = 0,
    PEDAL_COMBINE_ADDITIVE_CLAMP  = 1,
    PEDAL_COMBINE_UART_PRIORITY   = 2,
    PEDAL_COMBINE_PHYSICAL_ONLY   = 3,
    PEDAL_COMBINE_UART_ONLY       = 4,
    PEDAL_COMBINE_COUNT
} pedal_combinator_t;

/* ====================================================================
 *  Жизненный цикл
 * ==================================================================== */

void svc_pedals_init(void);
void svc_pedals_update(void);  /**< Вызывать 100 Гц */

/* ====================================================================
 *  Эффективные (комбинированные) значения — для рампы и моторов
 * ==================================================================== */

/** @return эффективный газ (после комбинатора), 0–1023 */
uint16_t svc_pedals_get_gas(void);

/** @return эффективный тормоз (после комбинатора), 0–1023 */
uint16_t svc_pedals_get_brake(void);

/**
 * @brief Целевой PWM с учётом тормоза и режима
 * @param max_pwm        Максимальный PWM текущего режима
 * @param motor_deadzone Мёртвая зона мотора
 * @return 0..max_pwm
 */
uint16_t svc_pedals_get_target_pwm(uint16_t max_pwm, uint8_t motor_deadzone);

uint8_t  svc_pedals_is_brake_active(void);
uint8_t  svc_pedals_is_gas_active(void);

/* ====================================================================
 *  Раздельный доступ к компонентам (для телеметрии)
 * ==================================================================== */

/** Физический газ (только АЦП + фильтр + калибровка + кривая) */
uint16_t svc_pedals_get_gas_physical(void);
uint16_t svc_pedals_get_brake_physical(void);

/** Виртуальный (UART) газ */
uint16_t svc_pedals_get_gas_uart(void);
uint16_t svc_pedals_get_brake_uart(void);

/** RAW значения АЦП (для калибровки) */
uint16_t svc_pedals_get_gas_raw(void);
uint16_t svc_pedals_get_brake_raw(void);

/* ====================================================================
 *  Управление UART-педалями
 * ==================================================================== */

/**
 * @brief Установить виртуальную педаль газа (от UART)
 * Сбрасывается watchdog'ом через uart_pedal_timeout_ms без вызова.
 * @param v 0–1023
 */
void svc_pedals_set_uart_gas(uint16_t v);

/** Установить виртуальную педаль тормоза */
void svc_pedals_set_uart_brake(uint16_t v);

/** Мгновенно сбросить обе UART-педали в 0 */
void svc_pedals_release_uart(void);

/**
 * @brief Статус UART-педалей
 * @return bit 0: gas_uart > 0; bit 1: brake_uart > 0
 */
uint8_t svc_pedals_get_uart_active_flags(void);
