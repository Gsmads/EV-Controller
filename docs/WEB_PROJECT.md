# EV Control Room — Web Project

**Назначение этого документа:** дать любому новому чату ИЛИ человеку, который
открыл проект впервые, полный контекст для продолжения работы над веб-частью
без необходимости погружаться в код firmware.

**Статус на момент создания:** базовая инфраструктура готова — модульный
фронтенд + WebSocket-сервер + протокольная интеграция с контроллером.
Дашборд работает, можно расширять.

---

## 1. Что это такое

Веб-интерфейс для контроллера детского электромобиля. Подключается к Arduino
Nano через USB-UART, отображает телеметрию в реальном времени, позволяет
управлять машиной (виртуальные педали газа/тормоза, режимы, экстренная
остановка) и настраивать параметры в EEPROM удалённо.

В перспективе тот же UI будет хоститься на ESP32 (WiFi AP) внутри машины —
для управления со смартфона родителя в полевых условиях.

## 2. Архитектура

```
  ┌────────────┐   USB-UART     ┌──────────┐   WebSocket    ┌─────────┐
  │  Arduino   │ ◀═══════════▶  │  server  │ ◀════════════▶ │ browser │
  │  Nano      │  9600 baud     │  (.py)   │  port 8765     │  (JS)   │
  │            │  binary proto  │          │  JSON          │         │
  └────────────┘                │ + HTTP   │                │         │
                                │  :8000   │ ────statics─▶  │         │
                                └──────────┘                └─────────┘
```

**Python-сервер** делает три вещи одновременно:
1. Открывает Serial-порт, парсит бинарные пакеты от Arduino (CRC16-CCITT)
2. Конвертирует телеметрию в JSON, рассылает всем WebSocket-клиентам
3. Принимает JSON-команды от клиентов, конвертирует в бинарные пакеты,
   шлёт Arduino
4. Отдаёт статические файлы (HTML/CSS/JS) на `:8000` — для удобства

**Браузерный код** организован модульно. Каждый виджет — самостоятельный
ES-модуль, подписывается на нужные срезы централизованного состояния (Store),
не знает про другие виджеты. Добавление функционала = новый файл в `widgets/`,
не трогая остальные.

## 3. Структура файлов

```
tools/gui/
├── server.py                      ← WebSocket+HTTP сервер
└── static/                        ← Раздаётся как HTTP
    ├── index.html                 ← Минимальная оболочка с data-region атрибутами
    ├── css/style.css              ← Все стили в одном файле
    └── js/
        ├── app.js                 ← Точка входа: создаёт виджеты, подписывает на транспорт
        ├── transport.js           ← Абстракция (WebSocket → потом ESP32)
        ├── protocol.js            ← Команды: setGasVirtual, emergencyStop, ...
        ├── store.js               ← Реактивное состояние с subscribe
        └── widgets/
            ├── header.js          ← Шапка: порт, uptime, частота ШИМ, faults
            ├── battery.js         ← Вертикальный бар заряда (MVP-3+)
            ├── gauge.js           ← Циферблат: Speed внешняя, PWM внутренняя
            ├── charts.js          ← 3 графика во времени (Chart.js)
            ├── pedals.js          ← Слайдеры с плавным decay при отпускании
            ├── modes.js           ← Кнопки выбора режима
            ├── estop.js           ← Кнопки RELEASE и E-STOP
            └── panels.js          ← Левая колонка: sources, inputs, lights, stats
```

## 4. Ключевые архитектурные решения

### 4.1 Модель педалей: physical + uart с комбинатором

Это самое важное архитектурное изменение. На контроллере нет бинарного
«источник = педаль ИЛИ uart». Вместо этого:

```
physical_gas (ADC)  ─┐
                     ├─> combinator() ─> effective_gas ──┐
uart_gas (cmd)      ─┘                                    │
                                                          ├─> ramp → motor
physical_brake (ADC)─┐                                    │
                     ├─> combinator() ─> effective_brake ─┘
uart_brake (cmd)    ─┘
```

Комбинаторы настраиваются в EEPROM раздельно для газа и тормоза. По умолчанию
`PEDAL_COMBINE_MAX` — родитель может «доложить» поверх ребёнка. Сценарии:

- Ребёнок жмёт газ 30%, родитель добавляет 60% через UART → машина едет на 60%
- Родитель отпускает → машина возвращается к 30% (педаль ребёнка осталась)
- Ребёнок едет, родитель прижимает тормоз → машина тормозит
- Родитель отпускает тормоз → ребёнок продолжает ехать со своим газом

