/**
 * @file app_main.cpp
 * @brief Реализация кооперативного планировщика
 */
#include "app_main.h"
#include "hal_system.h"

static task_entry_t tasks[APP_MAX_TASKS];
static uint8_t task_count = 0;

void app_scheduler_init(void)
{
    task_count = 0;
    for (uint8_t i = 0; i < APP_MAX_TASKS; i++) {
        tasks[i].func = 0;
        tasks[i].interval_ms = 0;
        tasks[i].last_run_ms = 0;
        tasks[i].enabled = 0;
    }
}

uint8_t app_scheduler_add(void (*func)(void), uint16_t interval_ms)
{
    if (task_count >= APP_MAX_TASKS) return 0xFF;

    uint8_t id = task_count;
    tasks[id].func        = func;
    tasks[id].interval_ms = interval_ms;
    tasks[id].last_run_ms = hal_system_millis();
    tasks[id].enabled     = 1;
    task_count++;
    return id;
}

void app_scheduler_run(void)
{
    uint32_t now = hal_system_millis();

    for (uint8_t i = 0; i < task_count; i++) {
        if (!tasks[i].enabled || !tasks[i].func) continue;

        uint32_t elapsed = now - tasks[i].last_run_ms;
        if (elapsed >= tasks[i].interval_ms) {
            tasks[i].last_run_ms = now;  /* Без накопления пропущенных */
            tasks[i].func();
        }
    }
}

void app_scheduler_enable(uint8_t task_id, uint8_t enabled)
{
    if (task_id < task_count) {
        tasks[task_id].enabled = enabled;
    }
}
