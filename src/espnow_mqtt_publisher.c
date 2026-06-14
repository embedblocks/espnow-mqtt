/*
 * espnow_mqtt_publisher.c — Publisher role: state machine, channel scan,
 * REGISTER handshake, PUBLISH, keepalive, no-reply timeout, rediscovery.
 *
 * Phase 3 — CONTINUOUS mode full implementation.
 * Phase 5 — SLEEP mode (stubs only in this phase).
 *
 * Single Sender Principle: all esp_now_send() calls from the publisher
 * go through publisher_task. The only exception is publisher_scan_all_slots()
 * which sends REGISTER frames directly on the task during the scan loop
 * (the task IS publisher_task during scan).
 */

#include "espnow_mqtt.h"
#include "espnow_mqtt_proto.h"
#include "espnow_mqtt_internal.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#if defined(CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC)
#include "espnow_mqtt_hmac.h"
#endif
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

static const char *TAG = "espnow_mqtt";

/* Compile-time scan-cycle time warning. */
#if defined(CONFIG_ESPNOW_MQTT_CHANNEL_PROBE_MS) && \
    defined(CONFIG_ESPNOW_MQTT_MAX_CHANNEL) && \
    defined(CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS)
#if (CONFIG_ESPNOW_MQTT_CHANNEL_PROBE_MS * \
     CONFIG_ESPNOW_MQTT_MAX_CHANNEL * \
     CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS) > 3000
#warning "Scan cycle time > 3000ms: publish() may return ESP_ERR_NO_MEM during re-registration scans"
#endif
#endif

/* =========================================================================
 * EventGroup bits
 * ========================================================================= */

#define PROBE_EG_BIT_ACK        (1 << 0)  /* REGISTER_ACK received on current channel */
#define REGISTERED_EG_BIT       (1 << 1)  /* all slots registered */

/* =========================================================================
 * Topic slot
 * ========================================================================= */

typedef struct {
    char    topic_str[248];         /* ESPNOW_MAX_TOPIC_LEN(247) + 1 null */
    uint8_t topic_id;               /* 0 = not yet registered */
    bool    registered;
    bool    perm_rejected;
    uint8_t consecutive_rejections;
} espnow_publisher_topic_t;

/* =========================================================================
 * Publisher event types and event struct
 * ========================================================================= */

typedef enum {
    PUBLISHER_EVENT_REGISTER,          /* Full scan for all unregistered slots */
    PUBLISHER_EVENT_REGISTER_TOPIC,    /* Targeted scan for one slot */
    PUBLISHER_EVENT_REDISCOVER,        /* Full rediscovery (channel reset) */
    PUBLISHER_EVENT_NO_REPLY_TIMEOUT,  /* No-reply timer fired */
    PUBLISHER_EVENT_BROKER_ANNOUNCE,   /* Broker announced (channel hint) */
    PUBLISHER_EVENT_PUBLISH,           /* Application publish call */
    PUBLISHER_EVENT_KEEPALIVE_TICK,    /* Keepalive timer fired */
    PUBLISHER_EVENT_ID_UNKNOWN,        /* Broker sent ID_UNKNOWN for a slot */
    PUBLISHER_EVENT_DEINIT,            /* Shutdown signal */
} publisher_event_type_t;

typedef struct {
    publisher_event_type_t type;
    int                    topic_handle;              /* REGISTER_TOPIC, PUBLISH, ID_UNKNOWN */
    uint8_t                payload[ESPNOW_MAX_PAYLOAD_LEN]; /* inline copy for PUBLISH */
    size_t                 payload_len;
    SemaphoreHandle_t      done_sem;                  /* PUBLISH: signalled after send */
    esp_err_t              result;                    /* PUBLISH: result written by task */
    uint8_t                announce_channel;          /* BROKER_ANNOUNCE: hinted channel */
} publisher_event_t;

/* =========================================================================
 * Static storage for FreeRTOS queues (no heap allocation)
 * ========================================================================= */

static StaticQueue_t      s_publisher_queue_storage;
static publisher_event_t  s_publisher_queue_items[CONFIG_ESPNOW_MQTT_PUBLISHER_EVENT_QUEUE_SIZE];

/* =========================================================================
 * Static state — all cleared in publisher_state_reset()
 * ========================================================================= */

static espnow_publisher_topic_t  s_topics[CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS];
static int                       s_topic_count        = 0;
static uint16_t                  s_seq                = 0;
static uint8_t                   s_broker_mac[6]      = {0};
static uint8_t                   s_broker_channel     = 0;
volatile bool                    s_scan_active        = false;   /* extern in espnow_mqtt.c */
static volatile bool             s_scan_continuous    = false;
static volatile int              s_scanning_slot      = -1;      /* slot index currently being probed; -1 = not scanning */
static bool                      s_had_successful_session = false;
static bool                      s_broker_set         = false;
static bool                      s_initialized        = false;   /* publisher-local init flag */
static int                       s_rediscover_cycle_count = 0;
static espnow_mqtt_state_t       s_state              = ESPNOW_MQTT_STATE_UNPROVISIONED;

static EventGroupHandle_t        s_probe_eg           = NULL;    /* both modes */
static EventGroupHandle_t        s_registered_eg      = NULL;    /* CONTINUOUS only */
static QueueHandle_t             s_publisher_event_queue = NULL;
static TaskHandle_t              s_publisher_task_handle = NULL;
static TaskHandle_t              s_deinit_caller      = NULL;

#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS)
static TimerHandle_t             s_no_reply_timer     = NULL;
static TimerHandle_t             s_keepalive_timer    = NULL;
#endif

espnow_mqtt_publisher_stats_t    s_publisher_stats    = {0};     /* extern in espnow_mqtt.c */
int                              s_no_ack_consecutive = 0;       /* extern in espnow_mqtt.c */

/* =========================================================================
 * Forward declarations (internal)
 * ========================================================================= */

static void publisher_scan_all_slots(void);
static void publisher_scan_single_slot(int slot);
static esp_err_t publisher_send_frame(uint8_t topic_id,
                                       const void *payload, size_t payload_len);
static void reset_no_reply_timer(void);
static void update_state(void);
#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS)
static void publisher_task(void *arg);
#endif

/* =========================================================================
 * publisher_trigger_rediscover_async — called from espnow_mqtt.c send_cb
 * ========================================================================= */

