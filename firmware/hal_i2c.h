/**
 * @file hal_i2c.h
 * @brief HAL: I2C-интерфейс (для PCA9535 расширителя портов)
 * Реализация: hal_atmega328p.c
 */
#pragma once

#include <stdint.h>

void    hal_i2c_init(void);
uint8_t hal_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t data);
uint8_t hal_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data);
uint8_t hal_i2c_write_buf(uint8_t addr, uint8_t reg, const uint8_t *buf, uint8_t len);
uint8_t hal_i2c_read_buf(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len);
