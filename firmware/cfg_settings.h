/**
 * @file cfg_settings.h
 * @brief Менеджер настроек (EEPROM с CRC-защитой)
 *
 * Все настраиваемые параметры системы хранятся в единой структуре settings_t,
 * которая:
 * - при старте загружается из EEPROM (если CRC и magic валидны)
 * - при невалидных данных заполняется значениями по умолчанию
 * - может быть изменена по команде RS485 (SET_PARAM) без перепрошивки
 * - сохраняется в EEPROM явным вызовом cfg_settings_save()
 *
 * Структура EEPROM:
 *   [0x0000..0x0001]  MAGIC  (0x4556 = "EV")
 *   [0x0002]          VERSION (layout version, для миграции)
 *   [0x0003]          reserved
 *   [0x0004..0x00XX]  settings_t data
 *   [0x00XX+1..+2]    CRC16 (over magic+version+data)
 *
 * @version 2.0.0
 */
#pragma once

#include <stdint.h>
#include "cfg_board.h"   /* ADC_CHANNEL_COUNT */

/* ====================================================================
 *  Константы
 * ==================================================================== */

#define SETTINGS_MAGIC          0x4556  /* "EV" */
#define SETTINGS_VERSION        4       /* v4: каналы АЦП в настройках (ADR-0009) */

/** Размеры settings_t прошлых версий — для миграции.
 *
 *  Поля всегда дописываются В КОНЕЦ структуры, поэтому блок любой прошлой
 *  версии является префиксом блока текущей. Это превращает миграцию в одно
 *  правило «прочитать префикс поверх умолчаний» вместо ветки на каждую пару
 *  версий. Нарушить порядок добавления — значит сломать все переходы сразу.
 */
#define SETTINGS_SIZE_V2        182     /* слоистые педали */
#define SETTINGS_SIZE_V3        186     /* + параметры UART */

#define SETTINGS_EEPROM_OFFSET  0       /* Начальный адрес в EEPROM */

/* ====================================================================
 *  Типы данных
 * ==================================================================== */

/** Тип кривой отклика педали (совпадает с util_math response_curve_t) */
typedef enum {
    PEDAL_CURVE_LINEAR    = 0,
    PEDAL_CURVE_QUADRATIC = 1,
    PEDAL_CURVE_S_CURVE   = 2
} pedal_curve_t;

/**
 * @brief Коды допустимых скоростей UART (ADR-0019)
 *
 * Скорость хранится кодом, а не числом: произвольное значение в настройки
 * попасть не может, и делитель никогда не окажется бессмысленным.
 *
 * ВАЖНО: коды только дописываются в конец. Перестановка изменила бы смысл
 * уже сохранённых в EEPROM значений — плата после обновления заговорила бы
 * на другой скорости.
 */
typedef enum {
    UART_BAUD_CODE_9600   = 0,
    UART_BAUD_CODE_19200  = 1,
    UART_BAUD_CODE_38400  = 2,
    UART_BAUD_CODE_57600  = 3,
    UART_BAUD_CODE_115200 = 4,
    UART_BAUD_CODE_250000 = 5,   /**< Умолчание, ADR-0015 */
    UART_BAUD_CODE_500000 = 6,
    UART_BAUD_CODE_COUNT  = 7
} uart_baud_code_t;

/**
 * @brief Политика при нехватке места в буфере передачи (ADR-0016)
 *
 * Числовые значения совпадают с hal_uart_tx_policy_t. Дублирование
 * намеренное: cfg_settings — сквозной модуль и не включает заголовки HAL.
 */
typedef enum {
    TX_OVERFLOW_DROP_PACKET = 0,  /**< Умолчание: кадр не пишется целиком */
    TX_OVERFLOW_BLOCK       = 1   /**< Ждать освобождения места */
} tx_overflow_policy_t;

/** Идентификаторы режимов вождения */
typedef enum {
    DRIVE_MODE_LOCKED    = 0,
    DRIVE_MODE_NEUTRAL   = 1,
    DRIVE_MODE_HANDBRAKE = 2,
    DRIVE_MODE_ECO       = 3,
    DRIVE_MODE_NORMAL    = 4,
    DRIVE_MODE_SPORT     = 5,
    DRIVE_MODE_REVERSE   = 6,
    DRIVE_MODE_PARENT    = 7,
    DRIVE_MODE_FAILSAFE  = 8,
    DRIVE_MODE_COUNT     = 9
} drive_mode_id_t;

