/* Hyundai Continental/VDO receiver; independent BSD-2-Clause implementation.
 * 52 us symbols, 01=0/10=1 Manchester, ten bytes, CRC8 poly07 initAA.
 * Protocol reference: rtl_433 tpms_hyundai_vdo.c. */
#include "hyundai_decoder.h"
#include <string.h>

void hyundai_decoder_reset(HyundaiDecoder* d) {
    memset(d, 0, sizeof(*d));
}

float hyundai_pressure_psi(uint8_t raw) {
    return raw * 1.375f / 6.894757293168f;
}

static uint8_t packet_crc(const uint8_t* data) {
    uint8_t crc = 0xaa;
    for(unsigned i = 0; i < 9; i++) {
        crc ^= data[i];
        for(unsigned bit = 0; bit < 8; bit++)
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0x07 : 0));
    }
    return crc;
}

bool hyundai_decoder_symbol(HyundaiDecoder* d, bool level, HyundaiReading* result) {
    /* Last 17 symbols of preamble 0x55555556 (or inverted polarity). */
    d->sync = ((d->sync << 1) | (uint32_t)level) & 0x1ffff;
    if(!d->active) {
        if(d->sync == 0x15556 || d->sync == (0x15556 ^ 0x1ffff)) {
            d->active = true;
            d->inverted = d->sync != 0x15556;
            d->symbols = 0;
            memset(d->raw, 0, sizeof(d->raw));
        }
        return false;
    }
    if((d->symbols & 1) == 0) {
        d->first = level;
    } else {
        if(d->first == level) {
            d->active = false;
            return false;
        }
        unsigned bit = d->symbols / 2;
        d->raw[bit / 8] |= (uint8_t)((d->first ^ d->inverted) << (7 - bit % 8));
    }
    if(++d->symbols != 160) return false;
    d->active = false;
    if(packet_crc(d->raw) != d->raw[9]) return false;
    result->id = ((uint32_t)d->raw[1] << 24) | ((uint32_t)d->raw[2] << 16) |
                 ((uint32_t)d->raw[3] << 8) | d->raw[4];
    result->pressure_raw = d->raw[6];
    result->temperature_c = (int16_t)d->raw[7] - 50;
    result->state = d->raw[0];
    result->flags = d->raw[5] >> 4;
    result->repeat = d->raw[5] & 15;
    result->unknown = d->raw[8];
    return true;
}

bool hyundai_decoder_pulse(
    HyundaiDecoder* d, bool level, uint32_t duration_us, HyundaiReading* result) {
    /* Quantize runs with half-symbol rounding. Long gaps and short glitches
     * reset reception instead of stitching unrelated bursts together. */
    if(duration_us < 26 || duration_us > 208) {
        hyundai_decoder_reset(d);
        return false;
    }
    unsigned count = (duration_us + 26) / 52;
    bool received = false;
    for(unsigned i = 0; i < count; i++)
        if(hyundai_decoder_symbol(d, level, result)) received = true;
    return received;
}
