/**
 * @file hal_system.h
 * @brief HAL: системные функции (тик, watchdog, прерывания, сброс)
 *
 * Реализация: hal_atmega328p.c
 */
#pragma once

#include <stdint.h>

/** Инициализация системного тика и базовой периферии */
void     hal_system_init(void);

/** Системное время в миллисекундах (uptime, переполняется через ~49 дней) */
uint32_t hal_system_millis(void);

/** Микросекундная задержка. ТОЛЬКО для аппаратных протоколов (HX711, bit-bang). */
void     hal_system_delay_us(uint16_t us);

/** Программный сброс MCU */
void     hal_system_reset(void);

/** Включить аппаратный watchdog (8 сек на ATmega328P) */
void     hal_system_wdt_enable(void);

/** Сбросить watchdog (вызывать чаще, чем раз в 8 сек) */
void     hal_system_wdt_reset(void);

/** Глобально запретить прерывания */
void     hal_system_irq_disable(void);

/** Глобально разрешить прерывания */
void     hal_system_irq_enable(void);

/** Сохранить состояние прерываний и запретить их (для атомарных секций) */
uint8_t  hal_system_irq_save(void);

/** Восстановить ранее сохранённое состояние прерываний */
void     hal_system_irq_restore(uint8_t state);
