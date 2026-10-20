#include "tpms_config.h"
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
void tpms_config_default(TpmsConfig *c) {
    *c = (TpmsConfig){.generation = 1, .ids = {0, 0, 0, 0}};
}
int tpms_config_position(const TpmsConfig *c, uint32_t id) {
    if(id)
        for(unsigned i = 0; i < TPMS_WHEEL_COUNT; i++)
            if(c->ids[i] == id)
                return (int)i;
    return -1;
}
const char *tpms_wheel_label(int position) {
    static const char *labels[] = {"Front L", "Front R", "Rear L", "Rear R"};
    return position >= 0 && position < TPMS_WHEEL_COUNT ? labels[position] : "Unassigned";
}
bool tpms_config_assign(TpmsConfig *c, unsigned position, uint32_t id) {
    if(position >= TPMS_WHEEL_COUNT || c->ids[position] == id)
        return false;
    int previous = tpms_config_position(c, id);
    if(previous >= 0)
        c->ids[previous] = c->ids[position];
    c->ids[position] = id;
    c->generation++;
    return true;
}
void tpms_config_encode(const TpmsConfig *c, uint8_t *b) {
    memcpy(b, "TPC1", 4);
    put32(b + 4, c->generation);
    for(unsigned i = 0; i < TPMS_WHEEL_COUNT; i++)
        put32(b + 8 + i * 4, c->ids[i]);
    put32(b + 24, checksum(b, 24));
}
bool tpms_config_decode(TpmsConfig *c, const uint8_t *b, size_t n) {
    if(n != TPMS_CONFIG_BYTES || memcmp(b, "TPC1", 4) || get32(b + 24) != checksum(b, 24))
        return false;
    TpmsConfig result = {.generation = get32(b + 4)};
    for(unsigned i = 0; i < TPMS_WHEEL_COUNT; i++) {
        result.ids[i] = get32(b + 8 + i * 4);
        if(result.ids[i])
            for(unsigned j = 0; j < i; j++)
                if(result.ids[j] == result.ids[i])
                    return false;
    }
    *c = result;
    return true;
}
