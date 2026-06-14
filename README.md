# espnow_mqtt

Lightweight MQTT-like publish/subscribe over ESP-NOW for ESP-IDF ≥ 5.5.0.

---

## Overview

This component implements a **two-node-type architecture**. There is no third
party involved — the broker and the subscriber are the same physical node.

```
┌─────────────────────────────────┐       ESP-NOW        ┌──────────────────────────────────────┐
│  Publisher node                 │ ───────────────────► │  Broker node                         │
│                                 │                       │                                      │
│  • Sensor firmware              │                       │  • WiFi STA connected to AP          │
│  • No WiFi AP connection        │                       │  • Hosts the subscriber callbacks    │
│  • Battery powered              │                       │  • Forwards data upstream            │
│  • ESP32-C3 / ESP32-S3 etc.     │                       │  • ESP32 / ESP32-S3                  │
└─────────────────────────────────┘                       └──────────────────────────────────────┘
         sends sensor readings                                   receives + acts on them
         via ESP-NOW frames                                      (log, MQTT, HTTP, DB…)
```

**Publisher nodes** are sensor devices. They register their topic strings with
the broker once, then publish data using only a 1-byte `topic_id` — no string
overhead on the wire after the first exchange. They never join a WiFi AP.

**The broker node** is the single WiFi-connected hub. It maintains a registry
of `(publisher_mac, topic_string) → topic_id` and delivers incoming PUBLISH
frames to **subscriber callbacks registered on the same node**. The subscriber
is not a separate device — it is application code running on the broker node
that receives the data and decides what to do with it (forward to MQTT, write
to a database, trigger an actuator, etc.).

```
Broker node internals:
  recv_cb (WiFi task)
    └── REGISTER frames → registry → send REGISTER_ACK
    └── PUBLISH frames  → dispatch queue
          └── broker_dispatch_task
                └── subscriber callback 1: on_sensor_data() → forward to MQTT
                └── subscriber callback 2: on_sensor_data() → write to SD card
```

Frames are ≤ 250 bytes (ESP-NOW maximum). No fragmentation.

---

## Quick Start

### Broker side (≤ 15 lines)

```c
ESP_ERROR_CHECK(esp_now_init());
ESP_ERROR_CHECK(espnow_mqtt_init());

// Add each known publisher peer (boot-time)
esp_now_peer_info_t peer = { .peer_addr = {0x11,…}, .encrypt = false };
ESP_ERROR_CHECK(esp_now_add_peer(&peer));

ESP_ERROR_CHECK(espnow_mqtt_broker_start());

espnow_mqtt_sub_handle_t h;
ESP_ERROR_CHECK(espnow_mqtt_subscribe("sensors/+/temp", on_data, NULL, &h));
```

### Publisher side (≤ 15 lines)

```c
ESP_ERROR_CHECK(esp_now_init());
ESP_ERROR_CHECK(espnow_mqtt_init());
ESP_ERROR_CHECK(espnow_mqtt_set_broker(broker_mac));

espnow_mqtt_topic_handle_t h;
ESP_ERROR_CHECK(espnow_mqtt_register("sensors/node1/temp", &h));
ESP_ERROR_CHECK(espnow_mqtt_wait_registered(15000));

float temp = read_sensor();
ESP_ERROR_CHECK(espnow_mqtt_publish(h, &temp, sizeof(temp)));
```

See `examples/` for complete, buildable programs including WiFi init and the
three-phase broker boot sequence.

---

## Three-Phase Broker Boot

The broker must complete three phases to avoid missing publisher registrations
that occur before WiFi is up:

| Phase | When | What |
|-------|------|------|
| 1 | Before `esp_wifi_connect()` | `espnow_mqtt_broker_prepare()` — announces on last NVS channel |
| 2 | During WiFi association | Normal WiFi connect (ESP-NOW unavailable) |
| 3 | After `IP_EVENT_STA_GOT_IP` | `espnow_mqtt_broker_start()` — full broker online |

If you skip Phase 1, publishers that boot before the broker's WiFi connection
completes will not receive a `BROKER_ANNOUNCE` and will spend `CHANNEL_PROBE_MS
× MAX_CHANNEL` scanning before they find the broker.

---

## Topic Rules

**Publisher topics** (`is_pattern = false`):
- Length: 1–247 characters (excluding null)
- Characters: ASCII printable (0x20–0x7E)
- No `+` or `#`
- No leading `/`, trailing `/`, or `//`
- No `espnow/` reserved prefix

