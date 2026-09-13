/**
 * @file cfg_settings.c
 * @brief Реализация менеджера настроек
 *
 * Взаимодействует с EEPROM через HAL (hal_nvm.h).
 * Полностью платформонезависимый, кроме вызовов HAL.
 */
#include "cfg_settings.h"
#include "hal_nvm.h"
#include "util_crc.h"
#include "svc_pedals.h"  /* For pedal_combinator_t enum */

#include <string.h>  /* memcpy, memset */

/* ====================================================================
 *  Внутренние данные
 * ==================================================================== */

/** RAM-копия настроек (рабочая) */
static settings_t current_settings;

/** Флаг: загружено из EEPROM (vs defaults) */
static uint8_t loaded_from_eeprom = 0;

/* ====================================================================
 *  Заголовок EEPROM
 * ==================================================================== */

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  reserved;
} eeprom_header_t;

#define EEPROM_HEADER_SIZE  sizeof(eeprom_header_t)
#define EEPROM_DATA_OFFSET  (SETTINGS_EEPROM_OFFSET + EEPROM_HEADER_SIZE)
#define EEPROM_CRC_OFFSET   (EEPROM_DATA_OFFSET + sizeof(settings_t))

/* ====================================================================
 *  Значения по умолчанию
 * ==================================================================== */

/**
 * @brief Заполнить структуру значениями по умолчанию
 *
 * Эти значения используются при:
 * - первом запуске (EEPROM пустой)
 * - несовпадении CRC (повреждённые данные)
 * - явном сбросе к заводским
 */
