/**
 * @file hal_atmega328p.cpp
 * @brief Реализация HAL для ATmega328P (Arduino Nano)
 *
 * ЕДИНСТВЕННЫЙ файл в проекте, содержащий платформозависимый код.
 * При портировании — заменить этот файл на hal_<platform>.cpp.
 *
 * Реализует:
 * - hal_gpio:    режим, запись и чтение вывода прямо через регистры портов
 * - hal_adc:     10-бит АЦП через регистры ADMUX/ADCSRA
 * - hal_pwm:     Timer1 Phase-Correct/Fast PWM, произвольный TOP
 * - hal_system:  millis/micros на своём Timer0, watchdog, IRQ
 * - hal_nvm:     долговременная память, на этой платформе avr/eeprom.h
 * - util_rom:    чтение постоянной памяти, на этой платформе PROGMEM
 * - hal_uart:    регистры USART0 + кольца util_ring + RS485 DE/RE
 *
 * @version 1.0.0 (MVP-1)
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <avr/pgmspace.h>
#include <util/delay_basic.h>   /* _delay_loop_2 */

#include "cfg_board.h"
#include "hal_gpio.h"
#include "hal_adc.h"
#include "hal_pwm.h"
#include "hal_system.h"
#include "hal_nvm.h"
#include "hal_uart.h"
#include "util_ring.h"
#include "util_rom.h"

/* ====================================================================
 *  hal_gpio
 * ==================================================================== */

/*
 *  Номера выводов - те же, что печатает плата Nano, чтобы cfg_board.h
 *  читался рядом со схемой. Раскладка по портам ATmega328P:
 *
 *    D0..D7   -> PORTD, биты 0..7
 *    D8..D13  -> PORTB, биты 0..5
 *    A0..A5   -> PORTC, биты 0..5   (номера 14..19)
 *
 *  A6 и A7 у Nano только аналоговые, цифрового порта у них нет - для них
 *  функции возвращают отказ, а не делают вид, что записали (CLAUDE.md §7).
 */

typedef struct {
    volatile uint8_t *ddr;
    volatile uint8_t *port;
    volatile uint8_t *pin_reg;
    uint8_t           bit;
} gpio_map_t;

static uint8_t gpio_resolve(uint8_t pin, gpio_map_t *out)
{
    if (pin <= 7) {
        out->ddr = &DDRD; out->port = &PORTD; out->pin_reg = &PIND;
        out->bit = pin;
    } else if (pin <= 13) {
        out->ddr = &DDRB; out->port = &PORTB; out->pin_reg = &PINB;
        out->bit = (uint8_t)(pin - 8);
    } else if (pin <= 19) {
        out->ddr = &DDRC; out->port = &PORTC; out->pin_reg = &PINC;
        out->bit = (uint8_t)(pin - 14);
    } else {
        return 0;               /* цифрового порта у вывода нет */
    }
    return 1;
}

void hal_gpio_mode(uint8_t pin, gpio_mode_t mode)
{
    gpio_map_t m;
    if (!gpio_resolve(pin, &m)) return;

    uint8_t mask = (uint8_t)(1 << m.bit);
    uint8_t sreg = SREG;
    cli();                      /* чтение-изменение-запись регистра порта */
    switch (mode) {
        case GPIO_INPUT:
            *m.ddr  = (uint8_t)(*m.ddr  & ~mask);
            *m.port = (uint8_t)(*m.port & ~mask);   /* подтяжка выключена */
            break;
        case GPIO_INPUT_PULLUP:
            *m.ddr  = (uint8_t)(*m.ddr  & ~mask);
            *m.port = (uint8_t)(*m.port |  mask);   /* подтяжка включена */
            break;
        case GPIO_OUTPUT:
            *m.ddr  = (uint8_t)(*m.ddr  |  mask);
            break;
    }
    SREG = sreg;
}

void hal_gpio_write(uint8_t pin, gpio_state_t state)
{
    gpio_map_t m;
    if (!gpio_resolve(pin, &m)) return;

    uint8_t mask = (uint8_t)(1 << m.bit);
    uint8_t sreg = SREG;
    cli();
    if (state == GPIO_HIGH) *m.port = (uint8_t)(*m.port |  mask);
    else                    *m.port = (uint8_t)(*m.port & ~mask);
    SREG = sreg;
}