void publisher_trigger_rediscover_async(void)
{
    if (!s_publisher_event_queue) return;
    publisher_event_t evt = { .type = PUBLISHER_EVENT_REDISCOVER };
    xQueueSend(s_publisher_event_queue, &evt, 0); /* non-blocking; drop if full */
}

/* =========================================================================
 * Inline recv_cb handlers — called from espnow_mqtt.c
 * ========================================================================= */

void publisher_handle_register_ack(const uint8_t *src_mac,
                                    const uint8_t *data, int data_len)
{
    (void)src_mac;
    if (data_len < ESPNOW_MIN_LEN_REGISTER_ACK) return;

    uint8_t topic_id = data[1];

    if (topic_id > 0) {
        /*
         * Positive ACK: find the slot that is currently scanning.
         * The scanning slot has topic_id == 0 and registered == false.
         * We match on the channel probe event group bit — the scan loop
         * is waiting on PROBE_EG_BIT_ACK; setting that bit wakes it up
         * and it then reads s_probe_ack_topic_id.
         *
         * To avoid a race between recv_cb and publisher_task reading the
         * topic_id, we write the value into the first unregistered slot's
         * topic_id field AND set the probe event group bit. The task reads
         * the slot after the event group fires.
         *
         * Simpler scheme: the task sends REGISTER for slots sequentially;
         * while s_scan_active, the current slot being probed is the one
         * with topic_id == 0 at the lowest index. We store the ACK id in
         * a single-slot variable protected by the event group ordering.
         */
        /* Apply ACK only to the slot currently being probed.
         * Matching on s_scanning_slot prevents late/duplicate ACKs
         * (from earlier channel probes still in-flight) from being
         * applied to the wrong slot. */
        int target = (s_scanning_slot >= 0 && s_scanning_slot < s_topic_count)
                     ? s_scanning_slot : -1;
        if (target >= 0 && !s_topics[target].registered &&
            !s_topics[target].perm_rejected) {
            s_topics[target].topic_id              = topic_id;
            s_topics[target].registered            = true;
            s_topics[target].consecutive_rejections = 0;
            ESP_LOGI(TAG, "register_ack: slot %d topic='%s' id=%d",
                     target, s_topics[target].topic_str, topic_id);
        } else {
            /* Late/duplicate ACK — slot already registered or no scan active.
             * Silently discard: do not update any slot, do not set the bit. */
            ESP_LOGD(TAG, "register_ack: late/dup ACK id=%d discarded "
                     "(scanning_slot=%d)", topic_id, s_scanning_slot);
            update_state();
            return;
        }
        /* Signal the scan loop waiting on PROBE_EG_BIT_ACK. */
        if (s_probe_eg) {
            xEventGroupSetBitsFromISR(s_probe_eg, PROBE_EG_BIT_ACK, NULL);
        }
        /* Reset no-reply timer on any positive ACK. */
        reset_no_reply_timer();
    } else {
        /* Rejection: apply only to the slot currently being probed. */
        int rtarget = (s_scanning_slot >= 0 && s_scanning_slot < s_topic_count)
                      ? s_scanning_slot : -1;
        if (rtarget >= 0 && !s_topics[rtarget].registered &&
            !s_topics[rtarget].perm_rejected) {
            s_topics[rtarget].consecutive_rejections++;
            ESP_LOGD(TAG, "register_ack: slot %d rejected (%d/%d)",
                     rtarget, s_topics[rtarget].consecutive_rejections,
                     CONFIG_ESPNOW_MQTT_MAX_CONSECUTIVE_REJECTIONS);
            if (s_topics[rtarget].consecutive_rejections >=
                    CONFIG_ESPNOW_MQTT_MAX_CONSECUTIVE_REJECTIONS) {
                s_topics[rtarget].perm_rejected = true;
                ESP_LOGE(TAG, "register_ack: slot %d permanently rejected "
                         "topic='%s'. Call espnow_mqtt_clear_perm_rejected() "
                         "then rediscover() to retry.",
                         rtarget, s_topics[rtarget].topic_str);
            }
        } else {
            ESP_LOGD(TAG, "register_ack: late/dup rejection discarded "
                     "(scanning_slot=%d)", s_scanning_slot);
            update_state();
            return;
        }
        /* Signal the scan loop so it advances to the next channel / exits. */
        if (s_probe_eg) {
            xEventGroupSetBitsFromISR(s_probe_eg, PROBE_EG_BIT_ACK, NULL);
        }
        reset_no_reply_timer();
    }

    update_state();
}

void publisher_handle_id_unknown(const uint8_t *src_mac,
                                  const uint8_t *data, int data_len)
{
    (void)src_mac;
    if (data_len < ESPNOW_MIN_LEN_ID_UNKNOWN) return;

    uint8_t bad_id = data[1];
    for (int i = 0; i < s_topic_count; i++) {
        if (s_topics[i].topic_id == bad_id && s_topics[i].registered) {
            ESP_LOGW(TAG, "id_unknown: slot %d topic='%s' id=%d — re-registering",
                     i, s_topics[i].topic_str, bad_id);
            s_topics[i].topic_id   = 0;
            s_topics[i].registered = false;

            if (s_publisher_event_queue) {
                publisher_event_t evt = {
                    .type         = PUBLISHER_EVENT_REGISTER_TOPIC,
                    .topic_handle = i,
                };
                xQueueSend(s_publisher_event_queue, &evt, 0);
            }
            break;
        }
    }
    reset_no_reply_timer();
    update_state();
}

void publisher_handle_broker_announce(const uint8_t *src_mac,
                                       const uint8_t *data, int data_len)
{
    (void)data; (void)data_len;

    /* Only act on announces from our known broker. */
    if (memcmp(src_mac, s_broker_mac, 6) != 0) return;

    /* Capture the channel on which the announce arrived.
     * This is the channel the broker is currently on. */
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary, &second);

    s_scan_continuous = true;

    if (s_publisher_event_queue) {
        publisher_event_t evt = {
            .type             = PUBLISHER_EVENT_BROKER_ANNOUNCE,
            .announce_channel = primary,
        };
        /* Non-blocking: if queue full, the announce is dropped.
         * Recovery still works — the no-reply timer will fire eventually. */
        if (xQueueSend(s_publisher_event_queue, &evt, 0) != pdTRUE) {
            ESP_LOGD(TAG, "broker_announce: queue full, event dropped");
        }
    }
    /* DO NOT post PUBLISHER_EVENT_REDISCOVER here. The announce handler
     * in the task sets s_broker_channel from the hint BEFORE scanning. */
    reset_no_reply_timer();
}

