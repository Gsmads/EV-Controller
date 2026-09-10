/**
 * @file hal_atmega328p.cpp
 * @brief Реализация HAL для ATmega328P (Arduino Nano)
 *
 * ЕДИНСТВЕННЫЙ файл в проекте, содержащий платформозависимый код.
 * При портировании — заменить этот файл на hal_<platform>.cpp.
 *
 * Реализует:
 * - hal_gpio:    pinMode/digitalWrite/digitalRead через регистры
 * - hal_adc:     10-бит АЦП через регистры ADMUX/ADCSRA
 * - hal_pwm:     Timer1 Phase-Correct/Fast PWM, произвольный TOP
 * - hal_system:  millis (через Arduino core Timer2), watchdog, IRQ
 * - hal_eeprom:  avr/eeprom.h обёртка
 * - hal_uart:    обёртка над MICRO_UART + RS485 DE/RE
 *
 * @version 1.0.0 (MVP-1)
 */

#include <Arduino.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>

#include "cfg_board.h"
#include "hal_gpio.h"
#include "hal_adc.h"
#include "hal_pwm.h"
#include "hal_system.h"
#include "hal_eeprom.h"
#include "hal_uart.h"
#include "MICRO_UART.h"

/* ====================================================================
 *  hal_gpio
 * ==================================================================== */

void hal_gpio_mode(uint8_t pin, gpio_mode_t mode)
{
    switch (mode) {
        case GPIO_INPUT:        pinMode(pin, INPUT);        break;
        case GPIO_INPUT_PULLUP: pinMode(pin, INPUT_PULLUP); break;
        case GPIO_OUTPUT:       pinMode(pin, OUTPUT);       break;
    }
}

void hal_gpio_write(uint8_t pin, gpio_state_t state)
{
    digitalWrite(pin, (state == GPIO_HIGH) ? HIGH : LOW);
}

gpio_state_t hal_gpio_read(uint8_t pin)
{
    return (digitalRead(pin) == HIGH) ? GPIO_HIGH : GPIO_LOW;
}

/* ====================================================================
 *  hal_adc
 *
 *  ATmega328P: 10-бит АЦП, прескалер 128 → ~125 кГц тактирование,
 *  одна конверсия ≈ 104 мкс. Используем AVCC как опорное.
 * ==================================================================== */

void hal_adc_init(void)
{
    /* AVCC с внешним конденсатором на AREF */
    ADMUX = (1 << REFS0);

    /* Включить АЦП, прескалер 128 (16MHz/128 = 125kHz) */
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);

    /* Первое чтение для стабилизации */
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC)) {}
}

uint16_t hal_adc_read(uint8_t channel)
{
    /*
     * Каналы 0–7 на ATmega328P:
     *   0–5 → A0–A5 (обычные)
     *   6–7 → A6–A7 (только аналоговые, нет digital)
     *
     * Для Arduino pin mapping: A0=14, A1=15, ..., A6=20, A7=21
     * Но ADMUX принимает номер канала 0–7
     */
    if (channel >= 14) channel -= 14;  /* Arduino pin → ADC channel */
    channel &= 0x07;

    /* Выбор канала, сохраняя REFS */
    ADMUX = (ADMUX & 0xF0) | channel;

    /* Начать конверсию */
    ADCSRA |= (1 << ADSC);

    /* Ожидание завершения (блокирующее, ~104 мкс) */
    while (ADCSRA & (1 << ADSC)) {}

    /* Чтение результата: сначала ADCL, потом ADCH (порядок важен!) */
    return ADC;
}

/* ====================================================================
 *  hal_pwm — Timer1 (16-бит)
 *
 *  Конфигурации WGM для Timer1:
 *    Phase-Correct, TOP=ICR1:  WGM13:10 = 1010 (mode 10)
 *    Fast PWM, TOP=ICR1:       WGM13:10 = 1110 (mode 14)
 * ==================================================================== */

/* Внутреннее состояние таймеров (кэш для чтения без регистров) */
static struct {
    uint16_t top;
    uint16_t pwm[2];          /* [0]=CH_A, [1]=CH_B */
    pwm_mode_t mode;
    uint8_t prescaler_idx;
} pwm_state[PWM_TIMER_COUNT];

/* Таблица значений прескалера Timer1 */
static const uint16_t prescaler_values[] = { 0, 1, 8, 64, 256, 1024 };

