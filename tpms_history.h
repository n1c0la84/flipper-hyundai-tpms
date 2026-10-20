#pragma once
#include "hyundai_decoder.h"
#include <stddef.h>
#define TPMS_HISTORY_LIMIT 16
#define TPMS_CACHE_SECONDS 1800u
#define TPMS_HISTORY_BYTES 336u
typedef struct {
    HyundaiReading reading;
    uint32_t timestamp, frames;
} TpmsSaved;
typedef struct {
    uint32_t generation;
    unsigned count;
    TpmsSaved sensors[TPMS_HISTORY_LIMIT];
} TpmsHistory;
bool tpms_cache_recent(uint32_t timestamp, uint32_t now);
void tpms_history_update(TpmsHistory *history, const TpmsSaved *entry);
size_t tpms_history_encode(const TpmsHistory *history, uint8_t *bytes);
bool tpms_history_decode(TpmsHistory *history, const uint8_t *bytes, size_t size);