Другие комбинаторы для гибкости (можно менять без перепрошивки):
- `ADDITIVE_CLAMP`: clamp(physical + uart, 0, 1023)
- `UART_PRIORITY`: старая бинарная модель
- `PHYSICAL_ONLY` / `UART_ONLY`: для отладки

Watchdog внутри `svc_pedals`: если UART-команда не приходит 200мс (настраивается),
`uart_gas/brake` сбрасываются в 0. Безопасность при потере связи.

### 4.2 Слайдеры с плавным decay (UX)

При отпускании ползунка в UI:
1. JavaScript запускает анимацию `requestAnimationFrame` — 300мс ease-out к 0
2. Каждый кадр отправляет `SET_GAS_VIRTUAL(текущее)` с троттлингом 50мс
3. В конце шлёт `SET_GAS_VIRTUAL(0)`

Это даёт ощущение настоящей педали — она «возвращается» плавно. Никаких
рывков мотора, потому что значение плавно опускается через ramp.

Можно изменить длительность константой `DECAY_MS` в `widgets/pedals.js`.

### 4.3 Транспорт абстрагирован

`transport.js` экспортирует класс `Transport` с интерфейсом:
- `connect()`, `close()`, `send(obj)` — методы
- События `'connection'`, `'message'`, `'error'`

Сейчас реализация — WebSocket. Когда дойдём до ESP32, **тот же интерфейс**
можно реализовать иначе (WebSocket напрямую к ESP32, без Python-моста).
Виджеты не пострадают.

### 4.4 Store с subscribe — single source of truth

Все данные телеметрии, статус подключения, UI-состояние — в одном объекте.
Виджеты подписываются на изменения. Когда в `telemetry_packet_t` добавится
новое поле — оно автоматически появится в Store, виджеты-подписчики не сломаются,
а новые виджеты смогут читать его сразу.

## 5. Запуск

### 5.1 На стороне ПК

```bash
cd tools/gui
pip install pyserial websockets   # один раз
python server.py                   # автопоиск Arduino
```

Открыть в браузере: <http://localhost:8000>

Опции:
```bash
python server.py --port /dev/ttyUSB0 --baud 9600
python server.py --port COM3                    # Windows
python server.py --ws-port 8765 --http-port 8000
python server.py -v                              # verbose log
```

### 5.2 Что должно работать сразу

- Подключение к Arduino: статус-индикатор в шапке станет зелёным
- Телеметрия идёт 10 раз в секунду — все значения обновляются
- Циферблат: при нажатии физической педали газа → внутренняя голубая дуга растёт.
  Скорость (внешняя амбер) пока 0 — энкодеры подключатся в MVP-2
- Слайдеры внизу отправляют SET_GAS_VIRTUAL / SET_BRAKE_VIRTUAL.
  При отпускании плавно возвращаются к 0
- Под слайдерами: индикаторы `PHYS`, `UART`, `EFF` показывают компоненты
  и итоговое (после комбинатора) значение педали
- E-STOP отправляет emergencyStop (gas=0, brake=1023)
- RELEASE TO PEDALS отправляет releaseControl (обе UART-педали в 0)
- Mode-кнопки отправляют setMode

## 6. Протокол (JSON через WebSocket)

### 6.1 Клиент → Сервер

```javascript
{ cmd: 'setGasVirtual',   value: 500 }     // 0-1023
{ cmd: 'setBrakeVirtual', value: 200 }
{ cmd: 'setMode',         mode: 5 }        // drive_mode_id_t
{ cmd: 'emergencyStop' }
{ cmd: 'releaseControl' }
{ cmd: 'setParam', offset: 24, value: 1023, size: 2 }
{ cmd: 'saveSettings' }
{ cmd: 'resetDefaults' }
{ cmd: 'resetOdometer' }
{ cmd: 'ping' }
{ cmd: 'getTelemetry' }
{ cmd: 'setTelemRate', hz: 10 }
```

### 6.2 Сервер → Клиент

