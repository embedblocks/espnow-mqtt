/*
 * espnow_mqtt_broker.c — Broker role: topic registry, dispatch queue,
 * subscriber fan-out, BROKER_ANNOUNCE, NVS channel persistence,
 * seq tracking, timeout, peer management.
 *
 * Phase 4 — full implementation.
 *
 * Architecture:
 *   recv_cb (WiFi task) handles REGISTER and PUBLISH inline:
 *     - REGISTER:  registry write + REGISTER_ACK send (inline)
 *     - PUBLISH:   (topic_id,mac) lookup → ID_UNKNOWN inline OR enqueue
 *   broker_dispatch_task owns s_dispatch_queue and all mutable broker state
 *   except last_rx_ms (naturally-aligned uint32_t, safe atomic write).
 */

#include "espnow_mqtt.h"
#include "espnow_mqtt_proto.h"
#include "espnow_mqtt_internal.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"

/* Build-time contract check.
 * MAX_TOPICS must be >= MAX_PUBLISHER_TOPICS so the registry can hold
 * at least one fully-registered publisher. The correct sizing is:
 *   MAX_TOPICS >= expected_publisher_count x MAX_PUBLISHER_TOPICS
 * Set both in sdkconfig.defaults (or menuconfig) before building. */
#if CONFIG_ESPNOW_MQTT_MAX_TOPICS < CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS
#error "CONFIG_ESPNOW_MQTT_MAX_TOPICS must be >= CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS. "\
       "Correct sizing: MAX_TOPICS >= expected_publisher_count x MAX_PUBLISHER_TOPICS. "\
       "Update sdkconfig.defaults or run idf.py menuconfig → ESP-NOW MQTT."
#endif
#if defined(CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC)
#include "espnow_mqtt_hmac.h"
#endif
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "espnow_mqtt";

#define NVS_NAMESPACE   "espnow_mqtt"
#define NVS_KEY_LAST_CH "last_ch"

/* =========================================================================
 * Internal data structures
 * ========================================================================= */

/* Topic registry entry. */
typedef struct {
    char     topic[249];         /* 248 max protocol + 1 defensive slack */
    uint8_t  topic_id;
    uint8_t  sender_mac[6];
    bool     valid;              /* release/acquire: written via atomic store */
    uint8_t  _pad[3];
    uint32_t last_rx_ms;         /* naturally aligned: safe atomic uint32 write */
} espnow_topic_entry_t;          /* sizeof = 264 bytes */

/* Subscriber entry. */
typedef struct {
    char               pattern[249];
    espnow_mqtt_cb_t   cb;
    void              *user_ctx;
    bool               in_use;
    uint8_t            _pad[2];
} espnow_sub_entry_t;

/* Per-MAC seq tracking entry. */
typedef struct {
    uint8_t  mac[6];
    uint16_t last_seq;
    bool     first_frame;
    bool     valid;
} espnow_seq_entry_t;            /* 10 bytes */

/* Broker queue event types. */
typedef enum {
    BROKER_EVENT_PUBLISH      = 0,
    BROKER_EVENT_ANNOUNCE     = 1,
    BROKER_EVENT_TIMEOUT_SCAN = 2,
    BROKER_EVENT_PEER_CHANGE  = 3,
    BROKER_EVENT_DEINIT       = 4,
} broker_event_type_t;

/* Dispatch item (PUBLISH payload). */
typedef struct {
    uint8_t  sender_mac[6];
    uint8_t  data[250];
    int      data_len;
    uint32_t rx_timestamp;
    uint16_t seq;
    uint8_t  _pad[2];
} espnow_dispatch_item_t;        /* 268 bytes */

/* Queue item (union of all event payloads). */
typedef struct {
    broker_event_type_t  event_type;     /* 4 bytes */
    espnow_dispatch_item_t publish;      /* 268 bytes */
    uint8_t              purge_mac[6];   /* 6 bytes */
    bool                 purge_mac_valid; /* 1 byte */
    uint8_t              _pad[3];
} espnow_broker_queue_item_t;    /* ~284 bytes */

/* =========================================================================
 * Static storage
 * ========================================================================= */

static espnow_topic_entry_t   s_topic_registry[CONFIG_ESPNOW_MQTT_MAX_TOPICS];
static espnow_sub_entry_t     s_subs[CONFIG_ESPNOW_MQTT_MAX_SUBSCRIPTIONS];
static espnow_seq_entry_t     s_seq_track[CONFIG_ESPNOW_MQTT_MAX_TRACKED_PEERS];

