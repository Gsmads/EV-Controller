/**
 * @file svc_pedals.cpp
 * @brief Реализация педалей с layered physical + uart model
 *
 * @version 2.0.0 (MVP-1 + Layered Pedals)
 */
#include "svc_pedals.h"
#include "hal_adc.h"
#include "hal_system.h"
#include "cfg_board.h"
#include "cfg_settings.h"
#include "util_math.h"

/* ====================================================================
 *  Внутренние структуры
 * ==================================================================== */

typedef struct {
    /* Физическая педаль (от АЦП) */
    uint8_t  adc_channel;
    uint16_t raw;
    int32_t  ema_fp;
    uint16_t physical;     /* После всех преобразований, 0–1023 */

    /* UART-педаль (виртуальная) */
    uint16_t uart;         /* 0–1023 */
    uint32_t uart_last_ms; /* Время последнего set — для watchdog */
} pedal_t;

static pedal_t gas;
static pedal_t brake;

/* ====================================================================
 *  Вспомогательные функции
 * ==================================================================== */

/**
 * @brief Обработка одной педали: АЦП → EMA → калибровка → deadzone → кривая
 */
static void update_physical(pedal_t *p, uint16_t raw_min, uint16_t raw_max,
                            uint8_t deadzone, uint8_t curve_type, uint8_t ema_alpha)
{
    p->raw = hal_adc_read(p->adc_channel);
    p->ema_fp = util_ema_update(p->ema_fp, p->raw, ema_alpha);
    uint16_t filtered = util_ema_extract(p->ema_fp);

    uint16_t norm = util_map_u16(filtered, raw_min, raw_max, 0, 1023);

    if (norm <= (uint16_t)deadzone) {
        norm = 0;
    } else {
        uint32_t effective_range = 1023 - (uint32_t)deadzone;
        norm = (uint16_t)(((uint32_t)(norm - deadzone) * 1023UL) / effective_range);
    }

    if (curve_type < CURVE_COUNT) {
        norm = util_curve_apply(norm, (response_curve_t)curve_type);
    }

    p->physical = norm;
}

/**
 * @brief Применить комбинатор к двум значениям
 */
static uint16_t combine(uint16_t physical, uint16_t uart, pedal_combinator_t mode)
{
    switch (mode) {
        case PEDAL_COMBINE_MAX:
            return (physical > uart) ? physical : uart;

        case PEDAL_COMBINE_ADDITIVE_CLAMP: {
            uint32_t sum = (uint32_t)physical + (uint32_t)uart;
            return (sum > 1023) ? 1023 : (uint16_t)sum;
        }

        case PEDAL_COMBINE_UART_PRIORITY:
            return (uart > 0) ? uart : physical;

        case PEDAL_COMBINE_PHYSICAL_ONLY:
            return physical;

        case PEDAL_COMBINE_UART_ONLY:
            return uart;

        default:
            return (physical > uart) ? physical : uart;
    }
}

/**
 * @brief Watchdog: если UART-педаль не обновлялась — сбросить в 0
 */
static void apply_watchdog(pedal_t *p, uint32_t now, uint16_t timeout_ms)
{
    if (p->uart > 0 && (now - p->uart_last_ms) > timeout_ms) {
        p->uart = 0;
    }
}

/* ====================================================================
 *  Публичный API: lifecycle
 * ==================================================================== */

void svc_pedals_init(void)
{
    gas.adc_channel = PIN_PEDAL_GAS;
    brake.adc_channel = PIN_PEDAL_BRAKE;

    gas.raw = 0;
    brake.raw = 0;
    gas.physical = 0;
    brake.physical = 0;
    gas.uart = 0;
    brake.uart = 0;
    gas.uart_last_ms = 0;
    brake.uart_last_ms = 0;

    /* Первичное чтение для инициализации EMA */
    uint16_t g = hal_adc_read(gas.adc_channel);
    uint16_t b = hal_adc_read(brake.adc_channel);
    gas.ema_fp   = util_ema_init(g);
    brake.ema_fp = util_ema_init(b);
}

void svc_pedals_update(void)
{
    const settings_t *s = cfg_settings_get();

    /* Физические педали */
    update_physical(&gas,
                    s->pedal_gas_min, s->pedal_gas_max,
                    s->pedal_gas_deadzone, s->pedal_gas_curve,
                    s->pedal_ema_alpha);

    update_physical(&brake,
                    s->pedal_brake_min, s->pedal_brake_max,
                    s->pedal_brake_deadzone, s->pedal_brake_curve,
                    s->pedal_ema_alpha);

    /* Watchdog UART-педалей */
    uint32_t now = hal_system_millis();
    uint16_t timeout = s->uart_pedal_timeout_ms;
    if (timeout == 0) timeout = 200;  /* Default safety */
    apply_watchdog(&gas, now, timeout);
    apply_watchdog(&brake, now, timeout);
}

/* ====================================================================
 *  Публичный API: эффективные значения
 * ==================================================================== */

uint16_t svc_pedals_get_gas(void)
{
    const settings_t *s = cfg_settings_get();
    return combine(gas.physical, gas.uart, (pedal_combinator_t)s->gas_combinator);
}

uint16_t svc_pedals_get_brake(void)
{
    const settings_t *s = cfg_settings_get();
    return combine(brake.physical, brake.uart, (pedal_combinator_t)s->brake_combinator);
}

uint8_t svc_pedals_is_gas_active(void)   { return (svc_pedals_get_gas() > 0) ? 1 : 0; }
uint8_t svc_pedals_is_brake_active(void) { return (svc_pedals_get_brake() > 0) ? 1 : 0; }

uint16_t svc_pedals_get_target_pwm(uint16_t max_pwm, uint8_t motor_deadzone)
{
    /* §6.2: тормоз имеет ПРИОРИТЕТ над газом */
    if (svc_pedals_is_brake_active()) return 0;

    uint16_t g = svc_pedals_get_gas();
    if (g == 0) return 0;

    uint16_t target = (uint16_t)(((uint32_t)g * max_pwm) / 1023UL);

    if (target > 0 && target < motor_deadzone) {
        target = motor_deadzone;
    }

    return target;
}

/* ====================================================================
 *  Публичный API: компоненты
 * ==================================================================== */

uint16_t svc_pedals_get_gas_physical(void)   { return gas.physical; }
uint16_t svc_pedals_get_brake_physical(void) { return brake.physical; }
uint16_t svc_pedals_get_gas_uart(void)       { return gas.uart; }
uint16_t svc_pedals_get_brake_uart(void)     { return brake.uart; }
uint16_t svc_pedals_get_gas_raw(void)        { return gas.raw; }
uint16_t svc_pedals_get_brake_raw(void)      { return brake.raw; }

/* ====================================================================
 *  Публичный API: UART-педали
 * ==================================================================== */

void svc_pedals_set_uart_gas(uint16_t v)
{
    if (v > 1023) v = 1023;
    gas.uart = v;
    gas.uart_last_ms = hal_system_millis();
}

void svc_pedals_set_uart_brake(uint16_t v)
{
    if (v > 1023) v = 1023;
    brake.uart = v;
    brake.uart_last_ms = hal_system_millis();
}

void svc_pedals_release_uart(void)
{
    gas.uart = 0;
    brake.uart = 0;
}

uint8_t svc_pedals_get_uart_active_flags(void)
{
    uint8_t flags = 0;
    if (gas.uart > 0)   flags |= 0x01;
    if (brake.uart > 0) flags |= 0x02;
    return flags;
}