static void fill_defaults(settings_t *s)
{
    memset(s, 0, sizeof(settings_t));

    /* --- Педали --- */
    s->pedal_gas_min       = 10;
    s->pedal_gas_max       = 1000;
    s->pedal_brake_min     = 10;
    s->pedal_brake_max     = 1000;
    s->pedal_gas_deadzone  = 20;
    s->pedal_brake_deadzone= 20;
    s->pedal_gas_curve     = PEDAL_CURVE_LINEAR;
    s->pedal_brake_curve   = PEDAL_CURVE_LINEAR;
    s->pedal_ema_alpha     = 40;

    /* --- Профиль: Locked --- */
    s->profiles[DRIVE_MODE_LOCKED] = (drive_profile_t){
        .max_pwm = 0, .accel_rate = 0, .decel_rate = 0,
        .brake_rate_min = 0, .brake_rate_max = 0,
        .pwm_freq_hz = 7812, .pwm_resolution = 10,
        .direction = 0, .pedal_curve = 0xFF, ._pad = 0
    };

    /* --- Профиль: Neutral --- */
    s->profiles[DRIVE_MODE_NEUTRAL] = s->profiles[DRIVE_MODE_LOCKED];

    /* --- Профиль: Handbrake --- */
    s->profiles[DRIVE_MODE_HANDBRAKE] = s->profiles[DRIVE_MODE_LOCKED];

    /* --- Профиль: Eco --- */
    s->profiles[DRIVE_MODE_ECO] = (drive_profile_t){
        .max_pwm       = 400,       /* ~40% */
        .accel_rate    = 200,       /* Медленный разгон */
        .decel_rate    = 400,       /* Плавное замедление */
        .brake_rate_min= 300,
        .brake_rate_max= 1500,
        .pwm_freq_hz   = 7812,     /* Phase-Correct, 10 бит */
        .pwm_resolution= 10,
        .direction     = 0,
        .pedal_curve   = PEDAL_CURVE_QUADRATIC,  /* Спокойный отклик */
        ._pad = 0
    };

    /* --- Профиль: Normal --- */
    s->profiles[DRIVE_MODE_NORMAL] = (drive_profile_t){
        .max_pwm       = 716,       /* ~70% */
        .accel_rate    = 400,       /* Средний разгон */
        .decel_rate    = 600,
        .brake_rate_min= 400,
        .brake_rate_max= 2000,
        .pwm_freq_hz   = 7812,
        .pwm_resolution= 10,
        .direction     = 0,
        .pedal_curve   = PEDAL_CURVE_LINEAR,
        ._pad = 0
    };

    /* --- Профиль: Sport --- */
    s->profiles[DRIVE_MODE_SPORT] = (drive_profile_t){
        .max_pwm       = 1023,      /* 100% */
        .accel_rate    = 800,       /* Быстрый разгон */
        .decel_rate    = 800,
        .brake_rate_min= 500,
        .brake_rate_max= 2500,
        .pwm_freq_hz   = 15625,    /* Phase-Correct, 9 бит */
        .pwm_resolution= 9,
        .direction     = 0,
        .pedal_curve   = 0xFF,      /* Глобальная кривая */
        ._pad = 0
    };

    /* --- Профиль: Reverse --- */
    s->profiles[DRIVE_MODE_REVERSE] = (drive_profile_t){
        .max_pwm       = 300,       /* ~30% */
        .accel_rate    = 150,       /* Медленный */
        .decel_rate    = 400,
        .brake_rate_min= 300,
        .brake_rate_max= 1500,
        .pwm_freq_hz   = 7812,
        .pwm_resolution= 10,
        .direction     = 1,         /* Reverse! */
        .pedal_curve   = PEDAL_CURVE_QUADRATIC,
        ._pad = 0
    };

    /* --- Профиль: Parent (Remote) --- */
    s->profiles[DRIVE_MODE_PARENT] = (drive_profile_t){
        .max_pwm       = 500,       /* По умолчанию 50%, переопределяется по UART */
        .accel_rate    = 300,
        .decel_rate    = 500,
        .brake_rate_min= 400,
        .brake_rate_max= 2000,
        .pwm_freq_hz   = 7812,
        .pwm_resolution= 10,
        .direction     = 0,
        .pedal_curve   = 0xFF,
        ._pad = 0
    };

    /* --- Профиль: Failsafe (потеря связи) --- */
    s->profiles[DRIVE_MODE_FAILSAFE] = (drive_profile_t){
        .max_pwm       = 0,
        .accel_rate    = 0,
        .decel_rate    = 200,       /* Мягкое замедление Eco-уровня */
        .brake_rate_min= 200,
        .brake_rate_max= 200,
        .pwm_freq_hz   = 7812,
        .pwm_resolution= 10,
        .direction     = 0,
        .pedal_curve   = 0xFF,
        ._pad = 0
    };

    /* --- EPS PID --- */
    s->eps_kp       = 200;      /* Kp = 2.00 */
    s->eps_ki       = 10;       /* Ki = 0.10 */
    s->eps_kd       = 50;       /* Kd = 0.50 */
    s->eps_center   = 512;      /* Центр руля */

    /* --- Защита по току --- */
    s->current_limit_soft_ma = 15000;   /* 15А — мягкое ограничение */
    s->current_limit_hard_ma = 20000;   /* 20А — аварийное отключение */
    s->current_limit_time_ms = 3000;    /* 3 сек до аварийного */

    /* --- Watchdog RS485 --- */
    s->uart_timeout_ms = 500;           /* 500 мс без связи → failsafe */

    /* --- Электронный дифференциал --- */
    s->diff_enabled = 0;                /* Выключен по умолчанию */
    s->diff_gain    = 30;               /* 0.30 */

    /* --- Мотор --- */
    s->motor_deadzone = 30;             /* Минимальный PWM */

    /* --- Layered pedal model (v2 defaults) --- */
    s->gas_combinator       = PEDAL_COMBINE_MAX;       /* По умолчанию: max(physical, uart) */
    s->brake_combinator     = PEDAL_COMBINE_MAX;       /* Тормоз: всегда max — безопаснее */
    s->uart_pedal_timeout_ms = 200;                     /* Watchdog UART-педали */

    /* --- Параметры UART (v3) --- */
    s->uart_baud_code         = UART_BAUD_CODE_250000;  /* ADR-0015 */
    s->uart_tx_policy         = TX_OVERFLOW_DROP_PACKET;/* ADR-0016: умолчание */
    s->uart_baud_probation_ms = 10000;                  /* ADR-0019 */

    /* --- Каналы АЦП (v4), ADR-0009 ---
       Та же карта, что была зашита в cfg_board.h: A0…A3 и A6 — это каналы
       0…3 и 6. Умолчание, а не истина: проверяется тестером на плате. */
    s->adc_ch_pedal_gas      = 0;
    s->adc_ch_pedal_brake    = 1;
    s->adc_ch_current_right  = 2;
    s->adc_ch_steering_pos   = 3;
    s->adc_ch_current_left   = 6;
}