**Subscription patterns** (`is_pattern = true`):
- Same rules as above, plus `+` is allowed as a complete path segment
  (e.g. `sensors/+/temp` matches `sensors/node1/temp`)
- `#` wildcard is not supported in this version

---

## Kconfig Reference

### Role

In a real deployment you always build two separate firmwares:

| Firmware | Role to set | Result |
|----------|-------------|--------|
| Sensor / publisher node | `ESPNOW_MQTT_ROLE_PUBLISHER` | Broker code stripped, smaller flash |
| Hub / broker node | `ESPNOW_MQTT_ROLE_BROKER` | Publisher code stripped, smaller flash |

The `ROLE_BOTH` option compiles publisher and broker code into the same
firmware. **This is only useful during development** — for example to run both
roles on a single devkit to verify the component works before you have two
boards. It is the Kconfig default purely because it requires no configuration
to build. Do not use it in production firmware.

| Symbol | Type | Default | Notes |
|--------|------|---------|-------|
| `ESPNOW_MQTT_ROLE_PUBLISHER` | bool | n | **Use this for all sensor nodes** |
| `ESPNOW_MQTT_ROLE_BROKER` | bool | n | **Use this for the hub node** |
| `ESPNOW_MQTT_ROLE_BOTH` | bool | y | Development / single-board testing only |

### Publisher Options

| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS` | bool | y | Always-on publisher task |
| `ESPNOW_MQTT_PUBLISHER_MODE_SLEEP` | bool | n | Wake-publish-sleep (no task) |
| `ESPNOW_MQTT_MAX_PUBLISHER_TOPICS` | int | 1 | Topic slots per publisher (1–8) |
| `ESPNOW_MQTT_PUBLISHER_EVENT_QUEUE_SIZE` | int | 4 | Publish queue depth (2–16) |
| `ESPNOW_MQTT_CHANNEL_PROBE_MS` | int | 20 | Per-channel probe wait (10–200 ms) |
| `ESPNOW_MQTT_REGISTER_TIMEOUT_MS` | int | 500 | Single-slot scan budget (ms) |
| `ESPNOW_MQTT_REGISTER_MAX_RETRIES` | int | 5 | Cycles before slow retry |
| `ESPNOW_MQTT_REGISTER_SLOW_INTERVAL_MS` | int | 30000 | Slow retry period (ms) |
| `ESPNOW_MQTT_MAX_REDISCOVER_ATTEMPTS` | int | 0 | 0 = retry forever |
| `ESPNOW_MQTT_MAX_CONSECUTIVE_REJECTIONS` | int | 10 | Rejections before perm_rejected |
| `ESPNOW_MQTT_INTERNAL_ERR_THRESHOLD` | int | 5 | SEND_FAILs before rediscover |
| `ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS` | int | 30000 | Silence before rediscover (ms) |
| `ESPNOW_MQTT_PUBLISHER_KEEPALIVE_MS` | int | 10000 | Keepalive interval (ms) |
| `ESPNOW_MQTT_BOOT_JITTER_MS` | int | 500 | Max random boot delay (ms) |
| `ESPNOW_MQTT_MAX_CHANNEL` | int | 13 | Highest channel to probe |
| `ESPNOW_MQTT_PUBLISHER_TASK_STACK` | int | 4096 | Publisher task stack (bytes) |
| `ESPNOW_MQTT_PUBLISHER_TASK_PRIO` | int | 5 | Publisher task priority |

**Invariant (enforced at init):** `KEEPALIVE_MS` < `NO_REPLY_TIMEOUT_MS`

### Broker Options

| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `ESPNOW_MQTT_MAX_TOPICS` | int | 32 | Max `(mac, topic)` pairs (1–254) |
| `ESPNOW_MQTT_HEAP_TOPICS` | bool | n | Heap-allocate topic strings |
| `ESPNOW_MQTT_DISPATCH_QUEUE_SIZE` | int | 16 | Frame queue depth (4–64) |
| `ESPNOW_MQTT_QUEUE_DROP_POLICY` | int | 0 | 0 = drop newest, 1 = drop oldest |
| `ESPNOW_MQTT_MAX_TRACKED_PEERS` | int | 64 | Per-MAC seq tracking slots |
| `ESPNOW_MQTT_MAX_SUBSCRIPTIONS` | int | 16 | Local subscriber slots |
| `ESPNOW_MQTT_BROKER_DISPATCH_TASK_STACK` | int | 4096 | Dispatch task stack (bytes) |
| `ESPNOW_MQTT_BROKER_DISPATCH_TASK_PRIO` | int | 5 | Dispatch task priority |
| `ESPNOW_MQTT_ANNOUNCE_INTERVAL_MS` | int | 10000 | Announce broadcast interval (ms) |
| `ESPNOW_MQTT_PUBLISHER_TIMEOUT_MS` | int | 90000 | Radio-silence timeout (ms) |

**Invariant:** `ANNOUNCE_INTERVAL_MS` < publisher's `NO_REPLY_TIMEOUT_MS`

### Security

| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `ESPNOW_MQTT_PAYLOAD_HMAC` | bool | n | Enable HMAC-SHA256 payload integrity |
| `ESPNOW_MQTT_HMAC_SECRET` | string | `"change-me-before-deployment"` | HMAC key |

---

## Security Model

ESP-NOW frames are transmitted in plaintext. The optional HMAC feature
(`PAYLOAD_HMAC=y`) detects accidental RF corruption and naive replay within
a physically-controlled deployment — it is **not** a security mechanism
against active attackers or eavesdroppers.

The trust filter (`esp_now_is_peer_exist()`) gates all processing on the MAC
peer table. Frames from MACs not in the peer table are dropped silently before
any parsing.

For deployments requiring confidentiality, use ESP-NOW LMK encryption (requires
key distribution infrastructure outside this component's scope).

See `docs/05_security.md` for the full threat model.

---

## Factory Reset

If the broker's NVS channel cache becomes stale (e.g. after changing the WiFi
channel), erase it with:

```c
nvs_handle_t h;
nvs_open("espnow_mqtt", NVS_READWRITE, &h);
nvs_erase_key(h, "last_ch");
nvs_commit(h);
nvs_close(h);
```

Or erase the entire NVS partition:

```
idf.py erase-flash
```

After a factory reset the broker will scan all channels on next boot.
Publishers are unaffected — they always scan all channels when no hint is
available.

---

## OTA Publisher Replacement

When replacing a publisher node via OTA with different topic strings:

1. Call `espnow_mqtt_purge_mac(old_publisher_mac)` on the broker before
   the publisher reboots. This clears stale registry entries.
2. The newly-booted publisher will REGISTER its new topics and receive fresh
   `topic_id` assignments.

If you skip the purge, the old entries remain in the registry for the current
broker boot session (they are harmless but consume `MAX_TOPICS` slots).

---

## Ops Monitoring

Watch the broker UART output for these log lines:

| Log | Level | Meaning | Action |
|-----|-------|---------|--------|
| `"registry full"` | ERROR | `MAX_TOPICS` exhausted | Increase `MAX_TOPICS` or `BOOT_JITTER_MS`; broker reboot required to clear |
| `"malformed topic"` | WARN | Publisher sent invalid topic string | Publisher firmware bug — reflash |
| `"perm_rejected"` | ERROR | Publisher hit rejection limit | Publisher firmware bug — reflash; clear with `espnow_mqtt_clear_perm_rejected()` |
| `"HMAC verify failed"` | DEBUG | Tag mismatch (HMAC=y only) | RF corruption or wrong secret; check `HMAC_SECRET` matches on both sides |

Both `"registry full"` and `"malformed topic"` cause `topic_id=0` in the
`REGISTER_ACK` — the publisher cannot distinguish the cause. **Logs are the
only way to tell them apart.**

Rejection reason code in `REGISTER_ACK` is noted as a future wire-format
candidate (see `CHANGELOG.md`).

---

## RAM Budget (ESP32, default config)

| Item | Size |
|------|------|
| Topic registry (32 entries × ~264 B) | ~8.5 KB static |
| Dispatch queue (16 × 284 B) | ~4.5 KB static |
| Subscriber table (16 × ~280 B) | ~4.5 KB static |
| Publisher task stack | 4 KB |
| Broker dispatch task stack | 4 KB |
| **Total approx.** | **~26 KB** |

Reduce `MAX_TOPICS`, `DISPATCH_QUEUE_SIZE`, and task stacks to fit
RAM-constrained targets (ESP32-C3: 400 KB heap; ESP32-S2: 320 KB heap).