static StaticQueue_t              s_dispatch_queue_storage;
static espnow_broker_queue_item_t s_dispatch_items[CONFIG_ESPNOW_MQTT_DISPATCH_QUEUE_SIZE];
static QueueHandle_t              s_dispatch_queue      = NULL;
static TaskHandle_t               s_dispatch_task_handle = NULL;
static TaskHandle_t               s_deinit_caller       = NULL;
static TimerHandle_t              s_announce_timer      = NULL;
static TimerHandle_t              s_timeout_timer       = NULL;

static uint8_t          s_next_topic_id  = 1;
static uint8_t          s_current_channel = 0;
static bool             s_broker_started  = false;
static volatile bool    s_broker_cancel   = false;

/* Stats: defined here, extern-declared in espnow_mqtt_internal.h */
espnow_mqtt_stats_t     s_broker_stats   = {0};

static espnow_mqtt_timeout_cb_t    s_timeout_cb     = NULL;
static void                       *s_timeout_cb_ctx = NULL;
static espnow_mqtt_peer_event_cb_t s_peer_event_cb  = NULL;
static void                       *s_peer_event_ctx = NULL;

/* Subscriber table mutex — plain FreeRTOS mutex (App task + dispatch task). */
static SemaphoreHandle_t s_sub_mutex = NULL;

static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* =========================================================================
 * Seq tracking helpers (dispatch task only)
 * ========================================================================= */

static espnow_seq_entry_t *seq_track_find_or_create(const uint8_t *mac)
{
    espnow_seq_entry_t *free_slot = NULL;
    for (int i = 0; i < CONFIG_ESPNOW_MQTT_MAX_TRACKED_PEERS; i++) {
        if (s_seq_track[i].valid &&
            memcmp(s_seq_track[i].mac, mac, 6) == 0) {
            return &s_seq_track[i];
        }
        if (!s_seq_track[i].valid && !free_slot) {
            free_slot = &s_seq_track[i];
        }
    }
    if (free_slot) {
        memcpy(free_slot->mac, mac, 6);
        free_slot->last_seq   = 0;
        free_slot->first_frame = true;
        free_slot->valid       = true;
    }
    return free_slot;
}

static void seq_track_clear_all(void)
{
    memset(s_seq_track, 0, sizeof(s_seq_track));
}

/* =========================================================================
 * Topic registry helpers
 * ========================================================================= */

/* Find entry by (topic_id, sender_mac). Returns NULL if not found. */
static espnow_topic_entry_t *registry_find(uint8_t topic_id,
                                            const uint8_t *mac)
{
    for (int i = 0; i < CONFIG_ESPNOW_MQTT_MAX_TOPICS; i++) {
        if (s_topic_registry[i].valid &&
            s_topic_registry[i].topic_id == topic_id &&
            memcmp(s_topic_registry[i].sender_mac, mac, 6) == 0) {
            return &s_topic_registry[i];
        }
    }
    return NULL;
}

/* Find existing entry for (mac, topic_str) — for idempotent re-registration. */
static espnow_topic_entry_t *registry_find_by_topic(const uint8_t *mac,
                                                      const char *topic_str)
{
    for (int i = 0; i < CONFIG_ESPNOW_MQTT_MAX_TOPICS; i++) {
        if (s_topic_registry[i].valid &&
            memcmp(s_topic_registry[i].sender_mac, mac, 6) == 0 &&
            strcmp(s_topic_registry[i].topic, topic_str) == 0) {
            return &s_topic_registry[i];
        }
    }
    return NULL;
}

/* Count topics for a given MAC. */
static int registry_count_for_mac(const uint8_t *mac)
{
    int count = 0;
    for (int i = 0; i < CONFIG_ESPNOW_MQTT_MAX_TOPICS; i++) {
        if (s_topic_registry[i].valid &&
            memcmp(s_topic_registry[i].sender_mac, mac, 6) == 0) {
            count++;
        }
    }
    return count;
}

