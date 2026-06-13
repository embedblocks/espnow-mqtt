/*
 * broker_main.c — Basic broker example.
 *
 * Demonstrates the three-phase broker boot sequence and subscribes to
 * sensor data from basic_sensor publishers.
 *
 * Three-phase boot (required to avoid missing early publisher registrations):
 *
 *   Phase 1 — Pre-announce on last known channel
 *     WiFi up, ESP-NOW init, broker_prepare(), ESP-NOW deinit.
 *     Sends a best-effort BROKER_ANNOUNCE on the channel stored in NVS.
 *     Publishers that wake before WiFi association can register immediately.
 *
 *   Phase 2 — WiFi connect
 *     Connect to AP to obtain IP. ESP-NOW is unavailable during association.
 *
 *   Phase 3 — Broker start on locked channel
 *     ESP-NOW re-init, add publisher peers, broker_start().
 *     Sends BROKER_ANNOUNCE on the current channel, writes channel to NVS,
 *     starts periodic announce timer, starts dispatch task.
 *
 * Configuration before flashing:
 *   - Set PUBLISHER_MACS[][] to the publisher nodes' MAC addresses.
 *     Publishers print their MAC on boot:
 *       I (xxx) basic_sensor: publisher MAC: AA:BB:CC:DD:EE:FF
 *   - Set WIFI_SSID / WIFI_PASS to your AP credentials (or use menuconfig).
 *   - Build with CONFIG_ESPNOW_MQTT_ROLE_BROKER=y (sdkconfig.defaults sets this).
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "espnow_mqtt.h"

static const char *TAG = "basic_broker";

/* =========================================================================
 * Configuration — edit before flashing
 * ========================================================================= */

/*
 * WiFi AP credentials.
 * Alternatively set via: idf.py menuconfig → Example Configuration.
 */
#ifndef CONFIG_EXAMPLE_WIFI_SSID
#define WIFI_SSID  "YOUR_SSID"
#define WIFI_PASS  "YOUR_PASSWORD"
#else
#define WIFI_SSID  CONFIG_EXAMPLE_WIFI_SSID
#define WIFI_PASS  CONFIG_EXAMPLE_WIFI_PASSWORD
#endif

/* =========================================================================
 * Publisher MAC addresses — MUST configure before building.
 *
 * Step 1: Get each publisher board's MAC address.
 *
 *   Linux / macOS:
 *     esptool.py --port /dev/ttyUSB0 read_mac
 *
 *   Windows:
 *     esptool.py --port COM3 read_mac
 *
 *   (Replace the port with the one your publisher board is connected to.)
 *   The output looks like:  MAC: aa:bb:cc:dd:ee:ff
 *
 * Step 2: Fill in PUBLISHER_MACS below — one row per publisher board.
 *         Add or remove rows to match your deployment.
 *
 * Step 3: Set PUBLISHER_MACS_CONFIGURED to 1.
 *
 * Step 4: Build normally.
 *
 * See examples/broker/basic_broker/README.md for full instructions.
 * ========================================================================= */

/* Set to 1 after filling in PUBLISHER_MACS below. */
#define PUBLISHER_MACS_CONFIGURED 0

#if PUBLISHER_MACS_CONFIGURED == 0
#error "Publisher MAC addresses not configured! "        "Run 'esptool.py --port /dev/ttyUSB0 read_mac' (Linux/macOS) or "        "'esptool.py --port COM3 read_mac' (Windows) on each PUBLISHER board, "        "then fill in PUBLISHER_MACS[][] below and set PUBLISHER_MACS_CONFIGURED to 1. "        "See examples/broker/basic_broker/README.md for full instructions."
#endif

/*
 * One row per publisher board. Fill in after running esptool.py read_mac.
 * Example: if esptool prints "MAC: 11:22:33:44:55:66", add:
 *   {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
 */
static const uint8_t PUBLISHER_MACS[][6] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* publisher 1 — replace */
    /* {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, */  /* publisher 2 — uncomment and set */
};
#define PUBLISHER_COUNT  (sizeof(PUBLISHER_MACS) / sizeof(PUBLISHER_MACS[0]))

/* =========================================================================
 * WiFi event handling
 * ========================================================================= */

static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRIES    5