void hal_pwm_init(pwm_timer_id_t timer, const pwm_config_t *config)
{
    if (timer == PWM_TIMER_MOTORS) {
        /* --- Timer1: D9 (OC1A) + D10 (OC1B) --- */
        hal_gpio_mode(PIN_MOTOR_LEFT_PWM,  GPIO_OUTPUT);
        hal_gpio_mode(PIN_MOTOR_RIGHT_PWM, GPIO_OUTPUT);
        hal_gpio_write(PIN_MOTOR_LEFT_PWM,  GPIO_LOW);
        hal_gpio_write(PIN_MOTOR_RIGHT_PWM, GPIO_LOW);

        uint8_t sreg = SREG;
        cli();

        TCCR1A = 0;
        TCCR1B = 0;
        TCNT1  = 0;

        if (config->mode == PWM_MODE_PHASE_CORRECT) {
            /* WGM13:10 = 1010, Phase-Correct PWM, TOP=ICR1 */
            TCCR1A = (1 << COM1A1) | (1 << COM1B1) | (1 << WGM11);
            TCCR1B = (1 << WGM13);
        } else {
            /* WGM13:10 = 1110, Fast PWM, TOP=ICR1 */
            TCCR1A = (1 << COM1A1) | (1 << COM1B1) | (1 << WGM11);
            TCCR1B = (1 << WGM13) | (1 << WGM12);
        }

        /* Прескалер */
        uint8_t cs = config->prescaler & 0x07;
        TCCR1B |= cs;

        /* TOP */
        ICR1 = config->top;

        /* PWM = 0 */
        OCR1A = 0;
        OCR1B = 0;

        SREG = sreg;

        /* Сохраняем состояние */
        pwm_state[timer].top = config->top;
        pwm_state[timer].pwm[0] = 0;
        pwm_state[timer].pwm[1] = 0;
        pwm_state[timer].mode = config->mode;
        pwm_state[timer].prescaler_idx = cs;

    } else if (timer == PWM_TIMER_EPS) {
        /* --- Timer0: D6 (OC0A) --- */
        /* Timer0 используется Arduino для millis()!
         * В MVP-2+ рассмотреть перенос millis на Timer2
         * или использование другого подхода для EPS PWM.
         * Пока НЕ инициализируем Timer0 — оставляем для millis().
         */
        pwm_state[timer].top = 255;
        pwm_state[timer].pwm[0] = 0;
        pwm_state[timer].pwm[1] = 0;
    }
}

void hal_pwm_set(pwm_timer_id_t timer, pwm_channel_t channel, uint16_t value)
{
    if (timer >= PWM_TIMER_COUNT) return;
    if (value > pwm_state[timer].top) value = pwm_state[timer].top;

    if (timer == PWM_TIMER_MOTORS) {
        uint8_t sreg = SREG;
        cli();
        if (channel == PWM_CH_A) {
            OCR1A = value;
        } else {
            OCR1B = value;
        }
        SREG = sreg;
    }

    pwm_state[timer].pwm[channel & 1] = value;
}

uint16_t hal_pwm_get(pwm_timer_id_t timer, pwm_channel_t channel)
{
    if (timer >= PWM_TIMER_COUNT) return 0;
    return pwm_state[timer].pwm[channel & 1];
}

void hal_pwm_set_both(pwm_timer_id_t timer, uint16_t a, uint16_t b)
{
    if (timer >= PWM_TIMER_COUNT) return;
    uint16_t top = pwm_state[timer].top;
    if (a > top) a = top;
    if (b > top) b = top;

    if (timer == PWM_TIMER_MOTORS) {
        uint8_t sreg = SREG;
        cli();
        OCR1A = a;
        OCR1B = b;
        SREG = sreg;
    }

    pwm_state[timer].pwm[0] = a;
    pwm_state[timer].pwm[1] = b;
}

void hal_pwm_reconfigure(pwm_timer_id_t timer, const pwm_config_t *config)
{
    /* ПРЕДУСЛОВИЕ: оба канала PWM == 0 */
    hal_pwm_init(timer, config);
}

uint16_t hal_pwm_calc_top(uint32_t target_freq_hz, pwm_mode_t mode, uint8_t prescaler)
{
    if (prescaler > 5 || prescaler == 0 || target_freq_hz == 0) return 1023;

    uint32_t pv = prescaler_values[prescaler];
    uint32_t top;

    if (mode == PWM_MODE_PHASE_CORRECT) {
        /* f = F_CPU / (2 × prescaler × TOP) → TOP = F_CPU / (2 × pv × f) */
        top = BOARD_CLOCK_HZ / (2UL * pv * target_freq_hz);
    } else {
        /* f = F_CPU / (prescaler × (TOP+1)) → TOP = F_CPU / (pv × f) - 1 */
        top = BOARD_CLOCK_HZ / (pv * target_freq_hz) - 1;
    }

    if (top > 65535) top = 65535;
    if (top < 1) top = 1;
    return (uint16_t)top;
}

uint16_t hal_pwm_get_frequency(pwm_timer_id_t timer)
{
    if (timer >= PWM_TIMER_COUNT) return 0;

    uint16_t top = pwm_state[timer].top;
    uint8_t  pi  = pwm_state[timer].prescaler_idx;
    if (pi == 0 || pi > 5) return 0;

    uint32_t pv = prescaler_values[pi];

    if (pwm_state[timer].mode == PWM_MODE_PHASE_CORRECT) {
        return (uint16_t)(BOARD_CLOCK_HZ / (2UL * pv * (uint32_t)(top + 1)));
    } else {
        return (uint16_t)(BOARD_CLOCK_HZ / (pv * (uint32_t)(top + 1)));
    }
}

uint16_t hal_pwm_get_top(pwm_timer_id_t timer)
{
    if (timer >= PWM_TIMER_COUNT) return 0;
    return pwm_state[timer].top;
}

