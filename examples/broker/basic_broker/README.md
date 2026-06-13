# basic_broker — Broker Example

Demonstrates the three-phase broker boot sequence. Subscribes to
`sensors/+/temp` and `sensors/+/humidity` and logs every received value.

**Build will fail with a clear error message until you configure publisher MACs
and WiFi credentials. This is intentional — see Setup below.**

---

## Hardware

Any ESP32-family board with WiFi. Tested on ESP32-DevKitC-V4.
Not recommended on ESP32-S2 (RAM headroom for broker role is tight).

---

## Topology

```
┌─────────────────┐        ESP-NOW        ┌──────────────────────┐
│  basic_sensor   │ ────────────────────► │    basic_broker      │
│  (publisher)    │                       │    (broker)          │
│  No WiFi AP     │                       │    WiFi STA + AP ──► │──► Network
└─────────────────┘                       └──────────────────────┘
```

---

## Setup

### Step 1 — Get each publisher board's MAC address

Connect each **publisher** board to your computer and run:

**Linux / macOS:**
```bash
esptool.py --port /dev/ttyUSB0 read_mac
```

**Windows:**
```bash
esptool.py --port COM3 read_mac
```

> Replace `/dev/ttyUSB0` or `COM3` with the actual port your publisher board is on.
> On Linux, check `ls /dev/ttyUSB*` or `ls /dev/ttyACM*`.
> On Windows, check Device Manager → Ports (COM & LPT).

Example output:
```
esptool.py v4.7.0
...
MAC: 11:22:33:44:55:66
```

Alternatively, flash and run the `basic_sensor` example first — it prints its
MAC on the second line of boot output:
```
I (xxx) basic_sensor: publisher MAC: 11:22:33:44:55:66
```

### Step 2 — Configure publisher MACs in source

Open `main/broker_main.c` and make two edits:

```c
/* Change this from 0 to 1 */
#define PUBLISHER_MACS_CONFIGURED 1

/* Fill in one row per publisher board */
/* "MAC: 11:22:33:44:55:66"  →  {0x11, 0x22, 0x33, 0x44, 0x55, 0x66} */
static const uint8_t PUBLISHER_MACS[][6] = {
    {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},   /* publisher 1 */
    {0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC},   /* publisher 2 — add/remove rows as needed */
};
```

If you skip this step and try to build, you will get:
```
error: "Publisher MAC addresses not configured! Run 'esptool.py --port /dev/ttyUSB0 read_mac'..."
```

### Step 3 — Configure WiFi credentials

In `main/broker_main.c`:

```c
#define WIFI_SSID  "your-network-name"
#define WIFI_PASS  "your-network-password"
```

### Step 4 — Build and flash

```bash
cd examples/broker/basic_broker
idf.py set-target esp32
idf.py build flash monitor
```

---

## Expected UART Output

```
I (xxx) basic_broker: espnow_mqtt basic_broker — version 0.1.0
I (xxx) basic_broker: broker MAC: AA:BB:CC:DD:EE:FF
I (xxx) basic_broker: Phase 1: pre-announce
I (xxx) basic_broker: Phase 2: WiFi connect
I (xxx) basic_broker: WiFi connected — IP: 192.168.1.42
I (xxx) basic_broker: Phase 3: broker start
I (xxx) basic_broker: broker running — subscribed to sensors/+/temp and sensors/+/humidity
I (xxx) basic_broker: peer REGISTERED: 11:22:33:44:55:66  topic=sensors/node1/temp
I (xxx) basic_broker: peer REGISTERED: 11:22:33:44:55:66  topic=sensors/node1/humidity
I (xxx) basic_broker: rx topic=sensors/node1/temp          val=  20.10  seq=    0  mac=11:22:33:44:55:66
I (xxx) basic_broker: rx topic=sensors/node1/humidity      val=  50.00  seq=    1  mac=11:22:33:44:55:66
```

Note the **broker MAC** printed on the second line — you will need this
when configuring the publisher example (`BROKER_MAC[]`).

---

## Flash Order

**Flash the broker first.**

1. Note the broker MAC printed on boot (line 2 of the output above)
2. Put it in `BROKER_MAC[]` in `examples/publisher/basic_sensor/main/publisher_main.c`
3. This broker must already be running before publishers boot

---

## Factory Reset

To clear the stored channel from NVS (e.g. after changing WiFi channel):

**Via command line:**
```bash
idf.py erase-flash
```

**Selectively (from application code):**
```c
nvs_handle_t h;
nvs_open("espnow_mqtt", NVS_READWRITE, &h);
nvs_erase_key(h, "last_ch");
nvs_commit(h);
nvs_close(h);
```

---

## sdkconfig.defaults

```
CONFIG_ESPNOW_MQTT_ROLE_BROKER=y
CONFIG_ESPNOW_MQTT_MAX_TOPICS=32
CONFIG_ESPNOW_MQTT_MAX_SUBSCRIPTIONS=8
CONFIG_ESPNOW_MQTT_DISPATCH_QUEUE_SIZE=16
CONFIG_ESPNOW_MQTT_ANNOUNCE_INTERVAL_MS=10000
```
