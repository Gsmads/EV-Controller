> ⚠️ **ДОКУМЕНТ УСТАРЕЛ.** Описывает протокол v1 (`SET_SPEED`, `SET_BRAKE`,
> `control_source`, телеметрия 18 байт). В коде реализован v2:
> `SET_GAS_VIRTUAL`, `SET_BRAKE_VIRTUAL`, телеметрия 36 байт, без
> `control_source`. Истина — `firmware/app_protocol.h`.
> Приведение в соответствие — задача 0.7.1 из `SLICE_0_PLAN.md`.

# Протокол управления UART/RS485

**Версия:** 1.0.0  
**Baud:** 9600 (debug), 115200 (production RS485)

---

## Формат пакета

```
[0xAA] [LEN] [CMD] [PAYLOAD...] [CRC16_L] [CRC16_H]
```

| Поле | Размер | Описание |
|------|--------|----------|
| SYNC | 1 | Маркер начала (0xAA) |
| LEN  | 1 | Длина CMD + PAYLOAD (1..60) |
| CMD  | 1 | Идентификатор команды |
| PAYLOAD | 0..59 | Данные команды |
| CRC16 | 2 | CRC16-CCITT по LEN + CMD + PAYLOAD, little-endian |

Текстовый debug-вывод (Serial Monitor) не конфликтует — не начинается с 0xAA.

---

## Команды Host → Controller

| CMD | Имя | Payload | Описание |
|-----|-----|---------|----------|
| 0x01 | SET_SPEED | uint16 LE | Целевая скорость 0–1023 (UART-override) |
| 0x02 | SET_BRAKE | uint16 LE | Принудительное торможение 0–1023 |
| 0x03 | SET_MODE | uint8 | Режим вождения (0–8) |
| 0x04 | EMERGENCY_STOP | — | Мягкая аварийная остановка |
| 0x05 | RELEASE_CTRL | — | Вернуть управление педалям |
| 0x10 | SET_PARAM | u16 offset + u8 size + data | Изменить параметр настроек |
| 0x11 | SAVE_SETTINGS | — | Сохранить в EEPROM |
| 0x12 | RESET_DEFAULTS | — | Сброс к заводским |
| 0x20 | GET_TELEMETRY | — | Запрос одного пакета |
| 0x21 | SET_TELEM_RATE | uint8 | Авто-телеметрия (0=off, 1–50 Гц) |
| 0xFE | PING | — | Heartbeat |

## Ответы Controller → Host

| CMD | Имя | Payload | Описание |
|-----|-----|---------|----------|
| 0x80 | ACK | uint8 (CMD) | Команда принята |
| 0x81 | NACK | uint8 (CMD) + uint8 (err) | Ошибка |
| 0x82 | TELEMETRY | 18 байт struct | Телеметрия |
| 0x83 | PONG | — | Ответ на PING |

## Структура телеметрии (18 байт)

```c
typedef struct __attribute__((packed)) {
    uint16_t gas;            // Педаль газа (0–1023)
    uint16_t brake;          // Педаль тормоза (0–1023)
    uint16_t target_pwm;     // Целевой PWM (0–1023)
    uint16_t current_pwm;    // Текущий PWM (0–1023)
    uint16_t pwm_freq_hz;    // Частота ШИМ
    uint8_t  drive_mode;     // Режим (0–8)
    uint8_t  control_source; // 0=педали, 1=UART
    uint16_t faults;         // Флаги ошибок
    uint32_t uptime_ms;      // Время работы (мс)
} telemetry_packet_t;
```

---

## Приоритет управления

```
UART-команда SET_SPEED/SET_BRAKE
    │
    ▼
┌──────────────────────────┐
│ control_source = UART    │ ← UART переопределяет педали
│ uart_speed / uart_brake  │
└──────────┬───────────────┘
           │
     ┌─────┴─────┐
     │ Watchdog   │ нет команд за uart_timeout_ms
     │ (500 мс)   │ → автоматический release
     └─────┬─────┘
           │
           ▼
┌──────────────────────────┐
│ control_source = PEDALS  │ ← RELEASE_CTRL или watchdog
│ педали газа / тормоза    │
└──────────────────────────┘
```

---

## Python-утилита

### Установка

```bash
pip install pyserial
```

### Запуск

```bash
python ev_control.py                      # автопоиск Arduino
python ev_control.py --port COM3          # Windows
python ev_control.py --port /dev/ttyUSB0  # Linux
python ev_control.py --baud 115200        # другая скорость
```

### Команды

| Команда | Описание |
|---------|----------|
| `s 512` | Скорость 50% (UART-override) |
| `b 1023` | Полный тормоз |
| `r` | Вернуть управление педалям |
| `e` | АВАРИЙНАЯ ОСТАНОВКА |
| `m 5` | Режим Sport |
| `p` | Ping (проверка связи) |
| `t` | Запросить телеметрию |
| `save` | Сохранить настройки в EEPROM |
| `reset` | Сброс к заводским |
| `q` | Выход |

### Пример сессии

```
  Найден Arduino: /dev/ttyUSB0
  Подключено: /dev/ttyUSB0 @ 9600 baud

> p
  → Ping отправлен
  [PONG] Controller alive

> s 200
  → Скорость: 200
  ┌─ Телеметрия ──────────────────────────
  │ Газ:        0/1023   Тормоз:    0/1023
  │ Target:   200 ██████
  │ Current:  198 █████
  │ PWM:     7812 Hz   Mode: ECO
  │ Control: UART      Faults: 0x0000
  │ Uptime:  12.3s
  └────────────────────────────────────────

> r
  ✓ Управление возвращено педалям

> e
  ⚠ АВАРИЙНАЯ ОСТАНОВКА отправлена!
```