gpio_state_t hal_gpio_read(uint8_t pin)
{
    gpio_map_t m;
    if (!gpio_resolve(pin, &m)) return GPIO_LOW;
    return ((*m.pin_reg & (uint8_t)(1 << m.bit)) != 0) ? GPIO_HIGH : GPIO_LOW;
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

static uint16_t adc_bad_channel = 0;
static uint16_t adc_unbound = 0;

/* Таблица привязки сигналов к каналам (ADR-0020). ADC_CHANNEL_COUNT
   означает "не привязан": это значение недостижимо для настоящего канала. */
static uint8_t adc_signal_map[ANALOG_SIGNAL_COUNT] = {
    ADC_CHANNEL_COUNT, ADC_CHANNEL_COUNT, ADC_CHANNEL_COUNT,
    ADC_CHANNEL_COUNT, ADC_CHANNEL_COUNT
};

void hal_adc_bind(analog_signal_t signal, uint8_t channel)
{
    if (signal >= ANALOG_SIGNAL_COUNT) {
        return;
    }
    adc_signal_map[signal] = channel;
}

uint16_t hal_adc_unbound_count(void)
{
    return adc_unbound;
}

uint16_t hal_adc_read_signal(analog_signal_t signal)
{
    if (signal >= ANALOG_SIGNAL_COUNT ||
        adc_signal_map[signal] >= ADC_CHANNEL_COUNT) {
        if (adc_unbound != 0xFFFF) {
            adc_unbound++;
        }
        return 0;   /* обоснование в hal_adc.h */
    }
    return hal_adc_read(adc_signal_map[signal]);
}


uint16_t hal_adc_bad_channel_count(void)
{
    return adc_bad_channel;
}

uint16_t hal_adc_read(uint8_t channel)
{
    /*
     * Каналы 0–7 на ATmega328P: 0–5 выведены и как цифровые, 6–7 только
     * аналоговые. ADMUX принимает номер канала, и только его.
     *
     * Здесь раньше стояло «if (channel >= 14) channel -= 14;» — попытка
     * принять заодно нумерацию выводов Arduino, где A0 = 14. Функция
     * угадывала, что ей передали, по величине числа. Следом шло
     * «channel &= 0x07», превращавшее любое лишнее значение в валидный
     * канал молча: 9 читалось как канал 1. Обе строки убраны вместе
     * с переездом карты каналов в настройки (ADR-0009).
     */
    if (channel >= ADC_CHANNEL_COUNT) {
        if (adc_bad_channel != 0xFFFF) {
            adc_bad_channel++;
        }
        return 0;   /* безопасное значение; обоснование в hal_adc.h */
    }

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
        /* Timer0 занят системным временем (millis/micros, см. hal_system).
         * Ядро Arduino здесь ни при чём: таймер настраивает эта же прошивка,
         * и менять его режим ради ШИМ руля нельзя - остановятся часы.
         * ШИМ электроусилителя в MVP-2 пойдёт на Timer2, который свободен.
         * Пока Timer0 не трогаем вовсе. */
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
 *  Собственное системное время на Timer0 (предделитель 64).
 *  Timer1 занят ШИМ моторов, Timer2 свободен.
 *  Watchdog через avr/wdt.h.
 * ==================================================================== */

/*
 *  Системное время на собственном Timer0.
 *
 *  Предделитель 64 при 16 МГц: такт 4 мкс, переполнение каждые 256 тактов,
 *  то есть каждые 1024 мкс. Прерывание переполнения ведёт два счётчика.
 *
 *  Миллисекунды копятся точно, без дрейфа: переполнение даёт 1024 мкс, то
 *  есть миллисекунду и ещё 24 микросекунды. Остаток накапливается и раз в
 *  42 переполнения выдаёт лишнюю миллисекунду. Ошибки не остаётся вовсе -
 *  в отличие от округления 1024 до 1000 или до 1024.
 */

static volatile uint32_t timer0_overflows;
static volatile uint32_t timer0_millis;
static volatile uint16_t timer0_millis_frac_us;

ISR(TIMER0_OVF_vect)
{
    timer0_overflows++;

    uint16_t frac = timer0_millis_frac_us + 24;   /* 1024 = 1000 + 24 */
    uint32_t ms   = timer0_millis + 1;
    if (frac >= 1000) {
        frac -= 1000;
        ms++;
    }
    timer0_millis_frac_us = frac;
    timer0_millis = ms;
}

void hal_system_init(void)
{
    /* Timer0: обычный режим счёта, предделитель 64, прерывание переполнения.
       Timer1 занят ШИМ моторов, Timer2 свободен. */
    TCCR0A = 0;
    TCCR0B = (1 << CS01) | (1 << CS00);      /* clk/64 */
    TCNT0  = 0;
    TIFR0  = (1 << TOV0);                     /* сбросить висящий флаг */
    TIMSK0 = (1 << TOIE0);

    timer0_overflows      = 0;
    timer0_millis         = 0;
    timer0_millis_frac_us = 0;

    sei();
}

uint32_t hal_system_millis(void)
{
    uint8_t sreg = SREG;
    cli();
    uint32_t ms = timer0_millis;
    SREG = sreg;
    return ms;
}

uint32_t hal_system_micros(void)
{
    uint8_t sreg = SREG;
    cli();

    uint32_t ovf = timer0_overflows;
    uint8_t  cnt = TCNT0;

    /* Переполнение могло произойти между чтением счётчика и запретом
       прерываний: флаг взведён, обработчик ещё не отработал. Проверка
       cnt < 255 отсеивает случай, когда флаг относится к переполнению,
       которое произойдёт прямо сейчас. */
    if ((TIFR0 & (1 << TOV0)) && (cnt < 255)) {
        ovf++;
    }

    SREG = sreg;

    /* ovf x 1024 + cnt x 4 = (ovf x 256 + cnt) x 4 */
    return ((ovf << 8) | cnt) * 4UL;
}

void hal_system_delay_us(uint16_t us)
{
    /* _delay_loop_2 из avr-libc тратит ровно 4 такта на единицу счёта.
       При 16 МГц микросекунда - это 16 тактов, то есть 4 единицы.
       Потолок счётчика 65535 даёт предел 16383 мкс за вызов; больше
       никакому аппаратному протоколу здесь не нужно, а тихо обрезать
       нельзя - поэтому длинная задержка разбивается на части. */
    while (us > 16000) {
        _delay_loop_2(64000U);        /* 16000 мкс x 4 единицы; int на AVR 16-битный */
        us = (uint16_t)(us - 16000);
    }
    if (us) _delay_loop_2((uint16_t)(us * 4));
}

void hal_system_reset(void)
{
    /* ОСТОРОЖНО: на стоковом загрузчике Nano этот вызов не перезагружает
       плату, а выводит её из строя - загрузчик не сбрасывает WDRF и уходит
       в цикл перезагрузок (подробности в cfg_board.h, ADR-0022). Функция
       сейчас не вызывается ниоткуда; прежде чем её звать, нужен либо
       optiboot, либо другой способ сброса. */
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
 *  hal_nvm — долговременная память (ADR-0021)
 * ==================================================================== */

void hal_nvm_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    eeprom_read_block(buf, (const void *)(uintptr_t)addr, len);
}

void hal_nvm_write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    eeprom_update_block(buf, (void *)(uintptr_t)addr, len);
}

uint8_t hal_nvm_read_byte(uint16_t addr)
{
    return eeprom_read_byte((const uint8_t *)(uintptr_t)addr);
}

void hal_nvm_write_byte(uint16_t addr, uint8_t data)
{
    eeprom_update_byte((uint8_t *)(uintptr_t)addr, data);
}

/* ====================================================================
 *  hal_uart — USART0 на кольцах util_ring + RS485
 *
 *  Переносимая часть (индексы, заполненность, отбрасывание блока целиком)
 *  живёт в util_ring и проверяется десктопными тестами. Здесь остаётся то,
 *  что можно проверить только на железе: регистры, делитель и прерывания.
 *  Решение ADR-0018.
 * ==================================================================== */

static uint8_t     uart_rx_storage[RX_BUFFER_SIZE + 1];
static uint8_t     uart_tx_storage[TX_BUFFER_SIZE + 1];
static util_ring_t uart_rx;
static util_ring_t uart_tx;

static hal_uart_tx_policy_t uart_tx_policy = HAL_UART_TX_DROP_PACKET;

void hal_uart_init(uint32_t baud)
{
    util_ring_init(&uart_rx, uart_rx_storage, (uint8_t)(RX_BUFFER_SIZE + 1));
    util_ring_init(&uart_tx, uart_tx_storage, (uint8_t)(TX_BUFFER_SIZE + 1));

    /* Удвоитель включён всегда: на 16 МГц он даёт точный делитель для 250000
       (UBRR=7, ошибка 0,00 %) и не портит низкие скорости — 9600 получает
       UBRR=207 и ошибку +0,16 %. Прежний код выбирал режим директивой #if
       по константе, то есть скорость нельзя было задать во время выполнения.
       Округление до ближайшего, а не отбрасывание дробной части: при
       отбрасывании 115200 давала бы делитель на единицу больше и ошибку
       в полтора раза выше. Основание: ADR-0015. */
    UCSR0A |= (1 << U2X0);
    uint32_t divisor = 8UL * baud;
    uint16_t ubrr = (uint16_t)(((F_CPU + divisor / 2) / divisor) - 1);
    UBRR0H = (uint8_t)(ubrr >> 8);
    UBRR0L = (uint8_t)ubrr;

    /* Приём, передача, прерывание по завершении приёма. 8N1 — умолчание. */
    UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);

    /* RS485: пин направления как выход, начальное состояние — приём */
    hal_gpio_mode(PIN_RS485_DE_RE, GPIO_OUTPUT);
    hal_gpio_write(PIN_RS485_DE_RE, GPIO_LOW);
}

