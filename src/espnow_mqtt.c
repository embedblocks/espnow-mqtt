/*
 * espnow_mqtt.c — Common layer: init/deinit, recv_cb, send_cb,
 *                 topic_valid, passthrough callback, version.
 *
 * Phase 2 — full implementation.
 *
 * recv_cb routing rules (settled, Rev 7):
 *   Only PUBLISH goes to the dispatch queue.
 *   REGISTER is handled INLINE (registry write + REGISTER_ACK send inline).
 *   ID_UNKNOWN send is INLINE.
 *   Publisher receives REGISTER_ACK, ID_UNKNOWN, BROKER_ANNOUNCE all INLINE.
 *
 * Callback signatures (IDF >= 5.5.0):
 *   recv_cb: (const esp_now_recv_info_t *, const uint8_t *, int)
 *   send_cb: (const esp_now_send_info_t *, esp_now_send_status_t)
 */

#include "espnow_mqtt.h"
#include "espnow_mqtt_proto.h"
#include "espnow_mqtt_internal.h"

#include "esp_now.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "espnow_mqtt";

/* =========================================================================
 * Component version
 * ========================================================================= */

const char *espnow_mqtt_get_version(void)
{
    return "0.1.0";
}

/* =========================================================================
 * Static state — common layer
 * ========================================================================= */

static bool                          s_initialized     = false;
static espnow_mqtt_passthrough_cb_t  s_passthrough_cb  = NULL;
static void                         *s_passthrough_ctx = NULL;

/* =========================================================================
 * Publisher / broker init hooks — defined in the respective .c files.
 * Called from espnow_mqtt_init() and espnow_mqtt_deinit() to wire in
 * role-specific FreeRTOS object creation without putting everything here.
 * ========================================================================= */

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)
esp_err_t espnow_mqtt_publisher_init_hook(void);
void      espnow_mqtt_publisher_deinit_hook(void);
#endif

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
esp_err_t espnow_mqtt_broker_init_hook(void);
void      espnow_mqtt_broker_deinit_hook(void);
#endif

/* =========================================================================
 * Forward declarations for role-specific inline handlers.
 *
 * These are defined in espnow_mqtt_broker.c and espnow_mqtt_publisher.c
 * respectively. They are called inline from recv_cb — NOT via a queue.
 * Declared here rather than in espnow_mqtt_internal.h to keep them as
 * narrow as possible (only espnow_mqtt.c needs them).
 * ========================================================================= */

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
/* Broker inline handlers — defined in espnow_mqtt_broker.c */
void broker_handle_register(const uint8_t *src_mac,
                              const uint8_t *data, int data_len);
void broker_handle_publish(const uint8_t *src_mac,
                            const uint8_t *data, int data_len);
#endif

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)
/* Publisher inline handlers — defined in espnow_mqtt_publisher.c */
void publisher_handle_register_ack(const uint8_t *src_mac,
                                    const uint8_t *data, int data_len);
void publisher_handle_id_unknown(const uint8_t *src_mac,
                                  const uint8_t *data, int data_len);
void publisher_handle_broker_announce(const uint8_t *src_mac,
                                       const uint8_t *data, int data_len);

/* Publisher state accessed from send_cb. */
extern volatile bool                      s_scan_active;
extern espnow_mqtt_publisher_stats_t      s_publisher_stats;
extern int                                s_no_ack_consecutive;

/* SLEEP mode publish_sync() semaphore — defined in espnow_mqtt_publisher.c.
 * Only declared when SLEEP mode is selected; CONTINUOUS builds never see it. */
#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_SLEEP)
extern SemaphoreHandle_t     s_sync_done_sem;
extern esp_now_send_status_t s_sync_send_status;
#endif

#endif /* !ROLE_BROKER */

/* =========================================================================
 * topic_valid — single authoritative implementation
 * ========================================================================= */