/* =========================================================================
 * Timer callbacks (CONTINUOUS mode only)
 * ========================================================================= */

#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS)

static void no_reply_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!s_publisher_event_queue) return;
    publisher_event_t evt = { .type = PUBLISHER_EVENT_NO_REPLY_TIMEOUT };
    xQueueSend(s_publisher_event_queue, &evt, 0);
}

static void keepalive_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!s_publisher_event_queue) return;
    publisher_event_t evt = { .type = PUBLISHER_EVENT_KEEPALIVE_TICK };
    xQueueSend(s_publisher_event_queue, &evt, 0);
}

#endif /* CONTINUOUS */

/* =========================================================================
 * reset_no_reply_timer — called from recv_cb inline handlers only
 * ========================================================================= */

static void reset_no_reply_timer(void)
{
#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS)
#if CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS > 0
    if (s_no_reply_timer) {
        xTimerResetFromISR(s_no_reply_timer, NULL);
    }
#endif
#endif
}

/* =========================================================================
 * update_state — derive s_state from slot registration status
 * ========================================================================= */

static void update_state(void)
{
    if (s_topic_count == 0) return;

    int registered   = 0;
    int unregistered = 0;

    for (int i = 0; i < s_topic_count; i++) {
        if (s_topics[i].registered) registered++;
        else if (!s_topics[i].perm_rejected) unregistered++;
    }

    if (registered == s_topic_count) {
        s_state = ESPNOW_MQTT_STATE_REGISTERED;
        s_had_successful_session = true;
        if (s_registered_eg) {
            xEventGroupSetBitsFromISR(s_registered_eg, REGISTERED_EG_BIT, NULL);
        }
    } else if (registered > 0) {
        s_state = ESPNOW_MQTT_STATE_PARTIALLY_REGISTERED;
    } else {
        /* Do not downgrade to UNPROVISIONED here — that only happens via
         * MAX_REDISCOVER_ATTEMPTS in publisher_task. */
        if (s_state == ESPNOW_MQTT_STATE_REGISTERED ||
            s_state == ESPNOW_MQTT_STATE_PARTIALLY_REGISTERED) {
            s_state = ESPNOW_MQTT_STATE_UNREGISTERED;
        }
    }
}

/* =========================================================================
 * publisher_send_frame — send one PUBLISH frame
 * ========================================================================= */

static esp_err_t publisher_send_frame(uint8_t topic_id,
                                       const void *payload, size_t payload_len)
{
    uint8_t frame_buf[4 + ESPNOW_MAX_PAYLOAD_LEN];
    size_t  frame_len;

    /* Build PUBLISH header. */
    espnow_publish_frame_t *hdr = (espnow_publish_frame_t *)frame_buf;
    hdr->type     = ESPNOW_MSG_PUBLISH;
    hdr->topic_id = topic_id;
    espnow_proto_encode_seq(s_seq, &hdr->seq_lo, &hdr->seq_hi);

#if defined(CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC)
    /*
     * HMAC enabled: build the tagged payload into frame_buf+4.
     * espnow_hmac_build() prepends the 16-byte tag then copies the data.
     * Fails closed: on error, abort the send — never transmit an untagged frame
     * when the broker expects tags (would trigger permanent ID_UNKNOWN loop).
     */
    size_t    tagged_len = 0;
    esp_err_t hmac_ret   = espnow_hmac_build(topic_id, s_seq,
                                              payload, payload_len,
                                              frame_buf + 4, &tagged_len);
    if (hmac_ret != ESP_OK) {
        ESP_LOGE(TAG, "publisher_send_frame: HMAC build failed: %s",
                 esp_err_to_name(hmac_ret));
        s_seq++;   /* seq still advances to keep broker slot in sync */
        return hmac_ret;
    }
    frame_len = 4 + tagged_len;
#else
    /* HMAC disabled: plain payload copy. */
    frame_len = 4 + payload_len;
    if (payload && payload_len > 0) {
        memcpy(frame_buf + 4, payload, payload_len);
    }
#endif

    /* seq advances on every attempt, regardless of outcome. */
    s_seq++;

    esp_err_t ret = esp_now_send(s_broker_mac, frame_buf, frame_len);
    if (ret == ESP_OK) {
        s_publisher_stats.frames_sent++;
    } else if (ret == ESP_ERR_ESPNOW_NOT_FOUND) {
        s_publisher_stats.not_found_count++;
        publisher_trigger_rediscover_async();
    } else if (ret == ESP_ERR_ESPNOW_NO_MEM) {
        s_publisher_stats.no_mem_count++;
    }
    return ret;
}

/* =========================================================================
 * publisher_scan_single_slot — probe all channels for one slot
 *
 * Used for targeted re-registration after ID_UNKNOWN or late register().
 * ========================================================================= */

static void publisher_scan_single_slot(int slot)
{
    if (slot < 0 || slot >= s_topic_count) return;
    if (s_topics[slot].registered || s_topics[slot].perm_rejected) return;

    const char *topic = s_topics[slot].topic_str;
    size_t topic_len  = strlen(topic);

    /* REGISTER frame: 2 bytes header + topic string + null terminator. */
    uint8_t frame_buf[2 + ESPNOW_MAX_TOPIC_LEN + 1];
    size_t  frame_len = 2 + topic_len + 1;

    frame_buf[0] = ESPNOW_MSG_REGISTER;
    frame_buf[1] = ESPNOW_PROTO_VERSION;
    memcpy(frame_buf + 2, topic, topic_len + 1); /* include null */

    TickType_t probe_ticks = pdMS_TO_TICKS(CONFIG_ESPNOW_MQTT_CHANNEL_PROBE_MS);

    /* Try hint channel first. */
    uint8_t channels[14];
    int     n_channels = 0;

    if (s_broker_channel != 0) {
        channels[n_channels++] = s_broker_channel;
    }
    for (int ch = 1; ch <= CONFIG_ESPNOW_MQTT_MAX_CHANNEL; ch++) {
        if (ch != s_broker_channel) {
            channels[n_channels++] = (uint8_t)ch;
        }
    }

    TickType_t budget = pdMS_TO_TICKS(CONFIG_ESPNOW_MQTT_REGISTER_TIMEOUT_MS);
    TickType_t start  = xTaskGetTickCount();

    for (int i = 0; i < n_channels; i++) {
        if ((xTaskGetTickCount() - start) >= budget) break;
        if (s_topics[slot].registered) break;

        esp_wifi_set_channel(channels[i], WIFI_SECOND_CHAN_NONE);
        xEventGroupClearBits(s_probe_eg, PROBE_EG_BIT_ACK);
        s_scanning_slot = slot;
        esp_now_send(s_broker_mac, frame_buf, frame_len);
        s_publisher_stats.register_count++;

        EventBits_t bits = xEventGroupWaitBits(s_probe_eg, PROBE_EG_BIT_ACK,
                                                pdTRUE, pdFALSE, probe_ticks);
        if (bits & PROBE_EG_BIT_ACK) {
            if (s_topics[slot].registered) {
                s_broker_channel = channels[i];
                break;
            }
        }
    }
    s_scanning_slot = -1;
    /* Restore WiFi to locked broker channel after single-slot scan. */
    if (s_broker_channel != 0) {
        esp_wifi_set_channel(s_broker_channel, WIFI_SECOND_CHAN_NONE);
    }
    update_state();
}