static int s_wifi_retry = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry < WIFI_MAX_RETRIES) {
            s_wifi_retry++;
            ESP_LOGW(TAG, "WiFi disconnected — retry %d/%d",
                     s_wifi_retry, WIFI_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected — IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_retry = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

/* =========================================================================
 * Subscriber callback — called from broker_dispatch_task
 * ========================================================================= */

static void on_sensor_data(const char *topic,
                            const uint8_t *payload, size_t payload_len,
                            const uint8_t sender_mac[6],
                            uint16_t seq, void *user_ctx)
{
    (void)user_ctx;

    /* Guard against keepalives (zero-payload PUBLISHes). */
    if (payload_len == 0) {
        ESP_LOGD(TAG, "keepalive: topic=%s mac=%02x:...", topic, sender_mac[0]);
        return;
    }

    /* Expect a single float. */
    if (payload_len != sizeof(float)) {
        ESP_LOGW(TAG, "unexpected payload_len=%zu for topic=%s",
                 payload_len, topic);
        return;
    }

    float value;
    memcpy(&value, payload, sizeof(float));

    ESP_LOGI(TAG, "rx topic=%-35s  val=%7.2f  seq=%5u  mac=%02x:%02x:%02x:%02x:%02x:%02x",
             topic, (double)value, seq,
             sender_mac[0], sender_mac[1], sender_mac[2],
             sender_mac[3], sender_mac[4], sender_mac[5]);
}

/* =========================================================================
 * Peer lifecycle callback (informational)
 * ========================================================================= */

static void on_peer_event(espnow_mqtt_peer_event_t event,
                           const uint8_t mac[6],
                           const char *topic, void *ctx)
{
    (void)ctx;
    const char *ev_str =
        (event == ESPNOW_MQTT_PEER_EVENT_REGISTERED)   ? "REGISTERED"   :
        (event == ESPNOW_MQTT_PEER_EVENT_REREGISTERED) ? "REREGISTERED" : "TIMEOUT";
    ESP_LOGI(TAG, "peer %s: %02x:%02x:%02x:%02x:%02x:%02x  topic=%s",
             ev_str,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], topic);
}

/* =========================================================================
 * app_main
 * ========================================================================= */

void app_main(void)
{
    ESP_LOGI(TAG, "espnow_mqtt basic_broker — version %s",
             espnow_mqtt_get_version());

    /* Print own MAC so publishers can be configured with it. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "broker MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* NVS — required by WiFi stack and by espnow_mqtt NVS channel cache. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* ------------------------------------------------------------------ */
    /* Phase 1 — pre-announce on last known channel                        */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Phase 1: pre-announce");

    ESP_ERROR_CHECK(esp_now_init());
    ret = espnow_mqtt_broker_prepare();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "broker_prepare: %s", esp_err_to_name(ret));
    }
    ESP_ERROR_CHECK(esp_now_deinit());

    /* ------------------------------------------------------------------ */
    /* Phase 2 — connect to WiFi AP                                        */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Phase 2: WiFi connect");

    s_wifi_eg = xEventGroupCreate();

    esp_event_handler_instance_t inst_wifi, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &inst_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &inst_ip));

    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid,     WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASS,  sizeof(wifi_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connect failed after %d retries — halting",
                 WIFI_MAX_RETRIES);
        return;
    }
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi connect timeout — halting");
        return;
    }

    /* ------------------------------------------------------------------ */
    /* Phase 3 — broker start on locked channel                            */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "Phase 3: broker start");

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(espnow_mqtt_init());

    /* Add known publisher peers (boot-time: use raw esp_now_add_peer). */
    for (size_t i = 0; i < PUBLISHER_COUNT; i++) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, PUBLISHER_MACS[i], 6);
        peer.channel = 0;
        peer.encrypt  = false;
        ret = esp_now_add_peer(&peer);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "add_peer[%zu] failed: %s", i, esp_err_to_name(ret));
        }
    }

    /* Register callbacks before broker_start so no events are missed. */
    ESP_ERROR_CHECK(espnow_mqtt_set_peer_event_cb(on_peer_event, NULL));

    ESP_ERROR_CHECK(espnow_mqtt_broker_start());

    /* Subscribe to sensor topics. */
    espnow_mqtt_sub_handle_t h_temp, h_hum;
    ESP_ERROR_CHECK(espnow_mqtt_subscribe("sensors/+/temp",     on_sensor_data,
                                           NULL, &h_temp));
    ESP_ERROR_CHECK(espnow_mqtt_subscribe("sensors/+/humidity", on_sensor_data,
                                           NULL, &h_hum));

    ESP_LOGI(TAG, "broker running — subscribed to sensors/+/temp and sensors/+/humidity");

    /* Periodic stats log every 60 s. */
    uint32_t last_stats_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - last_stats_ms) >= 60000) {
            last_stats_ms = now_ms;
            espnow_mqtt_stats_t stats;
            if (espnow_mqtt_get_stats(&stats) == ESP_OK) {
                ESP_LOGI(TAG,
                    "stats: rx=%lu trusted=%lu rejected=%lu dispatched=%lu "
                    "dropped=%lu id_unk=%lu reg=%lu gaps=%lu reorder=%lu",
                    (unsigned long)stats.frames_received,
                    (unsigned long)stats.frames_trusted,
                    (unsigned long)stats.frames_rejected_trust,
                    (unsigned long)stats.frames_dispatched,
                    (unsigned long)stats.frames_dropped_queue,
                    (unsigned long)stats.frames_id_unknown,
                    (unsigned long)stats.registers_received,
                    (unsigned long)stats.seq_gaps,
                    (unsigned long)stats.seq_reordered);
            }
        }
    }
}
