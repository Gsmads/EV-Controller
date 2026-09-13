/**
 * @file svc_speed.cpp
 * @brief Реализация расчёта скорости колёс
 *
 * Математика пересчёта:
 *
 *   RPM (обороты в минуту):
 *     pulses_per_second = pulses_per_tick * update_freq_hz
 *     rev_per_second    = pulses_per_second / PULSES_PER_REV
 *     RPM               = rev_per_second * 60
 *                       = pulses_per_tick * update_freq_hz * 60 / PULSES_PER_REV
 *
 *   Линейная скорость:
 *     wheel_circumference_mm = π × D_mm
 *     mm_per_revolution      = wheel_circumference_mm
 *     mm_per_second          = (RPM / 60) × wheel_circumference_mm
 *     km/h                   = mm_per_second × 3600 / 1000000
 *                            = mm_per_second × 0.0036
 *
 *   Для km/h × 10 (для отображения XX.X):
 *     km/h × 10 = RPM × 60 × π × D_mm × 10 / 1 000 000
 *               = RPM × D_mm × 1885 / 1 000 000
 *     где 1885 ≈ 60 × π × 10 = 1884,956 (ошибка коэффициента +0,0024 %)
 *
 * @version 1.0.0
 */
#include "svc_speed.h"
#include "hal_encoder.h"
#include "cfg_board.h"
#include "svc_motor.h"  /* для motor_id_t — но мы используем свой enum, не нужно */

/* ====================================================================
 *  Внутреннее состояние
 * ==================================================================== */

static uint16_t rpm[SPEED_WHEEL_COUNT];
static uint16_t kmh_x10;
static uint32_t pulses_total[SPEED_WHEEL_COUNT];  /* для одометра */
static uint32_t odometer_mm;                       /* пройдено мм всего */

/* ====================================================================
 *  Реализация
 * ==================================================================== */

void svc_speed_init(void)
{
    for (uint8_t i = 0; i < SPEED_WHEEL_COUNT; i++) {
        rpm[i] = 0;
        pulses_total[i] = 0;
    }
    kmh_x10 = 0;
    odometer_mm = 0;
    hal_encoder_init();
}

void svc_speed_update(uint8_t update_freq_hz)
{
    if (update_freq_hz == 0) return;  /* защита от деления на 0 */

    /* hal_encoder_read_and_reset — атомарное чтение+сброс счётчика */
    uint16_t pulses_r = hal_encoder_read_and_reset((encoder_channel_t)ENCODER_RIGHT);
    uint16_t pulses_l = hal_encoder_read_and_reset((encoder_channel_t)ENCODER_LEFT);

    pulses_total[SPEED_WHEEL_RIGHT] += pulses_r;
    pulses_total[SPEED_WHEEL_LEFT]  += pulses_l;

    /* RPM = pulses_per_tick × update_freq_hz × 60 / PULSES_PER_REV */
    const uint32_t pulses_per_rev = ENCODER_PULSES_PER_REV;
    rpm[SPEED_WHEEL_RIGHT] = (uint32_t)pulses_r * update_freq_hz * 60UL / pulses_per_rev;
    rpm[SPEED_WHEEL_LEFT]  = (uint32_t)pulses_l * update_freq_hz * 60UL / pulses_per_rev;

    /* Средняя скорость двух колёс (для отображения общей скорости машины).
     * При повороте они будут различаться — это нормально. */
    uint32_t avg_rpm = ((uint32_t)rpm[SPEED_WHEEL_LEFT] + rpm[SPEED_WHEEL_RIGHT]) / 2;

    /* Дефект B-3 docs/AUDIT.md. Здесь стояло
     *
     *     kmh_x10 = avg_rpm * WHEEL_DIAMETER_MM * 188UL / 360000UL;
     *
     * что занижало скорость в 3,65 раза: при D = 200 мм и 300 RPM формула
     * давала 31 вместо 113. Комментарий над ней содержал «проверку»,
     * сходившуюся с числом π и потому выглядевшую убедительно.
     *
     * Вывод от физики, а не от кода:
     *   длина окружности         = π × D мм
     *   мм за час                = RPM × 60 × π × D
     *   км/ч                     = RPM × 60 × π × D / 1 000 000
     *   км/ч × 10                = RPM × D × (60 × π × 10) / 1 000 000
     *                            = RPM × D × 1885 / 1 000 000
     *
     * Коэффициент 1885 против точного 1884,956 даёт +0,0024 % — на порядки
     * меньше погрешности самого измерения.
     *
     * Прибавка 500000 перед делением — округление к ближайшему вместо
     * усечения. Усечение занижает всегда, а спидометр, систематически
     * показывающий меньше реального, — не та ошибка, которую хочется
     * иметь в машине с ребёнком.
     *
     * Эталонные точки проверены в tests/test_svc_speed.c.
     */
    kmh_x10 = (uint16_t)((avg_rpm * WHEEL_DIAMETER_MM * 1885UL + 500000UL) / 1000000UL);

    /* Одометр: расстояние = pulses × π × D / pulses_per_rev */
    uint32_t mm_per_pulse_x1000 = (314UL * WHEEL_DIAMETER_MM * 10UL) / pulses_per_rev;
    /* Используем среднее по двум колёсам, делим в конце */
    odometer_mm += ((uint32_t)(pulses_r + pulses_l) * mm_per_pulse_x1000) / 2000;
}

uint16_t svc_speed_get_rpm(speed_wheel_t wheel)
{
    if (wheel >= SPEED_WHEEL_COUNT) return 0;
    return rpm[wheel];
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
    return (rpm[SPEED_WHEEL_LEFT] == 0 && rpm[SPEED_WHEEL_RIGHT] == 0) ? 1 : 0;
}

void svc_speed_reset_odometer(void)
{
    odometer_mm = 0;
}
