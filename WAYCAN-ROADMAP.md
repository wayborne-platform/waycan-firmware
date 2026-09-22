# WayCAN firmware ideas

What a Wayborne-specific WiCAN firmware could do beyond stock ELM327, and in
what order. This is a design backlog, not a commitment. Every item is judged by
one question: does it make the trip data the Wayborne app ingests more
complete, more accurate, or more trustworthy?

Facts about the app this document leans on (as of 2026-09-21, see
`wayborne-sim/docs/INTEGRATION_NOTES.md` for line references):

- BLE GATT central on service `FEE0`, characteristic `FEE1`, speaking ELM327 text.
- Polls `010D` speed, `010C` RPM, `0111` throttle sequentially at 1 Hz.
- Reads the VIN (`0902`) once per connection; a `WAYB0RNE` prefix marks the bench simulator.
- Polls Mode 22 DIDs `F010`-`F015` (heading, yaw rate, gear, odometer) on the bench.
- Harsh braking is speed change below -8 mph/s and rapid acceleration above +6 mph/s,
  both computed by differencing consecutive 1 Hz speed samples.
- Trips are rebuilt from an append-only JSONL log split on CONNECTED/DISCONNECTED
  markers. Start: adapter answers. Suspend: disconnect. Close: 3 min adapter-unseen grace,
  30 s OBD-loss grace in the foreground, 5 min idle under 5 mph in the background.
- No adapter-side buffering exists. Samples while the phone is disconnected are lost.
- The product promise is a *verified* safety score aimed at insurance pricing.

What the fork already has (commit `9e5772e`): an opt-in build that samples the three
PIDs once per second through the ELM327 path, keeps 192 records in RAM, and replays
them by sequence number over a new GATT characteristic. It is exclusive: ELM327 on
`FEE1` is disabled in that mode. See `WAYCAN-PROTOCOL.md`.

---

## Principles

1. **Additive over exclusive.** Stock ELM327 on `FEE1` keeps working. New behaviour lives
   on new characteristics and new packet types. Old and new app builds both work against
   the same adapter, so no coordinated release is ever required.
2. **Adapter samples fast; the phone chooses its rate.** Do the high-rate work where the
   bus is. Stream live at whatever rate the subscriber asks for, and store at the 1 Hz
   cadence the logging pipeline, scoring, and Convex upload already expect.
3. **Survive the phone being absent.** Everything of value is written to flash before it is
   sent, and replay is by sequence number.
4. **Stay mergeable with upstream.** Everything gated behind `CONFIG_WAYCAN_*`, new code in
   `waycan_*.c`, host tests with sanitizers, CI building stock and telemetry variants.

---

## A. Transport and protocol

### A1. Push instead of poll

**What.** The adapter streams a compact binary frame (speed, RPM, throttle, and whatever
PIDs the car profile asks for) at 5 to 10 Hz over a notification characteristic. The app
becomes a decoder, not a chatty client.

**Why.** Today the app issues ELM327 commands at 1 Hz and parses ASCII replies, and most
of the adapter bugs chased so far live in that layer: NO DATA, stale frames eaten by the
probe counter, protocol re-detection on every reconnect, the 5-second command timeouts.
Firmware that pushes removes that whole class. Latency, BLE airtime and phone CPU all drop.

**How.** This is the umbrella the rest of section A and B1/B2 serve. The existing
telemetry characteristic already notifies records; push is that same channel driven live
at the sample rate, with A3 packing the frame and B2 sourcing it from CAN directly. The
subscriber picks the rate in its replay request: 10 Hz on the live screen, 1 Hz in the
background. Stored records stay at 1 Hz with window statistics so flash wear and upload
size hold, and replay of the stored ring covers every gap the live stream missed. One
packet format for both.

**Effort.** Medium once A2, A3 and B2 exist; small on top of them.

### A2. Coexist with ELM327 instead of replacing it

**What.** Run the sampler alongside the stock ELM327 channel. The sampler takes the ELM327
lock per request and yields whenever a phone command is pending on `FEE1`.

**Why.** The app depends on `FEE1` for the VIN, simulator detection, Mode 22 DIDs, and the
PID scan. Exclusive telemetry mode breaks all of those and forces a hard cutover.

**How.** Replace the "reject writes to `FEE1`" branches in `ble.c` with a priority flag the
sampler checks before each request. Drop the `can_tx_task` early return for WayCAN mode.
The app detects the telemetry characteristic during service discovery and subscribes if
present, otherwise it keeps polling.

**Effort.** Small. Mostly deleting exclusivity code.

### A3. Packed per-second record

