/**
 * @file test_util_cobs.c
 * @brief Юнит-тест COBS (ADR-0024)
 *
 * Главное свойство, ради которого COBS и берётся: в закодированных
 * данных не может быть байта 0x00. Оно проверяется не на отдельных
 * примерах, а перебором — на всех длинах до максимума кадра протокола
 * и на наборах, специально составленных из нулей.
 *
 * Эталонные кодировки взяты из определения COBS и посчитаны вручную,
 * а не сняты с этой же реализации.
 *
 * Компиляция: см. tests/Makefile
 */
#include <stdio.h>
#include <string.h>
#include "../firmware/util_cobs.h"

static int pass = 0, fail = 0;
#define ASSERT_EQ(a, b, msg) do { if ((a)==(b)) pass++; else { printf("  FAIL: %s (exp=%d got=%d)\n", msg, (int)(a), (int)(b)); fail++; } } while(0)
#define ASSERT_TRUE(c, msg)  do { if (c) pass++; else { printf("  FAIL: %s\n", msg); fail++; } } while(0)

/** Сравнить результат кодирования с эталоном, посчитанным вручную. */
static void expect_encoding(const uint8_t *src, uint8_t len,
                            const uint8_t *want, uint8_t want_len,
                            const char *msg)
{
    uint8_t got[300];
    int16_t n = util_cobs_encode(src, len, got, sizeof(got));
    if (n != (int16_t)want_len) {
        printf("  FAIL: %s (длина exp=%d got=%d)\n", msg, (int)want_len, (int)n);
        fail++;
        return;
    }
    if (memcmp(got, want, want_len) != 0) {
        printf("  FAIL: %s (байты не совпали)\n    ожидалось:", msg);
        for (uint8_t i = 0; i < want_len; i++) printf(" %02X", want[i]);
        printf("\n    получено: ");
        for (uint8_t i = 0; i < want_len; i++) printf(" %02X", got[i]);
        printf("\n");
        fail++;
        return;
    }
    pass++;
}

/* ====================================================================
 *  Эталонные кодировки из определения COBS
 * ==================================================================== */

void test_reference_encodings(void) {
    printf("--- test_reference_encodings ---\n");

    const uint8_t empty[1] = {0};
    const uint8_t want_empty[] = {0x01};
    expect_encoding(empty, 0, want_empty, 1, "пустой вход даёт один байт 01");

    const uint8_t one_zero[] = {0x00};
    const uint8_t want_one_zero[] = {0x01, 0x01};
    expect_encoding(one_zero, 1, want_one_zero, 2, "один ноль даёт 01 01");

    const uint8_t two_zeros[] = {0x00, 0x00};
    const uint8_t want_two_zeros[] = {0x01, 0x01, 0x01};
    expect_encoding(two_zeros, 2, want_two_zeros, 3, "два нуля дают 01 01 01");

    const uint8_t plain[] = {0x11, 0x22, 0x33};
    const uint8_t want_plain[] = {0x04, 0x11, 0x22, 0x33};
    expect_encoding(plain, 3, want_plain, 4, "без нулей — один код и данные");

    const uint8_t middle[] = {0x11, 0x00, 0x22};
    const uint8_t want_middle[] = {0x02, 0x11, 0x02, 0x22};
    expect_encoding(middle, 3, want_middle, 4, "ноль в середине разбивает на две группы");

    const uint8_t leading[] = {0x00, 0x11};
    const uint8_t want_leading[] = {0x01, 0x02, 0x11};
    expect_encoding(leading, 2, want_leading, 3, "ноль в начале");

    const uint8_t trailing[] = {0x11, 0x00};
    const uint8_t want_trailing[] = {0x02, 0x11, 0x01};
    expect_encoding(trailing, 2, want_trailing, 3, "ноль в конце даёт замыкающий 01");
}

void test_long_run_without_zeros(void) {
    printf("--- test_long_run_without_zeros ---\n");
    /* Ровно 254 ненулевых байта — граничный случай, где код достигает 0xFF
       и группа закрывается без продолжения. Ответ обязан быть
       FF плюс сами 254 байта, то есть 255 байт, без хвостового кода. */
    uint8_t src[254];
    for (int i = 0; i < 254; i++) src[i] = (uint8_t)(i + 1);  /* 1..254, нулей нет */

    uint8_t got[300];
    int16_t n = util_cobs_encode(src, 254, got, sizeof(got));
    ASSERT_EQ(255, n, "254 ненулевых байта дают ровно 255 байт");
    ASSERT_EQ(0xFF, got[0], "первый байт — код 0xFF");
    ASSERT_EQ(1, memcmp(got + 1, src, 254) == 0, "дальше идут данные как есть");

    uint8_t back[300];
    int16_t m = util_cobs_decode(got, (uint8_t)n, back, sizeof(back));
    ASSERT_EQ(254, m, "раскодировалось обратно в 254 байта");
    ASSERT_EQ(1, memcmp(back, src, 254) == 0, "и байты те же");
}