/* Allocate a free registry slot. Returns NULL if full. */
static espnow_topic_entry_t *registry_alloc(void)
{
    /* Warn at >80% occupancy before rejecting. */
    int used = 0;
    espnow_topic_entry_t *free_slot = NULL;
    for (int i = 0; i < CONFIG_ESPNOW_MQTT_MAX_TOPICS; i++) {
        if (s_topic_registry[i].valid) {
            used++;
        } else if (!free_slot) {
            free_slot = &s_topic_registry[i];
        }
    }
    if (!free_slot) {
        ESP_LOGE(TAG, "registry full (%d/%d). "
                 "Increase CONFIG_ESPNOW_MQTT_MAX_TOPICS or BOOT_JITTER_MS.",
                 used, CONFIG_ESPNOW_MQTT_MAX_TOPICS);
        return NULL;
    }
    if (used * 10 >= CONFIG_ESPNOW_MQTT_MAX_TOPICS * 8) { /* >80% */
        ESP_LOGW(TAG, "registry at %d%% capacity (%d/%d)",
                 (used * 100) / CONFIG_ESPNOW_MQTT_MAX_TOPICS,
                 used, CONFIG_ESPNOW_MQTT_MAX_TOPICS);
    }
    return free_slot;
}

/* =========================================================================
 * Pattern matching helper (dispatch task only)
 * ========================================================================= */

static bool topic_matches(const char *pattern, const char *topic)
{
    while (*pattern && *topic) {
        if (*pattern == '+') {
            /* Match one segment (no slash). */
            pattern++;
            while (*topic && *topic != '/') topic++;
        } else if (*pattern != *topic) {
            return false;
        } else {
            pattern++;
            topic++;
        }
    }
    return (*pattern == '\0' && *topic == '\0');
}

/* =========================================================================
 * NVS helpers
 * ========================================================================= */

static esp_err_t nvs_read_last_channel(uint8_t *ch)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) { *ch = 0; return ret; }
    uint8_t val = 0;
    ret = nvs_get_u8(nvs, NVS_KEY_LAST_CH, &val);
    nvs_close(nvs);
    *ch = (ret == ESP_OK) ? val : 0;
    return ret;
}

