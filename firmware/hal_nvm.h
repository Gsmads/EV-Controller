/**
 * @file hal_nvm.h
 * @brief HAL: долговременная память (то, что переживает выключение)
 *
 * Имя намеренно не называет технологию: на ATmega328P это EEPROM,
 * на STM32F410 — внутренняя flash с постраничным стиранием, дальше
 * может появиться внешняя микросхема. Программа сохраняет "надолго",
 * а чем это сделано — дело слоя железа (ADR-0021).
 *
 * ATmega328P: 1024 байта EEPROM.
 * Реализация: hal_atmega328p.c
 */
#pragma once

#include <stdint.h>

void    hal_nvm_read(uint16_t addr, uint8_t *buf, uint16_t len);
void    hal_nvm_write(uint16_t addr, const uint8_t *buf, uint16_t len);
uint8_t hal_nvm_read_byte(uint16_t addr);
void    hal_nvm_write_byte(uint16_t addr, uint8_t data);
