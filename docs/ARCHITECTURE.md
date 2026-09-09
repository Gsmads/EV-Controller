# Архитектура прошивки контроллера детского электромобиля (Блок 2)

**Версия:** 2.0.0  
**Дата:** 2026-05-11  
**Платформа:** Arduino Nano (ATmega328P, 16 МГц) — первичная; архитектура позволяет портирование  
**Документ:** описание слоёв, модулей, потоков данных, конечных автоматов и проектных решений

---

## 1. Принципы проектирования

### 1.1 Ключевые требования к архитектуре

1. **Безопасность прежде всего** — fail-safe поведение при любом отказе (потеря связи, 
   перегрузка, зависание). Watchdog, ограничения тока, приоритет тормоза — на уровне ядра.

2. **Платформонезависимость** — вся аппаратно-зависимая логика изолирована в HAL (Hardware 
   Abstraction Layer). Бизнес-логика (педали, рампа, PID, режимы) не содержит ни одного
   обращения к регистрам, Arduino-функциям или avr-специфичному коду.

3. **Конфигурируемость без перепрошивки** — все параметры (калибровка педалей, PID-коэффициенты,
   лимиты тока, профили режимов, частоты ШИМ) хранятся в EEPROM с CRC-защитой и могут 
   обновляться по RS485 из веб-интерфейса.

4. **Модульность** — каждый модуль имеет чётко определённый интерфейс (init/update/get/set),
   не зависит от деталей реализации других модулей и может быть протестирован изолированно 
   на десктопе.

5. **Предсказуемое тактирование** — кооперативный планировщик задач с фиксированными 
   интервалами, без блокирующих задержек (delay).

6. **Расширяемость** — добавление новых функций (ABS, traction control, дифференциал, 
   парктроники) не требует переписывания существующих модулей.

### 1.2 Чего мы НЕ используем из Arduino

| Функция Arduino | Замена в проекте | Причина |
|----------------|------------------|---------|
| `analogWrite()` | HAL PWM | Только 8 бит, не поддерживает Phase-Correct с ICR1 |
| `analogRead()` | HAL ADC | Обёртка для портируемости |
| `digitalWrite()` | HAL GPIO | Обёртка для портируемости |
| `pinMode()` | HAL GPIO | Обёртка для портируемости |
| `millis()` | HAL System | Обёртка для портируемости |
| `delay()` | **Запрещён** | Блокирует управление, нарушает тактирование |
| `Serial.print()` | MICRO_UART + PRINT | Экономия Flash, interrupt-driven |
| `Wire` (I2C) | HAL I2C | Обёртка для портируемости |
| `SPI` | HAL SPI | Обёртка для портируемости |

### 1.3 Соглашения об именовании

```
Файлы:     hal_<subsystem>.h    — HAL-интерфейсы
           drv_<device>.h       — драйверы устройств
           svc_<function>.h     — сервисные модули (бизнес-логика)
           app_<component>.h    — прикладной уровень
           cfg_<topic>.h        — конфигурация и настройки

Функции:   hal_<subsys>_<verb>()          — HAL: hal_adc_read()
           drv_<device>_<verb>()          — Драйвер: drv_hc595_write()
           svc_<module>_<verb>()          — Сервис: svc_ramp_update()
           app_<component>_<verb>()       — Приложение: app_main_loop()

Типы:      <module>_state_t              — Структуры состояния
           <module>_config_t             — Конфигурационные структуры
           <module>_id_t                 — Перечисления идентификаторов

Константы: <MODULE>_<NAME>               — Верхний регистр с подчёркиваниями
```

---

## 2. Слоистая архитектура

