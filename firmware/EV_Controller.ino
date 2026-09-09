/**
 * @file EV_Controller.ino
 * @brief Главный скетч контроллера детского электромобиля
 *
 * АРХИТЕКТУРА УПРАВЛЕНИЯ (v2 — layered pedals):
 *
 *   physical_gas (ADC) ─┐
 *                       ├─> combinator() ─> effective_gas ──┐
 *   uart_gas (cmd)     ─┘                                    │
 *                                                            ├─> ramp ─> motor
 *   physical_brake (ADC)─┐                                   │
 *                        ├─> combinator() ─> effective_brake┘
 *   uart_brake (cmd)    ─┘
 *
 * Никакой бинарной "control_source" — физические педали ВСЕГДА live.
 * UART просто накладывается сверху через комбинатор (по умолчанию MAX).
 * Watchdog svc_pedals: если UART-команд нет uart_pedal_timeout_ms — сброс в 0.
 *
 * Задачи планировщика:
 *   10  мс (100 Гц): педали + протокол + рампа + моторы
 *   100 мс (10 Гц):  скорость колёс + бинарная телеметрия
 *   200 мс (5 Гц):   текстовая телеметрия (Serial Monitor)
 *
 * @version 2.0.0 (MVP-1 + Layered Pedals + Protocol)
 */

#include "cfg_board.h"
#include "hal_gpio.h"
#include "hal_adc.h"
#include "hal_pwm.h"
#include "hal_system.h"
#include "hal_eeprom.h"
#include "hal_uart.h"
#include "hal_encoder.h"
#include "cfg_settings.h"
#include "util_math.h"
#include "util_crc.h"
#include "svc_pedals.h"
#include "svc_ramp.h"
#include "svc_motor.h"
#include "svc_speed.h"
#include "app_main.h"
#include "app_debug.h"
#include "app_protocol.h"
#include "MICRO_UART.h"
#include "PRINT.h"

#define CONTROL_FREQ_HZ     100
#define CONTROL_INTERVAL_MS 10
#define SPEED_FREQ_HZ       10
#define SPEED_INTERVAL_MS   100
#define DEBUG_INTERVAL_MS   200

static const drive_profile_t *current_profile = NULL;

/* ====================================================================
 *  Задача: цикл управления (100 Гц)
 * ==================================================================== */

static void task_control(void)
{
    /* 1. Парсинг входящих UART-пакетов (неблокирующий)
     *    Команды SET_GAS_VIRTUAL / SET_BRAKE_VIRTUAL направляются в svc_pedals.
     *    Watchdog svc_pedals сам сбросит виртуальные педали при отсутствии команд. */
    app_protocol_update();

    /* 2. Чтение и обработка педалей (АЦП + EMA + калибровка + кривая + watchdog) */
    svc_pedals_update();

    /* 3. Эффективные значения после комбинатора (physical + uart) */
    uint16_t target = svc_pedals_get_target_pwm(
        current_profile->max_pwm,
        cfg_settings_get()->motor_deadzone);
    uint16_t brake = svc_pedals_get_brake();

    /* 4. Рампа: плавное изменение PWM */
    uint16_t pwm = svc_ramp_update(target, brake, current_profile);

    /* 5. Вывод на моторы */
    svc_motor_set_pwm(pwm);
}

/* ====================================================================
 *  Задача: скорость + бинарная телеметрия (10 Гц)
 * ==================================================================== */

static void task_speed_telemetry(void)
{
    /* 1. Обновить расчёт скорости из энкодеров */
    svc_speed_update(SPEED_FREQ_HZ);

    /* 2. Собрать пакет телеметрии (v2 — расширенный) */
    telemetry_packet_t t;

    /* Педали — раздельно по источникам */
    t.gas_physical   = svc_pedals_get_gas_physical();
    t.gas_uart       = svc_pedals_get_gas_uart();
    t.gas_effective  = svc_pedals_get_gas();
    t.brake_physical = svc_pedals_get_brake_physical();
    t.brake_uart     = svc_pedals_get_brake_uart();
    t.brake_effective= svc_pedals_get_brake();

    /* PWM */
    t.target_pwm   = svc_pedals_get_target_pwm(
        current_profile->max_pwm,
        cfg_settings_get()->motor_deadzone);
    t.current_pwm  = svc_ramp_get_current();
    t.pwm_freq_hz  = svc_motor_get_frequency();

    /* Скорость */
    t.speed_rpm_l   = svc_speed_get_rpm(SPEED_WHEEL_LEFT);
    t.speed_rpm_r   = svc_speed_get_rpm(SPEED_WHEEL_RIGHT);
    t.speed_kmh_x10 = svc_speed_get_kmh_x10();

    /* Токи — заглушка до MVP-3 */
    t.current_ma_l = 0;
    t.current_ma_r = 0;

    /* Статус */
    t.drive_mode        = DRIVE_MODE_ECO;  /* TODO: svc_drive_mode в MVP-3 */
    t.uart_active_flags = svc_pedals_get_uart_active_flags();
    t.faults            = 0;                /* TODO: svc_safety в MVP-3 */
    t.uptime_ms         = hal_system_millis();

    app_protocol_send_telemetry(&t);
}

/* ====================================================================
 *  Задача: текстовая телеметрия для Serial Monitor (5 Гц)
 *  Срабатывает реже, чтобы не мешать бинарной телеметрии.
 * ==================================================================== */

static void task_debug(void)
{
    app_debug_telemetry(
        svc_pedals_get_gas(),
        svc_pedals_get_brake(),
        svc_pedals_get_target_pwm(
            current_profile->max_pwm,
            cfg_settings_get()->motor_deadzone),
        svc_ramp_get_current(),
        svc_motor_get_frequency());
}

/* ====================================================================
 *  SETUP / LOOP
 * ==================================================================== */

void setup()
{
    hal_system_init();
    hal_uart_init(UART_BAUD_DEBUG);
    hal_adc_init();

    cfg_settings_init();
    current_profile = cfg_settings_get_profile(DRIVE_MODE_ECO);

    svc_motor_init();
    svc_pedals_init();
    svc_speed_init();  /* инициализирует hal_encoder ISR */
    svc_ramp_init(CONTROL_FREQ_HZ);
    app_protocol_init();

    app_scheduler_init();
    app_scheduler_add(task_control,         CONTROL_INTERVAL_MS);
    app_scheduler_add(task_speed_telemetry, SPEED_INTERVAL_MS);
    app_scheduler_add(task_debug,           DEBUG_INTERVAL_MS);

    app_debug_init();
    app_debug_msg_P(PSTR("Protocol v2: layered pedals, 9600 baud"));
}

void loop()
{
    app_scheduler_run();
}