/* =========================================================================
 * publisher_scan_all_slots — full channel sweep for all unregistered slots
 * ========================================================================= */

static void publisher_scan_all_slots(void)
{
    s_scan_active = true;

    TickType_t probe_ticks = pdMS_TO_TICKS(CONFIG_ESPNOW_MQTT_CHANNEL_PROBE_MS);

    /* Boot jitter — one-time random delay to spread simultaneous registrations
     * after a broker reboot. Applied once per call to scan_all_slots. */
#if CONFIG_ESPNOW_MQTT_BOOT_JITTER_MS > 0
    {
        uint32_t jitter_ms = (uint32_t)(esp_timer_get_time() / 1000)
                             % (CONFIG_ESPNOW_MQTT_BOOT_JITTER_MS + 1);
        if (jitter_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(jitter_ms));
        }
    }
#endif

    int retry_cycle = 0;

    /* Outer loop: keep scanning until all slots registered or cancelled. */
    while (true) {
        /* Check for cancellation (DEINIT or new REDISCOVER event pending). */
        if (!s_publisher_event_queue) break;

        /* Count unregistered, non-perm-rejected slots. */
        int pending = 0;
        for (int i = 0; i < s_topic_count; i++) {
            if (!s_topics[i].registered && !s_topics[i].perm_rejected) pending++;
        }
        if (pending == 0) break;

        /* Build channel scan order: hint first, then 1..MAX_CHANNEL. */
        uint8_t channels[14];
        int     n_channels = 0;
        if (s_broker_channel != 0) {
            channels[n_channels++] = s_broker_channel;
        }
        for (int ch = 1; ch <= CONFIG_ESPNOW_MQTT_MAX_CHANNEL; ch++) {
            if (ch != s_broker_channel) {
                channels[n_channels++] = (uint8_t)ch;
            }
        }

        bool found_on_any_channel = false;

        /* Inner loop: for each unregistered slot, probe all channels. */
        for (int slot = 0; slot < s_topic_count; slot++) {
            if (s_topics[slot].registered || s_topics[slot].perm_rejected) continue;

            const char *topic    = s_topics[slot].topic_str;
            size_t      topic_len = strlen(topic);

            uint8_t frame_buf[2 + ESPNOW_MAX_TOPIC_LEN + 1];
            frame_buf[0] = ESPNOW_MSG_REGISTER;
            frame_buf[1] = ESPNOW_PROTO_VERSION;
            memcpy(frame_buf + 2, topic, topic_len + 1);
            size_t frame_len = 2 + topic_len + 1;

            for (int ci = 0; ci < n_channels; ci++) {
                esp_wifi_set_channel(channels[ci], WIFI_SECOND_CHAN_NONE);
                xEventGroupClearBits(s_probe_eg, PROBE_EG_BIT_ACK);

                /* Tell register_ack handler which slot this ACK belongs to.
                 * Must be set before esp_now_send so any fast ACK is routed
                 * correctly even before WaitBits returns. */
                s_scanning_slot = slot;

                esp_now_send(s_broker_mac, frame_buf, frame_len);
                s_publisher_stats.register_count++;

                EventBits_t bits = xEventGroupWaitBits(
                    s_probe_eg, PROBE_EG_BIT_ACK,
                    pdTRUE, pdFALSE, probe_ticks);

                if (bits & PROBE_EG_BIT_ACK) {
                    if (s_topics[slot].registered) {
                        s_broker_channel = channels[ci];
                        found_on_any_channel = true;
                        ESP_LOGI(TAG, "scan: slot %d registered on ch %d",
                                 slot, s_broker_channel);
                        break; /* break channel loop for this slot */
                    }
                    /* Rejection ACK: stop probing this slot on this cycle. */
                    break;
                }
                /* Timeout on this channel: try the next. */
            }
            s_scanning_slot = -1; /* slot probe done — discard any further late ACKs */
        }

        /* Re-count pending. */
        pending = 0;
        for (int i = 0; i < s_topic_count; i++) {
            if (!s_topics[i].registered && !s_topics[i].perm_rejected) pending++;
        }
        if (pending == 0) break;

        /* Full scan cycle completed without registering all slots. */
        if (!found_on_any_channel) {
            s_rediscover_cycle_count++;
            s_publisher_stats.rediscover_count++;
            ESP_LOGW(TAG, "scan: full scan cycle %d — broker not found",
                     s_rediscover_cycle_count);

            /* Check MAX_REDISCOVER_ATTEMPTS. */
            if (CONFIG_ESPNOW_MQTT_MAX_REDISCOVER_ATTEMPTS > 0 &&
                s_rediscover_cycle_count >=
                    CONFIG_ESPNOW_MQTT_MAX_REDISCOVER_ATTEMPTS) {
                ESP_LOGW(TAG, "scan: max rediscover attempts (%d) reached — "
                         "entering UNPROVISIONED. Call espnow_mqtt_rediscover() to reset.",
                         CONFIG_ESPNOW_MQTT_MAX_REDISCOVER_ATTEMPTS);
                s_state = ESPNOW_MQTT_STATE_UNPROVISIONED;
                break;
            }
        } else {
            /* Found broker on at least one channel; reset fail cycle. */
            retry_cycle = 0;
        }

        /* Backoff before next full cycle.
         * Exponential for first REGISTER_MAX_RETRIES cycles, then fixed slow interval. */
        uint32_t backoff_ms;
        if (retry_cycle < CONFIG_ESPNOW_MQTT_REGISTER_MAX_RETRIES) {
            backoff_ms = 500u << (uint32_t)retry_cycle;
            if (backoff_ms > CONFIG_ESPNOW_MQTT_REGISTER_SLOW_INTERVAL_MS) {
                backoff_ms = CONFIG_ESPNOW_MQTT_REGISTER_SLOW_INTERVAL_MS;
            }
            retry_cycle++;
        } else {
            backoff_ms = CONFIG_ESPNOW_MQTT_REGISTER_SLOW_INTERVAL_MS;
        }

        ESP_LOGD(TAG, "scan: backoff %lu ms before next cycle", (unsigned long)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }

    s_scanning_slot   = -1;
    s_scan_active     = false;
    s_scan_continuous = false;
    /* Restore WiFi channel to the locked broker channel.
     * The scan loop leaves the radio on the last probed channel.
     * All PUBLISH frames must go out on s_broker_channel. */
    if (s_broker_channel != 0) {
        esp_wifi_set_channel(s_broker_channel, WIFI_SECOND_CHAN_NONE);
        ESP_LOGD(TAG, "scan: restored WiFi to broker ch %d", s_broker_channel);
    }
    update_state();
}

