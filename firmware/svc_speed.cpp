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
 *     km/h × 10 = RPM × π × D_mm × 36 / 60000
 *               ≈ RPM × D_mm × 188 / 100000  (целочисленная аппроксимация)
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

    /* km/h × 10 ≈ RPM × D_mm × 188 / 100000
     * Точнее: km/h = RPM × π × D / 60000 (где D в мм)
     *         km/h × 10 = RPM × π × D × 10 / 60000
     *                   = RPM × D × 31.4159 / 60000
     *                   ≈ RPM × D × 188 / 360000  (с округлением)
     *
     * Для D=200мм, RPM=300: kmh_x10 = 300 × 200 × 188 / 360000 = 31.3 → 3.1 км/ч × 10 = 31
     *                       (проверка: 3.1 км/ч × 200мм * π × 60 / 1000 = 3.14 км/ч ✓)
     */
    kmh_x10 = (uint16_t)((avg_rpm * WHEEL_DIAMETER_MM * 188UL) / 360000UL);

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
