/**
 * @file util_math.h
 * @brief Математические утилиты (целочисленные, без float)
 *
 * Содержит переиспользуемые функции:
 * - map / clamp (линейное масштабирование и ограничение)
 * - EMA-фильтр (fixed-point)
 * - Кривые отклика педали (linear, quadratic, s-curve)
 * - PID-регулятор (fixed-point, anti-windup)
 * - Целочисленный аккумулятор рампы
 *
 * Все функции работают с целочисленной арифметикой.
 * Для fixed-point используется масштабирование ×256 (8 бит дробной части)
 * или ×100 (для коэффициентов PID).
 */
#pragma once

#include <stdint.h>

/* ====================================================================
 *  Базовые операции
 * ==================================================================== */

/**
 * @brief Линейное масштабирование (аналог Arduino map, но uint16)
 * @param x      Входное значение
 * @param in_min  Минимум входного диапазона
 * @param in_max  Максимум входного диапазона
 * @param out_min Минимум выходного диапазона
 * @param out_max Максимум выходного диапазона
 * @return Масштабированное значение, зажатое в [out_min, out_max]
 */
uint16_t util_map_u16(uint16_t x, uint16_t in_min, uint16_t in_max,
                      uint16_t out_min, uint16_t out_max);

/** Ограничение значения в диапазоне [min_val, max_val] */
uint16_t util_clamp_u16(uint16_t value, uint16_t min_val, uint16_t max_val);

/** Ограничение знакового значения */
int16_t  util_clamp_i16(int16_t value, int16_t min_val, int16_t max_val);

/** Абсолютное значение int16 */
uint16_t util_abs_i16(int16_t x);

/* ====================================================================
 *  EMA-фильтр (Exponential Moving Average)
 * ==================================================================== */

/**
 * @brief Обновление EMA-фильтра (fixed-point ×256)
 *
 * Формула: filtered += alpha × (raw×256 - filtered) / 256
 *
 * @param filtered_fp  Текущее фильтрованное значение (×256)
 * @param raw          Новое сырое значение (0–1023)
 * @param alpha        Коэффициент фильтра (0=нет обновления, 255=мгновенно)
 * @return Обновлённое фильтрованное значение (×256)
 */
int32_t util_ema_update(int32_t filtered_fp, uint16_t raw, uint8_t alpha);

/** Извлечь целую часть из fixed-point ×256 */
static inline uint16_t util_ema_extract(int32_t filtered_fp) {
    return (uint16_t)(filtered_fp >> 8);
}

/** Инициализировать фильтр начальным значением */
static inline int32_t util_ema_init(uint16_t initial) {
    return (int32_t)initial << 8;
}

/* ====================================================================
 *  Кривые отклика педалей
 * ==================================================================== */

/** Типы кривых отклика */
typedef enum {
    CURVE_LINEAR    = 0,    /**< output = input */
    CURVE_QUADRATIC = 1,    /**< output = input² / max (плавный старт) */
    CURVE_S_CURVE   = 2,    /**< output = 3x² - 2x³ (плавный старт и финиш) */
    CURVE_COUNT     = 3
} response_curve_t;

/**
 * @brief Применить кривую отклика к нормализованному значению
 * @param input      Входное значение (0–1023)
 * @param curve_type Тип кривой
 * @return Преобразованное значение (0–1023)
 */
uint16_t util_curve_apply(uint16_t input, response_curve_t curve_type);

/* ====================================================================
 *  PID-регулятор (fixed-point)
 * ==================================================================== */

/**
 * @brief Состояние PID-регулятора
 *
 * Коэффициенты kp, ki, kd хранятся ×100 (fixed-point, 2 десятичных знака).
 * Пример: kp=150 означает Kp=1.50
 */
typedef struct {
    int16_t  kp;            /**< Пропорциональный коэффициент ×100 */
    int16_t  ki;            /**< Интегральный коэффициент ×100 */
    int16_t  kd;            /**< Дифференциальный коэффициент ×100 */
    int32_t  integral;      /**< Накопленная интегральная составляющая */
    int16_t  prev_error;    /**< Ошибка на предыдущем шаге */
    int16_t  output_min;    /**< Минимальный выход */
    int16_t  output_max;    /**< Максимальный выход */
    int32_t  integral_max;  /**< Anti-windup: макс. значение интеграла */
} pid_state_t;

/**
 * @brief Инициализация PID-регулятора
 * @param pid     Указатель на структуру состояния
 * @param kp      Kp ×100
 * @param ki      Ki ×100
 * @param kd      Kd ×100
 * @param out_min Минимальный выход (например, -1023)
 * @param out_max Максимальный выход (например, +1023)
 */
void util_pid_init(pid_state_t *pid, int16_t kp, int16_t ki, int16_t kd,
                   int16_t out_min, int16_t out_max);

/**
 * @brief Один шаг PID-регулятора
 * @param pid   Указатель на структуру состояния
 * @param error Текущая ошибка (setpoint - measured)
 * @return Управляющее воздействие, зажатое в [output_min, output_max]
 */
int16_t util_pid_update(pid_state_t *pid, int16_t error);

/**
 * @brief Сброс интегральной составляющей и истории
 * Вызывать при смене режима, аварийной остановке и т.д.
 */
void util_pid_reset(pid_state_t *pid);

/**
 * @brief Обновить коэффициенты PID на лету
 * Не сбрасывает интеграл — для плавной смены параметров.
 */
void util_pid_set_gains(pid_state_t *pid, int16_t kp, int16_t ki, int16_t kd);

/* ====================================================================
 *  Аккумулятор рампы
 * ==================================================================== */

/**
 * @brief Состояние аккумулятора рампы
 *
 * Обеспечивает точное дробное приращение целочисленного значения.
 * При rate=150 и freq=100: средняя скорость точно 1.5 единицы/тик.
 */
typedef struct {
    uint16_t accumulator;   /**< Дробный остаток (0..freq-1) */
    uint16_t freq;          /**< Частота обновления (Гц) */
} ramp_accum_t;

/**
 * @brief Инициализация аккумулятора
 * @param ra   Указатель на структуру
 * @param freq Частота обновления (Гц)
 */
void util_ramp_accum_init(ramp_accum_t *ra, uint16_t freq);

/**
 * @brief Вычислить целочисленный шаг для текущего тика
 * @param ra           Указатель на аккумулятор
 * @param rate_per_sec Скорость (единиц в секунду)
 * @return Количество целых единиц для этого тика
 */
uint16_t util_ramp_accum_step(ramp_accum_t *ra, uint16_t rate_per_sec);

/** Сброс аккумулятора */
void util_ramp_accum_reset(ramp_accum_t *ra);