```
┌─────────────────────────────────────────────────────────────────┐
│  Layer 3: APPLICATION                                           │
│  ┌──────────┐  ┌──────────────┐  ┌────────────┐                │
│  │ app_main │  │ app_protocol │  │ app_diag   │                │
│  │ (loop,   │  │ (RS485 cmd   │  │ (DTC codes,│                │
│  │ scheduler│  │  parser)     │  │  logging)  │                │
│  └──────────┘  └──────────────┘  └────────────┘                │
├─────────────────────────────────────────────────────────────────┤
│  Layer 2: SERVICES (бизнес-логика, platform-independent)        │
│  ┌───────────┐ ┌──────────┐ ┌───────────┐ ┌──────────────────┐ │
│  │svc_pedals │ │ svc_ramp │ │ svc_motor │ │ svc_drive_mode   │ │
│  │(фильтр,  │ │(разгон/  │ │(координа- │ │(state machine,   │ │
│  │ калибр.,  │ │ тормож., │ │ ция двух  │ │ профили режимов) │ │
│  │ кривые)   │ │ аккумул.)│ │ моторов)  │ │                  │ │
│  └───────────┘ └──────────┘ └───────────┘ └──────────────────┘ │
│  ┌───────────┐ ┌──────────┐ ┌───────────┐ ┌──────────────────┐ │
│  │ svc_eps   │ │svc_safety│ │svc_speed  │ │svc_telemetry     │ │
│  │(PID руля, │ │(overcurr,│ │(encoder   │ │(формирование     │ │
│  │ адаптив.) │ │ watchdog,│ │ RPM,      │ │ пакетов)         │ │
│  │           │ │ fail-safe│ │ одометр)  │ │                  │ │
│  └───────────┘ └──────────┘ └───────────┘ └──────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│  Layer 1: DRIVERS (аппаратно-специфичная логика, через HAL)     │
│  ┌───────────┐ ┌──────────┐ ┌───────────┐ ┌──────────────────┐ │
│  │drv_zsx11h │ │drv_hc595 │ │drv_pca9535│ │ drv_hx711        │ │
│  │(мотор:    │ │(shift reg│ │(I2C GPIO  │ │ (тензодатчик     │ │
│  │ PWM+ctrl) │ │ SPI)     │ │ expander) │ │  24-bit ADC)     │ │
│  └───────────┘ └──────────┘ └───────────┘ └──────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│  Layer 0: HAL (аппаратная абстракция — ЕДИНСТВЕННЫЙ             │
│           слой, знающий о конкретном MCU)                       │
│  ┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐ ┌────────┐│
│  │hal_gpio│ │hal_adc│ │hal_pwm│ │hal_uart││hal_i2c│ │hal_spi ││
│  └───────┘ └───────┘ └───────┘ └───────┘ └───────┘ └────────┘│
│  ┌──────────┐ ┌────────────┐ ┌───────────┐                    │
│  │hal_eeprom│ │ hal_system │ │hal_encoder│                    │
│  │          │ │(tick, wdt, │ │(ext IRQ)  │                    │
│  │          │ │ irq, reset)│ │           │                    │
│  └──────────┘ └────────────┘ └───────────┘                    │
├─────────────────────────────────────────────────────────────────┤
│  Cross-cutting modules (доступны из любого слоя)                │
│  ┌────────────────┐ ┌────────────────┐ ┌─────────────────────┐ │
│  │ cfg_settings   │ │ cfg_board      │ │ util_math / crc     │ │
│  │ (EEPROM store, │ │ (pin map,      │ │ (map, clamp, EMA,   │ │
│  │  defaults,     │ │  timer assign, │ │  PID, response      │ │
│  │  versioning)   │ │  HW constants) │ │  curves, CRC16)     │ │
│  └────────────────┘ └────────────────┘ └─────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### 2.1 Правила зависимостей

```
Application  →  Services  →  Drivers  →  HAL
     ↓             ↓            ↓          ↓
  Cross-cutting (cfg_settings, cfg_board, util_*)