/* =========================================================================
 * publisher_task — main event loop (CONTINUOUS mode only)
 * ========================================================================= */

#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS)

static void publisher_task(void *arg)
{
    (void)arg;
    publisher_event_t evt;

    ESP_LOGI(TAG, "publisher_task: started");

    while (true) {
        if (xQueueReceive(s_publisher_event_queue, &evt,
                          portMAX_DELAY) != pdTRUE) continue;

        switch (evt.type) {

        case PUBLISHER_EVENT_REGISTER:
        case PUBLISHER_EVENT_REDISCOVER:
        case PUBLISHER_EVENT_NO_REPLY_TIMEOUT:
            ESP_LOGI(TAG, "publisher_task: event=%d — full rediscover", evt.type);
            /* Clear all slot registration state; preserve perm_rejected. */
            for (int i = 0; i < s_topic_count; i++) {
                s_topics[i].topic_id  = 0;
                s_topics[i].registered = false;
                /* Preserve perm_rejected and consecutive_rejections. */
            }
            if (s_registered_eg) {
                xEventGroupClearBits(s_registered_eg, REGISTERED_EG_BIT);
            }
            s_broker_channel = 0;
            update_state();
            publisher_scan_all_slots();
            break;

        case PUBLISHER_EVENT_BROKER_ANNOUNCE:
            ESP_LOGI(TAG, "publisher_task: BROKER_ANNOUNCE on ch %d",
                     evt.announce_channel);
            /*
             * Only re-register if:
             *   a) not yet registered (UNREGISTERED / UNPROVISIONED), OR
             *   b) broker has moved to a different channel than the one we
             *      locked onto — meaning our cached topic_ids are stale.
             *
             * If already REGISTERED on the same channel: the announce is
             * proof the broker is still alive. Reset the no-reply watchdog
             * timer only — do NOT clear slots or re-scan.
             */
            if (s_state == ESPNOW_MQTT_STATE_REGISTERED &&
                (evt.announce_channel == 0 ||
                 evt.announce_channel == s_broker_channel)) {
                /* Broker alive, same channel — reset no-reply watchdog only.
                 * Use xTimerReset (task context, not ISR). */
#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS)
#if CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS > 0
                if (s_no_reply_timer) {
                    xTimerReset(s_no_reply_timer, 0);
                }
#endif
#endif
                ESP_LOGD(TAG, "broker_announce: already registered on ch %d — no re-register",
                         s_broker_channel);
                break;
            }
            /* Broker moved channels or we are not yet registered — re-register. */
            if (evt.announce_channel != 0) {
                s_broker_channel = evt.announce_channel;
            }
            for (int i = 0; i < s_topic_count; i++) {
                s_topics[i].topic_id   = 0;
                s_topics[i].registered = false;
            }
            if (s_registered_eg) {
                xEventGroupClearBits(s_registered_eg, REGISTERED_EG_BIT);
            }
            if (s_had_successful_session) {
                s_rediscover_cycle_count = 0;
            }
            update_state();
            publisher_scan_all_slots();
            break;

        case PUBLISHER_EVENT_REGISTER_TOPIC:
            publisher_scan_single_slot(evt.topic_handle);
            break;

        case PUBLISHER_EVENT_PUBLISH:
            if (!s_topics[evt.topic_handle].registered) {
                evt.result = ESP_ERR_INVALID_STATE;
            } else {
                evt.result = publisher_send_frame(
                    s_topics[evt.topic_handle].topic_id,
                    evt.payload, evt.payload_len);
            }
            if (evt.done_sem) xSemaphoreGive(evt.done_sem);
            break;

        case PUBLISHER_EVENT_KEEPALIVE_TICK:
            for (int i = 0; i < s_topic_count; i++) {
                if (s_topics[i].registered) {
                    publisher_send_frame(s_topics[i].topic_id, NULL, 0);
                }
            }
            break;

        case PUBLISHER_EVENT_ID_UNKNOWN:
            /* Handled inline in recv_cb; this case should not reach the task.
             * Kept for defensive completeness. */
            break;

        case PUBLISHER_EVENT_DEINIT:
            ESP_LOGI(TAG, "publisher_task: DEINIT — exiting");
            xTaskNotifyGive(s_deinit_caller);
            vTaskDelete(NULL);
            return; /* unreachable */
        }
    }
}

#endif /* CONTINUOUS */

/* =========================================================================
 * Publisher init / deinit (called from espnow_mqtt_init / deinit in Phase 4)
 * For now wired directly into the public API.
 * ========================================================================= */

