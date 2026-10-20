#include "hyundai_decoder.h"
#include "radio_preset.h"
#include "tpms_log.h"
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SENSOR_LIMIT 16
#define PAGE_SIZE 4

typedef struct {
    uint32_t sequence;
    uint32_t duration;
    bool level;
} Pulse;

typedef struct {
    uint32_t id;
    HyundaiReading reading;
    uint32_t last_seen;
    bool seen, cached;
    uint32_t frames, total_frames, last_timestamp;
} Sensor;

typedef struct {
    FuriMessageQueue *pulses;
    FuriMessageQueue *inputs;
    FuriMutex *mutex;
    ViewPort *viewport;
    NotificationApp *notification;
    Sensor sensors[SENSOR_LIMIT];
    unsigned sensor_count, page;
    TpmsConfig config;
    bool settings, choosing, config_enqueued;
    unsigned selected_wheel, choice;
    uint32_t sequence, packets, lost_pulses;
    uint32_t pulse_rate, syncs, crc_errors, manchester_errors;
    float rssi;
    bool help, statistics;
    TpmsLog *log;
} App;

static void on_pulse(bool level, uint32_t duration, void *context) {
    App *app = context;
    Pulse pulse = {.sequence = app->sequence++, .duration = duration, .level = level};
    /* Called from the radio ISR: no allocation, drawing or blocking here.
     * Missing sequence numbers let the consumer reset after queue overflow. */
    furi_message_queue_put(app->pulses, &pulse, 0);
}

static void on_input(InputEvent *event, void *context) {
    App *app = context;
    furi_message_queue_put(app->inputs, event, 0);
}

static unsigned other_indices(const TpmsConfig *config, const Sensor *sensors, unsigned count,
                              unsigned *indices) {
    unsigned n = 0;
    for(unsigned i = 0; i < count; i++)
        if(tpms_config_position(config, sensors[i].id) < 0)
            indices[n++] = i;
    return n;
}
static unsigned page_count(App *app) {
    unsigned indices[SENSOR_LIMIT];
    unsigned count = other_indices(&app->config, app->sensors, app->sensor_count, indices);
    return 1 + (count ? (count + PAGE_SIZE - 1) / PAGE_SIZE : 1);
}
static void draw_settings(Canvas *canvas, const TpmsConfig *config, const Sensor *sensors,
                          unsigned count, unsigned wheel, bool choosing, unsigned choice,
                          TpmsLogStatus logging) {
    char text[64];
    if(!choosing) {
        canvas_draw_str(canvas, 0, 9, "Wheel assignments");
        for(unsigned i = 0; i < TPMS_WHEEL_COUNT; i++) {
            snprintf(text, sizeof(text), "%c %s", i == wheel ? '>' : ' ', tpms_wheel_label(i));
            canvas_draw_str(canvas, 0, 21 + i * 10, text);
            if(config->ids[i])
                snprintf(text, sizeof(text), "%08lx", (unsigned long)config->ids[i]);
            else
                snprintf(text, sizeof(text), "--");
            canvas_draw_str(canvas, 65, 21 + i * 10, text);
        }
        const char *status = logging.config_generation != config->generation ? "Saving..."
                             : !logging.config_ok ? "Save failed: hold OK"
                                                  : "Saved | OK: edit";
        canvas_draw_str(canvas, 0, 63, status);
        return;
    }
    snprintf(text, sizeof(text), "%s: select sensor", tpms_wheel_label(wheel));
    canvas_draw_str(canvas, 0, 9, text);
    if(!choice || choice > count) {
        canvas_draw_str(canvas, 0, 25, "Unassigned");
        canvas_draw_str(canvas, 0, 40, "Clear this wheel position");
    } else {
        const Sensor *sensor = &sensors[choice - 1];
        snprintf(text, sizeof(text), "%08lx (%s)", (unsigned long)sensor->id,
                 tpms_wheel_label(tpms_config_position(config, sensor->id)));
        canvas_draw_str(canvas, 0, 23, text);
        uint32_t age = (furi_get_tick() - sensor->last_seen) / furi_ms_to_ticks(1000);
        if((sensor->seen || sensor->cached) && age < TPMS_CACHE_SECONDS) {
            snprintf(text, sizeof(text), "%.1f PSI | %d C",
                     (double)hyundai_pressure_psi(sensor->reading.pressure_raw),
                     sensor->reading.temperature_c);
            canvas_draw_str(canvas, 0, 36, text);
            snprintf(text, sizeof(text), "Last %lus ago%s", (unsigned long)age,
                     sensor->cached ? " (old)" : "");
            canvas_draw_str(canvas, 0, 48, text);
        } else
            canvas_draw_str(canvas, 0, 40, "No recent reading");
    }
    canvas_draw_str(canvas, 0, 63, "OK: apply  Back: cancel");
}

