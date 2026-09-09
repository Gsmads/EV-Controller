/**
 * @file hal_encoder.h
 * @brief HAL: счётчики импульсов энкодеров колёс (внешние прерывания)
 *
 * INT0 (D2) — правое колесо
 * INT1 (D3) — левое колесо
 *
 * ISR инкрементирует volatile-счётчики; сервисный модуль
 * периодически читает и сбрасывает для расчёта RPM.
 *
 * Реализация: hal_atmega328p.c
 */
#pragma once

#include <stdint.h>

/** Канал энкодера */
typedef enum {
    ENCODER_RIGHT = 0,  /**< INT0 (D2) */
    ENCODER_LEFT  = 1,  /**< INT1 (D3) */
    ENCODER_COUNT = 2
} encoder_channel_t;

void     hal_encoder_init(void);

/**
 * @brief Получить накопленное количество импульсов и сбросить счётчик
 * @param ch Канал энкодера
 * @return Количество импульсов с последнего вызова (атомарное чтение+сброс)
 */
uint16_t hal_encoder_read_and_reset(encoder_channel_t ch);

/**
 * @brief Получить накопленное количество без сброса
 */
uint16_t hal_encoder_get_count(encoder_channel_t ch);
