# Changelog

All notable changes to espnow_mqtt will be documented here.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning: [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

## [0.1.0] — Initial release

### Added

**Wire protocol**
- 5-message protocol: REGISTER, REGISTER_ACK, PUBLISH, ID_UNKNOWN, BROKER_ANNOUNCE
- 250-byte frame discipline — no fragmentation
- 1-byte `topic_id` negotiation: string overhead on first frame only
- Little-endian 16-bit sequence counter in PUBLISH header

**Publisher role**
- CONTINUOUS mode: persistent publisher_task, channel scan, keepalive timer,
  no-reply timeout, exponential/slow-interval backoff, rediscovery
- SLEEP mode: blocking `register_sync()` / `publish_sync()` API, RTC state
  persistence across deep-sleep wakes
- Boot jitter: configurable random delay to spread simultaneous registrations
- Permanent-rejection detection: `perm_rejected` flag after N consecutive
  `REGISTER_ACK{topic_id=0}` responses; clearable via `espnow_mqtt_clear_perm_rejected()`

**Broker role**
- Static topic registry: `(publisher_mac, topic_string) → topic_id` mapping
- Subscriber dispatch task with `+` wildcard pattern matching
- Per-publisher sequence gap and duplicate detection (always-dispatch contract)
- Three-phase boot: `broker_prepare()` pre-announces on last NVS channel before
  WiFi connect; `broker_start()` goes fully online after IP assignment
- NVS channel persistence (`espnow_mqtt` namespace, `last_ch` key)
- Publisher radio-silence timeout callback
- Publisher lifecycle events: REGISTERED, REREGISTERED, TIMEOUT
- `espnow_mqtt_purge_mac()` for OTA publisher replacement
- Peer wrapper API: `espnow_mqtt_add_peer()` / `del_peer()` for runtime changes

**Security**
- MAC-based trust filter: frames from non-peer MACs dropped before any parsing
- Optional HMAC-SHA256 payload integrity (`PAYLOAD_HMAC=y`):
  16-byte truncated tag prepended to every PUBLISH payload; constant-time
  XOR-accumulate verify; mbedTLS software path only (not hardware HMAC peripheral)

**Build system**
- Role-split builds: `ROLE_PUBLISHER` and `ROLE_BROKER` strip the irrelevant
  `.c` file entirely from the link (not just `#if` guards)
- IDF ≥ 5.5.0 required (recv_cb struct form since 5.0; send_cb struct form since 5.5)
- Targets: esp32, esp32s3, esp32c3, esp32c6, esp32s2 (publisher role only on S2)

### Known Limitations

- No LMK/ESP-NOW encryption (requires key distribution infrastructure)
- No `#` wildcard in subscriptions (requires wire protocol extension)
- `REGISTER_ACK` carries no `rejection_reason` field — broker logs are the only
  way to distinguish `"registry full"` from `"malformed topic"`
- `REGISTER_ACK` and `ID_UNKNOWN` replies are sent inline in `recv_cb` (WiFi task)
  rather than deferred to a dispatch queue; a slow `esp_now_send()` stalls recv_cb
- Stale topic GC on REGISTER is not automatic — call `espnow_mqtt_purge_mac()`
  before an OTA reboot that changes topic strings
- `topic_id` counter does not reset within a broker boot session; wraps at 254
  (broker reboot required after 254 unique `(mac, topic)` pairs)
- HMAC secret is compiled into firmware; no per-device derivation in this version

### Future Candidates

- Dual-queue architecture: separate control queue (REGISTER_ACK, ID_UNKNOWN)
  from the PUBLISH dispatch queue to prevent control starvation under high load
- `rejection_reason` field in `REGISTER_ACK` wire format (breaks wire compatibility)
- Per-device HMAC secret derivation from MAC + master secret via eFuse
- `#` multi-level wildcard (requires wire protocol version bump)