bool espnow_mqtt_topic_valid(const char *topic_str, bool is_pattern)
{
    if (!topic_str) return false;

    size_t len = strlen(topic_str);
    if (len == 0 || len > ESPNOW_MAX_TOPIC_LEN) return false;

    /* Reserved prefix check. */
    if (strncmp(topic_str, "espnow/", 7) == 0) return false;

    /* Leading slash. */
    if (topic_str[0] == '/') return false;

    /* Trailing slash. */
    if (topic_str[len - 1] == '/') return false;

    /* Per-character validation. */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)topic_str[i];

        /* ASCII printable: 0x20-0x7E. */
        if (c < 0x20 || c > 0x7E) return false;

        /* Double slash. */
        if (c == '/' && i + 1 < len && topic_str[i + 1] == '/') return false;

        if (c == '#') {
            /* '#' always rejected — out of scope for this version. */
            return false;
        }

        if (c == '+') {
            if (!is_pattern) {
                /* '+' never allowed in publisher topics. */
                return false;
            }
            /*
             * '+' allowed in patterns only as a complete segment.
             * Valid:   "a/+/b"  "+/b"  "a/+"
             * Invalid: "a+b"    "a/+b" "a+/b"
             */
            bool prev_ok = (i == 0) || (topic_str[i - 1] == '/');
            bool next_ok = (i + 1 == len) || (topic_str[i + 1] == '/');
            if (!prev_ok || !next_ok) return false;
        }
    }

    return true;
}

/* =========================================================================
 * recv_cb — IDF >= 5.5.0 signature
 *
 * Routing order (settled, Rev 6/7):
 *   1. Trust filter (MAC in peer table) — silent drop if false
 *   2. data_len < 1 — silent drop
 *   3. Version check on REGISTER and BROKER_ANNOUNCE only
 *   4. Per-type minimum length check
 *   5. Type dispatch (inline for all types except PUBLISH on broker side)
 *   6. Passthrough for unknown types from trusted peers
 * ========================================================================= */

static void espnow_recv_cb(const esp_now_recv_info_t *esp_now_info,
                             const uint8_t *data, int data_len)
{
    if (!esp_now_info || !data) return;
    const uint8_t *src_mac = esp_now_info->src_addr;

    /* --- Step 1: trust filter --- */
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
    /* Broker tracks frame counters. */
    s_broker_stats.frames_received++;
#endif

    if (!esp_now_is_peer_exist(src_mac)) {
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
        s_broker_stats.frames_rejected_trust++;
#endif
        return; /* silent drop — never log per-frame */
    }

    /* --- Step 2: minimum global length --- */
    if (data_len < 1) return;

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
    s_broker_stats.frames_trusted++;
#endif

    espnow_msg_type_t type = (espnow_msg_type_t)data[0];

    /* --- Step 3: version check (REGISTER and BROKER_ANNOUNCE only) --- */
    if (type == ESPNOW_MSG_REGISTER || type == ESPNOW_MSG_BROKER_ANNOUNCE) {
        if (data_len < 2) return; /* need version byte */
        if (data[1] != ESPNOW_PROTO_VERSION) {
            ESP_LOGW(TAG, "recv: version mismatch type=0x%02x got=0x%02x expect=0x%02x",
                     type, data[1], ESPNOW_PROTO_VERSION);
            return;
        }
    }

    /* --- Step 4 + 5: per-type length check and inline dispatch --- */
    switch (type) {

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
    /* ------------------------------------------------------------------ */
    /* Broker-side handlers                                                */
    /* ------------------------------------------------------------------ */

    case ESPNOW_MSG_REGISTER:
        if (data_len < ESPNOW_MIN_LEN_REGISTER) return;
        broker_handle_register(src_mac, data, data_len);
        break;

    case ESPNOW_MSG_PUBLISH:
        if (data_len < ESPNOW_MIN_LEN_PUBLISH) return;
        broker_handle_publish(src_mac, data, data_len);
        break;

    case ESPNOW_MSG_BROKER_ANNOUNCE:
        /* No-op on broker side — broker does not act on its own announces. */
        break;
#endif /* !ROLE_PUBLISHER */

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)
    /* ------------------------------------------------------------------ */
    /* Publisher-side handlers                                             */
    /* ------------------------------------------------------------------ */

    case ESPNOW_MSG_REGISTER_ACK:
        if (data_len < ESPNOW_MIN_LEN_REGISTER_ACK) return;
        publisher_handle_register_ack(src_mac, data, data_len);
        break;

    case ESPNOW_MSG_ID_UNKNOWN:
        if (data_len < ESPNOW_MIN_LEN_ID_UNKNOWN) return;
        publisher_handle_id_unknown(src_mac, data, data_len);
        break;

    /* BROKER_ANNOUNCE is only received by publishers in BOTH builds.
     * In a BROKER-only build this case is dead code but still compiles. */
    case ESPNOW_MSG_BROKER_ANNOUNCE:
        if (data_len < ESPNOW_MIN_LEN_BROKER_ANNOUNCE) return;
        publisher_handle_broker_announce(src_mac, data, data_len);
        break;
#endif /* !ROLE_BROKER */

    default:
        /* Unknown type from a trusted peer — offer to passthrough callback. */
        if (s_passthrough_cb) {
            s_passthrough_cb(src_mac, data, data_len, s_passthrough_ctx);
        }
        break;
    }
}

