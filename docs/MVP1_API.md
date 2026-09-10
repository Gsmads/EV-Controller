# MVP-1: API Documentation (Architecture v2)

**Версия:** 2.0.0  
**Дата:** 2026-05-12  
**Платформа:** Arduino Nano (ATmega328P, 16 МГц)  
**Scope:** Базовое управление двумя моторами с педалями газа и тормоза

---

## Архитектура

```
  Педаль газа (A0) ──┐         cfg_settings (EEPROM)
                     │              │
  Педаль тормоза (A1)┘              │ профиль режима,
         │                          │ калибровка, кривые
    [hal_adc]                       │
         │                          ▼
    [svc_pedals] ──→ target ──→ [svc_ramp] ──→ [svc_motor] ──→ [hal_pwm]
         │            pwm          │                │           Timer1
         │                         │                │        D9 (OC1A)
    [app_debug] ←──────────────────┘                │        D10 (OC1B)
         │                                          │
    [hal_uart] ──→ Serial (9600)                    │
    MICRO_UART                              [app_scheduler]
                                             10ms / 200ms
```

4-слойная архитектура: HAL → Drivers → Services → Application.
Платформозависимый код — ТОЛЬКО в `hal_atmega328p.cpp`.

---

## Структура файлов

### HAL (Layer 0) — аппаратная абстракция

| Файл | Описание |
|------|----------|
| `hal_gpio.h` | Цифровые пины (mode/read/write) |
| `hal_adc.h` | АЦП 10-бит |
| `hal_pwm.h` | ШИМ: Timer1 (моторы), Timer0 (EPS) |
| `hal_system.h` | millis, watchdog, IRQ save/restore |
| `hal_eeprom.h` | Чтение/запись EEPROM |
| `hal_uart.h` | UART + RS485 DE/RE |
| `hal_spi.h` | SPI (MVP-3+) |
| `hal_i2c.h` | I2C (MVP-3+) |
| `hal_encoder.h` | Счётчики импульсов колёс (MVP-2+) |
| **`hal_atmega328p.cpp`** | **Реализация всего HAL для ATmega328P** |

### Config (Cross-cutting)

| Файл | Описание |
|------|----------|
| `cfg_board.h` | Карта пинов, таймеры, условная компиляция |
| `cfg_settings.h` | Типы настроек (settings_t, drive_profile_t) |
| `cfg_settings.cpp` | EEPROM: load/save/defaults, CRC16 |

### Utils (Cross-cutting)

| Файл | Описание |
|------|----------|
| `util_math.h/.cpp` | map, clamp, EMA, кривые, PID, аккумулятор рампы |
| `util_crc.h/.cpp` | CRC16-CCITT |

### Services (Layer 2)

| Файл | Описание |
|------|----------|
| `svc_pedals.h/.cpp` | Чтение педалей: EMA → калибровка → deadzone → кривая |
| `svc_ramp.h/.cpp` | Рампа разгона/торможения с профилями |
| `svc_motor.h/.cpp` | Вывод PWM на моторы с масштабированием к TOP |

### Application (Layer 3)

| Файл | Описание |
|------|----------|
| `app_main.h/.cpp` | Кооперативный планировщик задач |
| `app_debug.h/.cpp` | Телеметрия через UART |
| `EV_Controller.ino` | Главный скетч |

### Существующие модули

| Файл | Описание |
|------|----------|
| `MICRO_UART.h/.cpp` | Прерывательный UART (Grbl) |
| `PRINT.h/.cpp` | Форматирование чисел без sprintf |
| `Inc.h` | Мастер-включений для PRINT.cpp |

---

## Ключевые API

### cfg_settings — настройки в EEPROM

```c
cfg_settings_init();                          // Загрузка из EEPROM или defaults
cfg_settings_save();                          // Сохранение в EEPROM
cfg_settings_reset_defaults();                // Сброс к заводским
const settings_t* s = cfg_settings_get();     // Чтение
settings_t* s = cfg_settings_get_mutable();   // Запись
cfg_settings_set_field(offset, data, size);   // Обновление по RS485

const drive_profile_t* p = cfg_settings_get_profile(DRIVE_MODE_ECO);
// p->max_pwm, p->accel_rate, p->decel_rate, p->brake_rate_min/max
// p->pwm_freq_hz, p->pwm_resolution, p->direction, p->pedal_curve
```

9 профилей режимов: Locked, Neutral, Handbrake, Eco, Normal, Sport, Reverse, Parent, Failsafe.

### hal_pwm — управление частотой ШИМ

```c
pwm_config_t cfg = {
    .mode = PWM_MODE_PHASE_CORRECT,
    .top = 1023,    // 10-бит → 7812 Гц
    .prescaler = 1  // без деления
};
hal_pwm_init(PWM_TIMER_MOTORS, &cfg);
hal_pwm_set_both(PWM_TIMER_MOTORS, left_pwm, right_pwm);

// Смена частоты (только при PWM=0!):
cfg.top = hal_pwm_calc_top(15625, PWM_MODE_PHASE_CORRECT, 1);  // → 511 (9-бит)
hal_pwm_reconfigure(PWM_TIMER_MOTORS, &cfg);
```

### svc_pedals — педали

```c
svc_pedals_init();
svc_pedals_update();                    // 100 Гц
uint16_t gas   = svc_pedals_get_gas();  // 0–1023
uint16_t brake = svc_pedals_get_brake();
uint16_t target = svc_pedals_get_target_pwm(max_pwm, motor_deadzone);
```

Параметры (калибровка, кривая, фильтр) берутся из cfg_settings автоматически.

### svc_ramp — рампа

```c
svc_ramp_init(100);  // 100 Гц
uint16_t pwm = svc_ramp_update(target, brake, &profile);
// Скорости из drive_profile_t: accel_rate, decel_rate, brake_rate_min/max
```

### app_scheduler — планировщик

```c
app_scheduler_init();
app_scheduler_add(task_control,   10);   // 100 Гц
app_scheduler_add(task_telemetry, 200);  // 5 Гц
// В loop():
app_scheduler_run();
```

---

## Тестирование

### Десктопные тесты (71 тест)

```bash
cd tests

# util_math: 33 теста (map, clamp, EMA, curves, PID, accumulator)
g++ -std=c++11 -I.. -o test_util_math test_util_math.c ../util_math.cpp
./test_util_math

# cfg_settings: 28 тестов (defaults, save/load, CRC corruption, profiles)
g++ -std=c++11 -I.. -o test_cfg_settings test_cfg_settings.c ../cfg_settings.cpp ../util_crc.cpp
./test_cfg_settings

# svc_ramp: 10 тестов (eco/sport accel, profile switch, brake, reset)
g++ -std=c++11 -I.. -o test_svc_ramp test_svc_ramp.cpp ../svc_ramp.cpp ../util_math.cpp
./test_svc_ramp
```

### Тестирование на железе

1. Подключить потенциометры к A0 (газ) и A1 (тормоз)
2. Осциллоскоп или LED на D9/D10
3. Serial Monitor 9600 бод
4. Калибровка: замерить фактический RAW-диапазон потенциометров,
   обновить через RS485 (MVP-4+) или в defaults cfg_settings.c

---

## Подготовка к MVP-2

| Модуль | Что добавится |
|--------|---------------|
| `hal_encoder` | ISR для INT0/INT1, счётчики импульсов |
| `svc_speed` | Расчёт RPM, км/ч, одометр |
| `drv_hx711` | Чтение тензодатчика усилия руля |
| `svc_eps` | PID-регулятор EPS (util_pid уже готов!) |

Timer0 (D6) — для EPS. Нужно решить конфликт с millis().
