# WayCAN BLE protocol v1

This protocol is an experimental extension for WiCAN-OBD hardware v3.00.
It exists only in builds with `CONFIG_WAYCAN_TELEMETRY=y`. The runtime mode
activates when the saved protocol is ELM327 at boot. The normal build retains
stock ELM327 operation. The existing Wayborne mobile client is not compatible
with telemetry mode yet.

## Sampling and ownership

The adapter requests Mode 01 speed `0D`, RPM `0C`, and throttle `11` sequentially,
once per one-second cycle. It uses the upstream ELM327 implementation, with a
single-response limit and approximately 205 ms response timeout per request.
It does not run AutoPID or accept concurrent ELM327 commands in this mode.
BLE writes to `FEE1` fail, the TCP/UDP command server does not start, and incoming
WebSocket CAN commands are ignored. MQTT does not start in telemetry mode.
The PID-scan endpoint refuses requests until another mode boots.

The sampler checks these conditions before each diagnostic request:

- The device is awake and CAN is enabled.
- BLE and voltage-based sleep are enabled in the saved settings.
- MQTT is disabled in the saved settings.
- A valid supply-voltage reading exceeds the configured sleep threshold.

No phone connection is required. The build-time `CONFIG_WAYCAN_OBD_PROTOCOL`
selects protocol 6 through 9. There is no automatic protocol detection.
Only the three fixed read requests are sent; no diagnostic writes or DTC clears
are part of this protocol. Diagnostic reads still generate CAN traffic.

The voltage check uses upstream's cached reading, not an ignition signal.
One request can finish after voltage falls. Low voltage stops new requests;
the upstream sleep task still controls radio shutdown and wakeup. Smart
alternators and EV power behavior can cause missing samples with this policy.
Parked current, sleep/wake behavior, and ECU compatibility require hardware tests.

## GATT interface

| Item | Value |
| --- | --- |
| Existing service | `0000fee0-0000-1000-8000-00805f9b34fb` |
| Telemetry characteristic | `06b45b3f-572e-47ef-b6db-5ad6c121676d` |
| Properties | Read status, write replay cursor, notify records |
| Security | Encrypted, authenticated reads, writes, and CCC access |
| CCC values | `00 00` disables, `01 00` enables notifications |
| Packet size | 20 bytes, including at the default ATT MTU of 23 |

The characteristic is appended after the existing attributes. Other protocol
modes reject access to it. Prepared writes and nonzero offsets are unsupported.
A client must enable notifications and then write a replay cursor. Enabling the
CCC alone does not send records. Disconnect, CCC changes, and BLE shutdown clear
the stream authorization; each new connection needs a new replay request.

### Status read

All multi-byte header integers are unsigned little-endian.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 1 | Version, `1` |
| 1 | 1 | Packet type, `0` |
| 2 | 4 | Random boot ID |
| 6 | 4 | Oldest retained sequence |
| 10 | 4 | Next sequence to be assigned |
| 14 | 4 | Current adapter uptime, milliseconds |
| 18 | 2 | Buffer capacity, `192` records |

An empty buffer has equal oldest and next sequences. No values in this packet
mean that the engine is running or that the last reading is fresh.

### Replay request

The characteristic accepts exactly ten bytes:

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 1 | Version, `1` |
| 1 | 1 | Operation, `1` |
| 2 | 4 | Boot ID from the status read |
| 6 | 4 | First desired sequence, inclusive |

A boot mismatch, future sequence, invalid length/version/operation, or missing
subscription rejects the write. Invalid writes do not change an existing cursor.
Use a write with response so the client observes rejection. A request for a
sequence older than the buffer resumes at the oldest retained sequence. That
sequence jump reports lost history. Reading status and requesting its next
sequence skips existing history and starts live delivery.

### Sample notification

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 1 | Version, `1` |
| 1 | 1 | Packet type, `1` |
| 2 | 4 | Boot ID |
| 6 | 4 | Sequence |
| 10 | 4 | Adapter uptime when the response was received, milliseconds |
| 14 | 1 | Mode 01 PID, `0D`, `0C`, or `11` |
| 15 | 1 | Payload length: `1`, `2`, or `0` when unavailable |
| 16 | 2 | Raw OBD payload bytes A, B; unused bytes are zero |
| 18 | 2 | Reserved, zero |

Speed is A in km/h. RPM is `(256 * A + B) / 4`. Throttle is `100 * A / 255` percent.
These payload bytes keep their OBD order, unlike the little-endian header fields.
Length zero means no valid response arrived for that request. Its timestamp is
the request completion time, and its payload is zero, not a zero-valued reading.

Timestamps describe adapter receipt, not the ECU's measurement instant. Each PID
has its own timestamp. All records survive disconnect, but not reboot or power
loss. Upstream restarts the device after its sleep/wake sequence, so history does
not survive that restart either.

## Delivery limits

The buffer holds 192 records, about 64 seconds at three requests per second,
including unavailable responses. New records overwrite the oldest. Notifications
do not remove records. Successful BLE submission advances the stream cursor;
congestion and failed submission leave it retryable.

Successful submission is not an application acknowledgment. A client must track
the last contiguous sequence it saved, detect gaps, and request replay. Repeated
records are possible. Deduplication uses adapter identity, boot ID, and sequence.
Reconnecting requires reading status again; a changed boot ID starts a new session.
The random 32-bit boot ID is not a globally unique trip identifier.

Sequence and uptime use modulo-2^32 arithmetic. Uptime wraps after about 49.7 days.
Replay accepts cursors no more than 2^31-1 records behind next. Across reconnects,
clients must not confuse uptime with Unix time. A status read can anchor adapter
uptime to a phone clock, with uncertainty from Bluetooth delay and clock drift.
The adapter does not retain GPS, phone motion, or wall-clock time.
