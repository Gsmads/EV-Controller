/**
 * @file util_ring.cpp
 * @brief Реализация кольцевого буфера
 *
 * Обоснование решений — в шапке util_ring.h.
 */
#include "util_ring.h"

/** Следующий индекс по кольцу. */
static inline uint8_t next_index(uint8_t i, uint8_t size) {
    uint8_t n = (uint8_t)(i + 1);
    return (n == size) ? 0 : n;
}

void util_ring_init(util_ring_t *r, uint8_t *storage, uint8_t size) {
    r->buf  = storage;
    r->size = size;
    util_ring_reset(r);
}

void util_ring_reset(util_ring_t *r) {
    r->head    = 0;
    r->tail    = 0;
    r->dropped = 0;
}

uint8_t util_ring_count(const util_ring_t *r) {
    uint8_t head = r->head;
    uint8_t tail = r->tail;
    if (head >= tail) {
        return (uint8_t)(head - tail);
    }
    return (uint8_t)(r->size - (tail - head));
}

uint8_t util_ring_free(const util_ring_t *r) {
    return (uint8_t)(r->size - 1 - util_ring_count(r));
}

uint8_t util_ring_is_empty(const util_ring_t *r) {
    return (uint8_t)(r->head == r->tail);
}

uint8_t util_ring_put(util_ring_t *r, uint8_t b) {
    uint8_t next = next_index(r->head, r->size);
    if (next == r->tail) {
        return 0;               /* полно: голова догнала бы хвост */
    }
    r->buf[r->head] = b;
    r->head = next;             /* индекс двигается ПОСЛЕ записи байта */
    return 1;
}

uint8_t util_ring_put_all(util_ring_t *r, const uint8_t *data, uint8_t len) {
    if (util_ring_free(r) < len) {
        if (r->dropped != 0xFFFF) {
            r->dropped++;       /* счётчик замирает, а не обнуляется */
        }
        return 0;
    }
    for (uint8_t i = 0; i < len; i++) {
        util_ring_put(r, data[i]);
    }
    return 1;
}

int16_t util_ring_get(util_ring_t *r) {
    if (r->head == r->tail) {
        return -1;              /* B-2: пустота непредставима в данных */
    }
    uint8_t b = r->buf[r->tail];
    r->tail = next_index(r->tail, r->size);
    return (int16_t)b;
}

uint16_t util_ring_dropped(const util_ring_t *r) {
    return r->dropped;
}
