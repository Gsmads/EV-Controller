/**
 * @file svc_speed.cpp
 * @brief Реализация расчёта скорости по времени между импульсами
 *
 * ==== Почему период, а не счёт импульсов (ADR-0023) ====
 *
 * Прежний способ считал импульсы за окно 100 мс:
 *   RPM = импульсов × частота × 60 / импульсов_на_оборот
 * При 12 импульсах на оборот и 10 Гц один импульс — это 50 об/мин,
 * то есть 1,9 км/ч. Показать 7,4 км/ч такой способ не может в принципе:
 * между 5,7 и 7,5 у него нет ни одного значения.
 *
 * Измерение времени между двумя соседними импульсами при разрешении
 * таймера 4 мкс даёт на тех же 400 об/мин период 12500 мкс с ошибкой
 * 0,03 % — на четыре порядка точнее.
 *
 * ==== Вывод формул ====
 *
 * Один оборот — это ppr импульсов, значит период оборота = period × ppr.
 *
 *   об/мин = 60 000 000 / (period_мкс × ppr)
 *
 * Линейная скорость: за оборот колесо проходит π·D мм.
 *
 *   мм/ч    = (60 000 000 / (period × ppr)) × π·D × 60
 *   км/ч    = мм/ч / 1 000 000
 *   км/ч×100 = 60 000 000 × π·D × 60 × 100 / (period × ppr × 1 000 000)
 *            = D × (60 × π × 100 × 60) / (period × ppr)
 *            = D × 1 131 000 / (period × ppr)
 *
 * Точное значение 60·π·100·60 = 1 130 973,36; округление до 1 131 000
 * даёт +0,0024 % — на порядки меньше погрешности самого датчика.
 *
 * Проверочная точка: D = 200 мм, ppr = 12, period = 12500 мкс.
 *   об/мин = 60 000 000 / 150 000 = 400
 *   км/ч×100 = 200 × 1 131 000 / 150 000 = 1508  →  15,08 км/ч
 * Сверка от физики: 400 об/мин × π × 200 мм = 251 327 мм/мин
 *   = 15,08 км/ч. Сходится.
 *
 * ==== Почему нет промежуточного целого об/мин ====
 *
 * км/ч считается прямо из периода, а не из округлённых об/мин: иначе
 * округление происходило бы дважды и на низких скоростях съедало бы
 * ту самую десятую долю, ради которой всё и делается.
 *
 * @version 2.0.0
 */
#include "svc_speed.h"
#include "hal_encoder.h"
#include "hal_system.h"
#include "cfg_board.h"
#include "cfg_settings.h"

/* Умолчания обязаны укладываться в те же границы, что и значения из
 * настроек. Вывод границ - в cfg_board.h; проверка здесь ловит правку
 * умолчания, сделанную мимо реестра параметров. */
#if WHEEL_DIAMETER_MM > WHEEL_DIAMETER_MM_MAX || WHEEL_DIAMETER_MM < 1
#error "WHEEL_DIAMETER_MM вне границ: числитель расчёта скорости переполнит uint32"
#endif
#if ENCODER_PULSES_PER_REV > ENCODER_PPR_MAX || ENCODER_PULSES_PER_REV < 1
#error "ENCODER_PULSES_PER_REV вне границ: ноль даёт деление на ноль, потолок - переполнение"
#endif

/* ====================================================================
 *  Внутреннее состояние
 * ==================================================================== */

typedef struct {
    speed_state_t state;
    uint16_t      rpm;
    uint16_t      kmh_x100;
} wheel_state_t;

static wheel_state_t wheel[SPEED_WHEEL_COUNT];
static uint16_t      kmh_x10;
static uint32_t      odometer_mm;
static uint32_t      odometer_frac_x1000;  /* тысячные миллиметра, чтобы не терять остаток */

/* ====================================================================
 *  Вспомогательные функции
 * ==================================================================== */

/** Колесо → канал энкодера. Нумерация у них разная, и это единственное место,
 *  где о расхождении нужно помнить. */
static encoder_channel_t channel_of(speed_wheel_t w)
{
    return (w == SPEED_WHEEL_LEFT) ? ENCODER_LEFT : ENCODER_RIGHT;
}

/** Импульсов на оборот из настроек. Ноль невозможен по реестру
 *  параметров, но деление на него было бы молчаливой катастрофой. */
static uint32_t pulses_per_rev(void)
{
    uint16_t v = cfg_settings_get()->encoder_pulses_per_rev;
    return (v == 0) ? (uint32_t)ENCODER_PULSES_PER_REV : (uint32_t)v;
}

/** Диаметр колеса из настроек. */
static uint32_t wheel_diameter_mm(void)
{
    uint16_t v = cfg_settings_get()->wheel_diameter_mm;
    return (v == 0) ? (uint32_t)WHEEL_DIAMETER_MM : (uint32_t)v;
}

/** Путь на один импульс в тысячных миллиметра: π·D·1000 / ppr */
static uint32_t mm_per_pulse_x1000(void)
{
    /* 31416 / 10000 = π с точностью +0,0026 % */
    return (31416UL * wheel_diameter_mm()) / (10UL * pulses_per_rev());
}

/* ====================================================================
 *  Публичный API
 * ==================================================================== */