static void nvs_write_last_channel(uint8_t ch)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed — last_channel not persisted");
        return;
    }
    if (nvs_set_u8(nvs, NVS_KEY_LAST_CH, ch) != ESP_OK) {
        ESP_LOGW(TAG, "NVS write last_channel failed");
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* =========================================================================
 * send_register_ack — called inline from recv_cb
 * ========================================================================= */

static void send_register_ack(const uint8_t *mac, uint8_t topic_id)
{
    espnow_register_ack_frame_t ack = {
        .type     = ESPNOW_MSG_REGISTER_ACK,
        .topic_id = topic_id,
    };
    esp_err_t ret = esp_now_send(mac, (const uint8_t *)&ack, sizeof(ack));
    if (ret != ESP_OK) {
        /* Failure is non-fatal — publisher retries idempotently. */
        ESP_LOGD(TAG, "send_register_ack: esp_now_send failed: %s",
                 esp_err_to_name(ret));
    }
}

/* =========================================================================
 * broker_handle_register — inline in recv_cb
 * ========================================================================= */

void broker_handle_register(const uint8_t *src_mac,
                              const uint8_t *data, int data_len)
{
    if (!s_broker_started) return;

    /* Extract null-terminated topic string from data[2..]. */
    if (data_len < 3) { /* need at least type+version+1 char */
        send_register_ack(src_mac, 0);
        return;
    }

    /* Ensure the topic is null-terminated within the frame. */
    const char *topic_str = (const char *)(data + 2);
    int max_topic_bytes   = data_len - 2;
    bool null_found = false;
    for (int i = 0; i < max_topic_bytes; i++) {
        if (topic_str[i] == '\0') { null_found = true; break; }
    }
    if (!null_found) {
        ESP_LOGW(TAG, "register: frame not null-terminated, rejecting");
        send_register_ack(src_mac, 0);
        return;
    }

    /* Validate topic string. */
    if (!espnow_mqtt_topic_valid(topic_str, false)) {
        ESP_LOGW(TAG, "register: invalid topic='%s', rejecting", topic_str);
        send_register_ack(src_mac, 0);
        return;
    }

    /* Idempotent re-registration check. */
    espnow_topic_entry_t *existing = registry_find_by_topic(src_mac, topic_str);
    if (existing) {
        /* Re-registration: update seq baseline and send the same topic_id. */
        
        ESP_LOGI(TAG, "register: re-registration mac=%02x:...: topic='%s' id=%d",
                 src_mac[0], topic_str, existing->topic_id);
        espnow_seq_entry_t *se = seq_track_find_or_create(src_mac);
        if (se) se->first_frame = true;
        send_register_ack(src_mac, existing->topic_id);
        s_broker_stats.registers_received++;
        if (s_peer_event_cb) {
            s_peer_event_cb(ESPNOW_MQTT_PEER_EVENT_REREGISTERED,
                            src_mac, topic_str, s_peer_event_ctx);
        }
        return;
    }

    /* Per-MAC quota check. */
    int mac_count = registry_count_for_mac(src_mac);
    if (mac_count >= CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS) {
        ESP_LOGW(TAG, "register: mac=%02x:... exceeded per-MAC topic quota (%d)",
                 src_mac[0], CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS);
        send_register_ack(src_mac, 0);
        return;
    }

    /* topic_id wrap guard. */
    if (s_next_topic_id == 0) {
        ESP_LOGW(TAG, "register: topic_id counter wrapped — broker reboot required");
        send_register_ack(src_mac, 0);
        return;
    }

    /* Allocate registry slot. */
    espnow_topic_entry_t *entry = registry_alloc();
    if (!entry) {
        send_register_ack(src_mac, 0);
        return;
    }

    /* Fill entry (valid=false still — not yet visible to dispatch task). */
    strlcpy(entry->topic, topic_str, sizeof(entry->topic));
    entry->topic_id = s_next_topic_id++;
    memcpy(entry->sender_mac, src_mac, 6);
    entry->last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
    /* Release store: set valid last so dispatch task sees a consistent entry. */
    __atomic_store_n(&entry->valid, true, __ATOMIC_RELEASE);

    /* Set first_frame on seq table. */
    espnow_seq_entry_t *se = seq_track_find_or_create(src_mac);
    if (se) se->first_frame = true;

    ESP_LOGI(TAG, "register: mac=%02x:%02x:%02x:%02x:%02x:%02x "
             "topic='%s' id=%d",
             src_mac[0], src_mac[1], src_mac[2],
             src_mac[3], src_mac[4], src_mac[5],
             topic_str, entry->topic_id);

    send_register_ack(src_mac, entry->topic_id);
    s_broker_stats.registers_received++;

    if (s_peer_event_cb) {
        s_peer_event_cb(ESPNOW_MQTT_PEER_EVENT_REGISTERED,
                        src_mac, topic_str, s_peer_event_ctx);
    }
}

/* =========================================================================
 * broker_handle_publish — inline in recv_cb
 * ========================================================================= */

void broker_handle_publish(const uint8_t *src_mac,
                            const uint8_t *data, int data_len)
{
    if (!s_dispatch_queue) return;

    uint8_t  topic_id = data[1];
    uint16_t seq      = espnow_proto_decode_seq(data[2], data[3]);

    /* Registry lookup by (topic_id, sender_mac). */
    espnow_topic_entry_t *entry = registry_find(topic_id, src_mac);
    if (!entry) {
        /* Send ID_UNKNOWN inline. */
        espnow_id_unknown_frame_t id_unk = {
            .type     = ESPNOW_MSG_ID_UNKNOWN,
            .topic_id = topic_id,
        };
        esp_err_t ret = esp_now_send(src_mac, (const uint8_t *)&id_unk, 2);
        if (ret != ESP_OK) {
            s_broker_stats.id_unknown_send_failures++;
        }
        s_broker_stats.frames_id_unknown++;
        return;
    }

    /* Update last_rx_ms (atomic uint32 write — no mutex needed). */
    __atomic_store_n(&entry->last_rx_ms,
                     (uint32_t)(esp_timer_get_time() / 1000),
                     __ATOMIC_RELAXED);

    /* Build queue item. */
    espnow_broker_queue_item_t item;
    memset(&item, 0, sizeof(item));
    item.event_type = BROKER_EVENT_PUBLISH;
    memcpy(item.publish.sender_mac, src_mac, 6);

    size_t copy_len = (size_t)data_len < sizeof(item.publish.data)
                    ? (size_t)data_len
                    : sizeof(item.publish.data);
    memcpy(item.publish.data, data, copy_len);
    item.publish.data_len     = (int)copy_len;
    item.publish.rx_timestamp = (uint32_t)(esp_timer_get_time() / 1000);
    item.publish.seq          = seq;

    /* Enqueue. */
    if (xQueueSend(s_dispatch_queue, &item, 0) == pdTRUE) {
        s_broker_stats.frames_dispatched++;
    } else {
#if CONFIG_ESPNOW_MQTT_QUEUE_DROP_POLICY == 1
        /* Drop oldest: dequeue head and re-enqueue new item. */
        espnow_broker_queue_item_t discarded;
        xQueueReceive(s_dispatch_queue, &discarded, 0);
        xQueueSend(s_dispatch_queue, &item, 0);
        s_broker_stats.frames_dropped_queue++;
#else
        /* Drop newest (default). */
        s_broker_stats.frames_dropped_queue++;
#endif
    }
}

/* =========================================================================
 * Dispatch task helpers
 * ========================================================================= */

static void broker_handle_publish_dispatch(const espnow_dispatch_item_t *pub)
{
    /* Re-resolve registry — entry may have been purged since enqueue. */
    uint8_t topic_id = pub->data[1];
    espnow_topic_entry_t *entry = registry_find(topic_id, pub->sender_mac);
    if (!entry) {
        ESP_LOGD(TAG, "dispatch: topic_id=%d mac=%02x:... not in registry — dropped",
                 topic_id, pub->sender_mac[0]);
        return;
    }

    const char *topic_str = entry->topic;
    uint16_t    seq       = pub->seq;

    /* Seq tracking — always dispatch regardless of anomaly. */
    espnow_seq_entry_t *se = seq_track_find_or_create(pub->sender_mac);
    if (se) {
        if (se->first_frame) {
            se->first_frame = false; /* baseline: no anomaly on first frame */
        } else {
            int16_t delta = (int16_t)(seq - se->last_seq);
            if (delta > 1) {
                s_broker_stats.seq_gaps += (uint32_t)(delta - 1);
            } else if (delta <= 0) {
                s_broker_stats.seq_reordered++;
            }
        }
        se->last_seq = seq;
    }

    /* Extract payload. */
    const uint8_t *payload     = pub->data + 4;
    size_t         payload_len = (pub->data_len > 4) ? (size_t)(pub->data_len - 4) : 0;

#if defined(CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC)
    /*
     * HMAC verify — runs in dispatch task, not recv_cb.
     * On failure: drop silently, increment counter, return.
     * Constant-time compare inside espnow_hmac_verify() (XOR-accumulate).
     * The full wire payload (tag + data) is passed; verify strips the tag
     * internally. Subscriber callback then sees only the data portion.
     */
    if (!espnow_hmac_verify(topic_id, seq, payload, payload_len)) {
        s_broker_stats.hmac_failures++;
        ESP_LOGD(TAG, "dispatch: HMAC verify failed topic_id=%d mac=%02x:...",
                 topic_id, pub->sender_mac[0]);
        return;
    }
    /* Strip the 16-byte tag: subscriber sees only the data portion. */
    payload     += 16;
    payload_len  = (payload_len >= 16) ? (payload_len - 16) : 0;
#endif

    /* Fan-out to matching subscribers. */
    if (xSemaphoreTake(s_sub_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < CONFIG_ESPNOW_MQTT_MAX_SUBSCRIPTIONS; i++) {
            if (s_subs[i].in_use && topic_matches(s_subs[i].pattern, topic_str)) {
                s_subs[i].cb(topic_str, payload, payload_len,
                             pub->sender_mac, seq, s_subs[i].user_ctx);
            }
        }
        xSemaphoreGive(s_sub_mutex);
    }
}

static void broker_handle_announce_event(void)
{
    espnow_broker_announce_frame_t ann = {
        .type    = ESPNOW_MSG_BROKER_ANNOUNCE,
        .version = ESPNOW_PROTO_VERSION,
    };
    esp_err_t ret = esp_now_send(BROADCAST_MAC, (const uint8_t *)&ann, 2);
    if (ret != ESP_OK) {
        s_broker_stats.announce_send_failures++;
        ESP_LOGW(TAG, "announce: send failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "announce: sent on ch %d", s_current_channel);
    }
}

static void broker_handle_timeout_scan(void)
{
    if (!s_timeout_cb) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < CONFIG_ESPNOW_MQTT_MAX_TOPICS; i++) {
        if (!s_topic_registry[i].valid) continue;
        uint32_t silence = now_ms - s_topic_registry[i].last_rx_ms;
        if (silence >= CONFIG_ESPNOW_MQTT_PUBLISHER_TIMEOUT_MS) {
            s_timeout_cb(s_topic_registry[i].sender_mac,
                         s_topic_registry[i].topic,
                         silence,
                         s_timeout_cb_ctx);
        }
    }
}

static void broker_handle_peer_change(const espnow_broker_queue_item_t *item)
{
    if (item->purge_mac_valid) {
        /* Targeted purge by MAC. */
        for (int i = 0; i < CONFIG_ESPNOW_MQTT_MAX_TOPICS; i++) {
            if (s_topic_registry[i].valid &&
                memcmp(s_topic_registry[i].sender_mac, item->purge_mac, 6) == 0) {
                memset(&s_topic_registry[i], 0, sizeof(espnow_topic_entry_t));
            }
        }
    } else {
        /* Full orphan scan. */
        for (int i = 0; i < CONFIG_ESPNOW_MQTT_MAX_TOPICS; i++) {
            if (s_topic_registry[i].valid &&
                !esp_now_is_peer_exist(s_topic_registry[i].sender_mac)) {
                memset(&s_topic_registry[i], 0, sizeof(espnow_topic_entry_t));
            }
        }
    }
    /* Clear seq table — entries re-create lazily with first_frame=true. */
    seq_track_clear_all();
}

/* =========================================================================
 * Dispatch task
 * ========================================================================= */

static void broker_dispatch_task(void *arg)
{
    (void)arg;
    espnow_broker_queue_item_t item;
    ESP_LOGI(TAG, "broker_dispatch_task: started");

    while (true) {
        if (xQueueReceive(s_dispatch_queue, &item,
                          portMAX_DELAY) != pdTRUE) continue;

        if (s_broker_cancel) break;

        switch (item.event_type) {
        case BROKER_EVENT_PUBLISH:
            broker_handle_publish_dispatch(&item.publish);
            break;
        case BROKER_EVENT_ANNOUNCE:
            broker_handle_announce_event();
            break;
        case BROKER_EVENT_TIMEOUT_SCAN:
            broker_handle_timeout_scan();
            break;
        case BROKER_EVENT_PEER_CHANGE:
            broker_handle_peer_change(&item);
            break;
        case BROKER_EVENT_DEINIT:
            goto exit;
        default:
            break;
        }
    }

exit:
    /* Clear subscriber table under mutex before exit. */
    if (xSemaphoreTake(s_sub_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        memset(s_subs, 0, sizeof(s_subs));
        xSemaphoreGive(s_sub_mutex);
    }

    /* Drain remaining queue items. */
    while (xQueueReceive(s_dispatch_queue, &item, 0) == pdTRUE) { /* drain */ }

    xTaskNotifyGive(s_deinit_caller);
    ESP_LOGI(TAG, "broker_dispatch_task: exited");
    vTaskDelete(NULL);
}

/* =========================================================================
 * Timer callbacks — post to queue; never call esp_now_send() directly
 * ========================================================================= */

static void announce_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!s_dispatch_queue) return;
    espnow_broker_queue_item_t item = { .event_type = BROKER_EVENT_ANNOUNCE };
    xQueueSend(s_dispatch_queue, &item, 0);
}