void hal_uart_set_tx_policy(hal_uart_tx_policy_t policy)
{
    uart_tx_policy = policy;
}

uint16_t hal_uart_tx_dropped(void)
{
    return util_ring_dropped(&uart_tx);
}

uint8_t hal_uart_write_buf(const uint8_t *buf, uint8_t len)
{
    if (uart_tx_policy == HAL_UART_TX_BLOCK) {
        /* Ожидание готовности регистра, а не delay(): время ограничено
           скоростью линии. У передатчика нет управления потоком, поэтому
           кольцо опустошается независимо ни от чего, и цикл конечен.
           Границы посчитаны в ADR-0016: на 250000 один байт — 40 мкс,
           кадр телеметрии в полностью занятое кольцо — 1,64 мс. */
        for (uint8_t i = 0; i < len; i++) {
            while (util_ring_free(&uart_tx) == 0) {
                UCSR0B |= (1 << UDRIE0);   /* передача точно идёт */
            }
            util_ring_put(&uart_tx, buf[i]);
            UCSR0B |= (1 << UDRIE0);
        }
        return 1;
    }

    if (!util_ring_put_all(&uart_tx, buf, len)) {
        return 0;                          /* счётчик увеличен внутри */
    }
    UCSR0B |= (1 << UDRIE0);
    return 1;
}