**What.** One 20-byte record per cycle carrying speed, RPM, throttle, and window statistics
(see B1), instead of one record per PID.

**Why.** Three notifications and three flash writes per second become one. Today a 10-byte
header wraps a single-byte payload.

**How.** New packet type. Keep the existing per-PID type for a release so the protocol
tests still pass, then retire it.

### A4. Negotiate a larger MTU and tune connection parameters

**What.** Accept MTU up to 247 so records can grow past 20 bytes when needed. Use a longer
connection interval and slave latency when only 1 Hz traffic flows.

**Why.** Battery on the phone side and headroom for richer records. The app already
handles chunked notifications, so a larger MTU only reduces chunking.

### A5. Advertise buffer state

**What.** Put "records pending" and "engine running" bits in the advertising manufacturer
data.

**Why.** The phone can decide whether to connect and backfill without a full connection
and pairing handshake. iOS background scanning can wake the app on this.

### A6. Request packet for extra PIDs through the telemetry characteristic

**What.** A write operation that asks the adapter to include an extra PID or DID in the
next N records.

**Why.** Lets the app pull heading, odometer, coolant, or fuel through the push channel
rather than through serialized ELM327 round trips. Lower priority once A2 exists, since
`FEE1` remains available.

---

## B. Sampling

### B1. Sample fast, emit window statistics

**What.** Poll speed at 5 to 10 Hz on the bus. Every second emit the latest speed, RPM and
throttle plus the window's min speed, max speed, peak acceleration and peak deceleration.

**Why.** Braking and acceleration events come from differencing 1 Hz samples. A short hard
brake between two samples is averaged away, and ELM327 round-trip jitter adds noise. Peak
deceleration measured at 10 Hz on the adapter is what the scoring pipeline actually wants,
delivered at the rate it already handles.

**How.** Needs B2 to be practical. Peak accel is computed from consecutive fast samples
with the adapter's timer, not the phone's arrival time.

**Effort.** Medium. The record format and the app's event services both change.

### B2. Talk to CAN directly for the fixed PIDs

**What.** Send `7DF 02 01 0D` and read `7E8` directly for the sampler's own requests
instead of formatting ASCII, pushing it through `elm327_process_cmd`, and parsing the ASCII
reply.

**Why.** The ELM327 path carries a roughly 205 ms timeout per request and ties up the
diagnostic channel. Direct single-frame requests complete in tens of milliseconds and make
10 Hz sampling realistic. Upstream's AutoPID is prior art in the tree.

**Risk.** Multi-ECU responses and 29-bit addressing need the same handling ELM327 already
has. Keep the ELM327 path as the fallback and select per vehicle.

### B3. Adaptive rate

**What.** Sample at the fast rate only while speed is nonzero. Drop to 1 Hz at idle and to
a heartbeat when the engine is off but the ECU still answers.

**Why.** Flash wear, bus load, and parked current.

---

## C. Persistence and trips

### C1. Flash-backed ring buffer

**What.** Move the record ring from RAM to the 300K SPIFFS partition (or a dedicated raw
partition with a simple two-sector log). Around 15,000 records, roughly four hours at one
record per second.

**Why.** The RAM buffer holds 64 seconds and is wiped by the restart upstream performs
after its sleep/wake cycle, which lands exactly at trip boundaries. This is the change
that turns "buffer a minute" into "capture the whole trip without the phone".

**How.** Append-only with a fixed record size so a sequence number maps to an offset.
Erase the oldest sector when full. Write synchronously for markers (C3), batched for
samples.

**Risk.** Flash wear at 1 Hz is fine for years; at 10 Hz raw it is not, which is why B1
emits per-second records.

### C2. Sequence counter and clock anchor in RTC noinit memory

**What.** Keep the sequence counter, boot session ID, and a wall-clock offset in
`RTC_NOINIT_ATTR` storage. The phone writes its current time to the adapter on every
connection.

**Why.** RTC noinit memory survives the software restart in the sleep path on the
ESP32-C3 (not a power cut). Records collected while the phone was away then carry real
timestamps, and sequence numbers stay monotonic across the restart, so replay does not
need a new session each time the car wakes.

### C3. Engine on/off markers

**What.** A marker packet type, in the same ring as samples, emitted when the adapter
sees the engine start or stop. Payload: event kind, uptime of the transition, reason code
(RPM nonzero, RPM zero, no ECU response, voltage drop).

**Why.** The app infers trip edges from proxies with 30 s to 5 min grace timers. The
adapter can see the engine directly, every second, with or without a phone nearby.
Markers map one to one onto the app's CONNECTED/DISCONNECTED reconstruction model.

