/**
 * @file hal_pwm.h
 * @brief HAL: управление ШИМ (таймеры)
 *
 * Абстрагирует конфигурацию таймеров MCU для генерации ШИМ-сигналов.
 * Поддерживает:
 * - Phase-Correct и Fast PWM
 * - Произвольный TOP (разрешение/частота)
 * - Реконфигурацию на лету (только при PWM=0)
 * - Два независимых таймера (моторы + EPS)
 *
 * На ATmega328P:
 *   PWM_TIMER_MOTORS (Timer1): OC1A=D9, OC1B=D10, 16-бит
 *   PWM_TIMER_EPS    (Timer0): OC0A=D6,           8-бит
 *
 * Реализация: hal_atmega328p.c
 */
#pragma once

#include <stdint.h>

/* ====================================================================
 *  Типы
 * ==================================================================== */

/** Идентификатор аппаратного таймера */
typedef enum {
    PWM_TIMER_MOTORS = 0,   /**< Timer1: D9 (OC1A) + D10 (OC1B) */
    PWM_TIMER_EPS    = 1,   /**< Timer0: D6 (OC0A)              */
    PWM_TIMER_COUNT  = 2
} pwm_timer_id_t;

/** Канал ШИМ внутри таймера */
typedef enum {
    PWM_CH_A = 0,           /**< Первый канал (OC1A / OC0A) */
    PWM_CH_B = 1            /**< Второй канал (OC1B / OC0B, только Timer1) */
} pwm_channel_t;

/** Режим генерации ШИМ */
typedef enum {
    PWM_MODE_PHASE_CORRECT = 0,  /**< f = F_CPU / (2 × TOP) */
    PWM_MODE_FAST          = 1   /**< f = F_CPU / (TOP + 1) */
} pwm_mode_t;

/** Конфигурация таймера */
typedef struct {
    pwm_mode_t mode;        /**< Phase-Correct или Fast */
    uint16_t   top;         /**< TOP-значение (определяет разрешение и частоту) */
    uint8_t    prescaler;   /**< Индекс прескалера: 0=off, 1=1, 2=8, 3=64, 4=256, 5=1024 */
} pwm_config_t;

/* ====================================================================
 *  API
 * ==================================================================== */

/**
 * @brief Инициализация таймера для ШИМ
 * @param timer  Идентификатор таймера
 * @param config Конфигурация (режим, TOP, прескалер)
 *
 * Настраивает регистры таймера, устанавливает пины как выходы,
 * начальный PWM = 0.
 */
void hal_pwm_init(pwm_timer_id_t timer, const pwm_config_t *config);

/**
 * @brief Установить значение ШИМ на одном канале
 * @param timer   Идентификатор таймера
 * @param channel Канал (A или B)
 * @param value   Значение 0..config.top
 *
 * Атомарная запись (cli/sei для 16-битных регистров).
 */
void hal_pwm_set(pwm_timer_id_t timer, pwm_channel_t channel, uint16_t value);

/**
 * @brief Получить текущее значение ШИМ
 * @param timer   Идентификатор таймера
 * @param channel Канал
 * @return Текущее значение OCRnx
 */
uint16_t hal_pwm_get(pwm_timer_id_t timer, pwm_channel_t channel);

/**
 * @brief Установить ШИМ на оба канала одновременно (одна блокировка)
 * @param timer Идентификатор таймера
 * @param a     Значение канала A
 * @param b     Значение канала B
 */
void hal_pwm_set_both(pwm_timer_id_t timer, uint16_t a, uint16_t b);

/**
 * @brief Реконфигурация таймера (смена частоты/разрешения)
 *
 * ПРЕДУСЛОВИЕ: оба канала PWM == 0 И моторы остановлены!
 * Нарушение → undefined behavior (глитчи, повреждение драйверов).
 *
 * @param timer  Идентификатор таймера
 * @param config Новая конфигурация
 */
void hal_pwm_reconfigure(pwm_timer_id_t timer, const pwm_config_t *config);

/**
 * @brief Вычислить TOP для заданной частоты
 * @param target_freq_hz Целевая частота в Гц
 * @param mode           Phase-Correct или Fast
 * @param prescaler      Индекс прескалера
 * @return Значение TOP (ICR1)
 *
 * Phase-Correct: TOP = F_CPU / (2 × prescaler_val × freq) - 1
 * Fast:          TOP = F_CPU / (prescaler_val × freq) - 1
 */
uint16_t hal_pwm_calc_top(uint32_t target_freq_hz, pwm_mode_t mode, uint8_t prescaler);

/**
 * @brief Получить текущую частоту ШИМ
 * @param timer Идентификатор таймера
 * @return Частота в Гц
 */
uint16_t hal_pwm_get_frequency(pwm_timer_id_t timer);

/**
 * @brief Получить текущее значение TOP таймера
 * @param timer Идентификатор таймера
 * @return Текущий TOP (максимальное значение PWM)
 */
uint16_t hal_pwm_get_top(pwm_timer_id_t timer);