/**
 * @brief Профиль режима вождения
 *
 * Определяет характеристики движения для каждого режима.
 * Хранится в EEPROM как часть settings_t.
 */
typedef struct __attribute__((packed)) {
    uint16_t max_pwm;           /**< Максимальный PWM (0–1023) */
    uint16_t accel_rate;        /**< Скорость разгона (PWM/сек) */
    uint16_t decel_rate;        /**< Скорость замедления (отпускание газа) */
    uint16_t brake_rate_min;    /**< Мин. скорость торможения педалью */
    uint16_t brake_rate_max;    /**< Макс. скорость торможения педалью */
    uint16_t pwm_freq_hz;       /**< Целевая частота ШИМ (Гц) */
    uint8_t  pwm_resolution;    /**< Разрешение: 9 или 10 бит */
    uint8_t  direction;         /**< 0=forward, 1=reverse */
    uint8_t  pedal_curve;       /**< Переопределение кривой (0xFF=глобальная) */
    uint8_t  _pad;              /**< Выравнивание до чётного размера */
} drive_profile_t;

/**
 * @brief Полная структура настроек
 *
 * Все поля, которые могут быть изменены удалённо.
 * Сохраняется в EEPROM с CRC16.
 *
 * ВАЖНО: при добавлении полей — инкрементировать SETTINGS_VERSION
 * и добавить миграцию в cfg_settings_migrate().
 */
typedef struct __attribute__((packed)) {
    /* --- Калибровка педалей --- */
    uint16_t pedal_gas_min;         /**< RAW АЦП: отпущена */
    uint16_t pedal_gas_max;         /**< RAW АЦП: нажата */
    uint16_t pedal_brake_min;
    uint16_t pedal_brake_max;
    uint8_t  pedal_gas_deadzone;    /**< Мёртвая зона (0–100) */
    uint8_t  pedal_brake_deadzone;
    uint8_t  pedal_gas_curve;       /**< pedal_curve_t */
    uint8_t  pedal_brake_curve;
    uint8_t  pedal_ema_alpha;       /**< EMA коэффициент (0–255) */
    uint8_t  _pad1;

    /* --- Профили режимов вождения --- */
    drive_profile_t profiles[DRIVE_MODE_COUNT];

    /* --- PID руля (EPS) --- */
    int16_t  eps_kp;                /**< ×100 */
    int16_t  eps_ki;                /**< ×100 */
    int16_t  eps_kd;                /**< ×100 */
    uint16_t eps_center;            /**< Калибровочный центр руля (АЦП) */

    /* --- Защита по току --- */
    uint16_t current_limit_soft_ma; /**< Мягкое ограничение (мА) */
    uint16_t current_limit_hard_ma; /**< Аварийное отключение (мА) */
    uint16_t current_limit_time_ms; /**< Время до аварийного отключения */

    /* --- Watchdog RS485 --- */
    uint16_t uart_timeout_ms;       /**< Таймаут потери связи */

    /* --- Электронный дифференциал --- */
    uint8_t  diff_enabled;          /**< 0=выкл, 1=вкл */
    uint8_t  diff_gain;             /**< ×100 (0..100 → 0.00..1.00) */

    /* --- Мотор --- */
    uint8_t  motor_deadzone;        /**< Минимальный PWM для вращения */
    uint8_t  _pad2;

    /* --- Layered pedal model (v2) --- */
    uint8_t  gas_combinator;        /**< pedal_combinator_t для газа */
    uint8_t  brake_combinator;      /**< pedal_combinator_t для тормоза */
    uint16_t uart_pedal_timeout_ms; /**< Watchdog виртуальных педалей */

    /* --- Параметры UART (v3) ---
       Дописаны в конец намеренно: так блок версии 2 остаётся префиксом
       блока версии 3, и миграция сводится к чтению префикса. */
    uint8_t  uart_baud_code;        /**< uart_baud_code_t, ADR-0019 */
    uint8_t  uart_tx_policy;        /**< tx_overflow_policy_t, ADR-0016 */
    uint16_t uart_baud_probation_ms;/**< Испытательный период скорости, ADR-0019 */

    /* --- Каналы АЦП (v4), ADR-0009 ---
       Соответствие сигналов входам определяется тем, как легли провода при
       монтаже, и по схеме не восстанавливается. Это индексы каналов АЦП
       (0…7), а не номера выводов платы: любой сигнал читается с любого
       входа. Валидация — cfg_settings_validate_adc(). */
    uint8_t  adc_ch_pedal_gas;
    uint8_t  adc_ch_pedal_brake;
    uint8_t  adc_ch_current_right;
    uint8_t  adc_ch_current_left;
    uint8_t  adc_ch_steering_pos;

} settings_t;

