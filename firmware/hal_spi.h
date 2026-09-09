/**
 * @file hal_spi.h
 * @brief HAL: SPI-интерфейс (для HC595 сдвигового регистра)
 * Реализация: hal_atmega328p.c
 */
#pragma once

#include <stdint.h>

void    hal_spi_init(void);
uint8_t hal_spi_transfer(uint8_t data);
void    hal_spi_cs_low(uint8_t cs_pin);
void    hal_spi_cs_high(uint8_t cs_pin);
