/**
 * @file svc_motor.h
 * @brief Координация двух моторов колёс
 *
 * MVP-1: простой вывод PWM через HAL.
 * MVP-3+: добавится FSM безопасного переключения DIR/BRAKE/STOP,
 *          электронный дифференциал, защита по току.
 *
 * Все внутренние вычисления ведутся в нормализованных единицах 0–1023.
 * При разном разрешении ШИМ (9/10 бит) масштабируется при записи в HAL.
 */
#pragma once
#include <stdint.h>

typedef enum {
    MOTOR_LEFT  = 0,
    MOTOR_RIGHT = 1,
    MOTOR_COUNT = 2
} motor_id_t;

/** Инициализация: настройка Timer1 PWM согласно текущему профилю */
void svc_motor_init(void);

/**
 * @brief Установить нормализованный PWM (0–1023) для обоих моторов
 *
 * Масштабируется к текущему TOP таймера автоматически.
 */
void svc_motor_set_pwm(uint16_t pwm_normalized);

/** Получить фактический PWM (нормализованный 0–1023) */
uint16_t svc_motor_get_pwm(motor_id_t id);

/** Аварийная остановка (мгновенный сброс PWM) */
void svc_motor_emergency_stop(void);

/** Текущая частота ШИМ (Гц) */
uint16_t svc_motor_get_frequency(void);
