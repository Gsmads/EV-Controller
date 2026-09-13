/**
 * @file cfg_params.cpp
 * @brief Таблицы реестра параметров
 *
 * Обоснование границ — в шапке cfg_params.h. Смещения берутся через
 * offsetof, а не переписываются числами: поле, переехавшее внутри
 * структуры, не может рассинхронизироваться с таблицей.
 */
#include "cfg_params.h"
#include "util_rom.h"
#include "svc_pedals.h"   /* PEDAL_COMBINE_COUNT */

#include <stddef.h>       /* offsetof */

#define S(field)  ((uint16_t)offsetof(settings_t, field))
#define P(field)  ((uint16_t)offsetof(drive_profile_t, field))

/* Границы, вытекающие из устройства системы. */
#define ADC_RAW_MAX        1023   /* АЦП десятибитный */
#define PWM_MAX            1023   /* ШИМ десятибитный */
#define CURVE_MAX          2      /* pedal_curve_t: linear, quadratic, s-curve */
#define PERCENT_MAX        100
#define CURRENT_MAX_MA     20000  /* ACS712-20A меряет до 20 А */
/* Частота ШИМ: F_CPU / (делитель × (TOP+1)). При девяти битах и делителе 1
   это 31250 Гц, при десяти битах и делителе 1024 — 15,26 Гц. Границы взяты
   как объединение достижимого, а не как «разумный диапазон». */
#define PWM_FREQ_MIN_HZ    15
#define PWM_FREQ_MAX_HZ    31250

/* ====================================================================
 *  Поля профиля режима. Описаны один раз на все девять профилей.
 * ==================================================================== */

static const param_desc_t UTIL_ROM profile_params[] = {
    /* id, смещение в drive_profile_t, тип, флаги, min, max */
    { 0x0101, P(max_pwm),        PARAM_U16, PARAM_FLAG_NONE,     0, PWM_MAX },
    /* Темп, равный нулю, означает «никогда»: разгон не начнётся,
       замедление не произойдёт, торможение не сработает. Для тормоза
       это прямо дефект S-3, ради которого реестр и заводился. */
    { 0x0102, P(accel_rate),     PARAM_U16, PARAM_FLAG_TYPE_MAX, 1, 65535 },
    { 0x0103, P(decel_rate),     PARAM_U16, PARAM_FLAG_TYPE_MAX, 1, 65535 },
    { 0x0104, P(brake_rate_min), PARAM_U16, PARAM_FLAG_TYPE_MAX, 1, 65535 },
    { 0x0105, P(brake_rate_max), PARAM_U16, PARAM_FLAG_TYPE_MAX, 1, 65535 },
    { 0x0106, P(pwm_freq_hz),    PARAM_U16, PARAM_FLAG_NONE,
                                 PWM_FREQ_MIN_HZ, PWM_FREQ_MAX_HZ },
    { 0x0107, P(pwm_resolution), PARAM_U8,  PARAM_FLAG_NONE,     9, 10 },
    { 0x0108, P(direction),      PARAM_U8,  PARAM_FLAG_NONE,     0, 1 },
    /* 0xFF означает «брать общую кривую», поэтому допустимо сверх диапазона. */
    { 0x0109, P(pedal_curve),    PARAM_U8,  PARAM_FLAG_ALLOW_FF, 0, CURVE_MAX },
    { 0x010A, P(_pad),           PARAM_U8,  PARAM_FLAG_READONLY, 0, 0 },
};

#define PROFILE_PARAM_COUNT ((uint8_t)(sizeof(profile_params) / sizeof(profile_params[0])))

/* ====================================================================
 *  Одиночные поля settings_t
 * ==================================================================== */