/* =========================================================================
 * send_cb — IDF >= 5.5.0 signature
 *
 * Publisher role only. In BROKER-only builds this function body is empty
 * (the guard strips the publisher state access). The callback is still
 * registered so the compiler sees a valid function pointer.
 * ========================================================================= */

static void espnow_send_cb(const esp_now_send_info_t *tx_info,
                             esp_now_send_status_t status)
{
    (void)tx_info; /* des_addr available if needed; not used currently */

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)
#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_SLEEP)
    /*
     * SLEEP mode: publish_sync() creates s_sync_done_sem before sending.
     * We capture the L1 ACK result and signal the semaphore so publish_sync()
     * can unblock and return the result to the caller.
     * This fires from the WiFi task — xSemaphoreGiveFromISR is used for safety
     * even though send_cb runs in task context on IDF >= 5.5.
     */
    if (s_sync_done_sem) {
        s_sync_send_status = status;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_sync_done_sem, &woken);
        portYIELD_FROM_ISR(woken);
        return; /* do not fall through to CONTINUOUS path */
    }
#else
    /* CONTINUOUS mode: track consecutive SEND_FAILs and trigger rediscover. */
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_no_ack_consecutive = 0;
    } else {
        /* ESP_NOW_SEND_FAIL = L1 ACK not received.
         *
         * REGISTER frames sent during a channel scan always produce SEND_FAIL
         * on wrong channels (no one to ACK them) — this is expected behaviour,
         * not an RF quality indicator. Exclude them from no_ack_count and from
         * the consecutive-fail rediscover counter so scans don't trigger a
         * spurious rediscover loop and don't pollute the stats.
         *
         * s_scan_active is set true for the entire duration of a channel sweep
         * (publisher_scan_all_slots / publisher_scan_single_slot) and cleared
         * immediately when the scan finishes. */
        if (!s_scan_active) {
            /* Real PUBLISH frame SEND_FAIL — count it and check threshold. */
            s_publisher_stats.no_ack_count++;
            s_no_ack_consecutive++;
            if (s_no_ack_consecutive >=
                    CONFIG_ESPNOW_MQTT_INTERNAL_ERR_THRESHOLD) {
                s_no_ack_consecutive = 0;
                publisher_trigger_rediscover_async();
            }
        }
        /* Scan REGISTER SEND_FAILs: silently dropped — no stat, no counter. */
    }
#endif /* SLEEP vs CONTINUOUS */
#endif /* !ROLE_BROKER */
}

/* =========================================================================
 * espnow_mqtt_init
 * ========================================================================= */