#if CONFIG_ESPNOW_MQTT_PUBLISHER_TIMEOUT_MS > 0
static void timeout_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!s_dispatch_queue) return;
    espnow_broker_queue_item_t item = { .event_type = BROKER_EVENT_TIMEOUT_SCAN };
    xQueueSend(s_dispatch_queue, &item, 0);
}
#endif

/* =========================================================================
 * Public API
 * ========================================================================= */

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)

esp_err_t espnow_mqtt_subscribe(const char *pattern, espnow_mqtt_cb_t cb,
                                 void *user_ctx,
                                 espnow_mqtt_sub_handle_t *handle_out)
{
    if (!pattern || !cb || !handle_out) return ESP_ERR_INVALID_ARG;
    if (!s_broker_started)             return ESP_ERR_INVALID_STATE;
    if (!espnow_mqtt_topic_valid(pattern, true)) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_sub_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int slot = -1;
    for (int i = 0; i < CONFIG_ESPNOW_MQTT_MAX_SUBSCRIPTIONS; i++) {
        if (!s_subs[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        xSemaphoreGive(s_sub_mutex);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(s_subs[slot].pattern, pattern, sizeof(s_subs[slot].pattern));
    s_subs[slot].cb       = cb;
    s_subs[slot].user_ctx = user_ctx;
    s_subs[slot].in_use   = true;
    xSemaphoreGive(s_sub_mutex);

    *handle_out = slot;
    ESP_LOGI(TAG, "subscribe: slot %d pattern='%s'", slot, pattern);
    return ESP_OK;
}

esp_err_t espnow_mqtt_unsubscribe(espnow_mqtt_sub_handle_t handle)
{
    if (handle < 0 || handle >= CONFIG_ESPNOW_MQTT_MAX_SUBSCRIPTIONS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_broker_started) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_sub_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_subs[handle].in_use) {
        xSemaphoreGive(s_sub_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_subs[handle], 0, sizeof(espnow_sub_entry_t));
    xSemaphoreGive(s_sub_mutex);
    return ESP_OK;
}

esp_err_t espnow_mqtt_set_timeout_cb(espnow_mqtt_timeout_cb_t cb, void *ctx)
{
    s_timeout_cb     = cb;
    s_timeout_cb_ctx = ctx;
    return ESP_OK;
}

esp_err_t espnow_mqtt_set_peer_event_cb(espnow_mqtt_peer_event_cb_t cb, void *ctx)
{
    s_peer_event_cb  = cb;
    s_peer_event_ctx = ctx;
    return ESP_OK;
}

esp_err_t espnow_mqtt_broker_prepare(void)
{
    uint8_t last_ch = 0;
    esp_err_t ret = nvs_read_last_channel(&last_ch);
    if (ret != ESP_OK || last_ch == 0) {
        ESP_LOGI(TAG, "broker_prepare: no stored channel — skipping pre-announce");
        return ESP_OK;
    }

    /* Add broadcast peer temporarily. */
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    /* Best-effort pre-announce on last known channel. */
    esp_wifi_set_channel(last_ch, WIFI_SECOND_CHAN_NONE);
    espnow_broker_announce_frame_t ann = {
        .type    = ESPNOW_MSG_BROKER_ANNOUNCE,
        .version = ESPNOW_PROTO_VERSION,
    };
    esp_now_send(BROADCAST_MAC, (const uint8_t *)&ann, 2);
    vTaskDelay(pdMS_TO_TICKS(20)); /* best-effort TX drain */

    esp_now_del_peer(BROADCAST_MAC);
    ESP_LOGI(TAG, "broker_prepare: pre-announced on ch %d", last_ch);
    return ESP_OK;
}

esp_err_t espnow_mqtt_broker_start(void)
{
    if (s_broker_started) {
        ESP_LOGE(TAG, "broker_start: already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create subscriber mutex. */
    s_sub_mutex = xSemaphoreCreateMutex();
    if (!s_sub_mutex) return ESP_ERR_NO_MEM;

    /* Create dispatch queue (static). */
    s_dispatch_queue = xQueueCreateStatic(
        CONFIG_ESPNOW_MQTT_DISPATCH_QUEUE_SIZE,
        sizeof(espnow_broker_queue_item_t),
        (uint8_t *)s_dispatch_items,
        &s_dispatch_queue_storage);
    if (!s_dispatch_queue) {
        vSemaphoreDelete(s_sub_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Create dispatch task. */
    s_broker_cancel = false;
    BaseType_t created = xTaskCreate(
        broker_dispatch_task, "broker_dispatch",
        CONFIG_ESPNOW_MQTT_BROKER_DISPATCH_TASK_STACK,
        NULL,
        CONFIG_ESPNOW_MQTT_BROKER_DISPATCH_TASK_PRIO,
        &s_dispatch_task_handle);
    if (created != pdPASS) {
        vQueueDelete(s_dispatch_queue);
        vSemaphoreDelete(s_sub_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Get current WiFi channel. */
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&s_current_channel, &second);

    /* Write last_channel to NVS. */
    nvs_write_last_channel(s_current_channel);

    /* Announce immediately. */
    broker_handle_announce_event();

    /* Start announce timer. */
    s_announce_timer = xTimerCreate("broker_announce",
                                     pdMS_TO_TICKS(CONFIG_ESPNOW_MQTT_ANNOUNCE_INTERVAL_MS),
                                     pdTRUE, /* auto-reload */
                                     NULL,
                                     announce_timer_cb);
    if (s_announce_timer) xTimerStart(s_announce_timer, 0);

    /* Start timeout timer. */
#if CONFIG_ESPNOW_MQTT_PUBLISHER_TIMEOUT_MS > 0
    s_timeout_timer = xTimerCreate("broker_timeout",
                                    pdMS_TO_TICKS(CONFIG_ESPNOW_MQTT_PUBLISHER_TIMEOUT_MS / 3),
                                    pdTRUE,
                                    NULL,
                                    timeout_timer_cb);
    if (s_timeout_timer) xTimerStart(s_timeout_timer, 0);
#endif

    /* Add broadcast peer permanently. */
    esp_now_peer_info_t bcast_peer = {};
    memcpy(bcast_peer.peer_addr, BROADCAST_MAC, 6);
    bcast_peer.encrypt = false;
    esp_now_add_peer(&bcast_peer);

    /* Sync initial peer table state. */
    espnow_mqtt_on_peer_list_changed();

    s_broker_started = true;
    ESP_LOGI(TAG, "broker_start: running on ch %d", s_current_channel);
    return ESP_OK;
}

esp_err_t espnow_mqtt_get_stored_channel(uint8_t *channel_out)
{
    if (!channel_out) return ESP_ERR_INVALID_ARG;
    return nvs_read_last_channel(channel_out);
}

static esp_err_t post_peer_change(const uint8_t *purge_mac)
{
    if (!s_dispatch_queue) return ESP_ERR_INVALID_STATE;
    espnow_broker_queue_item_t item = {
        .event_type      = BROKER_EVENT_PEER_CHANGE,
        .purge_mac_valid = (purge_mac != NULL),
    };
    if (purge_mac) memcpy(item.purge_mac, purge_mac, 6);
    return (xQueueSend(s_dispatch_queue, &item, pdMS_TO_TICKS(50)) == pdTRUE)
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t espnow_mqtt_on_peer_list_changed(void)
{
    if (!s_broker_started) return ESP_ERR_INVALID_STATE;
    return post_peer_change(NULL);
}

esp_err_t espnow_mqtt_purge_mac(const uint8_t mac[6])
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    if (!s_broker_started) return ESP_ERR_INVALID_STATE;
    return post_peer_change(mac);
}

esp_err_t espnow_mqtt_add_peer(const esp_now_peer_info_t *peer)
{
    if (!peer) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = esp_now_add_peer(peer);
    if (ret != ESP_OK) return ret;
    return espnow_mqtt_on_peer_list_changed();
}

esp_err_t espnow_mqtt_del_peer(const uint8_t peer_addr[6])
{
    if (!peer_addr) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = esp_now_del_peer(peer_addr);
    if (ret != ESP_OK) return ret;
    return espnow_mqtt_on_peer_list_changed();
}

esp_err_t espnow_mqtt_get_stats(espnow_mqtt_stats_t *stats_out)
{
    if (!stats_out) return ESP_ERR_INVALID_ARG;
    if (!s_broker_started) return ESP_ERR_INVALID_STATE;
    *stats_out = s_broker_stats;
    return ESP_OK;
}

esp_err_t espnow_mqtt_reset_broker_stats(void)
{
    if (!s_broker_started) return ESP_ERR_INVALID_STATE;
    memset(&s_broker_stats, 0, sizeof(s_broker_stats));
    return ESP_OK;
}

#endif /* !CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER */

/* =========================================================================
 * Broker init/deinit hooks — called from espnow_mqtt_init/deinit
 * ========================================================================= */

esp_err_t espnow_mqtt_broker_init_hook(void)
{
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
    memset(s_topic_registry, 0, sizeof(s_topic_registry));
    memset(s_subs,            0, sizeof(s_subs));
    memset(s_seq_track,       0, sizeof(s_seq_track));
    memset(&s_broker_stats,   0, sizeof(s_broker_stats));
    s_next_topic_id  = 1;
    s_broker_started = false;
    s_broker_cancel  = false;
#endif
    return ESP_OK;
}

void espnow_mqtt_broker_deinit_hook(void)
{
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
    if (!s_broker_started) return;

    /* Stop timers FIRST. */
    if (s_announce_timer) {
        xTimerStop(s_announce_timer, pdMS_TO_TICKS(100));
        xTimerDelete(s_announce_timer, pdMS_TO_TICKS(100));
        s_announce_timer = NULL;
    }
    if (s_timeout_timer) {
        xTimerStop(s_timeout_timer, pdMS_TO_TICKS(100));
        xTimerDelete(s_timeout_timer, pdMS_TO_TICKS(100));
        s_timeout_timer = NULL;
    }

    /* Signal dispatch task to exit. */
    if (s_dispatch_task_handle && s_dispatch_queue) {
        s_broker_cancel = true;
        s_deinit_caller = xTaskGetCurrentTaskHandle();
        espnow_broker_queue_item_t item = { .event_type = BROKER_EVENT_DEINIT };
        xQueueSend(s_dispatch_queue, &item, pdMS_TO_TICKS(1000));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        s_dispatch_task_handle = NULL;
    }

    if (s_dispatch_queue) {
        vQueueDelete(s_dispatch_queue);
        s_dispatch_queue = NULL;
    }
    if (s_sub_mutex) {
        vSemaphoreDelete(s_sub_mutex);
        s_sub_mutex = NULL;
    }

    s_broker_started = false;
#endif
}