```

- **Вниз — можно.** Каждый слой вызывает только нижестоящий.
- **Вверх — нельзя.** HAL не знает о существовании сервисов.
- **Горизонтально — через интерфейс.** svc_motor вызывает svc_ramp, 
  но не лезет в его внутреннее состояние.
- **Cross-cutting — из любого слоя.** cfg_settings и утилиты доступны всем.

---

## 3. Cross-cutting: конфигурация и настройки

### 3.1 cfg_board — карта оборудования

Статическая конфигурация, определённая на этапе компиляции. Описывает конкретную плату:

```c
// cfg_board.h — ЧТО подключено и КУДА
#define BOARD_PIN_MOTOR_L_PWM       9
#define BOARD_PIN_MOTOR_R_PWM      10
#define BOARD_TIMER_MOTORS          1       // Timer1 для моторов
#define BOARD_TIMER_EPS             0       // Timer0 для EPS
#define BOARD_HAS_HC595             1       // Есть сдвиговый регистр
#define BOARD_HAS_PCA9535           1       // Есть расширитель портов
#define BOARD_ENCODER_PULSES_PER_REV  12    // Уточнить экспериментально
#define BOARD_WHEEL_DIAMETER_MM     200     // Уточнить замером
```

Не сохраняется в EEPROM — это аппаратные факты.

### 3.2 cfg_settings — хранилище настроек

Все параметры, которые пользователь (родитель) может менять, хранятся в EEPROM.

**Структура хранения:**

```
┌──────────────────────────────────────────────────┐
│ EEPROM Layout (1024 bytes on ATmega328P)          │
│                                                   │
│ [0x00..0x01]  magic          (0xEV01)             │
│ [0x02]        version        (layout version)     │
│ [0x03]        flags          (bit field)           │
│ [0x04..0x?? ] settings_data  (settings_t struct)  │
│ [last 2]      crc16          (over all above)     │
│                                                   │
│ Свободное место — для будущих расширений          │
└──────────────────────────────────────────────────┘
```

**Что хранится (полный перечень):**

```c
typedef struct {
    // --- Калибровка педалей ---
    uint16_t pedal_gas_min;         // RAW АЦП при отпущенной
    uint16_t pedal_gas_max;         // RAW АЦП при нажатой
    uint16_t pedal_brake_min;
    uint16_t pedal_brake_max;
    uint8_t  pedal_gas_deadzone;    // 0–100, в единицах из 1023
    uint8_t  pedal_brake_deadzone;
    uint8_t  pedal_gas_curve;       // 0=linear, 1=quadratic, 2=s-curve
    uint8_t  pedal_brake_curve;
    uint8_t  pedal_ema_alpha;       // EMA коэффициент (0–255)
    
    // --- Профили режимов вождения (массив) ---
    drive_profile_t profiles[DRIVE_MODE_COUNT];
    
    // --- PID руля ---
    int16_t  eps_kp;                // ×100 (fixed-point, 2 десятичных знака)
    int16_t  eps_ki;
    int16_t  eps_kd;
    uint16_t eps_steering_center;   // Центр руля (калибровка)
    
    // --- Защита по току ---
    uint16_t current_limit_soft;    // Порог мягкого ограничения (мА)
    uint16_t current_limit_hard;    // Порог аварийного отключения (мА)
    uint16_t current_limit_time_ms; // Время до аварийного отключения
    
    // --- Watchdog RS485 ---
    uint16_t uart_timeout_ms;       // Timeout потери связи
    
    // --- Электронный дифференциал ---
    uint8_t  diff_enabled;
    uint8_t  diff_gain;             // ×100 (0..100 → 0.00..1.00)
    
    // --- Мотор ---
    uint8_t  motor_pwm_deadzone;    // Минимальный PWM для вращения
    
} settings_t;
```

**Профиль режима вождения:**

```c
typedef struct {
    uint16_t max_pwm;               // 0–1023
    uint16_t accel_rate;            // PWM/сек
    uint16_t decel_rate;            // PWM/сек (отпускание газа)
    uint16_t brake_rate_min;        // PWM/сек (лёгкое торможение)
    uint16_t brake_rate_max;        // PWM/сек (экстренное торможение)
    uint16_t pwm_frequency_hz;      // Целевая частота ШИМ
    uint8_t  pwm_resolution_bits;   // 9 или 10 бит
    uint8_t  direction;             // 0=forward, 1=reverse
    uint8_t  pedal_curve_override;  // 0xFF = использовать глобальную кривую
} drive_profile_t;
```

**API cfg_settings:**

```c
void     cfg_settings_init(void);           // Загрузка из EEPROM или defaults
void     cfg_settings_save(void);           // Запись в EEPROM с CRC
void     cfg_settings_reset_defaults(void); // Сброс к заводским

// Прямой доступ к структуре (read-only)
const settings_t* cfg_settings_get(void);

// Изменение отдельных параметров (write-through: RAM + EEPROM)
void     cfg_settings_set_u8(uint16_t offset, uint8_t value);
void     cfg_settings_set_u16(uint16_t offset, uint16_t value);
void     cfg_settings_set_i16(uint16_t offset, int16_t value);
void     cfg_settings_set_blob(uint16_t offset, const void* data, uint8_t size);

// Получить профиль конкретного режима
const drive_profile_t* cfg_settings_get_profile(drive_mode_id_t mode);
```

---

## 4. Layer 0: HAL — Hardware Abstraction Layer

### 4.1 Принцип

Каждый HAL-модуль — пара файлов:
- `hal_<subsys>.h` — **платформонезависимый** интерфейс (typedef, прототипы)
- `hal_atmega328p.c` — **конкретная реализация** для ATmega328P

При портировании на STM32/ESP32/RP2040 заменяется ТОЛЬКО файл реализации.

### 4.2 hal_gpio

```c
typedef enum { GPIO_INPUT, GPIO_INPUT_PULLUP, GPIO_OUTPUT } gpio_mode_t;
typedef enum { GPIO_LOW = 0, GPIO_HIGH = 1 } gpio_state_t;

void        hal_gpio_mode(uint8_t pin, gpio_mode_t mode);
void        hal_gpio_write(uint8_t pin, gpio_state_t state);
gpio_state_t hal_gpio_read(uint8_t pin);
```

### 4.3 hal_adc

```c
void     hal_adc_init(void);
uint16_t hal_adc_read(uint8_t channel);   // 0–1023 (10-bit)
```

### 4.4 hal_pwm

Ключевой HAL для этого проекта. Абстрагирует Timer1 и Timer0.

```c
typedef enum { 
    PWM_TIMER_MOTORS = 0,   // Timer1: D9 + D10
    PWM_TIMER_EPS    = 1    // Timer0: D6
} pwm_timer_id_t;

typedef enum {
    PWM_CHANNEL_A = 0,      // OC1A / OC0A
    PWM_CHANNEL_B = 1       // OC1B / OC0B
} pwm_channel_t;

typedef enum {
    PWM_MODE_PHASE_CORRECT,
    PWM_MODE_FAST
} pwm_mode_t;

