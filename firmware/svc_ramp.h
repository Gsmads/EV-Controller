/**
 * @file svc_ramp.h
 * @brief Генератор рампы разгона/торможения
 *
 * Плавно изменяет PWM от текущего к целевому значению.
 * Скорости разгона/торможения берёт из drive_profile_t.
 * Использует целочисленный аккумулятор (util_math) для точных дробных шагов.
 */
#pragma once
#include <stdint.h>
#include "cfg_settings.h"

/**
 * @brief Инициализация рампы
 * @param freq_hz Частота вызова update (Гц), обычно 100
 */
void svc_ramp_init(uint16_t freq_hz);

/**
 * @brief Обновление: вычислить новый PWM
 *
 * @param target_pwm  Целевой PWM (от педали или команды UART)
 * @param brake_value Значение педали тормоза (0–1023)
 * @param profile     Профиль режима вождения (содержит rates)
 * @return Новое значение PWM
 */
uint16_t svc_ramp_update(uint16_t target_pwm, uint16_t brake_value,
                         const drive_profile_t *profile);

uint16_t svc_ramp_get_current(void);
void     svc_ramp_reset(void);
uint8_t  svc_ramp_is_stopped(void);
uint8_t  svc_ramp_is_at_target(void);
