/**
 * @file app_debug.h
 * @brief Отладочная телеметрия через UART
 *
 * Формат: G:0512 B:0000 T:0200 C:0198 F:7812 R:A
 * Управляется BOARD_DEBUG_ENABLED в cfg_board.h
 */
#pragma once
#include <stdint.h>

void app_debug_init(void);

/**
 * @brief Вывод телеметрии (вызывать из планировщика, 5 Гц)
 */
void app_debug_telemetry(uint16_t gas, uint16_t brake,
                         uint16_t target, uint16_t current,
                         uint16_t freq_hz);

/** Вывод сообщения из PROGMEM */
void app_debug_msg_P(const char *msg);