static void draw(Canvas *canvas, void *context) {
    App *app = context;
    Sensor sensors[SENSOR_LIMIT];
    unsigned count, page;
    bool help, statistics, settings, choosing;
    unsigned wheel, choice;
    TpmsConfig config;
    uint32_t packets, lost, pulse_rate, syncs, crc_errors, manchester_errors;
    float rssi;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    memcpy(sensors, app->sensors, sizeof(sensors));
    count = app->sensor_count;
    page = app->page;
    help = app->help;
    statistics = app->statistics;
    settings = app->settings;
    choosing = app->choosing;
    wheel = app->selected_wheel;
    choice = app->choice;
    config = app->config;
    packets = app->packets;
    lost = app->lost_pulses;
    pulse_rate = app->pulse_rate;
    syncs = app->syncs;
    crc_errors = app->crc_errors;
    manchester_errors = app->manchester_errors;
    rssi = app->rssi;
    furi_mutex_release(app->mutex);
    TpmsLogStatus logging = tpms_log_status(app->log);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    char text[64];
    if(settings) {
        draw_settings(canvas, &config, sensors, count, wheel, choosing, choice, logging);
        return;
    }
    if(help) {
        canvas_draw_str(canvas, 0, 9, "433.92 MHz | TPMS v0.6");
        snprintf(text, sizeof(text), "RX %.0f dBm | %lu/s", (double)rssi,
                 (unsigned long)pulse_rate);
        canvas_draw_str(canvas, 0, 20, text);
        snprintf(text, sizeof(text), "Sync %lu | valid %lu", (unsigned long)syncs,
                 (unsigned long)packets);
        canvas_draw_str(canvas, 0, 30, text);
        snprintf(text, sizeof(text), "CRC bad %lu | Man bad %lu", (unsigned long)crc_errors,
                 (unsigned long)manchester_errors);
        canvas_draw_str(canvas, 0, 40, text);
        snprintf(text, sizeof(text), "Drop %lu | log lost %lu", (unsigned long)lost,
                 (unsigned long)logging.dropped);
        canvas_draw_str(canvas, 0, 50, text);
        snprintf(text, sizeof(text), "SD %s | errors %lu", logging.ready ? "logging" : "ERROR",
                 (unsigned long)logging.errors);
        canvas_draw_str(canvas, 0, 62, text);
        return;
    }
    unsigned indices[SENSOR_LIMIT];
    unsigned others = other_indices(&config, sensors, count, indices);
    unsigned pages = 1 + (others ? (others + PAGE_SIZE - 1) / PAGE_SIZE : 1);
    canvas_draw_str(canvas, 0, 8, page ? "Other sensors" : "Wheels");
    snprintf(text, sizeof(text), "%u/%u", page + 1, pages);
    canvas_draw_str(canvas, 107, 8, text);
    canvas_draw_str(canvas, 0, 18, statistics || page ? "Sensor ID" : "Wheel");
    canvas_draw_str(canvas, 53, 18, statistics ? "Count" : "PSI");
    canvas_draw_str(canvas, 84, 18, statistics ? "Last" : "C");
    if(!statistics)
        canvas_draw_str(canvas, 103, 18, "Age");
    if(page && !others) {
        canvas_draw_str(canvas, 0, 33, "No other sensors yet");
        canvas_draw_str(canvas, 0, 45, "Valid new IDs appear here");
    }
    uint32_t now = furi_get_tick();
    unsigned rows =
        page ? (others > (page - 1) * PAGE_SIZE ? others - (page - 1) * PAGE_SIZE : 0) : PAGE_SIZE;
    if(rows > PAGE_SIZE)
        rows = PAGE_SIZE;
    for(unsigned row = 0; row < rows; row++) {
        Sensor missing = {0};
        Sensor *s = &missing;
        if(page)
            s = &sensors[indices[(page - 1) * PAGE_SIZE + row]];
        else {
            missing.id = config.ids[row];
            for(unsigned i = 0; i < count; i++)
                if(missing.id && sensors[i].id == missing.id) {
                    s = &sensors[i];
                    break;
                }
        }
        int y = 28 + row * 9;
        if(!statistics && !page)
            canvas_draw_str(canvas, 0, y, tpms_wheel_label(row));
        else {
            if(s->id)
                snprintf(text, sizeof(text), "%08lx", (unsigned long)s->id);
            else
                snprintf(text, sizeof(text), "--");
            canvas_draw_str(canvas, 0, y, text);
        }
        uint32_t age = (now - s->last_seen) / furi_ms_to_ticks(1000);
        if(statistics) {
            if(s->frames < 1000)
                snprintf(text, sizeof(text), "%lu", (unsigned long)s->frames);
            else
                snprintf(text, sizeof(text), "%luk", (unsigned long)(s->frames / 1000));
            canvas_draw_str(canvas, 53, y, text);
            if(s->last_timestamp) {
                DateTime dt;
                datetime_timestamp_to_datetime(s->last_timestamp, &dt);
                snprintf(text, sizeof(text), "%02u:%02u:%02u", dt.hour, dt.minute, dt.second);
                canvas_draw_str(canvas, 80, y, text);
            } else
                canvas_draw_str(canvas, 84, y, "--");
            continue;
        }
        if((!s->seen && !s->cached) || age >= TPMS_CACHE_SECONDS) {
            canvas_draw_str(canvas, 53, y, "--");
            canvas_draw_str(canvas, 84, y, "--");
            canvas_draw_str(canvas, 103, y, "--");
            continue;
        }
        snprintf(text, sizeof(text), "%.1f", (double)hyundai_pressure_psi(s->reading.pressure_raw));
        canvas_draw_str(canvas, 53, y, text);
        snprintf(text, sizeof(text), "%d", s->reading.temperature_c);
        canvas_draw_str(canvas, 84, y, text);
        if(!s->seen)
            snprintf(text, sizeof(text), "old");
        else if(age < 60)
            snprintf(text, sizeof(text), "%lus", (unsigned long)age);
        else if(age < 3600)
            snprintf(text, sizeof(text), "%lum", (unsigned long)(age / 60));
        else
            snprintf(text, sizeof(text), "%luh", (unsigned long)(age / 3600));
        canvas_draw_str(canvas, 103, y, text);
    }
    snprintf(text, sizeof(text), "%.0f dBm | %lu pkts | %s", (double)rssi, (unsigned long)packets,
             logging.ready && !logging.errors && !logging.dropped ? "SD" : "SD!");
    canvas_draw_str(canvas, 0, 63, text);
}

