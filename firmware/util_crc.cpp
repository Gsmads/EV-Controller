/**
 * @file util_crc.c
 * @brief Реализация CRC16-CCITT
 */
#include "util_crc.h"

uint16_t util_crc16_update(uint16_t crc, uint8_t data)
{
    crc ^= ((uint16_t)data << 8);
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x8000) {
            crc = (crc << 1) ^ 0x1021;
        } else {
            crc <<= 1;
        }
    }
    return crc;
}

uint16_t util_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc = util_crc16_update(crc, data[i]);
    }
    return crc;
}