static esp_err_t publisher_init(void)
{
    memset(s_topics,          0, sizeof(s_topics));
    memset(&s_publisher_stats, 0, sizeof(s_publisher_stats));
    s_topic_count            = 0;
    s_seq                    = 0;
    memset(s_broker_mac,      0, 6);
    s_broker_channel         = 0;
    s_scan_active            = false;
    s_scan_continuous        = false;
    s_scanning_slot          = -1;
    s_had_successful_session = false;
    s_broker_set             = false;
    s_rediscover_cycle_count = 0;
    s_no_ack_consecutive     = 0;
    s_state                  = ESPNOW_MQTT_STATE_UNPROVISIONED;

    /* s_probe_eg: used by both CONTINUOUS and SLEEP modes. */
    s_probe_eg = xEventGroupCreate();
    if (!s_probe_eg) return ESP_ERR_NO_MEM;

#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS)

    s_registered_eg = xEventGroupCreate();
    if (!s_registered_eg) {
        vEventGroupDelete(s_probe_eg);
        s_probe_eg = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_publisher_event_queue = xQueueCreateStatic(
        CONFIG_ESPNOW_MQTT_PUBLISHER_EVENT_QUEUE_SIZE,
        sizeof(publisher_event_t),
        (uint8_t *)s_publisher_queue_items,
        &s_publisher_queue_storage);
    if (!s_publisher_event_queue) {
        vEventGroupDelete(s_registered_eg);
        vEventGroupDelete(s_probe_eg);
        s_registered_eg = NULL;
        s_probe_eg      = NULL;
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS > 0
    s_no_reply_timer = xTimerCreate("pub_noreply",
                                     pdMS_TO_TICKS(CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS),
                                     pdFALSE,  /* one-shot */
                                     NULL,
                                     no_reply_timer_cb);
    if (!s_no_reply_timer) {
        vQueueDelete(s_publisher_event_queue);
        vEventGroupDelete(s_registered_eg);
        vEventGroupDelete(s_probe_eg);
        return ESP_ERR_NO_MEM;
    }
#endif

#if CONFIG_ESPNOW_MQTT_PUBLISHER_KEEPALIVE_MS > 0
    s_keepalive_timer = xTimerCreate("pub_keepalive",
                                      pdMS_TO_TICKS(CONFIG_ESPNOW_MQTT_PUBLISHER_KEEPALIVE_MS),
                                      pdTRUE,  /* auto-reload */
                                      NULL,
                                      keepalive_timer_cb);
    if (!s_keepalive_timer) {
        if (s_no_reply_timer) xTimerDelete(s_no_reply_timer, 0);
        vQueueDelete(s_publisher_event_queue);
        vEventGroupDelete(s_registered_eg);
        vEventGroupDelete(s_probe_eg);
        return ESP_ERR_NO_MEM;
    }
    xTimerStart(s_keepalive_timer, 0);
#endif

    BaseType_t created = xTaskCreate(
        publisher_task, "pub_task",
        CONFIG_ESPNOW_MQTT_PUBLISHER_TASK_STACK,
        NULL,
        CONFIG_ESPNOW_MQTT_PUBLISHER_TASK_PRIO,
        &s_publisher_task_handle);
    if (created != pdPASS) {
        if (s_keepalive_timer) xTimerDelete(s_keepalive_timer, 0);
        if (s_no_reply_timer)  xTimerDelete(s_no_reply_timer,  0);
        vQueueDelete(s_publisher_event_queue);
        vEventGroupDelete(s_registered_eg);
        vEventGroupDelete(s_probe_eg);
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS > 0
    xTimerStart(s_no_reply_timer, 0);
#endif

#endif /* CONTINUOUS */

    s_initialized = true;
    return ESP_OK;
}

static void publisher_deinit(void)
{
#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS)
    /* Stop timers FIRST before posting DEINIT to the queue. */
    if (s_keepalive_timer) {
        xTimerStop(s_keepalive_timer, pdMS_TO_TICKS(100));
        xTimerDelete(s_keepalive_timer, pdMS_TO_TICKS(100));
        s_keepalive_timer = NULL;
    }
    if (s_no_reply_timer) {
        xTimerStop(s_no_reply_timer, pdMS_TO_TICKS(100));
        xTimerDelete(s_no_reply_timer, pdMS_TO_TICKS(100));
        s_no_reply_timer = NULL;
    }

    if (s_publisher_task_handle && s_publisher_event_queue) {
        s_deinit_caller = xTaskGetCurrentTaskHandle();
        publisher_event_t evt = { .type = PUBLISHER_EVENT_DEINIT };
        xQueueSend(s_publisher_event_queue, &evt, pdMS_TO_TICKS(1000));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        s_publisher_task_handle = NULL;
    }

    if (s_publisher_event_queue) {
        vQueueDelete(s_publisher_event_queue);
        s_publisher_event_queue = NULL;
    }
    if (s_registered_eg) {
        vEventGroupDelete(s_registered_eg);
        s_registered_eg = NULL;
    }
#endif /* CONTINUOUS */

    if (s_probe_eg) {
        vEventGroupDelete(s_probe_eg);
        s_probe_eg = NULL;
    }

    s_initialized = false;
}

/* =========================================================================
 * Public API — full implementation
 * ========================================================================= */

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)

esp_err_t espnow_mqtt_set_broker(const uint8_t mac[6])
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_broker_set) {
        ESP_LOGE(TAG, "set_broker: already called this init cycle");
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_now_is_peer_exist(mac)) {
        ESP_LOGE(TAG, "set_broker: MAC not in ESP-NOW peer table");
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(s_broker_mac, mac, 6);
    s_broker_set = true;
    ESP_LOGI(TAG, "set_broker: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t espnow_mqtt_register(const char *topic_str,
                                espnow_mqtt_topic_handle_t *handle_out)
{
    if (!topic_str || !handle_out) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || !s_broker_set) return ESP_ERR_INVALID_STATE;
    if (!espnow_mqtt_topic_valid(topic_str, false)) return ESP_ERR_INVALID_ARG;
    if (s_topic_count >= CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS) {
        return ESP_ERR_NO_MEM;
    }

    int slot = s_topic_count;
    memset(&s_topics[slot], 0, sizeof(espnow_publisher_topic_t));
    strlcpy(s_topics[slot].topic_str, topic_str, sizeof(s_topics[slot].topic_str));
    s_topic_count++;
    *handle_out = slot;

    ESP_LOGI(TAG, "register: slot %d topic='%s'", slot, topic_str);

#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS)
    if (s_publisher_event_queue) {
        publisher_event_t evt;
        if (slot == 0) {
            evt.type = PUBLISHER_EVENT_REGISTER;
        } else {
            evt.type         = PUBLISHER_EVENT_REGISTER_TOPIC;
            evt.topic_handle = slot;
        }
        xQueueSend(s_publisher_event_queue, &evt, 0);
    }
#endif

    return ESP_OK;
}

esp_err_t espnow_mqtt_wait_registered(uint32_t timeout_ms)
{
#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_SLEEP)
    return ESP_ERR_INVALID_STATE;
#else
    if (!s_registered_eg) return ESP_ERR_INVALID_STATE;
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_registered_eg, REGISTERED_EG_BIT,
                                            pdFALSE, pdTRUE, ticks);
    return (bits & REGISTERED_EG_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
#endif
}

esp_err_t espnow_mqtt_publish(espnow_mqtt_topic_handle_t handle,
                               const void *payload, size_t payload_len)
{
    if (handle < 0 || handle >= s_topic_count) return ESP_ERR_INVALID_ARG;
#if defined(CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC)
    /* With HMAC, 16 tag bytes consume part of the 246-byte payload budget. */
    if (payload_len > ESPNOW_MAX_PAYLOAD_HMAC_LEN) return ESP_ERR_INVALID_SIZE;
#else
    if (payload_len > ESPNOW_MAX_PAYLOAD_LEN) return ESP_ERR_INVALID_SIZE;
#endif
    if (payload_len > 0 && !payload) return ESP_ERR_INVALID_ARG;
    if (s_state == ESPNOW_MQTT_STATE_UNPROVISIONED) return ESP_ERR_INVALID_STATE;
    if (s_topics[handle].perm_rejected) return ESP_ERR_INVALID_STATE;
    if (!s_topics[handle].registered)   return ESP_ERR_INVALID_STATE;

#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_SLEEP)
    return ESP_ERR_INVALID_STATE; /* Use publish_sync() in SLEEP mode. */
#else
    if (!s_publisher_event_queue) return ESP_ERR_INVALID_STATE;

    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) return ESP_ERR_NO_MEM;

    publisher_event_t evt = {
        .type         = PUBLISHER_EVENT_PUBLISH,
        .topic_handle = handle,
        .payload_len  = payload_len,
        .done_sem     = sem,
        .result       = ESP_OK,
    };
    if (payload && payload_len > 0) {
        memcpy(evt.payload, payload, payload_len);
    }

    if (xQueueSend(s_publisher_event_queue, &evt, 0) != pdTRUE) {
        vSemaphoreDelete(sem);
        s_publisher_stats.queue_full_count++;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(sem, portMAX_DELAY);
    esp_err_t result = evt.result;
    vSemaphoreDelete(sem);
    return result;
#endif
}

espnow_mqtt_state_t espnow_mqtt_get_state(void)
{
    return s_state;
}

uint8_t espnow_mqtt_get_broker_channel(void)
{
    return s_broker_channel;
}

esp_err_t espnow_mqtt_rediscover(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_rediscover_cycle_count = 0;
    s_state = ESPNOW_MQTT_STATE_UNREGISTERED;
    publisher_trigger_rediscover_async();
    return ESP_OK;
}

esp_err_t espnow_mqtt_clear_perm_rejected(espnow_mqtt_topic_handle_t handle)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (handle < 0 || handle >= s_topic_count) return ESP_ERR_INVALID_ARG;
    s_topics[handle].perm_rejected          = false;
    s_topics[handle].consecutive_rejections = 0;
    ESP_LOGI(TAG, "clear_perm_rejected: slot %d topic='%s'",
             handle, s_topics[handle].topic_str);
    return ESP_OK;
}

