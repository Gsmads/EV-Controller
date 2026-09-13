/**
 * @file test_svc_pedals.c
 * @brief Юнит-тест педалей: слоистая модель и работа с сигналами
 *
 * Проверка ADR-0020 с десктопной стороны: сервис обращается к сигналам
 * по имени и не знает ни одного номера канала АЦП. Мок подменяет
 * hal_adc_read_signal и запоминает, что именно у него спрашивали.
 *
 * Компиляция: см. tests/Makefile
 */
#include <stdio.h>
#include <string.h>
#include "../firmware/svc_pedals.h"
#include "../firmware/hal_adc.h"
#include "../firmware/cfg_settings.h"

static int pass = 0, fail = 0;
#define ASSERT_EQ(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%d got=%d)\n", msg, (int)(a), (int)(b)); fail++; } } while(0)

/* ==== Моки HAL ==== */

static uint16_t signal_value[ANALOG_SIGNAL_COUNT];
static uint16_t signal_reads[ANALOG_SIGNAL_COUNT];
static uint8_t  channel_reads;            /* обращений к hal_adc_read по номеру */
static uint32_t mock_millis_value;

uint16_t hal_adc_read_signal(analog_signal_t signal) {
    if (signal >= ANALOG_SIGNAL_COUNT) return 0;
    signal_reads[signal]++;
    return signal_value[signal];
}
uint16_t hal_adc_read(uint8_t channel) {
    (void)channel;
    channel_reads++;                      /* сервис не должен сюда попадать */
    return 0;
}
void     hal_adc_init(void) {}
void     hal_adc_bind(analog_signal_t s, uint8_t c) { (void)s; (void)c; }
uint16_t hal_adc_unbound_count(void) { return 0; }
uint16_t hal_adc_bad_channel_count(void) { return 0; }
uint32_t hal_system_millis(void) { return mock_millis_value; }

/* Мок EEPROM — cfg_settings нужен для калибровки и комбинаторов */
static uint8_t mock_eeprom[1024];
void hal_eeprom_read(uint16_t addr, uint8_t *buf, uint16_t len) {
    if (addr + len <= sizeof(mock_eeprom)) memcpy(buf, mock_eeprom + addr, len);
}
void hal_eeprom_write(uint16_t addr, const uint8_t *buf, uint16_t len) {
    if (addr + len <= sizeof(mock_eeprom)) memcpy(mock_eeprom + addr, buf, len);
}
uint8_t hal_eeprom_read_byte(uint16_t addr) {
    return (addr < sizeof(mock_eeprom)) ? mock_eeprom[addr] : 0xFF;
}
void hal_eeprom_write_byte(uint16_t addr, uint8_t d) {
    if (addr < sizeof(mock_eeprom)) mock_eeprom[addr] = d;
}

static void setup_clean(void) {
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));
    memset(signal_value, 0, sizeof(signal_value));
    memset(signal_reads, 0, sizeof(signal_reads));
    channel_reads = 0;
    mock_millis_value = 1000;
    cfg_settings_init();
    svc_pedals_init();
}

/* ====================================================================
 *  ADR-0020: сервис называет сигналы, а не каналы
 * ==================================================================== */

void test_reads_by_signal_name(void) {
    printf("--- test_reads_by_signal_name ---\n");
    setup_clean();
    svc_pedals_update();

    ASSERT_EQ(1, signal_reads[ANALOG_PEDAL_GAS] > 0,
              "ADR-0020: газ прочитан как сигнал ANALOG_PEDAL_GAS");
    ASSERT_EQ(1, signal_reads[ANALOG_PEDAL_BRAKE] > 0,
              "ADR-0020: тормоз прочитан как сигнал ANALOG_PEDAL_BRAKE");
    ASSERT_EQ(0, channel_reads,
              "ADR-0020: сервис ни разу не обратился к каналу по номеру");
}