/* ====================================================================
 *  Главное свойство: нуля в выходе не бывает
 * ==================================================================== */

/** Детерминированный генератор, чтобы тест повторялся байт в байт. */
static uint32_t lcg_state;
static uint8_t lcg_byte(void) {
    lcg_state = lcg_state * 1103515245UL + 12345UL;
    return (uint8_t)(lcg_state >> 16);
}

void test_encoded_never_contains_zero(void) {
    printf("--- test_encoded_never_contains_zero ---\n");
    /* Перебор по всем длинам кадра протокола и по четырём видам
       содержимого: сплошные нули, нулей нет, вперемешку, случайные. */
    uint8_t src[64], enc[80];
    int zero_found = 0, encoded_ok = 0;

    for (int pattern = 0; pattern < 4; pattern++) {
        lcg_state = 12345;
        for (uint8_t len = 0; len <= 63; len++) {
            for (uint8_t i = 0; i < len; i++) {
                switch (pattern) {
                    case 0: src[i] = 0x00; break;
                    case 1: src[i] = (uint8_t)(i + 1); break;
                    case 2: src[i] = (uint8_t)((i % 3 == 0) ? 0x00 : i + 1); break;
                    default: src[i] = lcg_byte(); break;
                }
            }
            int16_t n = util_cobs_encode(src, len, enc, sizeof(enc));
            if (n < 0) { printf("  FAIL: кодирование не удалось, len=%d\n", len); fail++; return; }
            encoded_ok++;
            for (int16_t i = 0; i < n; i++) {
                if (enc[i] == 0x00) zero_found++;
            }
        }
    }
    ASSERT_EQ(256, encoded_ok, "перебрано 4 набора по 64 длины");
    ASSERT_EQ(0, zero_found, "ГЛАВНОЕ: ни одного нулевого байта в выходе за весь перебор");
}

void test_roundtrip_over_all_lengths(void) {
    printf("--- test_roundtrip_over_all_lengths ---\n");
    uint8_t src[64], enc[80], back[80];
    int bad = 0, checked = 0;

    for (int pattern = 0; pattern < 4; pattern++) {
        lcg_state = 999;
        for (uint8_t len = 0; len <= 63; len++) {
            for (uint8_t i = 0; i < len; i++) {
                switch (pattern) {
                    case 0: src[i] = 0x00; break;
                    case 1: src[i] = 0xFF; break;
                    case 2: src[i] = (uint8_t)((i % 5 == 0) ? 0x00 : 0xAA); break;
                    default: src[i] = lcg_byte(); break;
                }
            }
            int16_t n = util_cobs_encode(src, len, enc, sizeof(enc));
            int16_t m = util_cobs_decode(enc, (uint8_t)n, back, sizeof(back));
            checked++;
            if (m != (int16_t)len || (len > 0 && memcmp(src, back, len) != 0)) bad++;
        }
    }
    ASSERT_EQ(256, checked, "перебрано 4 набора по 64 длины");
    ASSERT_EQ(0, bad, "каждый вход вернулся из кодирования без изменений");
}

void test_overhead_is_one_byte_for_protocol_frames(void) {
    printf("--- test_overhead_is_one_byte_for_protocol_frames ---\n");
    /* Обещание ADR-0024: на кадре протокола прибавка ровно один байт.
       Проверяем на максимальном кадре: 1 (cmd) + 60 (payload) + 2 (CRC). */
    uint8_t src[63], enc[80];
    for (int i = 0; i < 63; i++) src[i] = (uint8_t)(i + 1);
    int16_t n = util_cobs_encode(src, 63, enc, sizeof(enc));
    ASSERT_EQ(64, n, "63 байта без нулей дают 64");

    memset(src, 0, sizeof(src));
    n = util_cobs_encode(src, 63, enc, sizeof(enc));
    ASSERT_EQ(64, n, "63 нулевых байта дают те же 64 — прибавка не зависит от данных");
}

/* ====================================================================
 *  Отказы: раскодировать мусор нельзя
 * ==================================================================== */

void test_decode_rejects_zero_inside(void) {
    printf("--- test_decode_rejects_zero_inside ---\n");
    const uint8_t bad[] = {0x03, 0x11, 0x00};
    uint8_t out[16];
    ASSERT_EQ(-1, util_cobs_decode(bad, 3, out, sizeof(out)),
              "ноль внутри кадра означает, что это не кадр");
}