static TpmsSaved record(App *app, const HyundaiReading *reading, uint32_t timestamp,
                        uint32_t *session_frames) {
    TpmsSaved saved = {.reading = *reading, .timestamp = timestamp, .frames = 1};
    *session_frames = 0;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    unsigned index;
    for(index = 0; index < app->sensor_count; index++)
        if(app->sensors[index].id == reading->id)
            break;
    if(index == app->sensor_count && index < SENSOR_LIMIT) {
        app->sensors[index].id = reading->id;
        app->sensor_count++;
    }
    if(index < SENSOR_LIMIT) {
        app->sensors[index].reading = *reading;
        app->sensors[index].last_seen = furi_get_tick();
        app->sensors[index].seen = true;
        app->sensors[index].cached = false;
        app->sensors[index].last_timestamp = timestamp;
        if(app->sensors[index].frames < UINT32_MAX)
            app->sensors[index].frames++;
        if(app->sensors[index].total_frames < UINT32_MAX)
            app->sensors[index].total_frames++;
        saved.frames = app->sensors[index].total_frames;
        *session_frames = app->sensors[index].frames;
    }
    app->packets++;
    furi_mutex_release(app->mutex);
    return saved;
}

int32_t hyundai_tpms_app(void *context) {
    UNUSED(context);
    App *app = calloc(1, sizeof(App));
    app->pulses = furi_message_queue_alloc(1024, sizeof(Pulse));
    app->inputs = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->viewport = view_port_alloc();
    app->notification = furi_record_open(RECORD_NOTIFICATION);
    TpmsHistory restored;
    app->log = tpms_log_alloc(&restored, &app->config);
    app->config_enqueued = true;
    for(unsigned i = 0; i < TPMS_WHEEL_COUNT; i++)
        if(app->config.ids[i])
            app->sensors[app->sensor_count++].id = app->config.ids[i];
    uint32_t timestamp = furi_hal_rtc_get_timestamp();
    for(unsigned i = 0; i < restored.count; i++) {
        TpmsSaved *saved = &restored.sensors[i];
        bool recent = tpms_cache_recent(saved->timestamp, timestamp);
        unsigned index = 0;
        while(index < app->sensor_count && app->sensors[index].id != saved->reading.id)
            index++;
        if(index == app->sensor_count) {
            if(!recent || index >= SENSOR_LIMIT)
                continue;
            app->sensor_count++;
            app->sensors[index].id = saved->reading.id;
        }
        Sensor *sensor = &app->sensors[index];
        sensor->total_frames = saved->frames;
        if(recent) {
            sensor->reading = saved->reading;
            sensor->cached = true;
            sensor->last_timestamp = saved->timestamp;
            sensor->last_seen =
                furi_get_tick() - furi_ms_to_ticks((timestamp - saved->timestamp) * 1000);
        }
    }
    view_port_draw_callback_set(app->viewport, draw, app);
    view_port_input_callback_set(app->viewport, on_input, app);
    Gui *gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->viewport, GuiLayerFullscreen);

    furi_hal_power_suppress_charge_enter();
    furi_hal_subghz_reset();
    furi_hal_subghz_idle();
    furi_hal_subghz_load_custom_preset((const uint8_t *)hyundai_fsk_regs);
    furi_hal_subghz_set_frequency_and_path(433920000);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_start_async_rx(on_pulse, app);

    HyundaiDecoder decoder;
    hyundai_decoder_reset(&decoder);
    bool running = true, have_sequence = false;
    uint32_t previous = 0, last_draw = 0;
    uint32_t rate_start = furi_get_tick(), rate_edges = 0;
    uint32_t last_blink = 0;
    uint32_t sync_count = 0, crc_bad = 0, man_bad = 0;
    uint32_t history[24] = {0}, trace[TPMS_TRACE_LIMIT];
    unsigned history_position = 0, history_count = 0, trace_count = 0;
    bool tracing = false, trace_truncated = false;
    uint32_t trace_start = 0, trace_sequence = 0;
    float rssi_min = 0, rssi_max = 0, rssi_sum = 0;
    unsigned rssi_samples = 0;
    TpmsLogEvent event;

    while(running) {
        InputEvent input;
        while(furi_message_queue_get(app->inputs, &input, 0) == FuriStatusOk) {
            if(input.type != InputTypeShort &&
               !(input.type == InputTypeLong && input.key == InputKeyOk))
                continue;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(input.type == InputTypeLong) {
                if(app->settings)
                    app->config_enqueued = false; /* Retry an unsuccessful save. */
                else {
                    app->settings = true;
                    app->choosing = false;
                    app->selected_wheel = 0;
                    app->help = false;
                }
            } else if(app->settings) {
                if(input.key == InputKeyBack) {
                    if(app->choosing)
                        app->choosing = false;
                    else
                        app->settings = false;
                } else if(input.key == InputKeyUp || input.key == InputKeyDown) {
                    unsigned options = app->choosing ? app->sensor_count + 1 : TPMS_WHEEL_COUNT;
                    unsigned *selected = app->choosing ? &app->choice : &app->selected_wheel;
                    *selected =
                        (*selected + (input.key == InputKeyDown ? 1 : options - 1)) % options;
                } else if(input.key == InputKeyOk) {
                    if(!app->choosing) {
                        app->choice = 0;
                        for(unsigned i = 0; i < app->sensor_count; i++)
                            if(app->sensors[i].id == app->config.ids[app->selected_wheel])
                                app->choice = i + 1;
                        app->choosing = true;
                    } else {
                        uint32_t id = app->choice ? app->sensors[app->choice - 1].id : 0;
                        if(tpms_config_assign(&app->config, app->selected_wheel, id)) {
                            app->config_enqueued = false;
                            app->page = 0;
                        }
                        app->choosing = false;
                    }
                }
            } else if(input.key == InputKeyBack) {
                if(app->help)
                    app->help = false;
                else
                    running = false;
            } else if(input.key == InputKeyOk)
                app->help = !app->help;
            else if(input.key == InputKeyRight)
                app->page = (app->page + 1) % page_count(app);
            else if(input.key == InputKeyUp || input.key == InputKeyDown)
                app->statistics = !app->statistics;
            else if(input.key == InputKeyLeft)
                app->page = (app->page + page_count(app) - 1) % page_count(app);
            furi_mutex_release(app->mutex);
            view_port_update(app->viewport);
        }
        if(!running)
            break;
        Pulse pulse;
        for(unsigned i = 0; i < 256; i++) {
            if(furi_message_queue_get(app->pulses, &pulse, i == 0 ? furi_ms_to_ticks(10) : 0) !=
               FuriStatusOk)
                break;
            uint32_t encoded = (pulse.duration & 0x7fffffff) | (pulse.level ? 0x80000000u : 0);
            if(have_sequence && pulse.sequence != previous + 1) {
                if(tracing) {
                    memset(&event, 0, sizeof(event));
                    event.type = TpmsLogFrame;
                    event.result = TpmsFrameDropped;
                    event.timestamp = furi_hal_rtc_get_timestamp();
                    event.uptime_ms = trace_start;
                    event.sequence = trace_sequence;
                    event.symbols = decoder.symbols;
                    event.trace_count = trace_count;
                    event.truncated = true;
                    event.rssi = furi_hal_subghz_get_rssi();
                    memcpy(event.raw, decoder.raw, 10);
                    memcpy(event.trace, trace, trace_count * sizeof(uint32_t));
                    tpms_log_enqueue(app->log, &event);
                    tracing = false;
                }
                history_count = 0;
                history_position = 0;
                hyundai_decoder_reset(&decoder);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->lost_pulses += pulse.sequence - previous - 1;
                furi_mutex_release(app->mutex);
            }
            previous = pulse.sequence;
            have_sequence = true;
            HyundaiReading reading;
            rate_edges++;
            if(tracing) {
                if(trace_count < TPMS_TRACE_LIMIT)
                    trace[trace_count++] = encoded;
                else
                    trace_truncated = true;
            }
            HyundaiDecoder before = decoder;
            bool was_active = decoder.active;
            bool valid = hyundai_decoder_pulse(&decoder, pulse.level, pulse.duration, &reading);
            if(!was_active && decoder.active) {
                sync_count++;
                tracing = true;
                trace_count = 0;
                trace_truncated = false;
                trace_start = furi_get_tick();
                trace_sequence = pulse.sequence - history_count;
                for(unsigned n = 0; n < history_count; n++)
                    trace[trace_count++] =
                        history[(history_position + 24 - history_count + n) % 24];
                trace[trace_count++] = encoded;
            }
            history[history_position] = encoded;
            history_position = (history_position + 1) % 24;
            if(history_count < 24)
                history_count++;
            if(was_active && !decoder.active && !valid && pulse.duration >= 26 &&
               pulse.duration <= 208) {
                if(decoder.symbols == 160)
                    crc_bad++;
                else
                    man_bad++;
            }
            if(valid || (tracing && was_active && !decoder.active)) {
                memset(&event, 0, sizeof(event));
                event.type = TpmsLogFrame;
                event.result = valid                                           ? TpmsFrameValid
                               : decoder.symbols == 160                        ? TpmsFrameCrc
                               : pulse.duration >= 26 && pulse.duration <= 208 ? TpmsFrameManchester
                                                                               : TpmsFrameGap;
                event.timestamp = furi_hal_rtc_get_timestamp();
                event.uptime_ms = trace_start;
                event.sequence = trace_sequence;
                event.symbols = decoder.symbols;
                event.rssi = furi_hal_subghz_get_rssi();
                event.trace_count = tracing ? trace_count : 0;
                event.truncated = trace_truncated || !tracing;
                if(event.result == TpmsFrameGap) {
                    event.symbols = before.symbols;
                    memcpy(event.raw, before.raw, 10);
                } else
                    memcpy(event.raw, decoder.raw, 10);
                memcpy(event.trace, trace, trace_count * sizeof(uint32_t));
                if(valid) {
                    event.saved = record(app, &reading, event.timestamp, &event.session_frames);
                    event.config = app->config;
                    event.wheel_position = tpms_config_position(&app->config, reading.id);
                }
                tpms_log_enqueue(app->log, &event);
                tracing = false;
            }
            if(valid) {
                uint32_t now = furi_get_tick();
                if(!last_blink || now - last_blink >= furi_ms_to_ticks(250)) {
                    notification_message(app->notification, &sequence_blink_green_10);
                    last_blink = now;
                }
            }
        }
        uint32_t rate_elapsed = furi_get_tick() - rate_start;
        if(rate_elapsed >= furi_ms_to_ticks(1000)) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->pulse_rate =
                (uint32_t)((uint64_t)rate_edges * furi_ms_to_ticks(1000) / rate_elapsed);
            memset(&event, 0, sizeof(event));
            event.type = TpmsLogDiagnostic;
            event.timestamp = furi_hal_rtc_get_timestamp();
            event.uptime_ms = furi_get_tick();
            event.packets = app->packets;
            event.edges = app->pulse_rate;
            event.syncs = sync_count;
            event.crc_errors = crc_bad;
            event.manchester_errors = man_bad;
            event.dropped = app->lost_pulses;
            event.rssi = app->rssi;
            event.rssi_min = rssi_samples ? rssi_min : app->rssi;
            event.rssi_max = rssi_samples ? rssi_max : app->rssi;
            event.rssi_mean = rssi_samples ? rssi_sum / rssi_samples : app->rssi;
            furi_mutex_release(app->mutex);
            tpms_log_enqueue(app->log, &event);
            rssi_samples = 0;
            rssi_sum = 0;
            rate_edges = 0;
            rate_start = furi_get_tick();
        }
        if(furi_get_tick() - last_draw >= furi_ms_to_ticks(250)) {
            if(!app->config_enqueued) {
                memset(&event, 0, sizeof(event));
                event.type = TpmsLogConfig;
                event.config = app->config;
                app->config_enqueued = tpms_log_enqueue(app->log, &event);
            }
            float rssi = furi_hal_subghz_get_rssi();
            if(!rssi_samples || rssi < rssi_min)
                rssi_min = rssi;
            if(!rssi_samples || rssi > rssi_max)
                rssi_max = rssi;
            rssi_sum += rssi;
            rssi_samples++;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->rssi = rssi;
            app->syncs = sync_count;
            app->crc_errors = crc_bad;
            app->manchester_errors = man_bad;
            furi_mutex_release(app->mutex);
            view_port_update(app->viewport);
            last_draw = furi_get_tick();
        }
    }
    if(tracing) {
        memset(&event, 0, sizeof(event));
        event.type = TpmsLogFrame;
        event.result = TpmsFrameExit;
        event.timestamp = furi_hal_rtc_get_timestamp();
        event.uptime_ms = trace_start;
        event.sequence = trace_sequence;
        event.symbols = decoder.symbols;
        event.trace_count = trace_count;
        event.truncated = trace_truncated;
        event.rssi = furi_hal_subghz_get_rssi();
        memcpy(event.raw, decoder.raw, 10);
        memcpy(event.trace, trace, trace_count * sizeof(uint32_t));
        tpms_log_enqueue(app->log, &event);
    }
    furi_hal_subghz_stop_async_rx();
    furi_hal_subghz_sleep();
    furi_hal_power_suppress_charge_exit();
    view_port_enabled_set(app->viewport, false);
    gui_remove_view_port(gui, app->viewport);
    view_port_free(app->viewport);
    TpmsHistory final = {0};
    for(unsigned i = 0; i < app->sensor_count; i++) {
        Sensor *sensor = &app->sensors[i];
        if(sensor->seen || sensor->cached) {
            TpmsSaved saved = {.reading = sensor->reading,
                               .timestamp = sensor->last_timestamp,
                               .frames = sensor->total_frames};
            tpms_history_update(&final, &saved);
        }
    }
    tpms_log_free(app->log, &final, &app->config);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(app->pulses);
    furi_message_queue_free(app->inputs);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