static const param_desc_t UTIL_ROM scalar_params[] = {
    /* --- Калибровка педалей: сырые значения АЦП --- */
    { 0x0201, S(pedal_gas_min),         PARAM_U16, PARAM_FLAG_NONE, 0, ADC_RAW_MAX },
    { 0x0202, S(pedal_gas_max),         PARAM_U16, PARAM_FLAG_NONE, 0, ADC_RAW_MAX },
    { 0x0203, S(pedal_brake_min),       PARAM_U16, PARAM_FLAG_NONE, 0, ADC_RAW_MAX },
    { 0x0204, S(pedal_brake_max),       PARAM_U16, PARAM_FLAG_NONE, 0, ADC_RAW_MAX },
    { 0x0205, S(pedal_gas_deadzone),    PARAM_U8,  PARAM_FLAG_NONE, 0, PERCENT_MAX },
    { 0x0206, S(pedal_brake_deadzone),  PARAM_U8,  PARAM_FLAG_NONE, 0, PERCENT_MAX },
    { 0x0207, S(pedal_gas_curve),       PARAM_U8,  PARAM_FLAG_NONE, 0, CURVE_MAX },
    { 0x0208, S(pedal_brake_curve),     PARAM_U8,  PARAM_FLAG_NONE, 0, CURVE_MAX },
    /* Коэффициент ноль останавливает фильтр: значение никогда не обновится,
       то есть педаль замрёт в последнем прочитанном состоянии. */
    { 0x0209, S(pedal_ema_alpha),       PARAM_U8,  PARAM_FLAG_NONE, 1, 255 },
    { 0x020A, S(_pad1),                 PARAM_U8,  PARAM_FLAG_READONLY, 0, 0 },

    /* --- PID руля. Физической границы нет до замеров на железе --- */
    { 0x0301, S(eps_kp),      PARAM_I16, PARAM_FLAG_TYPE_MAX, -32768, 32767 },
    { 0x0302, S(eps_ki),      PARAM_I16, PARAM_FLAG_TYPE_MAX, -32768, 32767 },
    { 0x0303, S(eps_kd),      PARAM_I16, PARAM_FLAG_TYPE_MAX, -32768, 32767 },
    { 0x0304, S(eps_center),  PARAM_U16, PARAM_FLAG_NONE,     0, ADC_RAW_MAX },

    /* --- Защита по току: предел задаёт датчик --- */
    { 0x0401, S(current_limit_soft_ma), PARAM_U16, PARAM_FLAG_NONE, 0, CURRENT_MAX_MA },
    { 0x0402, S(current_limit_hard_ma), PARAM_U16, PARAM_FLAG_NONE, 0, CURRENT_MAX_MA },
    { 0x0403, S(current_limit_time_ms), PARAM_U16, PARAM_FLAG_TYPE_MAX, 0, 65535 },

    /* --- Таймауты связи. Верхняя граница неизвестна до замеров --- */
    { 0x0501, S(uart_timeout_ms),       PARAM_U16, PARAM_FLAG_TYPE_MAX, 1, 65535 },
    { 0x0502, S(uart_pedal_timeout_ms), PARAM_U16, PARAM_FLAG_TYPE_MAX, 1, 65535 },

    /* --- Электронный дифференциал --- */
    { 0x0601, S(diff_enabled), PARAM_U8, PARAM_FLAG_NONE, 0, 1 },
    { 0x0602, S(diff_gain),    PARAM_U8, PARAM_FLAG_NONE, 0, PERCENT_MAX },

    /* --- Мотор --- */
    { 0x0701, S(motor_deadzone), PARAM_U8, PARAM_FLAG_NONE, 0, 255 },
    { 0x0702, S(_pad2),          PARAM_U8, PARAM_FLAG_READONLY, 0, 0 },

    /* --- Комбинаторы педалей (ADR-0004) --- */
    { 0x0801, S(gas_combinator),   PARAM_U8, PARAM_FLAG_NONE, 0, PEDAL_COMBINE_COUNT - 1 },
    { 0x0802, S(brake_combinator), PARAM_U8, PARAM_FLAG_NONE, 0, PEDAL_COMBINE_COUNT - 1 },

    /* --- UART (ADR-0016, ADR-0019) --- */
    { 0x0901, S(uart_baud_code),  PARAM_U8, PARAM_FLAG_NONE, 0, UART_BAUD_CODE_COUNT - 1 },
    { 0x0902, S(uart_tx_policy),  PARAM_U8, PARAM_FLAG_NONE, 0, 1 },
    /* Границы испытательного периода названы в самом ADR-0019. */
    { 0x0903, S(uart_baud_probation_ms), PARAM_U16, PARAM_FLAG_NONE, 2000, 60000 },

    /* --- Карта каналов АЦП (ADR-0009, ADR-0020) --- */
    { 0x0A01, S(adc_ch_pedal_gas),     PARAM_U8, PARAM_FLAG_NONE, 0, ADC_CHANNEL_COUNT - 1 },
    { 0x0A02, S(adc_ch_pedal_brake),   PARAM_U8, PARAM_FLAG_NONE, 0, ADC_CHANNEL_COUNT - 1 },
    { 0x0A03, S(adc_ch_current_right), PARAM_U8, PARAM_FLAG_NONE, 0, ADC_CHANNEL_COUNT - 1 },
    { 0x0A04, S(adc_ch_current_left),  PARAM_U8, PARAM_FLAG_NONE, 0, ADC_CHANNEL_COUNT - 1 },
    { 0x0A05, S(adc_ch_steering_pos),  PARAM_U8, PARAM_FLAG_NONE, 0, ADC_CHANNEL_COUNT - 1 },
};

