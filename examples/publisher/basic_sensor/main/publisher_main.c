/*
 * publisher_main.c — Basic sensor publisher example.
 *
 * Demonstrates a CONTINUOUS-mode publisher registering two topics
 * (temperature + humidity) and publishing synthetic float readings every 5 s.
 *
 * Three-step startup:
 *   1. Init WiFi STA (no AP connect — publisher never joins an AP)
 *   2. espnow_mqtt_init() + set_broker() + register() both topics
 *   3. wait_registered(15 s), then publish loop
 *
 * Configuration before flashing:
 *   - Set BROKER_MAC[] to the broker node's MAC address.
 *     The broker prints its MAC on boot:
 *       I (xxx) basic_broker: broker MAC: AA:BB:CC:DD:EE:FF
 *   - Build with CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER=y (sdkconfig.defaults sets this).
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "espnow_mqtt.h"

static const char *TAG = "basic_sensor";

/* =========================================================================
 * Configuration — MUST edit before building
 * =========================================================================
 *
 * Step 1: Get the broker board's MAC address.
 *
 *   Linux / macOS:
 *     esptool.py --port /dev/ttyUSB0 read_mac
 *
 *   Windows:
 *     esptool.py --port COM3 read_mac
 *
 *   (Replace the port with the one your broker board is connected to.)
 *   The output looks like:  MAC: aa:bb:cc:dd:ee:ff
 *
 * Step 2: Fill in BROKER_MAC below with those six bytes.
 *
 * Step 3: Set BROKER_MAC_CONFIGURED to 1.
 *
 * Step 4: Build normally.
 *
 * See examples/publisher/basic_sensor/README.md for full instructions.
 * ========================================================================= */

/* Set to 1 after filling in BROKER_MAC below. */
#define BROKER_MAC_CONFIGURED 0

#if BROKER_MAC_CONFIGURED == 0
#error "Broker MAC address not configured! "        "Run 'esptool.py --port /dev/ttyUSB0 read_mac' (Linux/macOS) or "        "'esptool.py --port COM3 read_mac' (Windows) on the BROKER board, "        "then fill in BROKER_MAC[] below and set BROKER_MAC_CONFIGURED to 1. "        "See examples/publisher/basic_sensor/README.md for full instructions."
#endif

/*
 * Broker MAC address — fill in after running esptool.py read_mac on the broker.
 * Example: if esptool prints "MAC: aa:bb:cc:dd:ee:ff", set:
 *   static const uint8_t BROKER_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
 */
static const uint8_t BROKER_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/* Publish interval (ms). */
#define PUBLISH_INTERVAL_MS   5000

/* Stats log interval (ms). */
#define STATS_LOG_INTERVAL_MS 60000

/* =========================================================================
 * WiFi init — STA mode, no AP association
 * ========================================================================= */

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Do NOT call esp_wifi_connect() — publisher never joins an AP. */
}

/* =========================================================================
 * Subscriber callback (not used on publisher, but peer_event_cb is)
 * ========================================================================= */

static void on_publisher_state_change(espnow_mqtt_peer_event_t event,
                                       const uint8_t mac[6],
                                       const char *topic, void *ctx)
{
    /* Not applicable on a publisher-only build — this callback is a
     * broker-side API and will never fire here. Included for illustration. */
    (void)event; (void)mac; (void)topic; (void)ctx;
}

/* =========================================================================
 * Synthetic sensor readings
 * ========================================================================= */

static float next_temperature(void)
{
    /* Simulate a slowly-varying temperature: 20–30 °C sawtooth. */
    static float base = 20.0f;
    base += 0.1f;
    if (base > 30.0f) base = 20.0f;
    return base;
}

static float next_humidity(void)
{
    /* Simulate humidity oscillating 40–60 %. */
    static uint32_t tick = 0;
    return 50.0f + 10.0f * sinf((float)(tick++) * 0.1f);
}

/* =========================================================================
 * app_main
 * ========================================================================= */

void app_main(void)
{
    ESP_LOGI(TAG, "espnow_mqtt basic_sensor — version %s",
             espnow_mqtt_get_version());

    /* Print own MAC so the broker operator can add this node as a peer. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "publisher MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* NVS required by WiFi stack. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    ESP_ERROR_CHECK(esp_now_init());

    /* Add broker peer — application responsibility. */
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROKER_MAC, 6);
    peer.channel = 0;   /* 0 = use current channel */
    peer.encrypt  = false;
    esp_err_t add_ret = esp_now_add_peer(&peer);
    if (add_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %s — check BROKER_MAC",
                 esp_err_to_name(add_ret));
        return;
    }

    /* Init component. */
    ESP_ERROR_CHECK(espnow_mqtt_init());

    /* Designate the broker. */
    ESP_ERROR_CHECK(espnow_mqtt_set_broker(BROKER_MAC));

    /* Register topics. */
    espnow_mqtt_topic_handle_t h_temp, h_hum;
    ESP_ERROR_CHECK(espnow_mqtt_register("sensors/node1/temp",     &h_temp));
    ESP_ERROR_CHECK(espnow_mqtt_register("sensors/node1/humidity", &h_hum));

    ESP_LOGI(TAG, "waiting for broker registration (up to 15 s)...");
    ret = espnow_mqtt_wait_registered(15000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wait_registered: %s — will publish when registered",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "registered on ch %d — starting publish loop",
                 espnow_mqtt_get_broker_channel());
    }

    /* Publish loop. */
    uint32_t last_stats_ms = 0;
    while (1) {
        float temp = next_temperature();
        float hum  = next_humidity();

        /* Publish temperature. */
        ret = espnow_mqtt_publish(h_temp, &temp, sizeof(temp));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "pub temp=%.1f°C", (double)temp);
        } else {
            ESP_LOGW(TAG, "pub temp failed: %s", esp_err_to_name(ret));
        }

        /* Publish humidity. */
        ret = espnow_mqtt_publish(h_hum, &hum, sizeof(hum));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "pub hum=%.1f%%", (double)hum);
        } else {
            ESP_LOGW(TAG, "pub hum failed: %s", esp_err_to_name(ret));
        }

        /* Periodic stats log. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - last_stats_ms) >= STATS_LOG_INTERVAL_MS) {
            last_stats_ms = now_ms;
            espnow_mqtt_publisher_stats_t stats;
            if (espnow_mqtt_get_publisher_stats(&stats) == ESP_OK) {
                ESP_LOGI(TAG,
                    "stats: sent=%lu no_ack=%lu not_found=%lu "
                    "no_mem=%lu q_full=%lu rediscover=%lu register=%lu",
                    (unsigned long)stats.frames_sent,
                    (unsigned long)stats.no_ack_count,
                    (unsigned long)stats.not_found_count,
                    (unsigned long)stats.no_mem_count,
                    (unsigned long)stats.queue_full_count,
                    (unsigned long)stats.rediscover_count,
                    (unsigned long)stats.register_count);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PUBLISH_INTERVAL_MS));
    }
}