typedef struct {
    pwm_mode_t  mode;
    uint16_t    top;            // ICR1 / OCR0A  (определяет разрешение и частоту)
    uint8_t     prescaler;      // 0=off, 1=1, 2=8, 3=64, 4=256, 5=1024
} pwm_config_t;

void     hal_pwm_init(pwm_timer_id_t timer, const pwm_config_t *config);
void     hal_pwm_set(pwm_timer_id_t timer, pwm_channel_t channel, uint16_t value);
uint16_t hal_pwm_get(pwm_timer_id_t timer, pwm_channel_t channel);
void     hal_pwm_set_both(pwm_timer_id_t timer, uint16_t a, uint16_t b);

// Реконфигурация частоты (ТОЛЬКО при PWM=0 на обоих каналах!)
void     hal_pwm_reconfigure(pwm_timer_id_t timer, const pwm_config_t *config);

// Утилита: вычислить TOP для заданной частоты
uint16_t hal_pwm_calc_top(uint32_t target_freq_hz, pwm_mode_t mode, uint8_t prescaler);
uint16_t hal_pwm_get_frequency(pwm_timer_id_t timer);
```

### 4.5 hal_uart

```c
void     hal_uart_init(uint32_t baud);
void     hal_uart_write(uint8_t data);
uint8_t  hal_uart_read(void);              // SERIAL_NO_DATA если пусто
uint8_t  hal_uart_available(void);         // Количество байт в буфере
void     hal_uart_flush_rx(void);
void     hal_uart_set_rs485_dir(uint8_t tx_mode); // Управление DE/RE
```

Внутри — обёртка над MICRO_UART с добавлением RS485 DE/RE.

### 4.6 hal_spi

```c
void    hal_spi_init(void);
void    hal_spi_transfer(uint8_t data);
void    hal_spi_cs_low(uint8_t cs_pin);
void    hal_spi_cs_high(uint8_t cs_pin);
```

### 4.7 hal_i2c

```c
void    hal_i2c_init(void);
uint8_t hal_i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t data);
uint8_t hal_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data);
uint8_t hal_i2c_write_buf(uint8_t addr, uint8_t reg, const uint8_t *buf, uint8_t len);
uint8_t hal_i2c_read_buf(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len);
```

### 4.8 hal_eeprom

```c
void    hal_eeprom_read(uint16_t addr, uint8_t *buf, uint16_t len);
void    hal_eeprom_write(uint16_t addr, const uint8_t *buf, uint16_t len);
uint8_t hal_eeprom_read_byte(uint16_t addr);
void    hal_eeprom_write_byte(uint16_t addr, uint8_t data);
```

### 4.9 hal_system

```c
void     hal_system_init(void);
uint32_t hal_system_millis(void);           // Uptime в мс
void     hal_system_delay_us(uint16_t us);  // Микросекундная задержка (ТОЛЬКО для HAL!)
void     hal_system_reset(void);            // Программный сброс
void     hal_system_wdt_enable(void);       // Watchdog включить (8 сек)
void     hal_system_wdt_reset(void);        // Сброс watchdog
void     hal_system_irq_disable(void);      // cli()
void     hal_system_irq_enable(void);       // sei()
uint8_t  hal_system_irq_save(void);         // Сохранить SREG, cli()
void     hal_system_irq_restore(uint8_t s); // Восстановить SREG
```

### 4.10 hal_encoder

```c
typedef void (*hal_encoder_callback_t)(uint8_t channel);

void     hal_encoder_init(void);
void     hal_encoder_attach(uint8_t channel, hal_encoder_callback_t cb);
// ISR внутри HAL инкрементирует счётчики; сервис читает и сбрасывает
uint32_t hal_encoder_get_count(uint8_t channel);
void     hal_encoder_reset_count(uint8_t channel);
```

---

## 5. Layer 1: Drivers

### 5.1 drv_zsx11h — драйвер мотор-колеса

Абстрагирует управление одним мотором через ZS-X11H. Объединяет PWM (через HAL) 
и дискретные сигналы DIR/BRAKE/STOP (через drv_hc595).

```c
typedef enum {
    MOTOR_STATE_FREE,       // STOP=0: свободное вращение
    MOTOR_STATE_DRIVE,      // STOP=1, BRAKE=0: активное вращение
    MOTOR_STATE_BRAKE       // BRAKE=1: блокировка
} motor_state_t;

typedef enum {
    MOTOR_DIR_FORWARD = 0,
    MOTOR_DIR_REVERSE = 1
} motor_dir_t;

