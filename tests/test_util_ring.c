/**
 * @file test_util_ring.c
 * @brief Юнит-тест кольцевого буфера util_ring
 *
 * Красный до ADR-0018: модуля util_ring нет, UART живёт в MICRO_UART.cpp,
 * который не собирается вне Arduino IDE.
 *
 * Воспроизводит дефекты B-1 и B-2 docs/AUDIT.md:
 *   B-1 — запись в полный буфер затирала неотправленные байты;
 *   B-2 — байт 0xFF был неотличим от признака "данных нет".
 *
 * Компиляция: см. tests/Makefile
 */
#include <stdio.h>
#include <string.h>
#include "../firmware/util_ring.h"

static int pass = 0, fail = 0;
#define ASSERT(cond, msg) do { if (cond) pass++; else { printf("  FAIL: %s\n", msg); fail++; } } while(0)
#define ASSERT_EQ(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%d got=%d)\n", msg, (int)(a), (int)(b)); fail++; } } while(0)

/* Ёмкость 8 полезных байт: хранилище на один больше. */
#define STORAGE 9
static uint8_t storage[STORAGE];
static util_ring_t ring;

static void setup(void) {
    memset(storage, 0, sizeof(storage));
    util_ring_init(&ring, storage, STORAGE);
}

/* ==== базовые ==== */

static void test_empty(void) {
    printf("--- test_empty ---\n");
    setup();
    ASSERT_EQ(1, util_ring_is_empty(&ring), "новое кольцо пусто");
    ASSERT_EQ(0, util_ring_count(&ring), "занято 0");
    ASSERT_EQ(STORAGE - 1, util_ring_free(&ring), "свободно size-1");
    ASSERT_EQ(-1, util_ring_get(&ring), "чтение из пустого даёт -1");
}

static void test_put_get(void) {
    printf("--- test_put_get ---\n");
    setup();
    ASSERT_EQ(1, util_ring_put(&ring, 0x41), "байт записан");
    ASSERT_EQ(1, util_ring_count(&ring), "занято 1");
    ASSERT_EQ(0, util_ring_is_empty(&ring), "не пусто");
    ASSERT_EQ(0x41, util_ring_get(&ring), "прочитан тот же байт");
    ASSERT_EQ(1, util_ring_is_empty(&ring), "снова пусто");
}

static void test_fifo_order(void) {
    printf("--- test_fifo_order ---\n");
    setup();
    for (uint8_t i = 1; i <= 5; i++) util_ring_put(&ring, i);
    for (uint8_t i = 1; i <= 5; i++)
        ASSERT_EQ(i, util_ring_get(&ring), "порядок FIFO сохранён");
}

/* ==== B-2: сентинел 0xFF ==== */

static void test_byte_ff_is_data(void) {
    printf("--- test_byte_ff_is_data ---\n");
    setup();
    util_ring_put(&ring, 0xFF);
    int16_t got = util_ring_get(&ring);
    ASSERT_EQ(0xFF, got, "B-2: 0xFF читается как данные, а не как 'пусто'");
    ASSERT(got != -1, "B-2: 0xFF отличим от признака 'данных нет'");
    ASSERT_EQ(-1, util_ring_get(&ring), "после него кольцо пусто");
}

static void test_all_256_values(void) {
    printf("--- test_all_256_values ---\n");
    /* Проверяем каждое из 256 значений по отдельности: ни одно не является
       для кольца особенным. Индексы кольца восьмибитные намеренно — на AVR
       чтение uint8_t атомарно, а uint16_t прерывание может разорвать, — и
       ёмкость больше 255 байт заводить нельзя. */
    setup();
    int bad = 0;
    for (int v = 0; v <= 255; v++) {
        if (!util_ring_put(&ring, (uint8_t)v)) { bad++; continue; }
        if (util_ring_get(&ring) != v) bad++;
    }
    ASSERT_EQ(0, bad, "все 256 значений байта проходят без искажения");
    ASSERT_EQ(1, util_ring_is_empty(&ring), "кольцо пусто после прогона");
}

/* ==== B-1: переполнение ==== */

static void test_fill_to_capacity(void) {
    printf("--- test_fill_to_capacity ---\n");
    setup();
    for (uint8_t i = 0; i < STORAGE - 1; i++)
        ASSERT_EQ(1, util_ring_put(&ring, (uint8_t)(0x10 + i)), "запись до ёмкости проходит");
    ASSERT_EQ(0, util_ring_free(&ring), "свободного места не осталось");
}

