/**
 * @file svc_ramp.cpp
 * @brief Реализация генератора рампы
 *
 * Платформонезависимый. Использует util_ramp_accum для точного шага.
 * Скорости разгона/торможения берёт из drive_profile_t.
 */
#include "svc_ramp.h"
#include "util_math.h"

/* ====================================================================
 *  Внутренние данные
 * ==================================================================== */

static uint16_t     current_pwm;
static uint16_t     target_pwm_cached;
static ramp_accum_t accum;

/* ====================================================================
 *  Вспомогательные функции
 * ==================================================================== */

/**
 * @brief Скорость торможения педалью (линейная интерполяция min..max)
 */
static uint16_t brake_decel_rate(uint16_t brake_val,
                                 uint16_t rate_min, uint16_t rate_max)
{
    if (brake_val == 0)    return rate_min;
    if (brake_val >= 1023) return rate_max;
    uint32_t range = rate_max - rate_min;
    return rate_min + (uint16_t)((range * (uint32_t)brake_val) / 1023UL);
}

/**
 * @brief Применить шаг рампы вверх или вниз
 */
static void apply_step(uint16_t rate, uint8_t going_up, uint16_t bound)
{
    uint16_t steps = util_ramp_accum_step(&accum, rate);
    if (steps == 0) return;

    if (going_up) {
        uint16_t headroom = bound - current_pwm;
        if (steps >= headroom) {
            current_pwm = bound;
            util_ramp_accum_reset(&accum);
        } else {
            current_pwm += steps;
        }
    } else {
        uint16_t gap = current_pwm - bound;
        if (steps >= gap) {
            current_pwm = bound;
            util_ramp_accum_reset(&accum);
        } else {
            current_pwm -= steps;
        }
    }
}

/* ====================================================================
 *  Публичный API
 * ==================================================================== */

void svc_ramp_init(uint16_t freq_hz)
{
    current_pwm = 0;
    target_pwm_cached = 0;
    util_ramp_accum_init(&accum, freq_hz);
}

uint16_t svc_ramp_update(uint16_t target_pwm, uint16_t brake_value,
                         const drive_profile_t *profile)
{
    target_pwm_cached = target_pwm;

    if (brake_value > 0) {
        /* Торможение педалью: target = 0, скорость пропорциональна нажатию */
        target_pwm_cached = 0;
        if (current_pwm > 0) {
            uint16_t rate = brake_decel_rate(brake_value,
                                             profile->brake_rate_min,
                                             profile->brake_rate_max);
            apply_step(rate, 0, 0);
        }
    } else if (current_pwm < target_pwm) {
        /* Разгон */
        apply_step(profile->accel_rate, 1, target_pwm);
    } else if (current_pwm > target_pwm) {
        /* Замедление (отпускание газа) */
        apply_step(profile->decel_rate, 0, target_pwm);
    } else {
        /* На цели — сброс аккумулятора */
        util_ramp_accum_reset(&accum);
    }

    return current_pwm;
}

uint16_t svc_ramp_get_current(void) { return current_pwm; }

void svc_ramp_reset(void)
{
    current_pwm = 0;
    target_pwm_cached = 0;
    util_ramp_accum_reset(&accum);
}

uint8_t svc_ramp_is_stopped(void)   { return (current_pwm == 0) ? 1 : 0; }
uint8_t svc_ramp_is_at_target(void) { return (current_pwm == target_pwm_cached) ? 1 : 0; }