void test_decode_rejects_truncated_group(void) {
    printf("--- test_decode_rejects_truncated_group ---\n");
    /* Код обещает три байта данных, а пришёл один */
    const uint8_t bad[] = {0x04, 0x11};
    uint8_t out[16];
    ASSERT_EQ(-1, util_cobs_decode(bad, 2, out, sizeof(out)),
              "оборванная группа отвергается, а не дочитывается мусором");
}

void test_decode_rejects_leading_zero_code(void) {
    printf("--- test_decode_rejects_leading_zero_code ---\n");
    const uint8_t bad[] = {0x00};
    uint8_t out[16];
    ASSERT_EQ(-1, util_cobs_decode(bad, 1, out, sizeof(out)), "нулевой код невозможен");
}

void test_capacity_is_respected(void) {
    printf("--- test_capacity_is_respected ---\n");
    const uint8_t src[] = {0x11, 0x22, 0x33};
    uint8_t small[3];
    ASSERT_EQ(-1, util_cobs_encode(src, 3, small, 3),
              "кодирование не вылезает за ёмкость, а отказывается");

    const uint8_t enc[] = {0x04, 0x11, 0x22, 0x33};
    uint8_t tiny[2];
    ASSERT_EQ(-1, util_cobs_decode(enc, 4, tiny, 2),
              "раскодирование тоже не вылезает");
}

void test_null_arguments(void) {
    printf("--- test_null_arguments ---\n");
    uint8_t buf[8];
    const uint8_t src[] = {0x11};
    ASSERT_EQ(-1, util_cobs_encode(0, 1, buf, sizeof(buf)), "нет источника");
    ASSERT_EQ(-1, util_cobs_encode(src, 1, 0, 8), "нет приёмника");
    ASSERT_EQ(-1, util_cobs_encode(src, 1, buf, 0), "нулевая ёмкость");
    ASSERT_EQ(-1, util_cobs_decode(0, 1, buf, sizeof(buf)), "нет источника при разборе");
    ASSERT_EQ(-1, util_cobs_decode(src, 1, 0, 8), "нет приёмника при разборе");
}

void test_decode_of_empty_input(void) {
    printf("--- test_decode_of_empty_input ---\n");
    uint8_t out[8];
    ASSERT_EQ(0, util_cobs_decode(out, 0, out, sizeof(out)),
              "пустой вход даёт пустой выход, а не отказ");
}

void test_decode_in_place(void) {
    printf("--- test_decode_in_place ---\n");
    /* Протокол разбирает кадр прямо в приёмном буфере, экономя 63 байта
       ОЗУ. Это возможно потому, что выход COBS всегда короче входа, а
       запись всегда попадает в уже прочитанный байт. Свойство неочевидное,
       поэтому закреплено тестом на всех длинах кадра. */
    uint8_t src[64], enc[80], scratch[80];
    int bad = 0, checked = 0;

    for (int pattern = 0; pattern < 4; pattern++) {
        lcg_state = 4242;
        for (uint8_t len = 0; len <= 63; len++) {
            for (uint8_t i = 0; i < len; i++) {
                switch (pattern) {
                    case 0: src[i] = 0x00; break;
                    case 1: src[i] = 0xFF; break;
                    case 2: src[i] = (uint8_t)((i % 4 == 0) ? 0x00 : 0x5A); break;
                    default: src[i] = lcg_byte(); break;
                }
            }
            int16_t n = util_cobs_encode(src, len, enc, sizeof(enc));
            memcpy(scratch, enc, (size_t)n);

            /* Разбор на месте: приёмник и источник — один буфер */
            int16_t m = util_cobs_decode(scratch, (uint8_t)n, scratch, sizeof(scratch));
            checked++;
            if (m != (int16_t)len || (len > 0 && memcmp(src, scratch, len) != 0)) bad++;
        }
    }
    ASSERT_EQ(256, checked, "перебрано 4 набора по 64 длины");
    ASSERT_EQ(0, bad, "разбор на месте даёт тот же результат, что и в отдельный буфер");
}

int main(void) {
    printf("=========================================\n");
    printf("  util_cobs Unit Tests (ADR-0024)\n");
    printf("=========================================\n\n");
    test_reference_encodings();
    test_long_run_without_zeros();
    test_encoded_never_contains_zero();
    test_roundtrip_over_all_lengths();
    test_overhead_is_one_byte_for_protocol_frames();
    test_decode_rejects_zero_inside();
    test_decode_rejects_truncated_group();
    test_decode_rejects_leading_zero_code();
    test_capacity_is_respected();
    test_null_arguments();
    test_decode_of_empty_input();
    test_decode_in_place();

    printf("\n=========================================\n");
    printf("  Results: %d passed, %d failed\n", pass, fail);
    printf("=========================================\n");
    return fail > 0 ? 1 : 0;
}