void          drv_motor_init(void);
void          drv_motor_set_pwm(motor_id_t id, uint16_t pwm);
void          drv_motor_set_state(motor_id_t id, motor_state_t state);
void          drv_motor_set_direction(motor_id_t id, motor_dir_t dir);
motor_state_t drv_motor_get_state(motor_id_t id);
motor_dir_t   drv_motor_get_direction(motor_id_t id);
uint16_t      drv_motor_get_pwm(motor_id_t id);
```

### 5.2 drv_hc595 — сдвиговый регистр

```c
void    drv_hc595_init(void);
void    drv_hc595_write(uint8_t data);    // Защёлкивает все 8 бит
uint8_t drv_hc595_get(void);              // Текущее состояние (shadow reg)
void    drv_hc595_set_bit(uint8_t bit, uint8_t value);
```

### 5.3 drv_pca9535 — расширитель портов I2C

```c
void    drv_pca9535_init(void);
void    drv_pca9535_write_port0(uint8_t data);  // Выходы (SW0_1..SW0_8)
uint8_t drv_pca9535_read_port1(void);            // Входы (SW1_1..SW1_8)
void    drv_pca9535_set_bit(uint8_t port, uint8_t bit, uint8_t value);
```

### 5.4 drv_hx711 — тензодатчик

```c
void    drv_hx711_init(void);
int32_t drv_hx711_read(void);       // Сырое 24-битное значение
uint8_t drv_hx711_is_ready(void);   // Данные доступны?
void    drv_hx711_set_gain(uint8_t gain);  // 128 / 64 / 32
```

---

## 6. Layer 2: Services — бизнес-логика

### 6.1 svc_pedals — обработка педалей

```c
void     svc_pedals_init(void);
void     svc_pedals_update(void);          // Вызывать 100 Гц
uint16_t svc_pedals_get_gas(void);         // 0–1023 нормализованный
uint16_t svc_pedals_get_brake(void);
uint16_t svc_pedals_get_target_pwm(void);  // С учётом режима и тормоза
uint8_t  svc_pedals_is_brake_active(void);
```

**Кривые отклика педали (§9.7):** определяются параметром `pedal_gas_curve`:

```
LINEAR:    output = input
QUADRATIC: output = input² / 1023         (спокойный старт, резкий финиш)
S_CURVE:   output = 3·input²/1023² - 2·input³/1023³  (плавный старт и финиш)
```

Кривая применяется ПОСЛЕ калибровки и deadzone, ДО масштабирования к max_pwm.

### 6.2 svc_ramp — генератор рампы

Без изменений по логике; параметры берёт из `cfg_settings → drive_profile_t`.

```c
void     svc_ramp_init(void);
uint16_t svc_ramp_update(uint16_t target, uint16_t brake_val); // 100 Гц
uint16_t svc_ramp_get_current(void);
void     svc_ramp_reset(void);
uint8_t  svc_ramp_is_stopped(void);
```

### 6.3 svc_motor — координация двух моторов

Высокоуровневый модуль, который:
1. Получает PWM от svc_ramp.
2. Применяет электронный дифференциал (если включён).
3. Контролирует безопасное переключение DIR/BRAKE/STOP (§6.1 — FSM).
4. Подаёт PWM на drv_motor.

```c
typedef enum {
    MOTOR_CMD_STOP,             // Плавная остановка → FREE
    MOTOR_CMD_DRIVE_FORWARD,    // Движение вперёд
    MOTOR_CMD_DRIVE_REVERSE,    // Движение назад
    MOTOR_CMD_BRAKE,            // Активное торможение (блокировка)
    MOTOR_CMD_EMERGENCY         // Аварийная остановка
} motor_command_t;

void     svc_motor_init(void);
void     svc_motor_update(void);           // 100 Гц
void     svc_motor_set_command(motor_command_t cmd);
void     svc_motor_set_pwm(uint16_t pwm);  // Целевой PWM после рампы
uint16_t svc_motor_get_actual_pwm(motor_id_t id);  // Фактический (после дифф.)
```

**Конечный автомат безопасного переключения (§6.1):**

```
                ┌─────────┐
        ┌──────→│  IDLE   │←──────────────┐
        │       │ (PWM=0) │               │
        │       └────┬────┘               │
        │            │ cmd=DRIVE          │
        │            ▼                    │
        │       ┌─────────┐              │
        │       │ SET_DIR  │  (DIR=cmd)   │
        │       │ wait 50ms│              │
        │       └────┬────┘              │
        │            │                    │
        │            ▼                    │
        │       ┌─────────┐              │
        │       │  ENABLE  │  (STOP=1)    │
        │       │ wait 50ms│              │
        │       └────┬────┘              │
        │            │                    │
        │            ▼                    │
        │       ┌─────────┐              │
        │       │ RUNNING  │◄─── PWM>0    │
        │       │ (active) │              │
        │       └────┬────┘              │
        │            │ cmd=STOP/BRAKE     │
        │            │ or overcurrent     │
        │            ▼                    │
        │       ┌─────────┐              │
        │       │ RAMP_DOWN│  PWM→0       │
        │       │ (decel)  │              │
        │       └────┬────┘              │
        │            │ PWM=0 && speed=0   │
        │            ▼                    │
        │       ┌─────────┐              │
        │       │ SETTLING │  wait 100ms  │
        └───────│          │──────────────┘
                └─────────┘