/* ====================================================================
 *  hal_system
 *
 *  Использует Arduino core для millis() (Timer2).
 *  Watchdog через avr/wdt.h.
 * ==================================================================== */

void hal_system_init(void)
{
    /* Arduino core уже инициализировал Timer2 для millis() */
    /* Дополнительная инициализация не требуется */
}

uint32_t hal_system_millis(void)
{
    return millis();
}

void hal_system_delay_us(uint16_t us)
{
    delayMicroseconds(us);
}

void hal_system_reset(void)
{
    /* Используем watchdog для сброса */
    wdt_enable(WDTO_15MS);
    while (1) {}
}

void hal_system_wdt_enable(void)
{
    wdt_enable(WDTO_8S);
}

void hal_system_wdt_reset(void)
{
    wdt_reset();
}

void hal_system_irq_disable(void)
{
    cli();
}

void hal_system_irq_enable(void)
{
    sei();
}

uint8_t hal_system_irq_save(void)
{
    uint8_t sreg = SREG;
    cli();
    return sreg;
}

void hal_system_irq_restore(uint8_t state)
{
    SREG = state;
}

/* ====================================================================
 *  hal_eeprom
 * ==================================================================== */

void hal_eeprom_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    eeprom_read_block(buf, (const void *)(uintptr_t)addr, len);
}

void hal_eeprom_write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    eeprom_update_block(buf, (void *)(uintptr_t)addr, len);
}

uint8_t hal_eeprom_read_byte(uint16_t addr)
{
    return eeprom_read_byte((const uint8_t *)(uintptr_t)addr);
}

void hal_eeprom_write_byte(uint16_t addr, uint8_t data)
{
    eeprom_update_byte((uint8_t *)(uintptr_t)addr, data);
}

/* ====================================================================
 *  hal_uart — обёртка над MICRO_UART + RS485
 * ==================================================================== */

void hal_uart_init(uint32_t baud)
{
    (void)baud;  /* MICRO_UART использует BAUD_RATE из своего .h */
    serial_init();

    /* RS485: пин направления как выход, начальное состояние — приём */
    hal_gpio_mode(PIN_RS485_DE_RE, GPIO_OUTPUT);
    hal_gpio_write(PIN_RS485_DE_RE, GPIO_LOW);
}

void hal_uart_write(uint8_t data)
{
    serial_write(data);
}

void hal_uart_write_buf(const uint8_t *buf, uint8_t len)
{
    for (uint8_t i = 0; i < len; i++) {
        serial_write(buf[i]);
    }
}

uint8_t hal_uart_read(void)
{
    return serial_read();
}

uint8_t hal_uart_available(void)
{
    return serial_get_rx_buffer_count();
}

void hal_uart_flush_rx(void)
{
    serial_reset_read_buffer();
}

void hal_uart_set_rs485_tx(uint8_t tx_mode)
{
    hal_gpio_write(PIN_RS485_DE_RE, tx_mode ? GPIO_HIGH : GPIO_LOW);
}

/* ====================================================================
 *  hal_encoder — INT0 (D2 = правое колесо) / INT1 (D3 = левое колесо)
 *
 *  Подсчёт импульсов на восходящем фронте. ISR минимальны:
 *  инкремент volatile-счётчика. Чтение и сброс — атомарно с cli/sei.
 * ==================================================================== */

#include "hal_encoder.h"

static volatile uint16_t encoder_count[ENCODER_COUNT] = {0, 0};

ISR(INT0_vect) {
    /* D2 = правое колесо */
    encoder_count[ENCODER_RIGHT]++;
}

ISR(INT1_vect) {
    /* D3 = левое колесо */
    encoder_count[ENCODER_LEFT]++;
}

void hal_encoder_init(void)
{
    /* D2/D3 как входы с pull-up (драйверы колёс часто open-collector) */
    hal_gpio_mode(PIN_ENCODER_RIGHT, GPIO_INPUT_PULLUP);
    hal_gpio_mode(PIN_ENCODER_LEFT,  GPIO_INPUT_PULLUP);

    /* INT0 (D2): восходящий фронт */
    EICRA |= (1 << ISC01) | (1 << ISC00);
    /* INT1 (D3): восходящий фронт */
    EICRA |= (1 << ISC11) | (1 << ISC10);

    /* Сброс флагов, разрешение прерываний */
    EIFR  |= (1 << INTF0) | (1 << INTF1);
    EIMSK |= (1 << INT0)  | (1 << INT1);

    encoder_count[ENCODER_LEFT]  = 0;
    encoder_count[ENCODER_RIGHT] = 0;
}

uint16_t hal_encoder_read_and_reset(encoder_channel_t ch)
{
    if (ch >= ENCODER_COUNT) return 0;
    uint8_t sreg = SREG;
    cli();
    uint16_t v = encoder_count[ch];
    encoder_count[ch] = 0;
    SREG = sreg;
    return v;
}

uint16_t hal_encoder_get_count(encoder_channel_t ch)
{
    if (ch >= ENCODER_COUNT) return 0;
    uint8_t sreg = SREG;
    cli();
    uint16_t v = encoder_count[ch];
    SREG = sreg;
    return v;
}
