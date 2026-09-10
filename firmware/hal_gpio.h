/**
 * @file hal_gpio.h
 * @brief HAL: управление GPIO (цифровые пины)
 *
 * Платформонезависимый интерфейс для:
 * - настройки режима пина (вход/выход/подтяжка)
 * - чтения/записи цифрового значения
 *
 * Реализация: hal_atmega328p.c
 */
#pragma once

#include <stdint.h>

typedef enum {
    GPIO_INPUT         = 0,
    GPIO_INPUT_PULLUP  = 1,
    GPIO_OUTPUT        = 2
} gpio_mode_t;

typedef enum {
    GPIO_LOW  = 0,
    GPIO_HIGH = 1
} gpio_state_t;

/**
 * @brief Настройка режима пина
 * @param pin   Номер пина (платформозависимый, из cfg_board.h)
 * @param mode  Режим работы
 */
void hal_gpio_mode(uint8_t pin, gpio_mode_t mode);

/**
 * @brief Запись цифрового значения
 * @param pin   Номер пина
 * @param state HIGH или LOW
 */
void hal_gpio_write(uint8_t pin, gpio_state_t state);

/**
 * @brief Чтение цифрового значения
 * @param pin Номер пина
 * @return GPIO_LOW или GPIO_HIGH
 */
gpio_state_t hal_gpio_read(uint8_t pin);