uint8_t cfg_settings_validate_adc(const settings_t *s)
{
    const uint8_t ch[5] = {
        s->adc_ch_pedal_gas, s->adc_ch_pedal_brake,
        s->adc_ch_current_right, s->adc_ch_current_left,
        s->adc_ch_steering_pos
    };
    for (uint8_t i = 0; i < 5; i++) {
        if (ch[i] >= ADC_CHANNEL_COUNT) {
            return ADC_MAP_OUT_OF_RANGE;
        }
        for (uint8_t j = i + 1; j < 5; j++) {
            if (ch[i] == ch[j]) {
                return ADC_MAP_DUPLICATE;
            }
        }
    }
    return ADC_MAP_OK;
}

/** Таблица скоростей. Порядок соответствует uart_baud_code_t и не меняется. */
static const uint32_t baud_table[UART_BAUD_CODE_COUNT] = {
    9600UL, 19200UL, 38400UL, 57600UL, 115200UL, 250000UL, 500000UL
};

uint32_t cfg_settings_baud_from_code(uint8_t code)
{
    if (code >= UART_BAUD_CODE_COUNT) {
        return 0;   /* недопустимый код отличим от любой настоящей скорости */
    }
    return baud_table[code];
}

/* ====================================================================
 *  Внутренние функции EEPROM
 * ==================================================================== */

/**
 * @brief Вычислить CRC16 для заголовка + данных
 */
/**
 * @brief CRC блока настроек заданной версии и размера
 *
 * Версия и размер — параметры, а не константы, потому что при миграции
 * надо проверить целостность блока ПРОШЛОЙ версии его же правилами.
 */
static uint16_t compute_crc_of(const void *data, uint16_t size, uint8_t version)
{
    uint16_t crc = 0xFFFF;

    /* CRC по magic + version */
    uint16_t magic   = SETTINGS_MAGIC;
    uint8_t  reserved= 0;

    crc = util_crc16_update(crc, (uint8_t)(magic & 0xFF));
    crc = util_crc16_update(crc, (uint8_t)(magic >> 8));
    crc = util_crc16_update(crc, version);
    crc = util_crc16_update(crc, reserved);

    /* CRC по данным */
    const uint8_t *ptr = (const uint8_t *)data;
    for (uint16_t i = 0; i < size; i++) {
        crc = util_crc16_update(crc, ptr[i]);
    }

    return crc;
}

static uint16_t compute_crc(const settings_t *s)
{
    return compute_crc_of(s, sizeof(settings_t), SETTINGS_VERSION);
}

/* ====================================================================
 *  Публичный API
 * ==================================================================== */

/** Размер блока настроек по номеру версии. 0 — версия неизвестна. */
static uint16_t size_of_version(uint8_t version)
{
    switch (version) {
        case 2: return SETTINGS_SIZE_V2;
        case 3: return SETTINGS_SIZE_V3;
        case SETTINGS_VERSION: return sizeof(settings_t);
        default: return 0;
    }
}

/**
 * @brief Миграция настроек прошлой версии в текущую
 *
 * Одно правило вместо ветки на каждую пару версий: поля дописываются только
 * в конец структуры, поэтому блок любой прошлой версии — префикс текущего.
 * Читаем префикс поверх умолчаний, новые поля остаются с умолчаниями.
 *
 * Без миграции первый же запуск после обновления сбрасывал бы калибровку
 * педалей, которую добывают замерами на железе.
 *
 * Целостность старого блока проверяется правилами ЕГО версии: CRC считался
 * по его размеру и его номеру версии.
 *
 * @param version Версия блока в EEPROM
 * @return 1 — мигрировано, 0 — нельзя (умолчания уже проставлены)
 */
static uint8_t migrate_from(uint8_t version)
{
    uint16_t old_size = size_of_version(version);

    fill_defaults(&current_settings);

    if (old_size == 0 || old_size > sizeof(settings_t)) {
        return 0;   /* незнакомая или более новая версия — угадывать нечего */
    }

    uint8_t *dst = (uint8_t *)&current_settings;

    /* CRC старого блока: считаем по мере чтения, не держа копию в RAM —
       на AVR лишние 186 байт стека заметны. */
    uint16_t crc = 0xFFFF;
    crc = util_crc16_update(crc, (uint8_t)(SETTINGS_MAGIC & 0xFF));
    crc = util_crc16_update(crc, (uint8_t)(SETTINGS_MAGIC >> 8));
    crc = util_crc16_update(crc, version);
    crc = util_crc16_update(crc, 0);

    for (uint16_t i = 0; i < old_size; i++) {
        uint8_t b = hal_nvm_read_byte((uint16_t)(EEPROM_DATA_OFFSET + i));
        crc = util_crc16_update(crc, b);
        dst[i] = b;
    }

    uint8_t crc_buf[2];
    hal_nvm_read((uint16_t)(EEPROM_DATA_OFFSET + old_size), crc_buf, 2);
    uint16_t stored_crc = (uint16_t)crc_buf[0] | ((uint16_t)crc_buf[1] << 8);

    if (crc != stored_crc) {
        fill_defaults(&current_settings);   /* блок повреждён — не гадаем */
        return 0;
    }
    return 1;
}

