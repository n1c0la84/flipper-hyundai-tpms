#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define TPMS_WHEEL_COUNT 4
#define TPMS_CONFIG_BYTES 28
typedef struct {
    uint32_t generation;
    uint32_t ids[TPMS_WHEEL_COUNT]; /* FL, FR, RL, RR; zero = unassigned */
} TpmsConfig;
void tpms_config_default(TpmsConfig *config);
int tpms_config_position(const TpmsConfig *config, uint32_t id);
const char *tpms_wheel_label(int position);
bool tpms_config_assign(TpmsConfig *config, unsigned position, uint32_t id);
void tpms_config_encode(const TpmsConfig *config, uint8_t *bytes);
bool tpms_config_decode(TpmsConfig *config, const uint8_t *bytes, size_t size);
