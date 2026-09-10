/**
 * @file cfg_board.h
 * @brief Аппаратная конфигурация платы (compile-time)
 *
 * Карта пинов Arduino Nano, назначение таймеров, адреса I2C,
 * условная компиляция модулей. При портировании — заменить этот файл.
 */
#pragma once
#include <stdint.h>

/* ==== Идентификация ==== */
#define BOARD_NAME              "TED_CAR_DRIVER_V1"
#define BOARD_MCU               "ATmega328P"
#define BOARD_CLOCK_HZ          16000000UL

/* ==== Пины: UART / RS485 ==== */
#define PIN_UART_RX             0
#define PIN_UART_TX             1
#define PIN_RS485_DE_RE         4

/* ==== Пины: энкодеры (INT0/INT1) ==== */
#define PIN_ENCODER_RIGHT       2
#define PIN_ENCODER_LEFT        3

/* ==== Пины: HX711 ==== */
#define PIN_HX711_DOUT          5
#define PIN_HX711_SCK           12

/* ==== Пины: ШИМ ==== */
#define PIN_EPS_PWM             6
#define PIN_MOTOR_LEFT_PWM      9
#define PIN_MOTOR_RIGHT_PWM     10

/* ==== Пины: PCA9535, HC595, SPI ==== */
#define PIN_PCA9535_INT         7
#define PIN_HC595_LATCH         8
#define PIN_SPI_MOSI            11
#define PIN_SPI_SCK             13

/* ==== Аналоговые входы ==== */
#define PIN_PEDAL_GAS           A0
#define PIN_PEDAL_BRAKE         A1
#define PIN_CURRENT_RIGHT       A2
#define PIN_STEERING_POS        A3
#define PIN_CURRENT_LEFT        A6
#define PIN_ANALOG_RESERVE      A7

/* ==== HC595 битовые позиции ==== */
#define HC595_BIT_EPS_RIGHT     0
#define HC595_BIT_DIR_RIGHT     1
#define HC595_BIT_STOP_RIGHT    2
#define HC595_BIT_BRAKE_RIGHT   3
#define HC595_BIT_DIR_LEFT      4
#define HC595_BIT_STOP_LEFT     5
#define HC595_BIT_BRAKE_LEFT    6
#define HC595_BIT_EPS_LEFT      7

/* ==== PCA9535 ==== */
#define PCA9535_I2C_ADDR        0x20
#define PCA9535_P0_POWER_EPS    5
#define PCA9535_P0_POWER_MOT_R  6
#define PCA9535_P0_POWER_MOT_L  7

/* ==== Энкодеры и колёса ==== */
#define ENCODER_PULSES_PER_REV  12
#define WHEEL_DIAMETER_MM       200

/* ==== UART: размеры буферов (должны быть ДО включения MICRO_UART.h) ==== */
#define RX_BUFFER_SIZE          64
#define TX_BUFFER_SIZE          64

/* ==== Датчики тока (ACS712-20A) ==== */
#define CURRENT_SENSOR_MV_PER_A     100
#define CURRENT_SENSOR_ZERO_MV      2500

/* ==== Условная компиляция ==== */
/* #define BOARD_HAS_HC595      1 */    /* MVP-3+ */
/* #define BOARD_HAS_PCA9535    1 */    /* MVP-3+ */
/* #define BOARD_HAS_HX711      1 */    /* MVP-2+ */
/* #define BOARD_HAS_RS485      1 */    /* MVP-4+ */
/* #define BOARD_HAS_ENCODERS   1 */    /* MVP-2+ */
#define BOARD_DEBUG_ENABLED     1
