# EV Controller Firmware Project

**Назначение этого документа:** контекст для продолжения работы над прошивкой
контроллера в новом чате. Если веб не нужен — этого + ARCHITECTURE.md достаточно.

**Статус на момент создания:** MVP-1 завершён + Layered Pedals (v2) + протокол.
Готов к MVP-2 (EPS + энкодеры подключение).

---

## 1. Что это такое

Прошивка для Arduino Nano (ATmega328P) — контроллер детского электромобиля.
Управляет двумя моторами колёс через драйверы ZS-X11H, читает педали газа и
тормоза, общается с ESP32 (будущее) через RS485 по бинарному протоколу.

Проект частично описан в `EV_Controller_Specification.md` — это исходное ТЗ.
Архитектура прошивки задокументирована в `ARCHITECTURE.md`.

## 2. Архитектура: 4-слойная

```
┌─────────────────────────────────────────────────────────┐
│  Layer 3: APPLICATION                                   │
│  app_main (scheduler) | app_protocol | app_debug        │
├─────────────────────────────────────────────────────────┤
│  Layer 2: SERVICES (платформонезависимая бизнес-логика) │
│  svc_pedals | svc_ramp | svc_motor | svc_speed          │
├─────────────────────────────────────────────────────────┤
│  Layer 1: DRIVERS (через HAL)                          │
│  (пока минимально, MVP-3+: drv_hc595, drv_pca9535, ...) │
├─────────────────────────────────────────────────────────┤
│  Layer 0: HAL (Hardware Abstraction Layer)              │
│  hal_gpio | hal_adc | hal_pwm | hal_uart | hal_eeprom   │
│  hal_encoder | hal_system | hal_spi | hal_i2c           │
│                                                          │
│  Реализация для ATmega328P: hal_atmega328p.cpp          │
└─────────────────────────────────────────────────────────┘
```

**Главное правило:** платформонезависимый код ВЫШЕ HAL. При портировании на
STM32/ESP32 заменяется только `hal_atmega328p.cpp`.

## 3. Структура файлов firmware/

```
firmware/
├── EV_Controller.ino              ← Главный скетч (планировщик задач)
├── cfg_board.h                    ← Карта пинов, таймеры, ID
├── cfg_settings.h/.cpp            ← Настройки EEPROM с CRC16, defaults
├── hal_*.h                        ← Интерфейсы HAL (платформонезависимые)
├── hal_atmega328p.cpp             ← ЕДИНСТВЕННЫЙ платформозависимый файл
├── util_math.h/.cpp               ← map, clamp, EMA, PID, кривые, аккумулятор
├── util_crc.h/.cpp                ← CRC16-CCITT
├── svc_pedals.h/.cpp              ← Педали с layered model (физ+UART)
├── svc_ramp.h/.cpp                ← Рампа разгона/торможения
├── svc_motor.h/.cpp               ← Координация моторов, масштабирование PWM
├── svc_speed.h/.cpp               ← Скорость колёс из энкодеров (MVP-2+)
├── app_main.h/.cpp                ← Кооперативный планировщик
├── app_protocol.h/.cpp            ← Бинарный протокол UART/RS485
├── app_debug.h/.cpp               ← Текстовая телеметрия
├── MICRO_UART.h/.cpp              ← UART драйвер из Grbl
├── PRINT.h/.cpp                   ← Форматирование без sprintf
└── Inc.h                          ← Мастер-include
```

## 4. Что реализовано (MVP-1 + v2 Layered Pedals)

### 4.1 HAL для ATmega328P
- GPIO (через регистры или Arduino API)
- ADC 10-bit
- PWM на Timer1 (10/9-bit, Phase-Correct/Fast, конфигурируемая частота)
- UART (обёртка над MICRO_UART + DE/RE для RS485)
- EEPROM
- System (millis, watchdog, IRQ save/restore)
- Encoder ISR на INT0 (D2) / INT1 (D3) — для энкодеров колёс

### 4.2 Сервисы
- **svc_pedals**: ADC + EMA + калибровка + deadzone + кривая отклика +
  **layered physical+uart с настраиваемым комбинатором** + watchdog UART-педалей