void cfg_settings_init(void)
{
    loaded_from_eeprom = 0;

    /* 1. Читаем заголовок */
    eeprom_header_t header;
    hal_nvm_read(SETTINGS_EEPROM_OFFSET,
                    (uint8_t *)&header, sizeof(header));

    /* 2. Проверяем magic */
    if (header.magic != SETTINGS_MAGIC) {
        fill_defaults(&current_settings);
        return;
    }

    /* 3. Проверяем версию */
    if (header.version != SETTINGS_VERSION) {
        loaded_from_eeprom = migrate_from(header.version);
        if (loaded_from_eeprom && cfg_settings_validate_adc(&current_settings) != ADC_MAP_OK) {
            /* Блок цел, но карта каналов негодна: два сигнала на одном входе
               означали бы, что педаль газа читает ток мотора (ADR-0009). */
            fill_defaults(&current_settings);
            loaded_from_eeprom = 0;
        }
        return;
    }

    /* 4. Читаем данные */
    hal_nvm_read(EEPROM_DATA_OFFSET,
                    (uint8_t *)&current_settings, sizeof(settings_t));

    /* 5. Читаем сохранённый CRC */
    uint8_t crc_buf[2];
    hal_nvm_read(EEPROM_CRC_OFFSET, crc_buf, 2);
    uint16_t stored_crc = (uint16_t)crc_buf[0] | ((uint16_t)crc_buf[1] << 8);

    /* 6. Проверяем CRC */
    uint16_t computed_crc = compute_crc(&current_settings);
    if (stored_crc != computed_crc) {
        fill_defaults(&current_settings);
        return;
    }

    /* 7. Проверяем карту каналов АЦП (ADR-0009).
       CRC подтверждает целостность блока, но не его осмысленность: два
       сигнала на одном канале означали бы, что педаль газа читает ток
       мотора, и прочитанное было бы выдано за верное. */
    if (cfg_settings_validate_adc(&current_settings) != ADC_MAP_OK) {
        fill_defaults(&current_settings);
        return;
    }

    loaded_from_eeprom = 1;
}

void cfg_settings_save(void)
{
    /* 1. Записываем заголовок */
    eeprom_header_t header;
    header.magic    = SETTINGS_MAGIC;
    header.version  = SETTINGS_VERSION;
    header.reserved = 0;
    hal_nvm_write(SETTINGS_EEPROM_OFFSET,
                     (const uint8_t *)&header, sizeof(header));

    /* 2. Записываем данные */
    hal_nvm_write(EEPROM_DATA_OFFSET,
                     (const uint8_t *)&current_settings, sizeof(settings_t));

    /* 3. Вычисляем и записываем CRC */
    uint16_t crc = compute_crc(&current_settings);
    uint8_t crc_buf[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
    hal_nvm_write(EEPROM_CRC_OFFSET, crc_buf, 2);
}

void cfg_settings_reset_defaults(void)
{
    fill_defaults(&current_settings);
    loaded_from_eeprom = 0;
}

const settings_t* cfg_settings_get(void)
{
    return &current_settings;
}

settings_t* cfg_settings_get_mutable(void)
{
    return &current_settings;
}

const drive_profile_t* cfg_settings_get_profile(drive_mode_id_t mode)
{
    if (mode >= DRIVE_MODE_COUNT) return &current_settings.profiles[DRIVE_MODE_ECO];
    return &current_settings.profiles[mode];
}

uint8_t cfg_settings_set_field(uint16_t offset, const void *data, uint8_t size)
{
    if (offset + size > sizeof(settings_t)) return 1;

    uint8_t *ptr = (uint8_t *)&current_settings;
    memcpy(ptr + offset, data, size);
    return 0;
}

uint16_t cfg_settings_get_size(void)
{
    return sizeof(settings_t);
}

uint8_t cfg_settings_is_loaded_from_eeprom(void)
{
    return loaded_from_eeprom;
}