```

### 6.4 svc_drive_mode — конечный автомат режимов

```c
typedef enum {
    DRIVE_MODE_LOCKED = 0,      // Заблокировано (пароль)
    DRIVE_MODE_NEUTRAL,         // Нейтраль (STOP активен)
    DRIVE_MODE_HANDBRAKE,       // Ручник (BRAKE активен)
    DRIVE_MODE_ECO,             // Forward 1
    DRIVE_MODE_NORMAL,          // Forward 2
    DRIVE_MODE_SPORT,           // Forward 3
    DRIVE_MODE_REVERSE,         // Задний ход
    DRIVE_MODE_PARENT,          // Удалённое управление
    DRIVE_MODE_FAILSAFE,        // Потеря связи
    DRIVE_MODE_COUNT
} drive_mode_id_t;

void             svc_drive_mode_init(void);
void             svc_drive_mode_update(void);   // Читает входы PCA9535
void             svc_drive_mode_request(drive_mode_id_t mode); // Запрос смены
drive_mode_id_t  svc_drive_mode_get(void);
uint8_t          svc_drive_mode_is_driving(void); // В режиме движения?

// Текущий профиль (указатель в cfg_settings)
const drive_profile_t* svc_drive_mode_get_profile(void);
```

**Правила переходов (§6.4):**

- `forward ↔ reverse`: ТОЛЬКО через полную остановку (speed=0)
- `handbrake → forward`: ТОЛЬКО при отпущенном газе
- Любой → `locked`: плавная остановка, затем переход
- `failsafe`: автоматически при таймауте RS485

### 6.5 svc_speed — скорость колёс

```c
void     svc_speed_init(void);
void     svc_speed_update(void);            // 10 Гц (каждые 100 мс)
uint16_t svc_speed_get_rpm(motor_id_t id);  // Обороты в минуту
uint16_t svc_speed_get_kmh_x10(void);       // Скорость ×10 (для отображения XX.X)
uint32_t svc_speed_get_odometer_m(void);    // Пробег в метрах
uint8_t  svc_speed_is_stopped(void);        // Оба колеса стоят?
```

### 6.6 svc_safety — безопасность

```c
typedef uint16_t fault_flags_t;  // Битовое поле

#define FAULT_OVERCURRENT_R     (1 << 0)
#define FAULT_OVERCURRENT_L     (1 << 1)
#define FAULT_OVERCURRENT_EPS   (1 << 2)
#define FAULT_UART_TIMEOUT      (1 << 3)
#define FAULT_STEER_STALL       (1 << 4)
#define FAULT_BMS_LOST          (1 << 5)
#define FAULT_BATTERY_LOW       (1 << 6)
#define FAULT_BATTERY_CRITICAL  (1 << 7)
#define FAULT_MOTOR_STALL_R     (1 << 8)
#define FAULT_MOTOR_STALL_L     (1 << 9)

void           svc_safety_init(void);
void           svc_safety_update(void);     // 100 Гц: проверка токов, watchdog
fault_flags_t  svc_safety_get_faults(void);
void           svc_safety_clear_fault(fault_flags_t mask);
uint8_t        svc_safety_is_critical(void); // Требуется аварийная остановка?
uint16_t       svc_safety_get_current_ma(motor_id_t id); // Ток в мА
```

### 6.7 svc_eps — электроусилитель руля

```c
void     svc_eps_init(void);
void     svc_eps_update(void);             // 100 Гц (или выше)
void     svc_eps_enable(uint8_t enable);
int16_t  svc_eps_get_position(void);       // -512..+512 от центра
int16_t  svc_eps_get_error(void);
uint16_t svc_eps_get_output_pwm(void);
```

### 6.8 svc_telemetry — формирование телеметрии

```c
typedef struct {
    uint16_t speed_rpm_r;
    uint16_t speed_rpm_l;
    uint16_t current_ma_r;
    uint16_t current_ma_l;
    int16_t  steering_pos;
    uint16_t pedal_gas;
    uint16_t pedal_brake;
    uint8_t  drive_mode;
    fault_flags_t faults;
    uint32_t uptime_ms;
    uint16_t motor_pwm_r;
    uint16_t motor_pwm_l;
} telemetry_t;