#define SCALAR_PARAM_COUNT ((uint8_t)(sizeof(scalar_params) / sizeof(scalar_params[0])))

/* ==================================================================== */

static void load(const param_desc_t *src, param_desc_t *out)
{
    util_rom_read_block(out, src, (uint8_t)sizeof(param_desc_t));
}

/** Размер поля по типу. */
static uint8_t type_size(uint8_t type)
{
    return (type == PARAM_U8) ? 1 : 2;
}

uint8_t cfg_params_count(void)
{
    return (uint8_t)(SCALAR_PARAM_COUNT + PROFILE_PARAM_COUNT);
}

uint8_t cfg_params_get(uint8_t index, param_desc_t *out)
{
    if (index < SCALAR_PARAM_COUNT) {
        load(&scalar_params[index], out);
        return 1;
    }
    index = (uint8_t)(index - SCALAR_PARAM_COUNT);
    if (index < PROFILE_PARAM_COUNT) {
        load(&profile_params[index], out);
        return 1;
    }
    return 0;
}

uint8_t cfg_params_find_by_id(uint16_t id, param_desc_t *out)
{
    for (uint8_t i = 0; i < cfg_params_count(); i++) {
        param_desc_t d;
        (void)cfg_params_get(i, &d);
        if (d.id == id) {
            *out = d;
            return 1;
        }
    }
    return 0;
}

uint8_t cfg_params_find_by_offset(uint16_t offset, param_desc_t *out)
{
    const uint16_t profiles_at   = S(profiles);
    const uint16_t profile_size  = (uint16_t)sizeof(drive_profile_t);
    const uint16_t profiles_end  = (uint16_t)(profiles_at + profile_size * DRIVE_MODE_COUNT);

    if (offset >= profiles_at && offset < profiles_end) {
        uint16_t rel   = (uint16_t)(offset - profiles_at);
        uint16_t field = (uint16_t)(rel % profile_size);
        for (uint8_t i = 0; i < PROFILE_PARAM_COUNT; i++) {
            param_desc_t d;
            load(&profile_params[i], &d);
            if (d.offset == field) {
                /* Наружу отдаётся смещение в settings_t, а не в профиле:
                   вызывающий работает с одной системой координат. */
                d.offset = offset;
                *out = d;
                return 1;
            }
        }
        return 0;
    }

    for (uint8_t i = 0; i < SCALAR_PARAM_COUNT; i++) {
        param_desc_t d;
        load(&scalar_params[i], &d);
        if (d.offset == offset) {
            *out = d;
            return 1;
        }
    }
    return 0;
}

uint8_t cfg_params_decode(const param_desc_t *d, const void *data,
                          uint8_t size, int32_t *out)
{
    if (size != type_size(d->type)) {
        return PARAM_SIZE_MISMATCH;
    }
    const uint8_t *p = (const uint8_t *)data;
    if (d->type == PARAM_U8) {
        *out = (int32_t)p[0];
    } else {
        uint16_t raw = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        *out = (d->type == PARAM_I16) ? (int32_t)(int16_t)raw : (int32_t)raw;
    }
    return PARAM_OK;
}

uint8_t cfg_params_check(const param_desc_t *d, int32_t value)
{
    if (d->flags & PARAM_FLAG_READONLY) {
        return PARAM_READONLY;
    }
    if ((d->flags & PARAM_FLAG_ALLOW_FF) && value == 0xFF) {
        return PARAM_OK;
    }
    if (value < d->min || value > d->max) {
        return PARAM_OUT_OF_RANGE;
    }
    return PARAM_OK;
}