```javascript
// Телеметрия (10 Гц по умолчанию)
{
  type: 'telemetry',
  gasPhysical, gasUart, gasEffective,         // 0-1023 each
  brakePhysical, brakeUart, brakeEffective,
  targetPwm, currentPwm, pwmFreqHz,
  speedRpmL, speedRpmR, speedKmhX10,
  currentMaL, currentMaR,                     // 0 до MVP-3
  driveMode, uartActiveFlags, faults,
  uptimeMs,
}

// Статус подключения
{ type: 'status', state: 'connected'|'disconnected', port, error }

// Debug-текст от Arduino (если попадает в Serial помимо бинарных пакетов)
{ type: 'log', message: "..." }

{ type: 'ack',  cmd: 0x01 }                  // подтверждение
{ type: 'nack', cmd: 0x01, err: 0x02 }       // отказ
{ type: 'pong' }
```

### 6.3 Бинарный протокол Python ↔ Arduino

`[0xAA] [LEN] [CMD] [PAYLOAD...] [CRC16_L] [CRC16_H]`

LEN = длина CMD + PAYLOAD. CRC16-CCITT по LEN + CMD + PAYLOAD, little-endian.

Команды описаны в `firmware/app_protocol.h` (CMD_SET_GAS_VIRTUAL = 0x01 и т.д.).
**Если в firmware добавится новая команда — добавить в:**
1. `server.py` константы и `COMMAND_MAP`
2. `protocol.js` метод-обёртка
3. Виджет, который её использует

## 7. Что ещё не сделано / план развития

### 7.1 Ближайшие задачи (расширение)

- [ ] **Вкладка Settings & Console** — сейчас вкладка есть, но не реализована.
      Нужна форма для редактирования всех полей `settings_t`:
  - Калибровка педалей (min/max RAW, deadzone, кривая, EMA alpha)
  - 9 профилей режимов (max_pwm, accel/decel, частота ШИМ, разрешение, направление)
  - **Комбинаторы педалей** (выпадающий список из 5 вариантов на каждый)
  - PID EPS (Kp/Ki/Kd с numeric input)
  - Лимиты тока
  - Watchdog таймауты (UART RS485 + UART pedals)
  - Электронный дифференциал
  - Кнопки: Read from device / Apply / Save to EEPROM / Reset Defaults
  - Индикатор «Modified» если есть изменения
  - Раздел Console: вывод debug-текста + поле ручной отправки HEX-байт

  Поля настроек описаны в `firmware/cfg_settings.h` (settings_t struct).
  Их можно автоматически генерировать из схемы — это идея на потом.

- [ ] **L/R PWM раздельно** — когда появится электронный дифференциал (MVP-8),
      добавить в телеметрию `pwm_l` / `pwm_r` и показывать их под цифрой циферблата
      и как отдельные серии на графике PWM.

- [ ] **Длительное удержание тормоза = блокировка** — UI-уведомление когда
      brake удерживается >10с, и отдельный режим LOCKED.

- [ ] **Дополнительные графики** — токи моторов, мощность, температура когда
      будут датчики. Сейчас в `charts.js` есть `CHART_DEFS`, добавление графика =
      один объект.

- [ ] **Логирование** — экспорт телеметрии в CSV, проигрывание записанных сессий.

### 7.2 Долгосрочные (MVP-3+)

- [ ] **Подключение PCA9535 inputs** — индикаторы кнопок и переключателей
      машины (Ignition, Neutral, Handbrake, Mode switches) станут «живыми».
      Сейчас они статические заглушки в `panels.js → ExternalInputsWidget`.

- [ ] **Подключение BMS** — батарея получит реальные значения V/A/SOC.
      В `panels.js → BatteryWidget` уже подготовлены поля в Store
      (`batteryPct`, `voltageV`, `batteryAmps`).

- [ ] **ESP32 как хост UI** — перенести `static/` в SPIFFS/LittleFS ESP32,
      перенаправить транспорт на WiFi WebSocket напрямую к ESP32, убрать
      зависимость от Python-сервера.

### 7.3 Мобильная версия

В будущем (когда ESP32) — отдельная страница `mobile.html` с тем же
JS-ядром, но другой раскладкой:
- Landscape ориентация по умолчанию
- Большие touch-области для пальцев
- Виртуальные педали как в гонках на телефоне (свайп вверх/вниз)
- Виртуальный руль через гироскоп (`devicemotion` event)
- Tactile feedback через `navigator.vibrate()`

JS-модули `transport.js`, `protocol.js`, `store.js`, `widgets/*` —
переиспользуются. Только `app.js` и HTML структура меняются.

## 8. Как добавить новый виджет

