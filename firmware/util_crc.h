/**
 * @file util_crc.h
 * @brief CRC16-CCITT для проверки целостности данных
 *
 * Используется в:
 * - cfg_settings: проверка целостности EEPROM
 * - app_protocol: проверка пакетов RS485
 *
 * Полином: 0x1021 (CRC-CCITT)
 * Начальное значение: 0xFFFF
 */
#pragma once

#include <stdint.h>

/**
 * @brief Вычислить CRC16 блока данных
 * @param data Указатель на данные
 * @param len  Длина данных в байтах
 * @return CRC16
 */
uint16_t util_crc16(const uint8_t *data, uint16_t len);

/**
 * @brief Обновить CRC16 одним байтом (для потокового вычисления)
 * @param crc  Текущее значение CRC (начальное: 0xFFFF)
 * @param data Очередной байт
 * @return Обновлённое CRC
 */
uint16_t util_crc16_update(uint16_t crc, uint8_t data);