/* ====================================================================
 *  Публичный API
 * ==================================================================== */

/** Результат проверки каналов АЦП. */
typedef enum {
    ADC_MAP_OK          = 0,  /**< Карта каналов пригодна */
    ADC_MAP_OUT_OF_RANGE= 1,  /**< Канал больше либо равен ADC_CHANNEL_COUNT */
    ADC_MAP_DUPLICATE   = 2   /**< Два сигнала на одном канале */
} adc_map_status_t;

/**
 * @brief Проверить карту каналов АЦП
 *
 * ADR-0009 требует двух условий: номер меньше восьми и два сигнала не
 * на одном канале. Второе важнее первого: совпадение каналов означает,
 * что педаль газа читает ток мотора, и это не отказ, а неверные данные,
 * выданные за верные.
 *
 * @param s Настройки
 * @return adc_map_status_t
 */
uint8_t cfg_settings_validate_adc(const settings_t *s);

/**
 * @brief Скорость в бодах по коду
 *
 * @param code Код из uart_baud_code_t
 * @return Скорость, бод; 0 если код недопустим — у отказа должно быть
 *         отличимое значение, а не подстановка умолчания молча
 */
uint32_t cfg_settings_baud_from_code(uint8_t code);

/**
 * @brief Инициализация: загрузка настроек из EEPROM
 *
 * Если magic/CRC невалидны или version не совпадает — сброс к defaults.
 * При несовпадении version (но валидном magic) — попытка миграции.
 */
void cfg_settings_init(void);

/**
 * @brief Сохранить текущие настройки в EEPROM
 *
 * Записывает magic + version + settings + CRC16.
 * Вызывать явно после изменения параметров (не автоматически —
 * чтобы не изнашивать EEPROM при частых обновлениях).
 */
void cfg_settings_save(void);

/**
 * @brief Сброс к заводским настройкам
 *
 * Заполняет settings_t значениями по умолчанию.
 * НЕ сохраняет в EEPROM автоматически — вызвать cfg_settings_save() отдельно.
 */
void cfg_settings_reset_defaults(void);

/**
 * @brief Получить указатель на текущие настройки (read-only)
 * @return Указатель на settings_t в RAM
 */
const settings_t* cfg_settings_get(void);

/**
 * @brief Получить указатель на настройки для записи
 *
 * Используется для прямого изменения полей перед вызовом cfg_settings_save().
 * @return Указатель на settings_t в RAM (mutable)
 */
settings_t* cfg_settings_get_mutable(void);

/**
 * @brief Получить профиль конкретного режима вождения
 * @param mode Идентификатор режима
 * @return Указатель на drive_profile_t (read-only)
 */
const drive_profile_t* cfg_settings_get_profile(drive_mode_id_t mode);

/**
 * @brief Записать один параметр по смещению в структуре
 *
 * Универсальная функция для обновления настроек по команде RS485 (SET_PARAM).
 * Обновляет только RAM-копию. Для сохранения в EEPROM — вызвать save().
 *
 * @param offset Смещение поля в settings_t (offsetof)
 * @param data   Указатель на данные
 * @param size   Размер поля (1, 2 или 4 байта)
 * @return 0 = OK, 1 = offset+size выходит за границы структуры
 */
uint8_t cfg_settings_set_field(uint16_t offset, const void *data, uint8_t size);

/**
 * @brief Получить размер структуры настроек (для проверки и телеметрии)
 */
uint16_t cfg_settings_get_size(void);

/**
 * @brief Проверка: были ли настройки загружены из EEPROM (или defaults)
 * @return 1 если загружены из EEPROM, 0 если defaults
 */
uint8_t cfg_settings_is_loaded_from_eeprom(void);