Пример: добавить виджет температуры моторов.

```javascript
// widgets/temperature.js
export class TemperatureWidget {
  constructor(container, store) {
    this.container = container;
    this.store = store;
    this._build();
    this._unsubscribe = store.subscribe(() => this.render());
  }

  _build() {
    this.container.innerHTML = `
      <div class="temp-row">
        <span class="temp-label">L</span>
        <span class="temp-value mono" data-l>—</span>°C
      </div>
      <div class="temp-row">
        <span class="temp-label">R</span>
        <span class="temp-value mono" data-r>—</span>°C
      </div>
    `;
  }

  render() {
    const t = this.store.getTelemetry();
    // t.tempMotorL / t.tempMotorR появятся когда firmware начнёт их слать
    this.container.querySelector('[data-l]').textContent = t.tempMotorL ?? '—';
    this.container.querySelector('[data-r]').textContent = t.tempMotorR ?? '—';
  }

  destroy() { this._unsubscribe?.(); }
}
```

Затем в `app.js`:
```javascript
import { TemperatureWidget } from './widgets/temperature.js';
// ...
widgets.push(new TemperatureWidget(
  document.querySelector('[data-region="temperature"]'), store));
```

В `index.html` добавить место для виджета: `<div data-region="temperature"></div>`.

Всё. Никаких зависимостей с другими виджетами. Готов к работе как только
firmware начнёт слать `tempMotorL` / `tempMotorR` в телеметрии.

## 9. Дизайн-токены

Цветовая система отражает данные:
- `--accent-current` (амбер `#f5a623`) — «то что есть», current values
- `--accent-target`  (циан  `#5dc8e3`) — «то что запрошено», targets
- `--accent-uart`    (фиол. `#b794f6`) — UART-источник
- `--accent-gas`     (зелён.`#4ade80`) — газ (педаль)
- `--accent-brake`   (красн.`#ef4444`) — тормоз
- `--accent-success` / `--accent-warning` / `--accent-danger` — статусы

Шрифты: **Geist** (sans) + **Geist Mono** (телеметрия, числа).

Все размеры/цвета в CSS-переменных `:root` — менять одно место.

## 10. Связь с firmware

Файлы, которые **должны быть синхронизированы**:

| Контроллер | Веб | Что синхронизируем |
|------------|-----|---------------------|
| `app_protocol.h` | `server.py` константы CMD_* | Идентификаторы команд |
| `app_protocol.h` | `server.py` `parse_telemetry()` | Формат telemetry_packet_t |
| `cfg_settings.h` `drive_mode_id_t` | `protocol.js` `DRIVE_MODE` | Идентификаторы режимов |
| `cfg_settings.h` `settings_t` | (будущее) Settings UI | Структура настроек |
| `util_crc.cpp` | `server.py` `crc16_ccitt()` | Алгоритм CRC |

**Проверено**: Python CRC ↔ Arduino CRC дают идентичный результат
(тест в `tests/` для C-стороны + ручная проверка в Python).

## 11. История версий

- **v1.0** (текущая) — Базовый дашборд: gauge, charts, sliders, modes, e-stop.
  Бинарная модель control_source.
- **v2.0** (текущая) — Layered pedal model: physical + uart, конфигурируемый
  комбинатор. Расширенная телеметрия с раздельными значениями. Watchdog
  внутри svc_pedals. Слайдеры с плавным decay.
- **v3.0** (план) — Settings UI, layered settings management.
- **v4.0** (план) — Перенос на ESP32, мобильная версия.

---

## Контрольный список «продолжить работу»

Если ты в новом чате читаешь это — вот быстрый старт:

1. Прочти этот документ целиком — он содержит всё что нужно про веб.
2. Если нужны детали firmware — `docs/ARCHITECTURE.md` и `docs/MVP1_API.md`.
3. Структура файлов выше — карта проекта.
4. Перед изменениями уточни:
   - Что хочется добавить — новый виджет или изменить существующий?
   - Нужны ли изменения в firmware (новая команда, новое поле телеметрии)?
   - Если да — обнови соответствующие синхронизированные файлы (раздел 10).
5. Запусти проверку: `python server.py`, открой `http://localhost:8000`,
   убедись что подключение к Arduino работает и телеметрия идёт.
6. Сделай изменение. Каждый виджет — самостоятельный модуль, минимизируй
   связи между ними. Используй Store для коммуникации.