void svc_telemetry_init(void);
void svc_telemetry_update(void);             // 10 Гц
const telemetry_t* svc_telemetry_get(void);
```

---

## 7. Layer 3: Application

### 7.1 app_main — главный цикл и планировщик

Кооперативный планировщик с фиксированными интервалами:

```c
typedef struct {
    void (*func)(void);         // Указатель на функцию задачи
    uint16_t interval_ms;       // Интервал вызова
    uint32_t last_run_ms;       // Время последнего вызова
    uint8_t  enabled;           // Включена/выключена
} task_entry_t;
```

**Таблица задач:**

| Задача | Интервал | Приоритет | Описание |
|--------|----------|-----------|----------|
| `svc_pedals_update` | 10 мс | Высокий | Чтение педалей |
| `svc_safety_update` | 10 мс | Высокий | Проверка токов |
| `svc_ramp_update` (вызов из motor) | 10 мс | Высокий | Рампа |
| `svc_motor_update` | 10 мс | Высокий | Управление моторами |
| `svc_eps_update` | 10 мс | Высокий | PID руля |
| `svc_drive_mode_update` | 50 мс | Средний | Чтение переключателей |
| `svc_speed_update` | 100 мс | Средний | Расчёт скорости |
| `svc_telemetry_update` | 100 мс | Низкий | Отправка телеметрии |
| `app_protocol_update` | 10 мс | Средний | Парсинг входящих пакетов |
| `hal_system_wdt_reset` | 10 мс | Критический | Сброс watchdog |

**Гарантия тактирования:** если задача выполнялась дольше интервала, 
она запускается немедленно на следующей итерации (без накопления пропущенных вызовов).

### 7.2 app_protocol — парсер RS485

Бинарный протокол с пакетной структурой:

```
┌──────┬──────┬──────┬──────────────────┬──────────┐
│ SYNC │ LEN  │ CMD  │    PAYLOAD       │  CRC16   │
│ 0xAA │ 1B   │ 1B   │   0..N bytes     │  2B      │
└──────┴──────┴──────┴──────────────────┴──────────┘
```

Парсер — конечный автомат (byte-by-byte, неблокирующий).

---

## 8. Утилиты (util_*)

### 8.1 util_math

```c
uint16_t util_map(uint16_t x, uint16_t in_min, uint16_t in_max,
                  uint16_t out_min, uint16_t out_max);
uint16_t util_clamp(uint16_t value, uint16_t min_val, uint16_t max_val);
int16_t  util_clamp_i16(int16_t value, int16_t min_val, int16_t max_val);

// EMA фильтр (fixed-point)
int32_t  util_ema_update(int32_t filtered_fp, uint16_t raw, uint8_t alpha);

// Кривые отклика
uint16_t util_curve_apply(uint16_t input, uint8_t curve_type);

// PID регулятор (переиспользуемый)
typedef struct {
    int16_t  kp, ki, kd;        // ×100 (fixed-point)
    int32_t  integral;
    int16_t  prev_error;
    int16_t  output_min;
    int16_t  output_max;
    int16_t  integral_max;      // Anti-windup
} pid_state_t;

void    util_pid_init(pid_state_t *pid, int16_t kp, int16_t ki, int16_t kd,
                      int16_t out_min, int16_t out_max);
int16_t util_pid_update(pid_state_t *pid, int16_t error);
void    util_pid_reset(pid_state_t *pid);
```

### 8.2 util_crc

```c
uint16_t util_crc16(const uint8_t *data, uint16_t len);
uint16_t util_crc16_update(uint16_t crc, uint8_t data);  // Побайтовый
```

---

## 9. Управление частотой ШИМ (детали реализации)

Частота ШИМ привязана к профилю режима вождения через `drive_profile_t.pwm_frequency_hz` 
и `pwm_resolution_bits`.

### 9.1 Доступные комбинации (ATmega328P, Timer1, 16 МГц)

| Режим PWM | TOP (ICR1) | Разрешение | Частота | Комментарий |
|-----------|-----------|------------|---------|-------------|
| Phase-Correct | 1023 | 10 бит | 7 812 Гц | Базовый |
| Phase-Correct | 511 | 9 бит | 15 625 Гц | Высокоскоростной |
| Fast PWM | 1023 | 10 бит | 15 625 Гц | Альтернатива |
| Fast PWM | 511 | 9 бит | 31 250 Гц | Ультразвук |
| Phase-Correct | 799 | ~9.6 бит | 10 000 Гц | Компромисс |
| Phase-Correct | 399 | ~8.6 бит | 20 000 Гц | Тихий |

### 9.2 Алгоритм смены частоты

```
1. svc_motor_set_pwm(0)            // Цель = 0
2. svc_ramp — плавное замедление
3. Дождаться svc_speed_is_stopped() // Колёса остановились
4. hal_pwm_reconfigure(TIMER_MOTORS, &new_config)
5. Обновить svc_ramp max_pwm = new_top   // Новое разрешение
6. Разрешить движение
```

Смена частоты запрещена на ходу — только через полную остановку.

### 9.3 Масштабирование PWM при разном разрешении

Все внутренние вычисления ведутся в нормализованных единицах 0–1023 (10 бит).
При разрешении 9 бит (TOP=511) значение масштабируется при записи в HAL:

```c
// Внутри drv_motor_set_pwm():
actual_pwm = (pwm_normalized * current_top) / 1023;
hal_pwm_set(TIMER_MOTORS, channel, actual_pwm);
```

Это обеспечивает единый API независимо от текущей частоты/разрешения.

---

## 10. Поток данных: от педали до мотора

```
                     cfg_settings
                         │
   ┌─────────────────────┼──────────────────────┐
   │                     │                      │
   ▼                     ▼                      ▼