esp_err_t espnow_mqtt_init(void)
{
    if (s_initialized) {
        ESP_LOGE(TAG, "init: already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- Kconfig invariant checks (CONTINUOUS mode, non-SLEEP builds) ---- */
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER) && \
    defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_CONTINUOUS)

#if CONFIG_ESPNOW_MQTT_PUBLISHER_KEEPALIVE_MS > 0 && \
    CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS > 0
    if (CONFIG_ESPNOW_MQTT_PUBLISHER_KEEPALIVE_MS >=
            CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS) {
        ESP_LOGE(TAG, "init: KEEPALIVE_MS (%d) >= NO_REPLY_TIMEOUT_MS (%d) — "
                 "publisher will never trigger recovery. Fix Kconfig.",
                 CONFIG_ESPNOW_MQTT_PUBLISHER_KEEPALIVE_MS,
                 CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS);
        return ESP_ERR_INVALID_STATE;
    }
#endif

    /* In BOTH builds check announce interval vs publisher no-reply timeout. */
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
#if CONFIG_ESPNOW_MQTT_ANNOUNCE_INTERVAL_MS > 0 && \
    CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS > 0
    if (CONFIG_ESPNOW_MQTT_ANNOUNCE_INTERVAL_MS >=
            CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS) {
        ESP_LOGE(TAG, "init: ANNOUNCE_INTERVAL_MS (%d) >= NO_REPLY_TIMEOUT_MS (%d) — "
                 "publishers will time out before receiving an announce. Fix Kconfig.",
                 CONFIG_ESPNOW_MQTT_ANNOUNCE_INTERVAL_MS,
                 CONFIG_ESPNOW_MQTT_NO_REPLY_TIMEOUT_MS);
        return ESP_ERR_INVALID_STATE;
    }
#endif
#endif /* !ROLE_PUBLISHER */

#endif /* CONTINUOUS mode + non-broker */

    /* ---- Zero-peer warning (broker side) ---- */
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
    {
        esp_now_peer_num_t pnum = {0};
        if (esp_now_get_peer_num(&pnum) == ESP_OK && pnum.total_num == 0) {
            ESP_LOGW(TAG, "init: no peers in ESP-NOW peer table. "
                     "Add publisher peers via esp_now_add_peer() before init, "
                     "or use espnow_mqtt_add_peer() after init.");
        }
    }
#endif

    /* ---- Register ESP-NOW callbacks ---- */
    esp_err_t ret;

    ret = esp_now_register_recv_cb(espnow_recv_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init: esp_now_register_recv_cb failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_now_register_send_cb(espnow_send_cb);
    if (ret != ESP_OK) {
        esp_now_unregister_recv_cb();
        ESP_LOGE(TAG, "init: esp_now_register_send_cb failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* ---- Role-specific init (delegated to publisher/broker modules) ---- */
    /* Publisher init is called here via a forward-declared function to keep
     * the role-specific logic in its own .c file. broker_init() starts the
     * dispatch task; publisher_init() creates FreeRTOS objects and the task.
     * Both are implemented as no-ops in Phase 2 stubs and filled in Phase 3/4. */

    /* ---- Broker init ---- */
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
    ret = espnow_mqtt_broker_init_hook();
    if (ret != ESP_OK) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        return ret;
    }
#endif

    /* ---- Publisher init (creates task, queues, timers) ---- */
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)
    ret = espnow_mqtt_publisher_init_hook();
    if (ret != ESP_OK) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        ESP_LOGE(TAG, "init: publisher init failed: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    s_initialized = true;
    ESP_LOGI(TAG, "initialized (version %s)", espnow_mqtt_get_version());
    return ESP_OK;
}

/* =========================================================================
 * espnow_mqtt_deinit
 * ========================================================================= */

esp_err_t espnow_mqtt_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Deinit broker (stops timers, signals task, waits for exit). */
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)
    espnow_mqtt_broker_deinit_hook();
#endif

    /* Deinit publisher (stops timers, signals task, waits for exit). */
#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)
    espnow_mqtt_publisher_deinit_hook();
#endif

    /* Unregister callbacks — no new frames after this point. */
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();

    /* Clear passthrough callback. */
    s_passthrough_cb  = NULL;
    s_passthrough_ctx = NULL;

    s_initialized = false;
    ESP_LOGI(TAG, "deinitialized");
    return ESP_OK;
}

/* =========================================================================
 * espnow_mqtt_set_passthrough_cb
 * ========================================================================= */

esp_err_t espnow_mqtt_set_passthrough_cb(espnow_mqtt_passthrough_cb_t cb,
                                          void *ctx)
{
    s_passthrough_cb  = cb;
    s_passthrough_ctx = ctx;
    return ESP_OK;
}
