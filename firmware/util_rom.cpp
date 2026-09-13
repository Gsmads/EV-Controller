/**
 * @file util_rom.cpp
 * @brief Чтение постоянной памяти на платформах с единым адресным пространством
 *
 * Здесь лежит реализация для архитектур, где постоянная и оперативная память
 * адресуются одинаково: STM32, ESP32 и компьютер, на котором идут десктопные
 * тесты. Чтение сводится к разыменованию указателя.
 *
 * Для гарвардской архитектуры (AVR) нужен отдельный набор инструкций, и
 * реализация живёт в hal_atmega328p.cpp рядом с остальными платформенными
 * деталями. Поэтому весь файл закрыт условием: на AVR он пуст.
 *
 * Основание: ADR-0021.
 */
#include "util_rom.h"

#ifndef __AVR__

#include <string.h>

uint8_t util_rom_read_u8(const void *addr)
{
    return *(const uint8_t *)addr;
}

uint16_t util_rom_read_u16(const void *addr)
{
    const uint8_t *p = (const uint8_t *)addr;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void util_rom_read_block(void *dst, const void *src, uint8_t len)
{
    memcpy(dst, src, len);
}

#endif /* !__AVR__ */
