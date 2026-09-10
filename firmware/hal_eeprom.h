/**
 * @file hal_eeprom.h
 * @brief HAL: энергонезависимая память (EEPROM)
 *
 * ATmega328P: 1024 байта EEPROM.
 * Реализация: hal_atmega328p.c
 */
#pragma once

#include <stdint.h>

void    hal_eeprom_read(uint16_t addr, uint8_t *buf, uint16_t len);
void    hal_eeprom_write(uint16_t addr, const uint8_t *buf, uint16_t len);
uint8_t hal_eeprom_read_byte(uint16_t addr);
void    hal_eeprom_write_byte(uint16_t addr, uint8_t data);
