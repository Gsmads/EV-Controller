/**
 * @file util_math.c
 * @brief Реализация математических утилит
 *
 * Полностью платформонезависимый код — только целочисленная арифметика.
 * Тестируется на десктопе (g++).
 */
#include "util_math.h"

/* ====================================================================
 *  Базовые операции
 * ==================================================================== */

uint16_t util_map_u16(uint16_t x, uint16_t in_min, uint16_t in_max,
                      uint16_t out_min, uint16_t out_max)
{
    /* Защита от деления на ноль и инверсии диапазона */
    if (in_max <= in_min) return out_min;
    if (x <= in_min) return out_min;
    if (x >= in_max) return out_max;

    /* 32-бит для предотвращения переполнения */
    uint32_t range_in  = in_max - in_min;
    uint32_t range_out = out_max - out_min;
    return out_min + (uint16_t)(((uint32_t)(x - in_min) * range_out) / range_in);
}

uint16_t util_clamp_u16(uint16_t value, uint16_t min_val, uint16_t max_val)
{
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

int16_t util_clamp_i16(int16_t value, int16_t min_val, int16_t max_val)
{
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

uint16_t util_abs_i16(int16_t x)
{
    return (x < 0) ? (uint16_t)(-x) : (uint16_t)x;
}

/* ====================================================================
 *  EMA-фильтр
 * ==================================================================== */

int32_t util_ema_update(int32_t filtered_fp, uint16_t raw, uint8_t alpha)
{
    int32_t raw_fp = (int32_t)raw << 8;
    int32_t delta  = raw_fp - filtered_fp;
    return filtered_fp + ((delta * (int32_t)alpha) >> 8);
}

/* ====================================================================
 *  Кривые отклика педалей
 *
 *  Все вычисления в uint32_t для предотвращения переполнения.
 *  Входной диапазон: 0–1023.
 *  Выходной диапазон: 0–1023.
 * ==================================================================== */

uint16_t util_curve_apply(uint16_t input, response_curve_t curve_type)
{
    if (input == 0) return 0;
    if (input >= 1023) return 1023;

    switch (curve_type) {
        case CURVE_LINEAR:
            return input;

        case CURVE_QUADRATIC: {
            /*
             * output = input² / 1023
             * При input=512:  output = 262144 / 1023 ≈ 256 (~25%)
             * При input=1023: output = 1046529 / 1023 = 1023
             *
             * Эффект: спокойный старт, резкий финиш
             */
            uint32_t x = (uint32_t)input;
            return (uint16_t)((x * x) / 1023UL);
        }

        case CURVE_S_CURVE: {
            /*
             * S-кривая (кубическая): output = 3x² - 2x³
             * Нормализованная: t = input/1023, out = t²(3-2t) × 1023
             *
             * Целочисленная аппроксимация:
             *   t_fp = input (0..1023 рассматриваем как 0..1 ×1023)
             *   out = (3 × input² - 2 × input³/1023) / 1023
             *
             * Для предотвращения переполнения uint32:
             *   input³ при input=1023: 1023³ ≈ 1.07×10⁹ — помещается в uint32
             *   3 × input²: 3 × 1046529 = 3139587 — помещается
             */
            uint32_t x  = (uint32_t)input;
            uint32_t x2 = x * x;               /* max: 1046529 */
            uint32_t x3 = x2 * x;              /* max: ~1.07e9, OK для uint32 */
            /* out = (3·x² - 2·x³/1023) / 1023 */
            uint32_t num = 3UL * x2 - (2UL * x3) / 1023UL;
            return (uint16_t)(num / 1023UL);
        }

        default:
            return input;
    }
}

/* ====================================================================
 *  PID-регулятор
 *
 *  Коэффициенты ×100 (kp=150 означает Kp=1.50).
 *  Все вычисления в int32_t для предотвращения переполнения.
 *
 *  Формула:
 *    P = kp × error / 100
 *    I = ki × integral / 100  (integral += error на каждом шаге)
 *    D = kd × (error - prev_error) / 100
 *    output = clamp(P + I + D, output_min, output_max)
 *
 *  Anti-windup: integral зажимается в [-integral_max, +integral_max]
 * ==================================================================== */

void util_pid_init(pid_state_t *pid, int16_t kp, int16_t ki, int16_t kd,
                   int16_t out_min, int16_t out_max)
{
    pid->kp          = kp;
    pid->ki          = ki;
    pid->kd          = kd;
    pid->integral    = 0;
    pid->prev_error  = 0;
    pid->output_min  = out_min;
    pid->output_max  = out_max;
    /* Anti-windup: ограничиваем интеграл, чтобы I-составляющая
       сама по себе не могла превысить диапазон выхода */
    if (ki != 0) {
        pid->integral_max = ((int32_t)out_max * 100L) / (int32_t)ki;
        if (pid->integral_max < 0) pid->integral_max = -pid->integral_max;
    } else {
        pid->integral_max = 0;
    }
}

int16_t util_pid_update(pid_state_t *pid, int16_t error)
{
    /* P-составляющая */
    int32_t p_term = ((int32_t)pid->kp * (int32_t)error) / 100L;

    /* I-составляющая с anti-windup */
    pid->integral += (int32_t)error;
    if (pid->integral_max > 0) {
        if (pid->integral > pid->integral_max)
            pid->integral = pid->integral_max;
        else if (pid->integral < -pid->integral_max)
            pid->integral = -pid->integral_max;
    }
    int32_t i_term = ((int32_t)pid->ki * pid->integral) / 100L;

    /* D-составляющая */
    int32_t d_term = ((int32_t)pid->kd * (int32_t)(error - pid->prev_error)) / 100L;
    pid->prev_error = error;

    /* Суммарный выход */
    int32_t output = p_term + i_term + d_term;

    /* Зажатие выхода */
    if (output > (int32_t)pid->output_max) output = pid->output_max;
    if (output < (int32_t)pid->output_min) output = pid->output_min;

    return (int16_t)output;
}

void util_pid_reset(pid_state_t *pid)
{
    pid->integral   = 0;
    pid->prev_error = 0;
}

void util_pid_set_gains(pid_state_t *pid, int16_t kp, int16_t ki, int16_t kd)
{
    pid->kp = kp;
    pid->ki = ki;
    pid->kd = kd;
    /* Пересчёт anti-windup */
    if (ki != 0) {
        pid->integral_max = ((int32_t)pid->output_max * 100L) / (int32_t)ki;
        if (pid->integral_max < 0) pid->integral_max = -pid->integral_max;
    } else {
        pid->integral_max = 0;
    }
}

/* ====================================================================
 *  Аккумулятор рампы
 * ==================================================================== */

void util_ramp_accum_init(ramp_accum_t *ra, uint16_t freq)
{
    ra->accumulator = 0;
    ra->freq = freq;
}

uint16_t util_ramp_accum_step(ramp_accum_t *ra, uint16_t rate_per_sec)
{
    ra->accumulator += rate_per_sec;
    uint16_t steps = ra->accumulator / ra->freq;
    ra->accumulator %= ra->freq;
    return steps;
}

void util_ramp_accum_reset(ramp_accum_t *ra)
{
    ra->accumulator = 0;
}
