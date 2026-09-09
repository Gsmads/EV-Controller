/**
 * @file svc_motor.cpp
 * @brief Реализация координации моторов
 *
 * MVP-1: простой вывод PWM через HAL.
 * Масштабирование: все внутренние значения 0–1023 (10 бит),
 * при записи в HAL масштабируются к текущему TOP таймера.
 */
#include "svc_motor.h"
#include "hal_pwm.h"
#include "cfg_settings.h"

/* ====================================================================
 *  Внутренние данные
 * ==================================================================== */

static uint16_t current_norm[MOTOR_COUNT] = {0, 0};

/* ====================================================================
 *  Вспомогательные функции
 * ==================================================================== */

/**
 * @brief Масштабировать нормализованный PWM (0–1023) к текущему TOP
 *
 * При 10-бит (TOP=1023): 1:1
 * При 9-бит  (TOP=511):  value = value * 511 / 1023
 */
static uint16_t scale_to_top(uint16_t norm)
{
    uint16_t top = hal_pwm_get_top(PWM_TIMER_MOTORS);
    if (top == 1023) return norm;
    if (top == 0) return 0;
    return (uint16_t)(((uint32_t)norm * top) / 1023UL);
}

/* ====================================================================
 *  Публичный API
 * ==================================================================== */

void svc_motor_init(void)
{
    /* Получить профиль текущего режима для частоты ШИМ */
    const drive_profile_t *profile = cfg_settings_get_profile(DRIVE_MODE_ECO);

    /* Вычислить TOP для целевой частоты */
    pwm_config_t cfg;
    cfg.mode = PWM_MODE_PHASE_CORRECT;
    cfg.prescaler = 1;  /* Без деления */
    cfg.top = hal_pwm_calc_top(profile->pwm_freq_hz, cfg.mode, cfg.prescaler);

    /* Если профиль указывает 9-бит разрешение, ограничить TOP */
    if (profile->pwm_resolution == 9 && cfg.top > 511) {
        cfg.top = 511;
    }

    hal_pwm_init(PWM_TIMER_MOTORS, &cfg);

    current_norm[MOTOR_LEFT]  = 0;
    current_norm[MOTOR_RIGHT] = 0;
}

void svc_motor_set_pwm(uint16_t pwm_normalized)
{
    if (pwm_normalized > 1023) pwm_normalized = 1023;

    /* В MVP-1 оба мотора получают одинаковый PWM.
     * В MVP-8+ здесь будет электронный дифференциал. */
    uint16_t hw_pwm = scale_to_top(pwm_normalized);
    hal_pwm_set_both(PWM_TIMER_MOTORS, hw_pwm, hw_pwm);

    current_norm[MOTOR_LEFT]  = pwm_normalized;
    current_norm[MOTOR_RIGHT] = pwm_normalized;
}

uint16_t svc_motor_get_pwm(motor_id_t id)
{
    if (id >= MOTOR_COUNT) return 0;
    return current_norm[id];
}

void svc_motor_emergency_stop(void)
{
    hal_pwm_set_both(PWM_TIMER_MOTORS, 0, 0);
    current_norm[MOTOR_LEFT]  = 0;
    current_norm[MOTOR_RIGHT] = 0;
}

uint16_t svc_motor_get_frequency(void)
{
    return hal_pwm_get_frequency(PWM_TIMER_MOTORS);
}
