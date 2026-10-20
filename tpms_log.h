#pragma once
#include "tpms_config.h"
#include "tpms_history.h"
#include <furi.h>
#define TPMS_TRACE_LIMIT 192
typedef enum { TpmsLogDiagnostic, TpmsLogFrame, TpmsLogConfig, TpmsLogStop } TpmsLogType;
typedef enum {
    TpmsFrameValid,
    TpmsFrameCrc,
    TpmsFrameManchester,
    TpmsFrameGap,
    TpmsFrameDropped,
    TpmsFrameExit
} TpmsFrameResult;
typedef struct {
    TpmsLogType type;
    uint32_t timestamp, uptime_ms, sequence, session_frames;
    float rssi, rssi_min, rssi_max, rssi_mean;
    uint32_t packets, edges, syncs, crc_errors, manchester_errors, dropped;
    TpmsFrameResult result;
    TpmsSaved saved;
    TpmsConfig config;
    int8_t wheel_position;
    uint16_t symbols, trace_count;
    bool truncated;
    uint8_t raw[10];
    uint32_t trace[TPMS_TRACE_LIMIT]; /* high bit = high level; rest = duration us */
} TpmsLogEvent;
typedef struct TpmsLog TpmsLog;
typedef struct {
    uint32_t dropped, errors, frame_rows, diagnostic_rows;
    bool ready, state_ok, config_ok;
    uint32_t config_generation;
} TpmsLogStatus;
TpmsLog *tpms_log_alloc(TpmsHistory *restored, TpmsConfig *config);
bool tpms_log_enqueue(TpmsLog *log, const TpmsLogEvent *event);
TpmsLogStatus tpms_log_status(TpmsLog *log);
void tpms_log_free(TpmsLog *log, const TpmsHistory *final, const TpmsConfig *config);
