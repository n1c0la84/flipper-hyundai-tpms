#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    uint8_t pressure_raw;
    int16_t temperature_c;
    uint8_t state, flags, repeat, unknown;
} HyundaiReading;

typedef struct {
    uint32_t sync;
    uint16_t symbols;
    bool active, inverted, first;
    uint8_t raw[10];
} HyundaiDecoder;

void hyundai_decoder_reset(HyundaiDecoder* decoder);
bool hyundai_decoder_symbol(HyundaiDecoder* decoder, bool level, HyundaiReading* result);
bool hyundai_decoder_pulse(
    HyundaiDecoder* decoder, bool level, uint32_t duration_us, HyundaiReading* result);
float hyundai_pressure_psi(uint8_t raw);