esp_err_t espnow_mqtt_get_publisher_stats(espnow_mqtt_publisher_stats_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    /* Atomic copy — all fields are uint32_t or bool, naturally aligned. */
    *out = s_publisher_stats;
    return ESP_OK;
}

esp_err_t espnow_mqtt_reset_publisher_stats(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    memset(&s_publisher_stats, 0, sizeof(s_publisher_stats));
    return ESP_OK;
}

/* ---- SLEEP mode implementation (Phase 5) ---- */
#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_SLEEP)

/*
 * Synchronisation for publish_sync():
 *   s_sync_done_sem  — binary semaphore given by send_cb when it fires.
 *   s_sync_send_status — status written by send_cb before giving the sem.
 *
 * Both are declared extern in espnow_mqtt.c so the send_cb can signal them
 * without coupling the common layer to the publisher's full internals.
 * Only one publish_sync() may be in-flight at a time (linear wake cycle).
 */
SemaphoreHandle_t      s_sync_done_sem    = NULL;
esp_now_send_status_t  s_sync_send_status = ESP_NOW_SEND_SUCCESS;

esp_err_t espnow_mqtt_register_sync(uint8_t  hint_channel,
                                     uint8_t *channel_out,
                                     uint32_t timeout_ms)
{
    if (!s_initialized || !s_broker_set) {
        ESP_LOGE(TAG, "register_sync: not initialised or broker not set");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_topic_count == 0) {
        ESP_LOGE(TAG, "register_sync: no topics allocated — call register() first");
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t budget_ticks  = pdMS_TO_TICKS(timeout_ms);
    TickType_t start_ticks   = xTaskGetTickCount();
    TickType_t probe_ticks   = pdMS_TO_TICKS(CONFIG_ESPNOW_MQTT_CHANNEL_PROBE_MS);

    if (channel_out) *channel_out = 0;

    /* Build channel scan order: hint first (if provided), then 1..MAX_CHANNEL. */
    uint8_t channels[14];
    int     n_channels = 0;
    if (hint_channel != 0) {
        channels[n_channels++] = hint_channel;
    }
    for (int ch = 1; ch <= CONFIG_ESPNOW_MQTT_MAX_CHANNEL; ch++) {
        if (ch != hint_channel) {
            channels[n_channels++] = (uint8_t)ch;
        }
    }

    for (int slot = 0; slot < s_topic_count; slot++) {
        if (s_topics[slot].registered || s_topics[slot].perm_rejected) continue;

        const char *topic    = s_topics[slot].topic_str;
        size_t      tlen     = strlen(topic);
        uint8_t     frame[2 + ESPNOW_MAX_TOPIC_LEN + 1];
        frame[0] = ESPNOW_MSG_REGISTER;
        frame[1] = ESPNOW_PROTO_VERSION;
        memcpy(frame + 2, topic, tlen + 1); /* include null */
        size_t frame_len = 2 + tlen + 1;

        bool slot_done = false;
        for (int ci = 0; ci < n_channels && !slot_done; ci++) {
            /* Check budget remaining. */
            TickType_t elapsed = xTaskGetTickCount() - start_ticks;
            if (elapsed >= budget_ticks) break;

            esp_wifi_set_channel(channels[ci], WIFI_SECOND_CHAN_NONE);
            xEventGroupClearBits(s_probe_eg, PROBE_EG_BIT_ACK);

            esp_now_send(s_broker_mac, frame, frame_len);
            s_publisher_stats.register_count++;

            /* Wait for ACK — publisher_handle_register_ack() sets the bit. */
            TickType_t remaining = budget_ticks - (xTaskGetTickCount() - start_ticks);
            TickType_t wait      = (probe_ticks < remaining) ? probe_ticks : remaining;
            EventBits_t bits = xEventGroupWaitBits(s_probe_eg, PROBE_EG_BIT_ACK,
                                                    pdTRUE, pdFALSE, wait);
            if (bits & PROBE_EG_BIT_ACK) {
                if (s_topics[slot].registered) {
                    s_broker_channel = channels[ci];
                    if (channel_out) *channel_out = channels[ci];
                    ESP_LOGI(TAG, "register_sync: slot %d registered on ch %d",
                             slot, channels[ci]);
                    slot_done = true;
                    /* If hint succeeded, no need to scan further channels. */
                    if (ci == 0 && hint_channel != 0) break;
                }
                /* Rejection ACK: try next channel. */
            }
        }
    }

    update_state();

    /* Return OK only if every non-perm-rejected slot is now registered. */
    for (int i = 0; i < s_topic_count; i++) {
        if (!s_topics[i].registered && !s_topics[i].perm_rejected) {
            ESP_LOGW(TAG, "register_sync: slot %d not registered within timeout", i);
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

esp_err_t espnow_mqtt_publish_sync(espnow_mqtt_topic_handle_t handle,
                                    const void *payload, size_t payload_len,
                                    uint32_t timeout_ms)
{
    if (handle < 0 || handle >= s_topic_count) return ESP_ERR_INVALID_ARG;
#if defined(CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC)
    if (payload_len > ESPNOW_MAX_PAYLOAD_HMAC_LEN) return ESP_ERR_INVALID_SIZE;
#else
    if (payload_len > ESPNOW_MAX_PAYLOAD_LEN)  return ESP_ERR_INVALID_SIZE;
#endif
    if (payload_len > 0 && !payload)           return ESP_ERR_INVALID_ARG;
    if (!s_topics[handle].registered)          return ESP_ERR_INVALID_STATE;

    /* Create a one-shot binary semaphore. send_cb will give it. */
    s_sync_done_sem = xSemaphoreCreateBinary();
    if (!s_sync_done_sem) return ESP_ERR_NO_MEM;

    s_sync_send_status = ESP_NOW_SEND_SUCCESS; /* clear */

    esp_err_t ret = publisher_send_frame(
        s_topics[handle].topic_id, payload, payload_len);
    if (ret != ESP_OK) {
        vSemaphoreDelete(s_sync_done_sem);
        s_sync_done_sem = NULL;
        return ret;
    }

    /* Block until send_cb fires or timeout. */
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY
                                         : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_sync_done_sem, ticks) != pdTRUE) {
        vSemaphoreDelete(s_sync_done_sem);
        s_sync_done_sem = NULL;
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(s_sync_done_sem);
    s_sync_done_sem = NULL;

    /* send_cb wrote the L1 ACK result into s_sync_send_status. */
    return (s_sync_send_status == ESP_NOW_SEND_SUCCESS)
           ? ESP_OK
           : ESP_ERR_ESPNOW_NO_MEM; /* re-use closest error; caller retries */
}

esp_err_t espnow_mqtt_restore_rtc(const espnow_mqtt_rtc_state_t *rtc)
{
    if (!rtc) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /*
     * Caller has already validated rtc->magic == ESPNOW_MQTT_RTC_MAGIC.
     * We copy topic_ids and seq into the already-allocated slots.
     * Slots beyond s_topic_count are ignored.
     */
    int restore_count = s_topic_count;
    if (restore_count > CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS) {
        restore_count = CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS;
    }

    s_seq = rtc->seq;

    for (int i = 0; i < restore_count; i++) {
        s_topics[i].topic_id   = rtc->topic_ids[i];
        s_topics[i].registered = (rtc->topic_ids[i] != 0);
    }

    update_state();
    ESP_LOGI(TAG, "restore_rtc: seq=%u, %d slot(s) restored", rtc->seq, restore_count);
    return ESP_OK;
}

esp_err_t espnow_mqtt_snapshot_rtc(espnow_mqtt_rtc_state_t *rtc)
{
    if (!rtc) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    memset(rtc, 0, sizeof(*rtc));
    rtc->seq = s_seq;

    bool all_registered = (s_topic_count > 0);
    for (int i = 0; i < s_topic_count && i < CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS; i++) {
        rtc->topic_ids[i] = s_topics[i].topic_id;
        if (!s_topics[i].registered) all_registered = false;
    }

    /*
     * Set magic only when ALL slots are registered — a partial snapshot is
     * useless on the next wake because we cannot PUBLISH unregistered slots.
     * The application detects magic==0 and performs a cold-start register_sync.
     */
    rtc->magic = all_registered ? ESPNOW_MQTT_RTC_MAGIC : 0;

    /* hint_channel is NOT set here — caller reads espnow_mqtt_get_broker_channel(). */
    ESP_LOGI(TAG, "snapshot_rtc: seq=%u magic=%s",
             rtc->seq, all_registered ? "valid" : "INVALID (not all registered)");
    return ESP_OK;
}

#endif /* SLEEP */
#endif /* !ROLE_BROKER */

/* =========================================================================
 * Hook called from espnow_mqtt_init / espnow_mqtt_deinit in espnow_mqtt.c
 *
 * Phase 3: wire publisher_init / publisher_deinit into the common init.
 * These are not part of the public API — called only from espnow_mqtt.c.
 * ========================================================================= */

/* Forward-declared in espnow_mqtt.c for the wiring below. */
esp_err_t espnow_mqtt_publisher_init_hook(void)
{
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)
    return publisher_init();
#else
    return ESP_OK;
#endif
}

void espnow_mqtt_publisher_deinit_hook(void)
{
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)
    publisher_deinit();
#endif
}