**State machine.**

| From | To | Condition | Emit |
| --- | --- | --- | --- |
| OFF | CRANKING | RPM nonzero | nothing |
| CRANKING | RUNNING | RPM above ~400 for 3 samples | ENGINE_ON, stamped at first nonzero sample |
| RUNNING | STOPPING | RPM zero or no data | nothing; start hold timer |
| STOPPING | RUNNING | RPM returns inside hold window | nothing (start-stop system) |
| STOPPING | OFF | hold window expires (~90 s, runtime setting) | ENGINE_OFF, stamped at first zero sample |

**Vehicle rules.** The RPM rule alone fails on any car whose engine can be off while the
trip continues:

- **Full EVs.** No RPM at all. Teslas do not speak standard OBD; most other EVs answer
  some requests but nothing useful for RPM. Use ECU presence plus speed.
- **Hybrids.** Prius, RAV4 Hybrid, Camry Hybrid, Honda e:HEV, Volt and other plug-ins.
  The engine cuts at low speed, at lights, and when coasting, sometimes for minutes, so
  the hold window is not enough. The trip is on while the ECU answers and speed is
  nonzero, regardless of RPM.
- **Start-stop cars.** Most new gas cars from VW, BMW, Ford, Mazda and others cut the
  engine for 10 to 60 s at lights. The ~90 s hold covers nearly all of it, but a long
  light or a drive-through can still split a trip. Make the hold a per-vehicle setting.
- **ECUs that stay awake after key-off.** Some keep answering OBD for minutes after
  shutdown. The presence signal is late on those; RPM at zero still catches it.
- **Pre-2008 non-CAN protocols.** Unsupported by the WiCAN hardware anyway.

Two rules cover this: `rpm` for gas and diesel, `presence` (ECU answers and speed is
nonzero) for hybrids and EVs. Select per car from the VIN the app already reads and store
the choice in NVS.

**App first, firmware second.** The same state machine can run in the app today from the
RPM it already polls, and that is where it should start. Firmware markers only add value
once C1 and C2 exist, because their whole point is seeing edges the phone missed.

**Constraint.** Flush ENGINE_OFF to flash synchronously before the sleep path restarts
the chip. Key-on-engine-off must never open a trip; the sim's `cold_start_idle` and
`extended_idle` scenarios are ready-made regression cases.

### C4. Odometer and distance from the adapter

**What.** Integrate speed on the adapter and include trip distance in each record; read
the real odometer where the vehicle exposes it (`01A6` or OEM DIDs).

**Why.** Distance today comes from GPS fixes thinned to 100 points. Integrated OBD speed
at 10 Hz is more accurate in tunnels and cities, and it is what the app already does with
dead reckoning in sim mode.

---

## D. Trust and verification

### D1. Sign records

**What.** A per-device key provisioned at pairing (or at manufacturing for a Wayborne
branded batch). Each record, or each block of records, carries an HMAC-SHA256 truncated to
8 bytes over boot ID, sequence, and payload. The backend verifies.

**Why.** The product sells a verified score. The schema distinguishes simulated from real
sources, but nothing stops a phone from fabricating OBD entries. Signing is what makes
"verified" a property rather than a word.

**How.** Key exchange during the first authenticated BLE session, stored in NVS on the
adapter and escrowed to the backend under the user's account. Replay protection comes free
from boot ID plus sequence. Convex validates on upload and flags trips that fail.

**Effort.** Medium on the adapter, medium on the backend, small on the phone.

### D2. VIN binding

**What.** The adapter reads the VIN itself at each ENGINE_ON and includes it (or a hash)
in the session status packet and the signed block header.

**Why.** Ties a signed trip to a specific vehicle, not just a specific adapter. Needed for
any insurance claim that the score belongs to the insured car.

### D3. Tamper and unplug detection

**What.** Record a marker when supply voltage disappears entirely (adapter unplugged) versus
drops below threshold (car off). Count power cycles in RTC noinit memory.

**Why.** Distinguishes "did not drive" from "removed the adapter". Coverage gaps are a
signal insurers care about.

---

## E. Vehicle coverage

### E1. Protocol persisted from the app's negotiation

**What.** When the app runs its `ATSP6`..`ATSP9` probe and settles, the adapter stores the
result in NVS and uses it for autonomous sampling.

**Why.** Removes the compile-time `CONFIG_WAYCAN_OBD_PROTOCOL` setting without adding any
UI. The app already does the work.

### E2. Autonomous protocol detection

**What.** If no stored protocol exists, the adapter probes itself at ENGINE_ON.

**Why.** First drive after install works even if the app has not connected yet.

