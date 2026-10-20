#include "tpms_history.h"
#include <string.h>
static void put32(uint8_t *p, uint32_t v) {
    for(unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for(unsigned i = 0; i < 4; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static uint32_t checksum(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffff;
    for(size_t i = 0; i < n; i++) {
        c ^= p[i];
        for(unsigned b = 0; b < 8; b++)
            c = (c >> 1) ^ ((c & 1) ? 0xedb88320 : 0);
    }
    return ~c;
}
bool tpms_cache_recent(uint32_t timestamp, uint32_t now) {
    return timestamp && now >= timestamp && now - timestamp < TPMS_CACHE_SECONDS;
}
void tpms_history_update(TpmsHistory *h, const TpmsSaved *entry) {
    unsigned i = 0;
    while(i < h->count && h->sensors[i].reading.id != entry->reading.id)
        i++;
    if(i == h->count) {
        if(h->count < TPMS_HISTORY_LIMIT)
            h->count++;
        else {
            i = 0;
            for(unsigned j = 1; j < h->count; j++)
                if(h->sensors[j].timestamp < h->sensors[i].timestamp)
                    i = j;
        }
    }
    h->sensors[i] = *entry;
}
size_t tpms_history_encode(const TpmsHistory *h, uint8_t *b) {
    if(h->count > TPMS_HISTORY_LIMIT)
        return 0;
    size_t n = 12 + h->count * 20;
    memset(b, 0, n + 4);
    memcpy(b, "HTP1", 4);
    put32(b + 4, h->generation);
    b[8] = (uint8_t)h->count;
    for(unsigned i = 0; i < h->count; i++) {
        const TpmsSaved *s = &h->sensors[i];
        uint8_t *p = b + 12 + i * 20;
        put32(p, s->reading.id);
        p[4] = s->reading.pressure_raw;
        p[5] = s->reading.state;
        p[6] = s->reading.flags;
        p[7] = s->reading.repeat;
        p[8] = s->reading.unknown;
        uint16_t t = (uint16_t)s->reading.temperature_c;
        p[10] = (uint8_t)t;
        p[11] = (uint8_t)(t >> 8);
        put32(p + 12, s->timestamp);
        put32(p + 16, s->frames);
    }
    put32(b + n, checksum(b, n));
    return n + 4;
}
bool tpms_history_decode(TpmsHistory *h, const uint8_t *b, size_t size) {
    if(size < 16 || memcmp(b, "HTP1", 4) || b[8] > TPMS_HISTORY_LIMIT)
        return false;
    size_t n = 12 + b[8] * 20;
    if(size != n + 4 || get32(b + n) != checksum(b, n))
        return false;
    TpmsHistory result = {.generation = get32(b + 4), .count = b[8]};
    for(unsigned i = 0; i < result.count; i++) {
        TpmsSaved *s = &result.sensors[i];
        const uint8_t *p = b + 12 + i * 20;
        s->reading.id = get32(p);
        s->reading.pressure_raw = p[4];
        s->reading.state = p[5];
        s->reading.flags = p[6];
        s->reading.repeat = p[7];
        s->reading.unknown = p[8];
        s->reading.temperature_c = (int16_t)((uint16_t)p[10] | ((uint16_t)p[11] << 8));
        s->timestamp = get32(p + 12);
        s->frames = get32(p + 16);
        if(s->reading.temperature_c < -50 || s->reading.temperature_c > 205 ||
           s->reading.flags > 15 || s->reading.repeat > 15 || !s->frames)
            return false;
        for(unsigned j = 0; j < i; j++)
            if(result.sensors[j].reading.id == s->reading.id)
                return false;
    }
    *h = result;
    return true;
}
