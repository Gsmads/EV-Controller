/**
 * @file svc_speed.h
 * @brief Сервис расчёта скорости колёс
 *
 * Использует hal_encoder для подсчёта импульсов с датчиков скорости
 * (D2 = правое колесо, D3 = левое). Каждое колесо обрабатывается
 * независимо для будущего torque vectoring / ABS.
 *
 * В MVP-1 без подключённых датчиков скорость = 0.
 * В MVP-2 при подключении энкодеров — реальные значения.
 *
 * Параметры пересчёта берутся из cfg_board.h:
 *   ENCODER_PULSES_PER_REV — импульсов на оборот (уточнить экспериментально)
 *   WHEEL_DIAMETER_MM      — диаметр колеса в мм
 *
 * @version 1.0.0 (MVP-1 scaffold, MVP-2 ready)
 */
#pragma once
#include <stdint.h>

typedef enum {
    SPEED_WHEEL_LEFT  = 0,
    SPEED_WHEEL_RIGHT = 1,
    SPEED_WHEEL_COUNT = 2
} speed_wheel_t;

/** Инициализация. Вызывает hal_encoder_init() */
void svc_speed_init(void);

/**
 * @brief Обновление: чтение и сброс счётчиков, расчёт RPM и км/ч
 *
 * Вызывать с фиксированной частотой (по умолчанию 10 Гц).
 * Передавать ту же частоту, что и интервал планировщика, чтобы расчёт
 * был корректным независимо от настройки.
 *
 * @param update_freq_hz Частота вызова этой функции (Гц)
 */
void svc_speed_update(uint8_t update_freq_hz);

/** Обороты колеса в минуту (0 если энкодер не подключён) */
uint16_t svc_speed_get_rpm(speed_wheel_t wheel);

/**
 * @brief Средняя линейная скорость машины
 * @return км/ч × 10 (для отображения формата XX.X)
 */
uint16_t svc_speed_get_kmh_x10(void);

/** Пройденный путь в метрах (одометр) */
uint32_t svc_speed_get_odometer_m(void);

/** Машина остановлена? (RPM обоих колёс == 0) */
uint8_t svc_speed_is_stopped(void);

/** Сбросить одометр в 0 */
void svc_speed_reset_odometer(void);
