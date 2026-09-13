/**
 * @file app_debug.cpp
 * @brief Реализация отладочной телеметрии
 */
#include "app_debug.h"
#include "cfg_board.h"
#include "PRINT.h"
#include "hal_uart.h"

#ifdef BOARD_DEBUG_ENABLED

/**
 * @brief Печать 4-значного числа с ведущими нулями
 */
static void print_padded4(uint16_t n)
{
    if (n < 1000) hal_uart_write('0');
    if (n < 100)  hal_uart_write('0');
    if (n < 10)   hal_uart_write('0');
    print_uint32_base10((uint32_t)n);
}

void app_debug_init(void)
{
    printPgmString(PSTR("\r\n=== EV Controller MVP-1 (Arch v2) ===\r\n"));
    printPgmString(PSTR("Format: G:gas B:brake T:target C:current F:freq R:mode\r\n"));
    printPgmString(PSTR("========================================\r\n"));
}

void app_debug_telemetry(uint16_t gas, uint16_t brake,
                         uint16_t target, uint16_t current,
                         uint16_t freq_hz)
{
    printPgmString(PSTR("G:"));
    print_padded4(gas);
    printPgmString(PSTR(" B:"));
    print_padded4(brake);
    printPgmString(PSTR(" T:"));
    print_padded4(target);
    printPgmString(PSTR(" C:"));
    print_padded4(current);
    printPgmString(PSTR(" F:"));
    print_uint32_base10(freq_hz);

    printPgmString(PSTR(" R:"));
    if (current == 0)           hal_uart_write('S');  /* Stopped */
    else if (current < target)  hal_uart_write('A');  /* Accel */
    else if (current > target)  hal_uart_write('D');  /* Decel */
    else                        hal_uart_write('H');  /* Hold */

    printPgmString(PSTR("\r\n"));
}

void app_debug_msg_P(const char *msg)
{
    printPgmString(PSTR("[DBG] "));
    printPgmString(msg);
    printPgmString(PSTR("\r\n"));
}

#else /* !BOARD_DEBUG_ENABLED */

void app_debug_init(void) {}
void app_debug_telemetry(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {}
void app_debug_msg_P(const char *) {}

#endif