void test_does_not_touch_other_signals(void) {
    printf("--- test_does_not_touch_other_signals ---\n");
    setup_clean();
    svc_pedals_update();
    ASSERT_EQ(0, signal_reads[ANALOG_CURRENT_RIGHT], "ток правого мотора не трогается");
    ASSERT_EQ(0, signal_reads[ANALOG_CURRENT_LEFT],  "ток левого мотора не трогается");
    ASSERT_EQ(0, signal_reads[ANALOG_STEERING_POS],  "положение руля не трогается");
}

void test_signal_value_reaches_pedal(void) {
    printf("--- test_signal_value_reaches_pedal ---\n");
    setup_clean();
    /* Умолчания калибровки: pedal_gas_min = 10, pedal_gas_max = 1000 */
    signal_value[ANALOG_PEDAL_GAS] = 10;
    svc_pedals_update();
    ASSERT_EQ(10, svc_pedals_get_gas_raw(), "сырое значение сигнала дошло до педали");

    signal_value[ANALOG_PEDAL_GAS] = 500;
    svc_pedals_update();
    ASSERT_EQ(500, svc_pedals_get_gas_raw(), "и следующее тоже");
}

/* ====================================================================
 *  Слоистая модель: физическая педаль и виртуальная
 * ==================================================================== */

void test_physical_pedal_released(void) {
    printf("--- test_physical_pedal_released ---\n");
    setup_clean();
    signal_value[ANALOG_PEDAL_GAS] = 10;      /* минимум калибровки */
    for (int i = 0; i < 400; i++) svc_pedals_update();   /* сойтись фильтру */
    ASSERT_EQ(0, svc_pedals_get_gas(), "педаль в нижней точке — газа нет");
    ASSERT_EQ(0, svc_pedals_is_gas_active(), "признак нажатия снят");
}

void test_uart_pedal_adds_to_physical(void) {
    printf("--- test_uart_pedal_adds_to_physical ---\n");
    setup_clean();
    signal_value[ANALOG_PEDAL_GAS] = 10;      /* физическая отпущена */
    for (int i = 0; i < 400; i++) svc_pedals_update();
    ASSERT_EQ(0, svc_pedals_get_gas(), "до команды газа нет");

    svc_pedals_set_uart_gas(500);
    svc_pedals_update();
    ASSERT_EQ(500, svc_pedals_get_gas_uart(), "виртуальная педаль приняла значение");
    ASSERT_EQ(500, svc_pedals_get_gas(), "комбинатор max: газ пришёл по UART");
}

void test_uart_watchdog_releases_virtual_pedal(void) {
    printf("--- test_uart_watchdog_releases_virtual_pedal ---\n");
    setup_clean();
    signal_value[ANALOG_PEDAL_GAS] = 10;
    svc_pedals_set_uart_gas(800);
    svc_pedals_update();
    ASSERT_EQ(800, svc_pedals_get_gas_uart(), "виртуальный газ задан");

    /* Умолчание uart_pedal_timeout_ms = 200 мс */
    mock_millis_value += 1000;
    svc_pedals_update();
    ASSERT_EQ(0, svc_pedals_get_gas_uart(),
              "watchdog: команды перестали приходить — виртуальная педаль отпущена");
}

void test_release_uart(void) {
    printf("--- test_release_uart ---\n");
    setup_clean();
    svc_pedals_set_uart_gas(600);
    svc_pedals_set_uart_brake(700);
    svc_pedals_update();
    svc_pedals_release_uart();
    svc_pedals_update();
    ASSERT_EQ(0, svc_pedals_get_gas_uart(),   "явное освобождение обнуляет газ");
    ASSERT_EQ(0, svc_pedals_get_brake_uart(), "и тормоз");
}

int main(void) {
    printf("=========================================\n");
    printf("  svc_pedals Unit Tests\n");
    printf("=========================================\n\n");

    test_reads_by_signal_name();
    test_does_not_touch_other_signals();
    test_signal_value_reaches_pedal();
    test_physical_pedal_released();
    test_uart_pedal_adds_to_physical();
    test_uart_watchdog_releases_virtual_pedal();
    test_release_uart();

    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
