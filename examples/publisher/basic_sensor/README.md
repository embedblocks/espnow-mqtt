# basic_sensor — Publisher Example

Demonstrates a CONTINUOUS-mode publisher registering two topics
(`sensors/node1/temp` and `sensors/node1/humidity`) and publishing
synthetic float readings every 5 seconds.

**Build will fail with a clear error message until you configure the broker MAC.
This is intentional — see Setup below.**

---

## Hardware

Any ESP32-family board supported by the component.
Tested on ESP32-C3-DevKitM-1.

---

## Setup

### Step 1 — Get the broker board's MAC address

Connect the **broker** board to your computer and run:

**Linux / macOS:**
```bash
esptool.py --port /dev/ttyUSB0 read_mac
```

**Windows:**
```bash
esptool.py --port COM3 read_mac
```

> Replace `/dev/ttyUSB0` or `COM3` with the actual port your broker board is on.
> On Linux, check `ls /dev/ttyUSB*` or `ls /dev/ttyACM*`.
> On Windows, check Device Manager → Ports (COM & LPT).

Example output:
```
esptool.py v4.7.0
...
MAC: aa:bb:cc:dd:ee:ff
```

### Step 2 — Configure the MAC in source

Open `main/publisher_main.c` and make two edits:

```c
/* Change this from 0 to 1 */
#define BROKER_MAC_CONFIGURED 1

/* Fill in the six bytes from esptool output */
/* "MAC: aa:bb:cc:dd:ee:ff"  →  {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF} */
static const uint8_t BROKER_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
```

If you skip this step and try to build, you will get:
```
error: "Broker MAC address not configured! Run 'esptool.py --port /dev/ttyUSB0 read_mac'..."
```

### Step 3 — Build and flash

```bash
cd examples/publisher/basic_sensor
idf.py set-target esp32c3   # or esp32, esp32s3, esp32c6
idf.py build flash monitor
```

---

## Expected UART Output

```
I (xxx) basic_sensor: espnow_mqtt basic_sensor — version 0.1.0
I (xxx) basic_sensor: publisher MAC: 11:22:33:44:55:66
I (xxx) basic_sensor: waiting for broker registration (up to 15 s)...
I (xxx) basic_sensor: registered on ch 6 — starting publish loop
I (xxx) basic_sensor: pub temp=20.1°C
I (xxx) basic_sensor: pub hum=50.0%
I (xxx) basic_sensor: pub temp=20.2°C
...
```

Note the **publisher MAC** printed on the second line — you will need this
when configuring the broker example (`PUBLISHER_MACS[]`).

If registration fails within 15 s (broker not yet running), the publisher
continues trying asynchronously. Publishing will succeed once it registers.

---

## Flash Order

**Flash the broker first.** The broker must be running and have this publisher's
MAC in its `PUBLISHER_MACS[]` before the publisher boots — otherwise the broker
drops the publisher's frames (trust filter).

1. Note the publisher MAC printed on boot (line 2 of the output above)
2. Add it to `PUBLISHER_MACS[]` in `examples/broker/basic_broker/main/broker_main.c`
3. Flash and start the broker
4. Flash and start this publisher

---

## sdkconfig.defaults

```
CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER=y
CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS=y
CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS=2
CONFIG_ESPNOW_MQTT_BOOT_JITTER_MS=500
```
