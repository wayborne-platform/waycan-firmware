# Build and bench-test WayCAN

This firmware is a bench prototype for original WiCAN-OBD hardware v3.00.
Do not use these builds for daily driving before testing recovery, power draw,
and sleep/wake behavior on a spare adapter. The mobile app still speaks stock
ELM327 and cannot use telemetry mode yet.

## Build both variants

1. Install ESP-IDF **v5.5.2** with the ESP32-C3 toolchain.
2. Load the ESP-IDF environment with `. "$IDF_PATH/export.sh"`.
3. Run the host tests with `bash tests/run.sh`. They need a C compiler with
   AddressSanitizer and UndefinedBehaviorSanitizer, plus POSIX threads.
4. Run `bash tools/build-waycan.sh stock`.
5. Run `bash tools/build-waycan.sh telemetry`.

The scripts create independent configurations in `build/stock` and
`build/telemetry`. They do not edit the checked-in `sdkconfig`, flash hardware,
or change the partition layout. Firmware filenames retain the upstream naming
scheme and include the Git description. Identify the variant by its build
directory and accompanying `sdkconfig`.

The telemetry defaults select protocol 6, 11-bit CAN at 500 kbit/s. Before
connecting to a vehicle, select its already-verified protocol with
`idf.py -B build/telemetry menuconfig`, under **WayCAN telemetry**, then rebuild.
Protocols 7, 8, and 9 select 29-bit 500 kbit/s, 11-bit 250 kbit/s, and 29-bit
250 kbit/s respectively. Existing generated configurations take precedence over
defaults. Check `build/telemetry/sdkconfig` after changing settings.

The fork started at upstream commit `31803f9`; its firmware source matches
release `v4.21`, commit `f62eafb`. To reproduce the unmodified baseline in a
separate checkout, check out `v4.21` and run `idf.py build` with the same toolchain.

## Prepare a spare adapter

1. Confirm that the board is WiCAN-OBD hardware v3.00, not Pro or USB.
2. Back up its device configuration and retain its known-working stock firmware.
3. Follow upstream's USB recovery procedure on the spare adapter before testing
   modified firmware. Verify that the adapter returns to stock operation.
4. Connect the spare adapter to a correctly powered, isolated OBD/CAN bench with
   an ECU simulator. USB power alone is not sufficient for normal OBD operation.
5. Install the telemetry build using the upstream procedure for this hardware.
6. Set the saved protocol to ELM327, enable BLE and voltage-based sleep, disable
   MQTT, and restart the adapter. Use an appropriate sleep threshold for the bench.

The fork does not automatically change saved device settings. Sampling remains
off when its required settings or voltage checks fail. Keep testing off a real
vehicle until CAN traffic and the power transitions match expectations.

## Verify sampling and replay

Use a BLE client that supports authenticated pairing, characteristic reads,
writes with response, and notifications. Refer to the
[packet definitions](WAYCAN-PROTOCOL.md) for encoding.

1. Configure the ECU simulator to answer PIDs `0D`, `0C`, and `11` with known values.
2. Keep the adapter above its sleep threshold, with no BLE client connected.
3. Capture the CAN bus and verify that the adapter requests each PID once per cycle.
4. Pair a client, read telemetry status, enable notifications, and write a replay
   request for the reported oldest sequence.
5. Check the decoded values, record order, and per-response timestamps.
6. Save the last contiguous sequence and disconnect the phone for 10 seconds.
7. Reconnect, enable notifications again, and request the next sequence after the
   last saved record. Verify that replay includes readings collected during the gap.
8. Disconnect for longer than 64 seconds. Verify that a replay request for old
   history starts at the oldest retained record and exposes the sequence gap.
9. Stop one simulated PID response. Verify that length-zero records replace the
   missing data without reusing the preceding value.
10. Send a stock ELM327 command through `FEE1`. Verify that the write fails and
    that the CAN capture contains no corresponding diagnostic command.

## Verify power and recovery

1. Lower bench voltage below the sleep threshold during sampling. Verify that new
   requests stop after any in-flight request and the cached voltage updates.
2. Leave the adapter below threshold through the configured sleep delay. Measure
   current and confirm that the radios shut down.
3. Restore bench voltage and verify wakeup, reconnection, and a new boot ID.
4. Power-cycle the adapter. Verify that the client detects a new session instead
   of attaching old sequence numbers or timestamps to new readings.
5. Restore the stock build and verify that the existing Wayborne ELM327 client
   connects and reads the three PIDs again.

Host tests compile the actual buffer and sampler code. They replace FreeRTOS,
the clock, configuration reads, and the ELM327 exchange with controlled fakes.
They do not prove radio pairing, real CAN timing, ECU compatibility, parked
current, flashing recovery, or phone background behavior. Firmware CI compiles
both variants and checks that each image fits the existing OTA partition.