### E3. Lateral dynamics from OEM broadcast frames

**What.** Decode yaw rate, steering angle, and lateral acceleration from vehicle-specific
CAN IDs using the vehicle profiles already in this repo, and include them in records.

**Why.** Cornering and crash detection use the phone IMU, which is noisy in a cup holder
and silent on the bench. The bus has the real numbers on most cars built after 2010.

**Risk.** Per-manufacturer work with a long tail. Do it after the core loop is solid,
starting with whichever makes dominate the user base.

### E4. Extended PIDs for context

**What.** Fuel level, coolant temperature, engine load, ambient temperature, at low rate
(every 10 s).

**Why.** Cold-start and low-fuel scenarios already exist in the sim. Context for the coach
("you idled 6 minutes on a cold engine") and for future features. Cheap once B2 exists.

### E5. DTC snapshot at ENGINE_ON

**What.** Mode 03 read once per trip, included in the session status.

**Why.** Check-engine state is useful product context and a sanity check on data quality.
Read only; never clear.

---

## F. Operations

### F1. BLE OTA

**What.** Firmware update over the BLE connection, driven by the app, into the second OTA
slot with rollback on failed boot.

**Why.** Users will not join the adapter's WiFi access point and open a web page. The
partition table already has two 1740K OTA slots. Only the transport is missing.

**How.** Chunked writes on a dedicated characteristic, CRC per chunk, image signature
checked before `esp_ota_set_boot_partition`. Reuse upstream's OTA handling behind the
HTTP path.

**Effort.** Large. Highest-risk item on the list because a bad update bricks a device in
a car.

### F2. Runtime configuration over BLE

**What.** A config characteristic for sleep threshold, hold window, sample rate, vehicle
rule, and log level. Replaces the WiFi config page for Wayborne users.

**Why.** Every tunable above needs a way to be set from the app.

### F3. Adapter health in the status packet

**What.** Firmware version, uptime, CAN error counters, flash ring fill level, last reset
reason, supply voltage, parked-current estimate.

**Why.** Support triage without physical access. "Which firmware is on the bench dongle"
was an open question in the sim integration notes.

### F4. Pairing and identity UX

**What.** Static passkey printed on the device or shown by LED sequence, plus an
app-level device claim so one adapter belongs to one Wayborne account.

**Why.** Encrypted MITM pairing is already required. The missing part is making it not
painful and making ownership explicit for the household and friends features.

### F5. LED feedback

**What.** Distinct LED patterns for engine detected, buffering with no phone, phone
connected and streaming, and error.

**Why.** Cheap reassurance that the adapter is doing its job when the phone is in a pocket.

---

## G. Bench and testing

### G1. Sim scenarios as firmware gates

**What.** Run `wayborne-sim` scenarios against the telemetry build in CI on the bench rig
and assert on the record stream: marker timestamps, peak decel values, gap-free sequences
across a simulated sleep/wake.

**Why.** The sim exists to make the app testable without a car. The same rig makes the
firmware testable. `harsh_braking.yaml` is the acceptance test for B1.

### G2. Host tests for every state machine

**What.** Extend `tests/` with the engine state machine, the flash ring, and the signing
block, all with sanitizers.

**Why.** The existing test harness already fakes FreeRTOS, the clock, and the ELM327
exchange. Keep that discipline for everything new.

### G3. Power measurements as a release gate

**What.** Parked current and sleep/wake timing recorded per release on a spare adapter.

**Why.** A firmware that drains a battery over a weekend is worse than no firmware.

---

## Suggested order

| Phase | Items | Outcome |
| --- | --- | --- |
| 1 | A2, A3 | Telemetry can ship without breaking the current app |
| 2 | C1, C2, C3 | Whole trips captured with the phone absent, real timestamps, clean trip edges |
| 3 | B2, B1, A1 | Push replaces poll; event-grade braking and acceleration fidelity |
| 4 | E1, F2, F3 | No compile-time vehicle settings; supportable in the field |
| 5 | D1, D2 | "Verified" becomes provable |
| 6 | F1 | Updates without the WiFi page |
| 7 | A5, B3, C4, E4, E5, F4, F5 | Polish and product context |
| later | E3, D3, A6, E2 | Long-tail vehicle work and extras |

G1 through G3 run alongside every phase.

## Explicitly not doing

- Replacing upstream's voltage-based sleep. It stays as the power guardian; markers layer on top.
- Diagnostic writes or DTC clears from the adapter. Read only.
- MQTT, WiFi, or cloud connectivity from the adapter. The phone is the uplink.
- Targeting WiCAN Pro or USB hardware until v3.00 is solid.
