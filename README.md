# Hyundai TPMS for Flipper Zero

A standalone receiver for the Hyundai Continental/VDO TPMS protocol captured
from a European 2023 Hyundai i20 N. Open the app and it immediately
listens with the internal radio at 433.92 MHz, using ProtoView's TPMS 1 FSK
preset. No frequency selection or waveform navigation is needed.

## Status

Experimental v0.6. Cabin reception has been confirmed on a European 2023
Hyundai i20 N. One drive captured 286 valid frames in 15 min 27 sec, with all four
sensors received within 57 seconds and no reported queue losses or SD errors.
Reception still has gaps; the true missed-transmission rate is not measured.

The app supports recent-reading restoration, diagnostic SD logging and saved wheel
assignments. New installations start with all wheels unassigned. No vehicle sensor
IDs or captured radio recordings are distributed with this repository.

## Install

Built for Flipper target 7, Unleashed unlshd-093e, API 88.9.
Download `hyundai_tpms.fap` from this repository's Releases, or build it locally.
Copy the FAP to `/ext/apps/Sub-GHz/hyundai_tpms.fap` on the
Flipper SD card with qFlipper, then open **Apps > Sub-GHz > Hyundai TPMS**.
This is a separate app; your modified ProtoView can remain installed.

## Use

- The screen shows wheel position, pressure in PSI, temperature in Celsius, and
  elapsed time since the last valid packet. Age is displayed in seconds,
  minutes or hours. Old values remain visible with their age; they are not
  evidence that a sensor is still transmitting.
- `old` marks a reading restored from the previous session. It remains visibly
  stale until a new valid frame arrives. Readings expire after **30 minutes**,
  including while the app remains open; expired values show `--`. Cached values
  dated in the future or with no timestamp are not restored. The Flipper's RTC
  must be reasonably correct for expiry across launches.
- `--` means no recent reading is available.
- **Short OK:** show/hide reception diagnostics. **Hold OK:** open wheel settings.
  **Up/down:** toggle the pressure view and sensor-ID statistics (current-run count
  and last-reception time, HH:MM:SS). Restored sensors start at zero for the current
  run. **Left/right:** cycle Wheels and Other sensors pages. **Back:** close help
  or exit the app. Reception continues in diagnostics and settings.
- The first page lists **Front L**, **Front R**, **Rear L**, **Rear R** in that
  order, from the driver's perspective. New installations have no assigned IDs.
  Receive your sensors, then hold OK to assign them in settings. Existing SD
  assignments are retained when updating the app.
- **Up/down** shows full sensor IDs on the statistics screen. Unassigned
  Hyundai-VDO IDs appear on **Other sensors**, up to 16 sensors total. This page
  is always accessible with Left/right, even when empty. A new ID is added only
  after a CRC-valid packet using the supported Hyundai Continental/VDO protocol
  is decoded; other TPMS protocols and radio noise do not populate this page.
  Nearby cars can appear too. Additional Other sensors pages are added as needed.
  Current-run unassigned entries remain listed after their readings expire; only
  recent unassigned entries are restored on the next launch.
- Drive normally, then park and listen near a wheel. Sensors may be silent
  while parked; the app cannot wake them or make them transmit. Check screen
  readings while parked. Other Hyundai sensor protocols are not supported.
- The footer always shows radio RSSI in dBm and valid packet count.
  **OK** shows processed radio edges per second, preamble detections (Sync),
  CRC and Manchester rejects, and dropped queue edges. Radio noise also
  produces edges and occasional false preambles: these are not confirmed
  TPMS transmissions. Green LED flashes only for CRC-valid packets.
  These diagnostics do not change radio settings or decoder tolerances.

Recent readings and cumulative per-sensor counts are saved to SD. Wheel
labels are matched by sensor ID, including for restored readings. The footer
shows `SD` while logging is healthy and `SD!`
if storage failed or log events were dropped; **OK** shows log losses and SD
errors. Radio queue losses and logging queue losses are separate counters.
It only receives: there is no transmit path, pairing procedure or tyre-pressure
warning threshold in this version. PSI is a unit conversion of the sensor's
quantized pressure reading, not a claim of 0.1 PSI measurement accuracy.

## Wheel settings

Hold **OK** to open settings. Use **Up/down** to select a wheel and **OK** to
choose its sensor. Use **Up/down** to browse known IDs (including current assigned
IDs and recently detected unassigned sensors). The picker shows current position,
recent pressure, temperature and reading age. Select **Unassigned** to clear a
position. **OK** applies; **Back** cancels the picker or leaves settings.

Selecting a sensor already assigned to another position swaps the two assignments.
Selecting a replacement leaves the previous sensor on Other sensors. The main
wheel page always keeps Front L, Front R, Rear L, Rear R in that order. Positions
are from the driver's perspective. Update assignments after rotating/replacing
wheels; the app does not automatically localize sensors.

Changes save asynchronously to `/ext/apps_data/hyundai_tpms/config0.bin` and
`config1.bin`. Settings displays **Saving...**, **Saved**, or **Save failed**.
After a failure, hold OK in settings to retry. Wait for Saved before powering off.
Configuration has a version marker, generation counter and CRC, using alternating
files so an incomplete write can fall back to the previous valid version. With
no valid configuration, all four positions start unassigned. Assignments never
expire; cached pressure readings still expire after 30 minutes. Existing cache
and log files are preserved across this update.

## Privacy

