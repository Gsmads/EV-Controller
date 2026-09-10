/**
 * @file hal_uart.h
 * @brief HAL: последовательный порт с поддержкой RS485
 * Реализация: обёртка над MICRO_UART с добавлением RS485 DE/RE
 */
#pragma once

#include <stdint.h>

#define HAL_UART_NO_DATA  0xFF

void    hal_uart_init(uint32_t baud);
void    hal_uart_write(uint8_t data);
void    hal_uart_write_buf(const uint8_t *buf, uint8_t len);
uint8_t hal_uart_read(void);
uint8_t hal_uart_available(void);
void    hal_uart_flush_rx(void);

/** Управление направлением RS485 (HIGH=передача, LOW=приём) */
void    hal_uart_set_rs485_tx(uint8_t tx_mode);
