# espnow_mqtt

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5%2B-blue)
![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-orange)
![License](https://img.shields.io/badge/license-MIT-green)

MQTT-like publish/subscribe over ESP-NOW. Battery-powered sensor nodes publish
data to a single WiFi-connected hub — no cloud broker, no TCP stack on the sensors,
no fragmentation.

---

## Architecture

This component uses a **two-node-type** model. The broker and the subscriber are
the same physical node — there is no third party.

```
┌──────────────────────────┐      ESP-NOW      ┌──────────────────────────────────────┐
│  Publisher node          │ ────────────────► │  Broker node                         │
│                          │                   │                                      │
│  • Sensor firmware       │                   │  • WiFi STA — connected to AP        │
│  • No WiFi AP join       │                   │  • Runs subscriber callbacks         │
│  • Battery powered       │                   │  • Forwards data upstream            │
│  • ESP32-C3 / S3 / C6    │                   │  • ESP32 / S3                        │
└──────────────────────────┘                   └──────────────────────────────────────┘
      sends sensor readings                          receives + acts on them
      via ESP-NOW frames                             (MQTT, HTTP, database, actuator…)
```

**Publisher nodes** are sensor devices. They register their topic strings with the
broker once and receive a 1-byte `topic_id` in return. All subsequent PUBLISH frames
carry only the `topic_id` — no string overhead on the wire after the first exchange.
Publisher nodes never join a WiFi AP.

**The broker node** is the single WiFi-connected hub. It maintains a registry of
`(publisher_mac, topic_string) → topic_id` and delivers incoming PUBLISH frames to
**subscriber callbacks running on the same node**. The subscriber is not a separate
device — it is your application code, co-located on the broker, that decides what
to do with the data.

```
Broker node internals:

  recv_cb  (WiFi task)
    ├── REGISTER  →  registry lookup / allocate  →  send REGISTER_ACK
    └── PUBLISH   →  dispatch queue
                        └── broker_dispatch_task
                              ├── on_sensor_data()  →  forward to MQTT broker
                              └── on_sensor_data()  →  write to SD card / DB
```

Frames are ≤ 250 bytes (ESP-NOW maximum). No fragmentation. No TCP. No cloud dependency.

---

## Examples

Two complementary examples ship with the component — one for each node type.
Build and flash them together to see the full system working end to end.

| Example | Node type | What it does |
|---------|-----------|--------------|
| `examples/publisher/basic_sensor` | Publisher | Registers two topics (`sensors/node1/temp`, `sensors/node1/humidity`), publishes synthetic float readings every 5 s |
| `examples/broker/basic_broker` | Broker | Connects to WiFi, starts the broker, subscribes to `sensors/+/temp` and `sensors/+/humidity`, logs every received value |

The broker example uses the three-phase boot sequence — the correct pattern for
any production broker firmware. The publisher example uses CONTINUOUS mode and
demonstrates `wait_registered()`, periodic stats logging, and graceful handling
of the case where the broker is not yet available at publisher boot time.

Both examples require you to fill in the peer MAC address before building. The
README in each example directory gives step-by-step instructions including how
to read the MAC from a board without writing any code:

```bash
# Linux / macOS
esptool.py --port /dev/ttyUSB0 read_mac

# Windows
esptool.py --port COM3 read_mac
```

Flash order matters: **flash the broker first**. The broker must be running and
have the publisher's MAC in its peer table before the publisher boots, or the
publisher's REGISTER frames will be silently dropped by the trust filter.

---

## Features

* **Zero string overhead after registration** — 1-byte `topic_id` on every PUBLISH frame
* **Channel-agnostic** — publisher scans all 13 channels to find the broker; hint-first fast path on re-registration
* **Two publisher modes** — CONTINUOUS (always-on task) or SLEEP (blocking `register_sync` / `publish_sync` for deep-sleep wake cycles)
* **`+` wildcard subscriptions** — `sensors/+/temp` matches all nodes
* **Publisher keepalive** — zero-payload PUBLISH resets broker timeout timer without application involvement
* **Three-phase broker boot** — pre-announces on last known channel before WiFi connect so publishers that wake early are not missed
* **NVS channel persistence** — broker remembers its WiFi channel across reboots
* **Per-publisher sequence tracking** — gap and duplicate detection; frames always dispatched
* **Publisher lifecycle callbacks** — REGISTERED, REREGISTERED, TIMEOUT events
* **OTA-safe peer management** — `espnow_mqtt_purge_mac()` clears stale registry entries before reflash
* **Role-split builds** — PUBLISHER-only and BROKER-only strip the irrelevant code entirely
* **Optional HMAC-SHA256 payload integrity** — detects RF corruption and naive replay in physically-controlled deployments

---

## Chip Support

| Chip | Publisher | Broker |
|------|-----------|--------|
| ESP32 | ✓ | ✓ Tested |
| ESP32-S3 | ✓ | ✓ |
| ESP32-C3 | ✓ Tested | ✓ |
| ESP32-C6 | ✓ | ✓ |
| ESP32-S2 | ✓ | — RAM headroom tight; publisher role only recommended |

---

## Installation

```bash
idf.py add-dependency "espnow_mqtt^0.2.0"
```

Or in `idf_component.yml`:

```yaml
dependencies:
  espnow_mqtt: "^0.2.0"
```

Requires **ESP-IDF ≥ 5.5.0**. Both `recv_cb` and `send_cb` use the struct-parameter
form introduced in IDF 5.0 and 5.5 respectively.

---

## Quick Start

### Broker node

```c
// After esp_now_init() and esp_wifi_connect():
ESP_ERROR_CHECK(espnow_mqtt_init());

// Add publisher peers before broker_start (boot-time)
esp_now_peer_info_t peer = { .peer_addr = {0x11,…}, .encrypt = false };
ESP_ERROR_CHECK(esp_now_add_peer(&peer));

ESP_ERROR_CHECK(espnow_mqtt_broker_start());

// Register subscriber callbacks — these run on this same node
espnow_mqtt_sub_handle_t h;
ESP_ERROR_CHECK(espnow_mqtt_subscribe("sensors/+/temp", on_data, NULL, &h));
```

### Publisher node

```c
// After esp_now_init() (no WiFi AP connect needed):
ESP_ERROR_CHECK(espnow_mqtt_init());
ESP_ERROR_CHECK(espnow_mqtt_set_broker(broker_mac));

espnow_mqtt_topic_handle_t h;
ESP_ERROR_CHECK(espnow_mqtt_register("sensors/node1/temp", &h));
ESP_ERROR_CHECK(espnow_mqtt_wait_registered(15000));

float temp = read_sensor();
ESP_ERROR_CHECK(espnow_mqtt_publish(h, &temp, sizeof(temp)));
```

See `examples/` for complete buildable programs including WiFi init and the
three-phase broker boot sequence.

---

## Build Roles

In a real deployment you always build **two separate firmwares**:

| Firmware | Kconfig role | Effect |
|----------|-------------|--------|
| Sensor / publisher node | `ESPNOW_MQTT_ROLE_PUBLISHER` | Broker code stripped — smaller flash |
| Hub / broker node | `ESPNOW_MQTT_ROLE_BROKER` | Publisher code stripped — smaller flash |

`ESPNOW_MQTT_ROLE_BOTH` compiles both roles into one firmware. This is useful
only during development — to verify the component on a single devkit before
you have two boards. It is the Kconfig default so no configuration is required
to get started. **Do not use it in production firmware.**

Set the role in your project's `sdkconfig.defaults`:

```
# sensor node firmware
CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER=y

# hub node firmware
CONFIG_ESPNOW_MQTT_ROLE_BROKER=y
```

---

## Three-Phase Broker Boot

The broker must complete three phases to avoid missing publishers that boot
before the hub's WiFi connection is established:

| Phase | When | What |
|-------|------|------|
| 1 | Before `esp_wifi_connect()` | `espnow_mqtt_broker_prepare()` — announces on last NVS channel |
| 2 | During WiFi association | Normal WiFi connect — ESP-NOW unavailable |
| 3 | After `IP_EVENT_STA_GOT_IP` | `espnow_mqtt_broker_start()` — full broker online |

Skipping Phase 1 means publishers that wake before the broker's WiFi connection
completes will spend up to `CHANNEL_PROBE_MS × MAX_CHANNEL` scanning before they
find the broker. See `examples/broker/basic_broker` for the complete boot sequence.

---

## Topic Rules

**Publisher topics:**
- Length: 1–247 characters (excluding null terminator)
- Characters: ASCII printable only (0x20–0x7E)
- No `+` or `#`
- No leading `/`, trailing `/`, or `//` (double slash)
- No `espnow/` reserved prefix

**Subscription patterns:**
- All publisher topic rules apply, plus `+` is allowed as a complete path segment
- Example: `sensors/+/temp` matches `sensors/node1/temp` and `sensors/node42/temp`
- `#` multi-level wildcard is not supported in this version

---

## Deployment Contract

`ESPNOW_MQTT_MAX_PUBLISHER_TOPICS` and `ESPNOW_MQTT_MAX_TOPICS` are shared
constraints that both firmwares must agree on:

```
MAX_TOPICS  ≥  expected_publisher_count  ×  MAX_PUBLISHER_TOPICS
```

A build-time `#error` fires if `MAX_TOPICS < MAX_PUBLISHER_TOPICS`. Set both
in each firmware's `sdkconfig.defaults`. A broker built with `MAX_PUBLISHER_TOPICS=2`
will reject the third topic from any publisher built with `MAX_PUBLISHER_TOPICS=4`.

---

## Kconfig Reference

### Deployment contract (both firmwares)

| Symbol | Default | Description |
|--------|---------|-------------|
| `ESPNOW_MQTT_MAX_PUBLISHER_TOPICS` | 1 | Topics per publisher node (1–8). Must match across all nodes. |
| `ESPNOW_MQTT_MAX_TOPICS` | 32 | Total `(mac, topic)` pairs in broker registry (1–254). Must satisfy `≥ publisher_count × MAX_PUBLISHER_TOPICS`. |

### Publisher options

| Symbol | Default | Description |
|--------|---------|-------------|
| `ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS` | y | Always-on publisher task |
| `ESPNOW_MQTT_PUBLISHER_MODE_SLEEP` | n | Blocking wake-publish-sleep API — no task |
| `ESPNOW_MQTT_PUBLISHER_EVENT_QUEUE_SIZE` | 4 | Publish queue depth (2–16) |
| `ESPNOW_MQTT_CHANNEL_PROBE_MS` | 20 | Per-channel probe wait (10–200 ms) |
| `ESPNOW_MQTT_REGISTER_TIMEOUT_MS` | 500 | Single-slot scan budget (ms) |
| `ESPNOW_MQTT_REGISTER_MAX_RETRIES` | 5 | Fast cycles before slow retry |
| `ESPNOW_MQTT_REGISTER_SLOW_INTERVAL_MS` | 30000 | Slow retry interval (ms) |
| `ESPNOW_MQTT_MAX_REDISCOVER_ATTEMPTS` | 0 | 0 = retry forever |
| `ESPNOW_MQTT_MAX_CONSECUTIVE_REJECTIONS` | 10 | Rejections before `perm_rejected` |
| `ESPNOW_MQTT_INTERNAL_ERR_THRESHOLD` | 5 | Consecutive SEND_FAILs before rediscover |
| `ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS` | 30000 | Broker silence before rediscover (ms) |
| `ESPNOW_MQTT_PUBLISHER_KEEPALIVE_MS` | 10000 | Keepalive interval (ms) |
| `ESPNOW_MQTT_BOOT_JITTER_MS` | 500 | Max random boot delay — spreads simultaneous registrations (ms) |
| `ESPNOW_MQTT_MAX_CHANNEL` | 13 | Highest WiFi channel to probe |
| `ESPNOW_MQTT_PUBLISHER_TASK_STACK` | 4096 | Publisher task stack (bytes) |
| `ESPNOW_MQTT_PUBLISHER_TASK_PRIO` | 5 | Publisher task priority |

**Invariant (enforced at `espnow_mqtt_init`):** `KEEPALIVE_MS` < `NO_REPLY_TIMEOUT_MS`

### Broker options

| Symbol | Default | Description |
|--------|---------|-------------|
| `ESPNOW_MQTT_HEAP_TOPICS` | n | Heap-allocate topic strings (default: inline `char[249]`) |
| `ESPNOW_MQTT_DISPATCH_QUEUE_SIZE` | 16 | Frame dispatch queue depth (4–64) |
| `ESPNOW_MQTT_QUEUE_DROP_POLICY` | 0 | 0 = drop newest on full; 1 = drop oldest |
| `ESPNOW_MQTT_MAX_TRACKED_PEERS` | 64 | Publishers tracked for per-MAC seq counting |
| `ESPNOW_MQTT_MAX_SUBSCRIPTIONS` | 16 | Local subscriber callback slots |
| `ESPNOW_MQTT_BROKER_DISPATCH_TASK_STACK` | 4096 | Dispatch task stack (bytes) |
| `ESPNOW_MQTT_BROKER_DISPATCH_TASK_PRIO` | 5 | Dispatch task priority |
| `ESPNOW_MQTT_ANNOUNCE_INTERVAL_MS` | 10000 | BROKER_ANNOUNCE broadcast interval (ms) |
| `ESPNOW_MQTT_PUBLISHER_TIMEOUT_MS` | 90000 | Radio-silence timeout before callback (ms) |

**Invariant:** `ANNOUNCE_INTERVAL_MS` < publisher's `NO_REPLY_TIMEOUT_MS`

### Security

| Symbol | Default | Description |
|--------|---------|-------------|
| `ESPNOW_MQTT_PAYLOAD_HMAC` | n | Enable HMAC-SHA256 payload integrity checking |
| `ESPNOW_MQTT_HMAC_SECRET` | `"change-me-before-deployment"` | Shared HMAC key — replace before production |

---

## Security Model

ESP-NOW frames are transmitted in plaintext. The optional HMAC feature
(`PAYLOAD_HMAC=y`) detects accidental RF corruption and naive replay within
a physically-controlled deployment. It is **not** a security mechanism against
active attackers or eavesdroppers — frames remain readable on-air.

The trust filter (`esp_now_is_peer_exist()`) gates all frame processing on the
ESP-NOW peer table. Frames from MACs not in the peer table are dropped silently
before any parsing occurs.

When `PAYLOAD_HMAC=y` is enabled, the maximum PUBLISH payload is reduced from
246 to 230 bytes (16 bytes consumed by the truncated SHA256 tag). The HMAC
uses mbedTLS software path only — not the hardware HMAC peripheral, which is
absent on plain ESP32 (the typical broker target).

For deployments requiring confidentiality, use ESP-NOW LMK encryption. Key
distribution is outside the scope of this component.

---

## Notes

**Subscriber callback runs on the broker node.** `espnow_mqtt_cb_t` fires from
`broker_dispatch_task`. It may block on NVS, SPI, I2C, or network calls without
starving `recv_cb`. Do not call `espnow_mqtt_subscribe()` or
`espnow_mqtt_unsubscribe()` from inside the callback (deadlock).

**Guard against keepalives.** When the publisher sends a zero-payload PUBLISH
(keepalive), your subscriber callback fires with `payload_len == 0`. Always
check `payload_len` before dereferencing `payload` as typed data.

**Publisher task priority must be below the WiFi task (23).** If
`PUBLISHER_TASK_PRIO ≥ 23`, `esp_now_send()` may deadlock waiting for a
WiFi-task mutex.

**Broker dispatch task stack.** Increase `BROKER_DISPATCH_TASK_STACK` if your
subscriber callbacks perform stack-heavy work (JSON serialisation, MQTT publish,
NVS writes). Default 4096 bytes suits lightweight forwarding.

**Peer table is the trust boundary.** Add publisher peers via `esp_now_add_peer()`
(boot-time) or `espnow_mqtt_add_peer()` (runtime) before `broker_start()`. Any
frame from a MAC not in the peer table is dropped before the component sees it.

**Slots and topic_ids do not reset within a broker boot session.** After 254
unique `(mac, topic)` pairs, new registrations are rejected until the broker
reboots. Size `MAX_TOPICS` conservatively: `publisher_count × MAX_PUBLISHER_TOPICS × 2`
for OTA headroom.

---

## OTA Publisher Replacement

When reflashing a publisher with changed topic strings:

1. Call `espnow_mqtt_purge_mac(old_mac)` on the broker **before** the publisher reboots.
2. The publisher re-registers its new topics and receives fresh `topic_id` assignments.

Without the purge, old `(mac, topic)` entries remain for the current broker boot session.
They are harmless but consume `MAX_TOPICS` slots.

---

## Factory Reset

Clear the broker's NVS channel cache when the WiFi channel changes permanently:

```c
nvs_handle_t h;
nvs_open("espnow_mqtt", NVS_READWRITE, &h);
nvs_erase_key(h, "last_ch");
nvs_commit(h);
nvs_close(h);
```

Or erase the full NVS partition:

```bash
idf.py erase-flash
```

Publishers are unaffected — they always scan all channels when no hint is available.

---

## Ops Monitoring

Watch the broker's log output:

| Message | Level | Meaning | Action |
|---------|-------|---------|--------|
| `"registry full"` | ERROR | `MAX_TOPICS` exhausted | Increase `MAX_TOPICS`; broker reboot required |
| `"malformed topic"` | WARN | Publisher sent invalid topic string | Publisher firmware bug — reflash |
| `"perm_rejected"` | ERROR | Publisher hit consecutive rejection limit | Publisher firmware bug; call `espnow_mqtt_clear_perm_rejected()` |
| `"HMAC verify failed"` | DEBUG | Tag mismatch (`PAYLOAD_HMAC=y` only) | RF corruption or mismatched `HMAC_SECRET` |

`"registry full"` and `"malformed topic"` both result in `REGISTER_ACK{topic_id=0}`.
The publisher cannot distinguish them — **logs are the only way to tell them apart.**

---

## RAM Budget

Approximate static RAM on ESP32 at default config:

| Item | Size |
|------|------|
| Topic registry (32 entries × 264 B) | ~8.5 KB |
| Dispatch queue (16 × 284 B) | ~4.5 KB |
| Subscriber table (16 × ~280 B) | ~4.5 KB |
| Publisher task stack | 4.0 KB |
| Broker dispatch task stack | 4.0 KB |
| **Total approx.** | **~26 KB** |

Reduce `MAX_TOPICS`, `DISPATCH_QUEUE_SIZE`, and task stacks for RAM-constrained
targets. ESP32-C3 has ~400 KB heap; ESP32-S2 ~320 KB heap.

---

## Known Limitations

* No `#` multi-level wildcard in subscriptions (requires wire protocol extension)
* `REGISTER_ACK` carries no rejection reason field — broker logs distinguish causes
* `REGISTER_ACK` and `ID_UNKNOWN` replies sent inline in `recv_cb` (WiFi task context)
* Stale topic GC on REGISTER not automatic — call `espnow_mqtt_purge_mac()` before OTA
* `topic_id` counter does not reset within a broker boot session; wraps at 254

---

## License

MIT — see LICENSE file.