- **svc_ramp**: плавный разгон/торможение с целочисленным аккумулятором
- **svc_motor**: масштабирование 0-1023 → текущий TOP таймера, два канала PWM
- **svc_speed**: RPM + км/ч от энкодеров, одометр (заработает с подключёнными датчиками)

### 4.3 Приложение
- **app_main**: кооперативный планировщик задач без delay()
- **app_protocol**: бинарный протокол (SYNC+LEN+CMD+PAYLOAD+CRC16),
  команды управления, расширенная телеметрия (36 байт)
- **app_debug**: текстовая телеметрия для Serial Monitor

### 4.4 Настройки EEPROM (cfg_settings)
178 байт с CRC16. Содержит:
- Калибровку педалей (min/max RAW, deadzone, EMA, кривые отклика)
- 9 профилей режимов вождения (max_pwm, accel/decel, brake rates, частота ШИМ, разрешение)
- PID коэффициенты EPS (для MVP-2)
- Лимиты тока (для MVP-3)
- Watchdog таймауты (RS485 + UART pedals)
- Электронный дифференциал (для MVP-8)
- **Комбинаторы педалей** (новое в v2): gas_combinator, brake_combinator

## 5. Ключевое архитектурное решение: Layered Pedals (v2)

См. подробное описание в `WEB_PROJECT.md` §4.1. Суть:

- На контроллере НЕТ бинарного «source = PEDALS|UART»
- У каждой педали два значения: `physical` (от АЦП) и `uart` (от команд)
- Эффективное значение = `combinator(physical, uart)`, по умолчанию `MAX`
- Комбинатор настраивается в EEPROM (5 вариантов: MAX, ADDITIVE_CLAMP, UART_PRIORITY, PHYSICAL_ONLY, UART_ONLY)
- Watchdog внутри svc_pedals: если UART-команд нет 200мс → `uart` сбрасывается в 0
- Поэтому даже потеря связи безопасна — физическая педаль остаётся live

В `EV_Controller.ino` это видно в `task_control()`:
```cpp
app_protocol_update();   // Принимает команды, кладёт в svc_pedals.uart_*
svc_pedals_update();     // Читает физические педали, применяет watchdog
uint16_t target = svc_pedals_get_target_pwm(...);  // ← после комбинатора
uint16_t brake  = svc_pedals_get_brake();
uint16_t pwm    = svc_ramp_update(target, brake, profile);
svc_motor_set_pwm(pwm);
```

## 6. Что НЕ сделано / план MVP-2

### MVP-2: EPS + энкодеры + ABS-lite (следующий)
- [ ] Подключить датчики энкодеров колёс физически → svc_speed заработает
- [ ] **Определить экспериментально**: `ENCODER_PULSES_PER_REV` и `WHEEL_DIAMETER_MM`
      в `cfg_board.h`. Сейчас стоят 12 и 200мм — нужны реальные значения.
- [ ] HX711 драйвер: тензодатчик усилия на руле (D5/D12)
- [ ] svc_eps: PID-регулятор электроусилителя руля
  - `util_pid` уже готов в util_math
  - Использовать BTS7960 через D6 (Timer0) + Q0/Q7 на HC595 (последнее — в MVP-3)
- [ ] Решить конфликт Timer0 vs millis() — либо перенести системный тик на Timer2,
      либо использовать другой подход для EPS PWM

### MVP-3: HC595 + PCA9535 + режимы + защита
- [ ] drv_hc595: сдвиговый регистр через SPI (D8/D11/D13)
- [ ] drv_pca9535: расширитель портов I2C (PIN_PCA9535_INT = D7, addr 0x20)
- [ ] svc_drive_mode: конечный автомат 9 режимов с правилами переходов
  (forward↔reverse только через стоп; handbrake→forward только при отпущенном газе)
- [ ] svc_safety: контроль токов (ACS712 на A2/A6), watchdog, faults
- [ ] svc_motor: FSM безопасного переключения DIR/BRAKE/STOP (см. ARCHITECTURE.md §6.3)

### MVP-4+: RS485 + ESP32
- [ ] Полноценный RS485 (DE/RE через D4) — пока работает на 9600 бод USB-UART
- [ ] Watchdog потери связи: если 500мс без команд → DRIVE_MODE_FAILSAFE

