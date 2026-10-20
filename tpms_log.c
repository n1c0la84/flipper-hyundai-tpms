#include "tpms_log.h"
#include "radio_preset.h"
#include <furi_hal.h>
#include <stdio.h>
#include <stdlib.h>
#include <storage/storage.h>
#include <string.h>
#define DIRECTORY EXT_PATH("apps_data/hyundai_tpms")
struct TpmsLog {
    Storage *storage;
    FuriThread *thread;
    FuriMessageQueue *queue;
    FuriMutex *mutex;
    TpmsHistory history;
    TpmsConfig config;
    TpmsLogStatus status;
};
static void error(TpmsLog *l) {
    furi_mutex_acquire(l->mutex, FuriWaitForever);
    l->status.errors++;
    furi_mutex_release(l->mutex);
}
static bool write_text(TpmsLog *l, File *f, const char *text) {
    size_t n = strlen(text);
    bool ok = storage_file_write(f, text, n) == n;
    if(!ok)
        error(l);
    return ok;
}
static bool checkpoint(TpmsLog *l) {
    uint8_t bytes[TPMS_HISTORY_BYTES];
    char path[100];
    l->history.generation++;
    snprintf(path, sizeof(path), DIRECTORY "/state%lu.bin",
             (unsigned long)(l->history.generation & 1));
    size_t n = tpms_history_encode(&l->history, bytes);
    File *f = storage_file_alloc(l->storage);
    bool ok = storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok)
        ok = storage_file_write(f, bytes, n) == n && storage_file_sync(f);
    bool closed = storage_file_close(f);
    storage_file_free(f);
    ok = ok && closed;
    furi_mutex_acquire(l->mutex, FuriWaitForever);
    l->status.state_ok = ok;
    if(!ok)
        l->status.errors++;
    furi_mutex_release(l->mutex);
    return ok;
}
static void restore(TpmsLog *l) {
    for(unsigned slot = 0; slot < 2; slot++) {
        char path[100];
        uint8_t bytes[TPMS_HISTORY_BYTES];
        TpmsHistory h;
        snprintf(path, sizeof(path), DIRECTORY "/state%u.bin", slot);
        File *f = storage_file_alloc(l->storage);
        bool ok = storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING);
        if(ok) {
            uint64_t n = storage_file_size(f);
            ok = n <= sizeof(bytes) && storage_file_read(f, bytes, (size_t)n) == n &&
                 tpms_history_decode(&h, bytes, (size_t)n);
        }
        storage_file_close(f);
        storage_file_free(f);
        if(ok && (!l->history.count || (int32_t)(h.generation - l->history.generation) > 0))
            l->history = h;
    }
}
static bool save_config(TpmsLog *l, const TpmsConfig *config) {
    uint8_t bytes[TPMS_CONFIG_BYTES];
    char path[100];
    tpms_config_encode(config, bytes);
    snprintf(path, sizeof(path), DIRECTORY "/config%lu.bin",
             (unsigned long)(config->generation & 1));
    File *f = storage_file_alloc(l->storage);
    bool ok = storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok)
        ok = storage_file_write(f, bytes, sizeof(bytes)) == sizeof(bytes) && storage_file_sync(f);
    bool closed = storage_file_close(f);
    storage_file_free(f);
    ok = ok && closed;
    furi_mutex_acquire(l->mutex, FuriWaitForever);
    l->status.config_ok = ok;
    l->status.config_generation = config->generation;
    if(!ok)
        l->status.errors++;
    furi_mutex_release(l->mutex);
    if(ok)
        l->config = *config;
    return ok;
}
static void restore_config(TpmsLog *l) {
    tpms_config_default(&l->config);
    bool found = false;
    for(unsigned slot = 0; slot < 2; slot++) {
        char path[100];
        uint8_t bytes[TPMS_CONFIG_BYTES];
        TpmsConfig config;
        snprintf(path, sizeof(path), DIRECTORY "/config%u.bin", slot);
        File *f = storage_file_alloc(l->storage);
        bool ok = storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING);
        if(ok)
            ok = storage_file_size(f) == sizeof(bytes) &&
                 storage_file_read(f, bytes, sizeof(bytes)) == sizeof(bytes) &&
                 tpms_config_decode(&config, bytes, sizeof(bytes));
        storage_file_close(f);
        storage_file_free(f);
        if(ok && (!found || (int32_t)(config.generation - l->config.generation) > 0)) {
            l->config = config;
            found = true;
        }
    }
    l->status.config_ok = found;
    l->status.config_generation = l->config.generation;
}
static int32_t worker(void *context) {
    TpmsLog *l = context;
    char directory[140];
    File *packets = storage_file_alloc(l->storage);
    File *frames = storage_file_alloc(l->storage);
    File *diagnostics = storage_file_alloc(l->storage);
    storage_common_mkdir(l->storage, EXT_PATH("apps_data"));
    bool ok = storage_common_mkdir(l->storage, DIRECTORY) == FSE_OK ||
              storage_common_exists(l->storage, DIRECTORY);
    if(!l->status.config_ok)
        save_config(l, &l->config);
    uint32_t ts = furi_hal_rtc_get_timestamp();
    if(ok) {
        ok = false;
        for(unsigned i = 0; i < 1000; i++) {
            snprintf(directory, sizeof(directory), DIRECTORY "/run-%lu-%03u", (unsigned long)ts, i);
            if(storage_common_mkdir(l->storage, directory) == FSE_OK) {
                ok = true;
                break;
            }
        }
    }
    char path[180];
    if(ok) {
        snprintf(path, sizeof(path), "%s/packets.csv", directory);
        ok = storage_file_open(packets, path, FSAM_WRITE, FSOM_CREATE_NEW);
    }
    if(ok) {
        snprintf(path, sizeof(path), "%s/frames.csv", directory);
        ok = storage_file_open(frames, path, FSAM_WRITE, FSOM_CREATE_NEW);
    }
    if(ok) {
        snprintf(path, sizeof(path), "%s/diagnostics.csv", directory);
        ok = storage_file_open(diagnostics, path, FSAM_WRITE, FSOM_CREATE_NEW);
    }
    if(ok) {
        ok = write_text(l, packets,
                        "rtc_timestamp,uptime_ms,sensor_id,pressure_raw,pressure_psi,temperature_c,"
                        "state,flags,repeat,unknown,session_sensor_frames,total_sensor_frames,"
                        "frame_rssi_dbm,raw_hex,wheel_position,assignment_generation\n") &&
             write_text(l, frames,
                        "rtc_timestamp,uptime_ms,sequence,result,symbols,rssi_dbm,raw_hex,pulse_"
                        "count,truncated,pulses\n") &&
             write_text(
                 l, diagnostics,
                 "rtc_timestamp,uptime_ms,valid_frames,processed_edges_per_s,syncs,crc_errors,"
                 "manchester_errors,dropped_edges,rssi_last_dbm,rssi_min_dbm,rssi_max_dbm,rssi_"
                 "mean_dbm,log_queue_drops,storage_errors,frame_rows,diagnostic_rows\n");
        File *metadata = storage_file_alloc(l->storage);
        snprintf(path, sizeof(path), "%s/metadata.txt", directory);
        if(storage_file_open(metadata, path, FSAM_WRITE, FSOM_CREATE_NEW)) {
            write_text(
                l, metadata,
                "App: Hyundai TPMS v0.6\nFrequency: 433920000 Hz\nSymbol duration: 52 us\nCRC8: "
                "polynomial 07, initial AA\nCache expiry: 1800 seconds\nRTC timestamps use the "
                "Flipper clock; timezone/clock accuracy are not verified.\nFrame RSSI is sampled "
                "after decoding, not at the RF peak. Diagnostic RSSI is sampled four times per "
                "second.\nPulse traces include up to 24 preceding edges and are capped at 192 "
                "edges. These are preamble-triggered traces, not continuous IQ or all radio "
                "edges.\nPreset bytes: ");
            for(size_t i = 0; i < sizeof(hyundai_fsk_regs); i++) {
                char hex[4];
                snprintf(hex, sizeof(hex), "%02x", ((const uint8_t *)hyundai_fsk_regs)[i]);
                write_text(l, metadata, hex);
            }
            write_text(l, metadata, "\n");
            if(!storage_file_sync(metadata))
                error(l);
        } else
            error(l);
        storage_file_close(metadata);
        storage_file_free(metadata);
    }
    furi_mutex_acquire(l->mutex, FuriWaitForever);
    l->status.ready = ok;
    if(!ok)
        l->status.errors++;
    furi_mutex_release(l->mutex);
    TpmsLogEvent e;
    while(furi_message_queue_get(l->queue, &e, FuriWaitForever) == FuriStatusOk) {
        if(e.type == TpmsLogStop)
            break;
        if(e.type == TpmsLogConfig) {
            save_config(l, &e.config);
            continue;
        }
        char row[512];
        bool event_ok = ok;
        if(e.type == TpmsLogFrame) {
            char raw[21];
            for(unsigned i = 0; i < 10; i++)
                snprintf(raw + i * 2, 3, "%02x", e.raw[i]);
            if(e.result == TpmsFrameValid) {
                tpms_history_update(&l->history, &e.saved);
                if(ok) {
                    snprintf(row, sizeof(row),
                             "%lu,%lu,%08lx,%u,%.3f,%d,%u,%u,%u,%u,%lu,%lu,%.1f,%s,%s,%lu\n",
                             (unsigned long)e.timestamp, (unsigned long)e.uptime_ms,
                             (unsigned long)e.saved.reading.id, e.saved.reading.pressure_raw,
                             (double)hyundai_pressure_psi(e.saved.reading.pressure_raw),
                             e.saved.reading.temperature_c, e.saved.reading.state,
                             e.saved.reading.flags, e.saved.reading.repeat, e.saved.reading.unknown,
                             (unsigned long)e.session_frames, (unsigned long)e.saved.frames,
                             (double)e.rssi, raw, tpms_wheel_label(e.wheel_position),
                             (unsigned long)e.config.generation);
                    event_ok = write_text(l, packets, row);
                }
                checkpoint(l);
            }
            if(ok) {
                const char *names[] = {"valid", "crc_bad",       "manchester_bad",
                                       "gap",   "dropped_edges", "exit"};
                snprintf(row, sizeof(row), "%lu,%lu,%lu,%s,%u,%.1f,%s,%u,%u,",
                         (unsigned long)e.timestamp, (unsigned long)e.uptime_ms,
                         (unsigned long)e.sequence, names[e.result], e.symbols, (double)e.rssi, raw,
                         e.trace_count, e.truncated);
                event_ok = write_text(l, frames, row) && event_ok;
                size_t used = 0;
                for(unsigned i = 0; i < e.trace_count; i++) {
                    if(used > sizeof(row) - 24) {
                        row[used] = 0;
                        event_ok = write_text(l, frames, row) && event_ok;
                        used = 0;
                    }
                    used += (size_t)snprintf(row + used, sizeof(row) - used, "%c%lu%s",
                                             e.trace[i] >> 31 ? 'H' : 'L',
                                             (unsigned long)(e.trace[i] & 0x7fffffff),
                                             i + 1 == e.trace_count ? "" : ";");
                }
                row[used++] = '\n';
                row[used] = 0;
                event_ok = write_text(l, frames, row) && event_ok;
                furi_mutex_acquire(l->mutex, FuriWaitForever);
                l->status.frame_rows++;
                furi_mutex_release(l->mutex);
            }
        } else if(ok) {
            TpmsLogStatus st = tpms_log_status(l);
            snprintf(row, sizeof(row),
                     "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.1f,%.1f,%.1f,%.1f,%lu,%lu,%lu,%lu\n",
                     (unsigned long)e.timestamp, (unsigned long)e.uptime_ms,
                     (unsigned long)e.packets, (unsigned long)e.edges, (unsigned long)e.syncs,
                     (unsigned long)e.crc_errors, (unsigned long)e.manchester_errors,
                     (unsigned long)e.dropped, (double)e.rssi, (double)e.rssi_min,
                     (double)e.rssi_max, (double)e.rssi_mean, (unsigned long)st.dropped,
                     (unsigned long)st.errors, (unsigned long)st.frame_rows,
                     (unsigned long)st.diagnostic_rows);
            event_ok = write_text(l, diagnostics, row);
            furi_mutex_acquire(l->mutex, FuriWaitForever);
            l->status.diagnostic_rows++;
            furi_mutex_release(l->mutex);
        }
        if(ok && e.type == TpmsLogDiagnostic &&
           (!storage_file_sync(packets) || !storage_file_sync(frames) ||
            !storage_file_sync(diagnostics))) {
            error(l);
            event_ok = false;
        }
        if(ok && !event_ok) {
            ok = false;
            furi_mutex_acquire(l->mutex, FuriWaitForever);
            l->status.ready = false;
            furi_mutex_release(l->mutex);
        }
    }
    if(ok && (!storage_file_sync(packets) || !storage_file_sync(frames) ||
              !storage_file_sync(diagnostics)))
        error(l);
    storage_file_close(packets);
    storage_file_close(frames);
    storage_file_close(diagnostics);
    storage_file_free(packets);
    storage_file_free(frames);
    storage_file_free(diagnostics);
    return 0;
}
TpmsLog *tpms_log_alloc(TpmsHistory *restored, TpmsConfig *config) {
    TpmsLog *l = calloc(1, sizeof(TpmsLog));
    l->storage = furi_record_open(RECORD_STORAGE);
    l->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    l->queue = furi_message_queue_alloc(8, sizeof(TpmsLogEvent));
    restore(l);
    restore_config(l);
    *config = l->config;
    *restored = l->history;
    l->thread = furi_thread_alloc_ex("TpmsLog", 4096, worker, l);
    furi_thread_set_priority(l->thread, FuriThreadPriorityLow);
    furi_thread_start(l->thread);
    return l;
}
bool tpms_log_enqueue(TpmsLog *l, const TpmsLogEvent *e) {
    bool ok = furi_message_queue_put(l->queue, e, 0) == FuriStatusOk;
    if(!ok) {
        furi_mutex_acquire(l->mutex, FuriWaitForever);
        l->status.dropped++;
        furi_mutex_release(l->mutex);
    }
    return ok;
}
TpmsLogStatus tpms_log_status(TpmsLog *l) {
    furi_mutex_acquire(l->mutex, FuriWaitForever);
    TpmsLogStatus s = l->status;
    furi_mutex_release(l->mutex);
    return s;
}
void tpms_log_free(TpmsLog *l, const TpmsHistory *final, const TpmsConfig *config) {
    TpmsLogEvent e = {.type = TpmsLogStop};
    furi_message_queue_put(l->queue, &e, FuriWaitForever);
    furi_thread_join(l->thread);
    if(!l->status.config_ok || l->config.generation != config->generation)
        save_config(l, config);
    for(unsigned i = 0; i < final->count; i++)
        tpms_history_update(&l->history, &final->sensors[i]);
    if(l->history.count)
        checkpoint(l);
    furi_thread_free(l->thread);
    furi_message_queue_free(l->queue);
    furi_mutex_free(l->mutex);
    furi_record_close(RECORD_STORAGE);
    free(l);
}