void svc_speed_init(void)
{
    for (uint8_t i = 0; i < SPEED_WHEEL_COUNT; i++) {
        wheel[i].state    = SPEED_NO_DATA;
        wheel[i].rpm      = 0;
        wheel[i].kmh_x100 = 0;
    }
    kmh_x10             = 0;
    odometer_mm         = 0;
    odometer_frac_x1000 = 0;
    hal_encoder_init();
}

void svc_speed_update(void)
{
    uint32_t now     = hal_system_micros();
    uint32_t pulses  = 0;
    uint32_t sum_x100 = 0;
    uint8_t  moving   = 0;

    for (uint8_t i = 0; i < SPEED_WHEEL_COUNT; i++) {
        encoder_sample_t s;
        hal_encoder_take(channel_of((speed_wheel_t)i), &s);

        pulses += s.pulses;

        if (!s.has_period) {
            /* Ни одного измерения с запуска. Стоящее колесо и молчащий
               датчик отсюда неразличимы, поэтому состояние именно такое,
               а не "остановлено". */
            wheel[i].state    = SPEED_NO_DATA;
            wheel[i].rpm      = 0;
            wheel[i].kmh_x100 = 0;
            continue;
        }

        /* Возраст последнего импульса. Если он больше измеренного периода,
           значит колесо замедлилось и настоящий текущий период уже не
           меньше возраста. Без этой поправки показание замерло бы на
           последнем значении до самого порога остановки. */
        uint32_t age = now - s.last_pulse_us;
        uint32_t period = (age > s.period_us) ? age : s.period_us;

        if (period >= ENCODER_STOP_TIMEOUT_US) {
            /* Период длиннее порога — это не медленное движение, а
               остановка. Тем же правилом отсекается первый импульс после
               простоя: его период охватывает весь простой. */
            wheel[i].state    = SPEED_STOPPED;
            wheel[i].rpm      = 0;
            wheel[i].kmh_x100 = 0;
            continue;
        }

        uint32_t denom = period * pulses_per_rev();

        uint32_t rpm32 = (60000000UL + denom / 2) / denom;
        uint32_t kmh32 = (wheel_diameter_mm() * 1131000UL + denom / 2) / denom;

        /* Насыщение вместо усечения — это дефект T-3 в новом обличье.
         * Фильтр дребезга пропускает период от ENCODER_MIN_PERIOD_US, то есть
         * до 25 000 об/мин. В uint16 это влезает, а км/ч×100 — нет: при
         * периоде 287 мкс точное значение 65 679 усеклось бы до 143, то есть
         * колесо, крутящееся на 17 400 об/мин, показало бы 1,4 км/ч. Быстрое
         * выглядит медленным — ровно тот молчаливый отказ, из-за которого
         * T-3 и записан.
         *
         * Настоящий потолок скорости известен только после замеров на
         * железе; выдумывать его здесь нельзя, поэтому ограничение стоит по
         * пределу типа. Упёршееся в предел значение заведомо абсурдно и
         * видно, а завёрнутое — правдоподобно и невидимо. */
        wheel[i].rpm      = (rpm32 > 65535UL) ? (uint16_t)65535U : (uint16_t)rpm32;
        wheel[i].kmh_x100 = (kmh32 > 65535UL) ? (uint16_t)65535U : (uint16_t)kmh32;
        wheel[i].state    = SPEED_MOVING;

        sum_x100 += wheel[i].kmh_x100;
        moving++;
    }

    /* Средняя скорость по колёсам, которые действительно едут. При
       повороте колёса крутятся по-разному — это нормально и усредняется.
       Колесо без данных в среднее не входит: иначе одно молчащее колесо
       занижало бы скорость машины вдвое. */
    if (moving > 0) {
        uint32_t avg_x100 = (sum_x100 + moving / 2) / moving;
        kmh_x10 = (uint16_t)((avg_x100 + 5) / 10);
    } else {
        kmh_x10 = 0;
    }

    /* Одометр: путь считается импульсами, а не временем. Остаток
       переносится между вызовами, иначе деление съедало бы его каждый
       раз и путь систематически занижался. Делим на 2 — среднее по двум
       колёсам. */
    if (pulses > 0) {
        odometer_frac_x1000 += (pulses * mm_per_pulse_x1000()) / 2;
        odometer_mm         += odometer_frac_x1000 / 1000;
        odometer_frac_x1000 %= 1000;
    }
}

speed_state_t svc_speed_get_state(speed_wheel_t w)
{
    if (w >= SPEED_WHEEL_COUNT) return SPEED_NO_DATA;
    return wheel[w].state;
}

uint16_t svc_speed_get_rpm(speed_wheel_t w)
{
    if (w >= SPEED_WHEEL_COUNT) return 0;
    return wheel[w].rpm;
}

uint16_t svc_speed_get_kmh_x10(void)
{
    return kmh_x10;
}

uint32_t svc_speed_get_odometer_m(void)
{
    return odometer_mm / 1000;
}

uint8_t svc_speed_is_stopped(void)
{
    for (uint8_t i = 0; i < SPEED_WHEEL_COUNT; i++) {
        if (wheel[i].state == SPEED_MOVING) return 0;
    }
    return 1;
}

void svc_speed_reset_odometer(void)
{
    odometer_mm         = 0;
    odometer_frac_x1000 = 0;
}

uint16_t svc_speed_get_glitch_count(speed_wheel_t w)
{
    if (w >= SPEED_WHEEL_COUNT) return 0;
    return hal_encoder_glitch_count(channel_of(w));
}