void hal_uart_write(uint8_t data)
{
    (void)hal_uart_write_buf(&data, 1);
}

int16_t hal_uart_read(void)
{
    return util_ring_get(&uart_rx);
}

uint8_t hal_uart_available(void)
{
    return util_ring_count(&uart_rx);
}

void hal_uart_flush_rx(void)
{
    util_ring_reset(&uart_rx);
}

/** Приём байта. Переполнение кольца приёма молча теряет байт: кадр
    всё равно не сойдётся по CRC, и парсер отбросит его сам. */
ISR(USART_RX_vect)
{
    uint8_t data = UDR0;
    (void)util_ring_put(&uart_rx, data);
}

/** Регистр передачи освободился. Выключаем прерывание, когда отдавать
    больше нечего, иначе оно возникало бы непрерывно. */
ISR(USART_UDRE_vect)
{
    int16_t b = util_ring_get(&uart_tx);
    if (b < 0) {
        UCSR0B &= ~(1 << UDRIE0);
        return;
    }
    UDR0 = (uint8_t)b;
}

void hal_uart_set_rs485_tx(uint8_t tx_mode)
{
    hal_gpio_write(PIN_RS485_DE_RE, tx_mode ? GPIO_HIGH : GPIO_LOW);
}

/* ====================================================================
 *  hal_encoder — INT0 (D2 = правое колесо) / INT1 (D3 = левое колесо)
 *
 *  ADR-0023: скорость считается по времени между импульсами, а не по
 *  их количеству за окно. ISR берёт метку времени и запоминает разность
 *  с предыдущей; счётчик импульсов остаётся для одометра.
 *
 *  ISR по-прежнему короткий: hal_system_micros() - это чтение TCNT0 и
 *  счётчика переполнений, десятки тактов, без деления. Прерывания внутри
 *  обработчика уже запрещены, так что атомарность достаётся даром.
 * ==================================================================== */