## 7. Параметры для уточнения экспериментально

| Параметр | Где | Значение | Как определить |
|----------|-----|----------|----------------|
| `ENCODER_PULSES_PER_REV` | cfg_board.h | 12 | Подключить датчик, прокрутить колесо вручную ровно на 1 оборот, посчитать импульсы. Можно через `hal_encoder_get_count` и серийный вывод |
| `WHEEL_DIAMETER_MM` | cfg_board.h | 200 | Замерить рулеткой |
| `PEDAL_GAS_RAW_MIN/MAX` | settings (defaults в cfg_settings.cpp) | 10/1000 | Замерить АЦП при отпущенной и нажатой педали. Веб-Settings (MVP-3) позволит подкрутить без перепрошивки |
| `PEDAL_BRAKE_RAW_MIN/MAX` | то же | то же | то же |
| `MOTOR_PWM_DEADZONE` | settings.motor_deadzone | 30 | Минимальный PWM при котором мотор начинает вращаться. Зависит от моторов и батареи |
| EPS PID коэффициенты | settings.eps_kp/ki/kd | 200/10/50 (×100) | Подобрать на реальном руле, посмотреть на отклик через телеметрию |
| Профили режимов (accel/decel) | settings.profiles[*] | См. defaults | На реальной машине с ребёнком — комфортно/безопасно |

## 8. Тестирование

### Десктопные unit-тесты (требуют g++)
```bash
cd tests
make run
```
Сейчас 71 тест: util_math (33), cfg_settings (28), svc_ramp (10).

### Тесты, которые нужно добавить
- [ ] `test_svc_pedals.c` — тестировать combinator во всех режимах,
      watchdog UART, корректное поведение при разных кривых отклика
- [ ] `test_svc_speed.c` — расчёт RPM/км/ч с известными импульсами на оборот
- [ ] `test_app_protocol.c` — парсер пакетов (валидный/битый CRC/частичный пакет/мусор)

### Тестирование на железе
- Серийный монитор @ 9600 бод покажет debug-телеметрию каждые 200мс
- Бинарная телеметрия в той же UART идёт каждые 100мс — нужен Python-сервер
  чтобы её увидеть (см. `tools/gui/server.py`)
- Для отладки протокола без веба: использовать старую `tools/ev_control.py`
  (текстовая CLI) — но она устарела, обновить при необходимости

## 9. Связь с веб-проектом

См. `WEB_PROJECT.md` §10 — список синхронизированных файлов.

**Если меняешь firmware:**
- Добавил/изменил поле в `telemetry_packet_t` (app_protocol.h) →
  обнови `parse_telemetry()` в `tools/gui/server.py` + поля в `store.js`
- Добавил новую CMD_* константу → добавь её в `server.py` `COMMAND_MAP`
  и метод-обёртку в `protocol.js`
- Изменил `settings_t` → когда будет Settings UI, поля надо описать в схеме

## 10. История версий

- **v1.0** (MVP-1) — Базовая прошивка: педали + рампа + 2 мотора + бинарный протокол
- **v2.0** (текущая) — Layered pedal model: physical + uart с комбинатором,
  расширенная телеметрия 36 байт, svc_speed scaffolding, HAL encoder ISR
- **v3.0** (план, MVP-2) — EPS + энкодеры + ABS-lite
- **v4.0** (план, MVP-3) — HC595 + PCA9535 + режимы + safety
- **v5.0** (план, MVP-4) — RS485 + полная интеграция с ESP32

---

## Контрольный список «продолжить работу с firmware»

1. Прочти `ARCHITECTURE.md` (детали слоёв), `EV_Controller_Specification.md` (ТЗ)
2. Открой `firmware/EV_Controller.ino` — это точка входа
3. Перед изменением определи слой: HAL / Service / Application
4. Платформозависимый код — ТОЛЬКО в `hal_atmega328p.cpp`. В свежем сервисе
   используй HAL-функции, не avr/io.h
5. После любого изменения проверь десктопные тесты: `cd tests && make run`
6. Если меняешь протокол или телеметрию — обнови WEB_PROJECT.md и
   синхронизируй с `tools/gui/server.py`