Sensor IDs can identify individual tyre transmitters. Wheel configuration, cached
readings and SD logs contain the IDs received by your own device. These files
remain on your SD card; review or redact them before sharing. Raw captures, logs,
configuration files and built binaries are excluded from Git by default. Test
fixtures use invented IDs and a generated CRC-valid packet.

## SD logs

Every launch creates a new folder, so previous trips are retained:

`/ext/apps_data/hyundai_tpms/run-<RTC timestamp>-<sequence>/`

- **packets.csv:** every logged CRC-valid frame, including repeated frames;
  sensor ID, raw pressure, PSI, temperature, state/flags/repeat/unknown bytes,
  current-run and cumulative sensor counts, reception RSSI and raw ten-byte frame.
  v0.5 appends `wheel_position` and `assignment_generation`, captured when the
  packet was received. Unassigned sensors are logged as `Unassigned`; later
  configuration changes do not relabel earlier packets.
- **frames.csv:** preamble-triggered candidates, including valid frames, CRC or
  Manchester rejects, gaps/glitches, dropped-edge interruptions and an unfinished
  candidate at exit. Includes sequence, decoded symbol count, partial/full raw
  bytes, RSSI, pulse count, truncation flag and `H52;L104;...` timings in microseconds.
  Each trace includes up to 24 preceding edges and is capped at 192 edges.
- **diagnostics.csv:** approximately once per second, valid-frame count,
  processed edges/s, syncs, CRC/Manchester rejects, dropped radio edges,
  sampled RSSI minimum/maximum/mean/latest, dropped log events, storage errors
  and written event counts. Background RSSI is sampled four times per second.
- **metadata.txt:** app version, frequency, decoder constants, radio-register
  preset, cache expiry and measurement limitations.

Frame RSSI is sampled when decoding finishes, **not at the signal peak**. The
trace uptime denotes preamble detection; its preceding edges predate that tick.
RTC timestamps use the Flipper clock and are not guaranteed UTC or accurate.
`raw_hex` on a rejected candidate is partial decoder evidence, not a valid packet.

This is bounded pulse logging around detected preambles, not continuous raw-edge
or IQ recording. Noise or transmissions that never match a preamble only appear
in aggregate diagnostics. A full queue loses events and increments `log lost`;
no promise is made that every RF transmission is captured. CSVs sync about once
per second and on normal exit; sudden power loss may lose recent buffered rows.

Copy the run folder with qFlipper to share a reception test. Logs stay on SD until
you remove them; the 30-minute expiry hides cached readings and **does not delete
logs**. State snapshots are `state0.bin` / `state1.bin` in the parent directory.

## Implementation

The radio interrupt only queues timed level changes. A separate low-priority
worker writes the SD logs through a bounded, nonblocking event queue; the radio
consumer never waits for SD writes. Queue saturation is counted rather than
blocking reception. Recent readings use two alternating CRC32-protected history
files, allowing the loader to fall back to the previous valid snapshot after an
interrupted write. An orderly exit drains queued logs, syncs files, and saves
latest sensor values even if a prior log event was dropped. No cache file is
trusted without length, checksum and field validation.

The app decodes pulse runs as 52 us symbols, locates the preamble in either polarity, checks
Manchester encoding and CRC8 (poly 0x07, init 0xaa), then updates the table.
Dropped queue samples reset decoding to avoid joining unrelated data.
Drawing copies the table under a mutex; the radio callback never draws or blocks.

The FSK register preset is from antirez's ProtoView and its BSD2 licence is
included in LICENSE. The Hyundai decoder is an independent implementation of
protocol facts described by rtl_433:
https://github.com/merbanan/rtl_433/blob/master/src/devices/tpms_hyundai_vdo.c

## Build and validate

Install Python and [uFBT](https://github.com/flipperdevices/flipperzero-ufbt),
then configure uFBT for the firmware installed on your Flipper. The supplied
binary targets Unleashed unlshd-093e, target 7, API 88.9. A different firmware
may need a rebuild with its matching SDK.

Run from the repository root:

```sh
ufbt
python3 -B tests/test_decoder.py --synthetic-only
python3 -B tests/test_history.py
python3 -B tests/test_config.py
```

The built app is written to `dist/hyundai_tpms.fap`. The portable decoder tests
require Python 3 and a C compiler named `cc`. To additionally replay your own
captures, install `rtl_433` and provide a directory of 1 MS/s CU8 recordings:

```sh
python3 -B tests/test_decoder.py /path/to/captures
```

The original capture session is not included. Its 333 recordings matched the
C decoder against rtl_433: 177 valid packets in 47 positive recordings, including
inverse polarity. Synthetic checks cover malformed/truncated frames, corrupt
bits, noise, repeated frames, gaps and reset after dropped data.

The FAP builds and passes SDK import checks against API 88.9. Cache tests use
AddressSanitizer/UndefinedBehaviorSanitizer to check 30-minute boundaries,
next-day/future timestamps, full-capacity round trips, truncation, corruption and
invalid states. Configuration tests cover all 24 wheel arrangements, swaps,
replacement, unassignment, generation wrap, and corrupt/duplicate-ID files.
On-device v0.5 checks verified a menu-driven swap, save and restoration across
launches, then restored the test mapping. The new log header and zero storage
errors were checked. No live TPMS packet was received during the indoor settings
test; assignment columns on live packets and changed-settings RF performance
remain to be checked during a drive.

## License and attribution

BSD-2-Clause; see [LICENSE](LICENSE). The TPMS 1 radio preset originates from
[antirez/ProtoView](https://github.com/antirez/protoview). The decoder implements
protocol facts independently; it does not incorporate rtl_433 source code.