#include "hal_encoder.h"
#include "hal_system.h"

typedef struct {
    volatile uint32_t period_us;
    volatile uint32_t last_pulse_us;
    volatile uint16_t pulses;
    volatile uint16_t glitches;
    volatile uint8_t  has_period;
    volatile uint8_t  primed;      /**< первая метка времени уже взята */
} encoder_state_t;

static encoder_state_t encoder_state[ENCODER_COUNT];

/**
 * @brief Общее тело обработчика фронта
 *
 * Вызывается из ISR, поэтому прерывания уже запрещены и атомарность
 * обеспечена аппаратно.
 */
static inline void encoder_pulse(uint8_t ch, uint32_t now)
{
    encoder_state_t *e = &encoder_state[ch];

    if (!e->primed) {
        /* Первый фронт с запуска: предыдущей метки нет, разность считать
           не от чего. Период появится на втором фронте. */
        e->primed = 1;
        e->last_pulse_us = now;
        e->pulses++;
        return;
    }

    uint32_t dt = now - e->last_pulse_us;   /* беззнаковая разность переживает переполнение */

    if (dt < ENCODER_MIN_PERIOD_US) {
        /* Физически невозможный интервал: наводка или дребезг. Фронт не
           считается импульсом вообще - иначе он испортил бы и период,
           и одометр. */
        e->glitches++;
        return;
    }

    e->period_us     = dt;
    e->last_pulse_us = now;
    e->pulses++;
    e->has_period    = 1;
}

ISR(INT0_vect) {
    encoder_pulse(ENCODER_RIGHT, hal_system_micros());
}

ISR(INT1_vect) {
    encoder_pulse(ENCODER_LEFT, hal_system_micros());
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

    for (uint8_t i = 0; i < ENCODER_COUNT; i++) {
        encoder_state[i].period_us     = 0;
        encoder_state[i].last_pulse_us = 0;
        encoder_state[i].pulses        = 0;
        encoder_state[i].glitches      = 0;
        encoder_state[i].has_period    = 0;
        encoder_state[i].primed        = 0;
    }

    /* Сброс флагов, разрешение прерываний */
    EIFR  |= (1 << INTF0) | (1 << INTF1);
    EIMSK |= (1 << INT0)  | (1 << INT1);
}

void hal_encoder_take(encoder_channel_t ch, encoder_sample_t *out)
{
    if (out == 0) return;

    if (ch >= ENCODER_COUNT) {
        out->period_us     = 0;
        out->last_pulse_us = 0;
        out->pulses        = 0;
        out->has_period    = 0;
        return;
    }

    encoder_state_t *e = &encoder_state[ch];

    uint8_t sreg = SREG;
    cli();
    out->period_us     = e->period_us;
    out->last_pulse_us = e->last_pulse_us;
    out->pulses        = e->pulses;
    out->has_period    = e->has_period;
    e->pulses          = 0;
    SREG = sreg;
}

uint16_t hal_encoder_glitch_count(encoder_channel_t ch)
{
    if (ch >= ENCODER_COUNT) return 0;
    uint8_t sreg = SREG;
    cli();
    uint16_t v = encoder_state[ch].glitches;
    SREG = sreg;
    return v;
}

/* ====================================================================
 *  util_rom — чтение постоянной памяти (ADR-0021)
 *
 *  У AVR гарвардская архитектура: флеш и RAM адресуются разными
 *  инструкциями, и обычное разыменование указателя на данные с атрибутом
 *  progmem прочитало бы RAM по тому же численному адресу. Поэтому нужен
 *  pgm_read_*, и место ему здесь — это платформенная деталь.
 *
 *  Для платформ с единым адресным пространством те же функции реализованы
 *  в util_rom.cpp разыменованием указателя; там файл закрыт условием
 *  #ifndef __AVR__, здесь — наоборот, поэтому двух определений не бывает.
 * ==================================================================== */

uint8_t util_rom_read_u8(const void *addr)
{
    return pgm_read_byte(addr);
}

uint16_t util_rom_read_u16(const void *addr)
{
    return pgm_read_word(addr);
}

void util_rom_read_block(void *dst, const void *src, uint8_t len)
{
    memcpy_P(dst, src, len);
}