┌────────┐        ┌─────────────┐        ┌──────────┐
│hal_adc │        │drive_profile│        │svc_safety│
│read A0 │        │(rates, max) │        │(current) │
└───┬────┘        └──────┬──────┘        └────┬─────┘
    │                    │                    │
    ▼                    │                    │ overcurrent?
┌──────────┐             │                    │ → limit_pwm
│svc_pedals│             │                    │
│ EMA filt │             │                    │
│ calibrate│             │                    │
│ deadzone │             │                    │
│ curve    │             │                    │
└──┬───┬───┘             │                    │
   │   │                 │                    │
   │   │ target_pwm      │                    │
   │   │ (0 if brake)    │ rates              │
   │   │                 │                    │
   ▼   ▼                 ▼                    ▼
┌─────────────────────────────────────────────────┐
│                   svc_ramp                       │
│  accumulator arithmetic → smooth PWM transition  │
└──────────────────────┬──────────────────────────┘
                       │ output_pwm
                       ▼
              ┌─────────────────┐
              │    svc_motor    │
              │ direction FSM   │
              │ e-differential  │
              │ safety clamp    │
              └───┬─────────┬───┘
                  │         │
                  ▼         ▼
           ┌──────────┬──────────┐
           │drv_motor │drv_motor │
           │  LEFT    │  RIGHT   │
           └──┬───────┴────┬─────┘
              │            │
              ▼            ▼
        ┌──────────┐ ┌──────────┐
        │ hal_pwm  │ │ hal_pwm  │
        │ OC1A/D9  │ │ OC1B/D10 │
        └──────────┘ └──────────┘
```

---

## 11. План реализации по MVP (обновлённый)

Каждый MVP реализует модули, которые вписываются в архитектуру.
Нет throwaway-кода — каждый файл остаётся в проекте.

| MVP | Модули | Что добавляется |
|-----|--------|----------------|
| **1** | HAL (gpio, adc, pwm, uart, eeprom, system), cfg_board, cfg_settings, util_math, util_crc, svc_pedals, svc_ramp, svc_motor (basic), app_main (scheduler), debug | Фундамент + педали + 2 мотора |
| **2** | hal_encoder, drv_hx711, svc_eps, svc_speed, util_pid | EPS с PID, энкодеры колёс |
| **3** | hal_spi, hal_i2c, drv_hc595, drv_pca9535, svc_drive_mode, svc_safety | HC595+PCA9535, режимы, защита |
| **4** | hal_uart (RS485 mode), app_protocol, svc_telemetry | RS485 протокол, телеметрия |
| **5–9** | Расширения на существующей архитектуре | Дифференциал, ABS, парктроники... |

---

## 12. Ресурсы ATmega328P — бюджет

| Ресурс | Доступно | Оценка использования | Запас |
|--------|----------|---------------------|-------|
| Flash | 32 КБ | ~18–22 КБ | ~30% |
| RAM | 2 КБ | ~1.2–1.5 КБ | ~25% |
| EEPROM | 1 КБ | ~200–300 байт | ~70% |
| Таймеры | Timer0, Timer1, Timer2 | Timer1=моторы, Timer0=EPS, Timer2=millis | Все заняты |
| Прерывания | INT0, INT1, UART, Timer | Все используются | — |
| АЦП каналов | A0–A7 | A0–A3, A6 используются | A7 резерв |

**Критическое ограничение:** Timer2 на ATmega328P используется Arduino core для millis().
В HAL-реализации нужно учесть, что millis() зависит от Timer2 и не перенастраивать его.
Альтернатива — свой системный тик на Timer2, но это усложнит отладку через Serial Monitor.

---

## 13. Тестирование

### 13.1 Уровни тестирования

| Уровень | Среда | Что тестируется |
|---------|-------|----------------|
| Unit (десктоп) | g++ на PC | util_*, svc_pedals, svc_ramp, svc_motor FSM, svc_drive_mode FSM, cfg_settings, util_pid, app_protocol parser |
| Integration (десктоп) | g++ на PC | Цепочка pedals→ramp→motor, drive_mode→motor transitions |
| Hardware | Arduino + осциллоскоп | HAL, драйверы, реальные моторы |

### 13.2 Мок-объекты для десктопных тестов

HAL-функции заменяются моками:
```c
// test_mock_hal.h
static uint16_t mock_adc_values[8] = {0};
uint16_t hal_adc_read(uint8_t ch) { return mock_adc_values[ch]; }

static uint16_t mock_pwm_values[4] = {0};
void hal_pwm_set(..., uint16_t val) { mock_pwm_values[...] = val; }
```

Это позволяет тестировать всю бизнес-логику без железа.