static void test_overflow_does_not_corrupt(void) {
    printf("--- test_overflow_does_not_corrupt ---\n");
    setup();
    for (uint8_t i = 0; i < STORAGE - 1; i++) util_ring_put(&ring, (uint8_t)(0x10 + i));

    ASSERT_EQ(0, util_ring_put(&ring, 0xEE), "B-1: запись в полное кольцо отвергнута");
    ASSERT_EQ(STORAGE - 1, util_ring_count(&ring), "B-1: занято не изменилось");

    int bad = 0;
    for (uint8_t i = 0; i < STORAGE - 1; i++)
        if (util_ring_get(&ring) != (int16_t)(0x10 + i)) bad++;
    ASSERT_EQ(0, bad, "B-1: ранее записанные байты не затёрты");
}

/* ==== ADR-0016: отбрасывание пакета целиком ==== */

static void test_put_all_fits(void) {
    printf("--- test_put_all_fits ---\n");
    setup();
    const uint8_t packet[4] = {1, 2, 3, 4};
    ASSERT_EQ(1, util_ring_put_all(&ring, packet, 4), "пакет помещается — записан");
    ASSERT_EQ(4, util_ring_count(&ring), "занято 4");
    ASSERT_EQ(0, util_ring_dropped(&ring), "потерь нет");
}

static void test_put_all_drops_whole_packet(void) {
    printf("--- test_put_all_drops_whole_packet ---\n");
    setup();
    /* Оставляем 3 свободных байта, пробуем записать 4. */
    for (uint8_t i = 0; i < STORAGE - 1 - 3; i++) util_ring_put(&ring, 0xA0);
    ASSERT_EQ(3, util_ring_free(&ring), "свободно ровно 3");

    const uint8_t packet[4] = {1, 2, 3, 4};
    ASSERT_EQ(0, util_ring_put_all(&ring, packet, 4), "ADR-0016: пакет не влезает — отвергнут");
    ASSERT_EQ(3, util_ring_free(&ring), "ADR-0016: не записан ни один байт пакета");
    ASSERT_EQ(1, util_ring_dropped(&ring), "ADR-0016: счётчик потерь вырос ровно на 1");
}

static void test_dropped_counter_accumulates(void) {
    printf("--- test_dropped_counter_accumulates ---\n");
    setup();
    const uint8_t packet[STORAGE] = {0};
    for (int i = 0; i < 3; i++) util_ring_put_all(&ring, packet, STORAGE);
    ASSERT_EQ(3, util_ring_dropped(&ring), "три отказа — счётчик 3");
    ASSERT_EQ(1, util_ring_is_empty(&ring), "кольцо так и осталось пустым");
}

static void test_put_all_exact_fit(void) {
    printf("--- test_put_all_exact_fit ---\n");
    setup();
    uint8_t packet[STORAGE - 1];
    for (uint8_t i = 0; i < STORAGE - 1; i++) packet[i] = (uint8_t)(0x50 + i);
    ASSERT_EQ(1, util_ring_put_all(&ring, packet, STORAGE - 1), "пакет ровно по размеру записан");
    ASSERT_EQ(0, util_ring_free(&ring), "свободного места не осталось");
    ASSERT_EQ(0, util_ring_dropped(&ring), "потерь нет");
}

/* ==== обёртывание индексов ==== */

static void test_wraparound(void) {
    printf("--- test_wraparound ---\n");
    setup();
    /* Прогоняем через кольцо втрое больше байт, чем его ёмкость. */
    int bad = 0;
    for (int i = 0; i < 3 * (STORAGE - 1); i++) {
        uint8_t v = (uint8_t)(i & 0xFF);
        if (!util_ring_put(&ring, v)) { bad++; continue; }
        if (util_ring_get(&ring) != v) bad++;
    }
    ASSERT_EQ(0, bad, "индексы обёртываются без потери данных");
    ASSERT_EQ(1, util_ring_is_empty(&ring), "кольцо пусто после прогона");
}

static void test_reset(void) {
    printf("--- test_reset ---\n");
    setup();
    for (uint8_t i = 0; i < 5; i++) util_ring_put(&ring, i);
    util_ring_reset(&ring);
    ASSERT_EQ(1, util_ring_is_empty(&ring), "после сброса пусто");
    ASSERT_EQ(-1, util_ring_get(&ring), "после сброса чтение даёт -1");
    ASSERT_EQ(STORAGE - 1, util_ring_free(&ring), "после сброса свободна вся ёмкость");
}

int main(void) {
    printf("=========================================\n");
    printf("  util_ring Unit Tests\n");
    printf("=========================================\n\n");

    test_empty();
    test_put_get();
    test_fifo_order();
    test_byte_ff_is_data();
    test_all_256_values();
    test_fill_to_capacity();
    test_overflow_does_not_corrupt();
    test_put_all_fits();
    test_put_all_drops_whole_packet();
    test_dropped_counter_accumulates();
    test_put_all_exact_fit();
    test_wraparound();
    test_reset();

    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
