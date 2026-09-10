/**
 * @file app_main.h
 * @brief Кооперативный планировщик задач
 *
 * Запускает задачи с фиксированными интервалами без delay().
 * Если задача пропустила интервал — запускается немедленно
 * (без накопления пропущенных вызовов).
 *
 * Использование:
 *   app_scheduler_init();
 *   // В loop():
 *   app_scheduler_run();
 */
#pragma once
#include <stdint.h>

/** Максимальное количество задач */
#define APP_MAX_TASKS  10

/** Запись задачи в таблице */
typedef struct {
    void     (*func)(void);     /**< Указатель на функцию */
    uint16_t interval_ms;       /**< Интервал вызова (мс) */
    uint32_t last_run_ms;       /**< Время последнего вызова */
    uint8_t  enabled;           /**< 1 = активна */
} task_entry_t;

/** Инициализация планировщика */
void app_scheduler_init(void);

/**
 * @brief Добавить задачу
 * @param func        Функция задачи (void → void)
 * @param interval_ms Интервал вызова (мс)
 * @return Индекс задачи (для enable/disable) или 0xFF при переполнении
 */
uint8_t app_scheduler_add(void (*func)(void), uint16_t interval_ms);

/** Выполнить один проход планировщика (вызывать из loop) */
void app_scheduler_run(void);

/** Включить/выключить задачу */
void app_scheduler_enable(uint8_t task_id, uint8_t enabled);
