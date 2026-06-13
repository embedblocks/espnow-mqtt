/*
 * espnow_mqtt.h — Public API for the espnow_mqtt component.
 *
 * Include this header in both publisher and broker firmware. Role-specific
 * symbols are guarded so that calling a stripped symbol produces a linker
 * error rather than a silent runtime failure.
 *
 *   PUBLISHER build: broker symbols absent at link time.
 *   BROKER build:    publisher symbols absent at link time.
 *   BOTH build:      all symbols present (default).
 *
 * Requires ESP-IDF >= 5.5.0.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_now.h"    /* for esp_now_peer_info_t */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Shared types — always present regardless of role
 * ========================================================================= */

/**
 * Opaque handle for a registered publisher topic.
 * Returned by espnow_mqtt_register(). Pass to espnow_mqtt_publish().
 * Valid until espnow_mqtt_deinit(). Internally an index into the static
 * topic slot array.
 */
typedef int espnow_mqtt_topic_handle_t;

/** Sentinel value indicating an invalid or unallocated topic handle. */
#define ESPNOW_MQTT_TOPIC_HANDLE_INVALID  (-1)

/** Opaque handle for a broker subscription. Returned by espnow_mqtt_subscribe(). */
typedef int espnow_mqtt_sub_handle_t;

/** Sentinel value indicating an invalid subscription handle. */
#define ESPNOW_MQTT_SUB_HANDLE_INVALID    (-1)

/* =========================================================================
 * Publisher state
 * ========================================================================= */

/**
 * Publisher operating states.
 *
 * State transitions:
 *   UNPROVISIONED        --set_broker()-----------> UNREGISTERED
 *   UNREGISTERED         --all slots ACK'd--------> REGISTERED
 *   UNREGISTERED         --some slots ACK'd-------> PARTIALLY_REGISTERED
 *   PARTIALLY_REGISTERED --all slots ACK'd--------> REGISTERED
 *   REGISTERED           --ID_UNKNOWN/timeout-----> PARTIALLY_REGISTERED or UNREGISTERED
 *   REGISTERED           --BROKER_ANNOUNCE--------> UNREGISTERED
 *   any                  --MAX_REDISCOVER exceeded-> UNPROVISIONED
 *   UNPROVISIONED        --espnow_mqtt_rediscover()-> UNREGISTERED
 */
typedef enum {
    /** Broker MAC not set, or MAX_REDISCOVER_ATTEMPTS exceeded.
     *  publish() returns ESP_ERR_INVALID_STATE.
     *  Call espnow_mqtt_rediscover() to reset. */
    ESPNOW_MQTT_STATE_UNPROVISIONED,

    /** No slots have a valid topic_id. Channel scan in progress. */
    ESPNOW_MQTT_STATE_UNREGISTERED,

    /** At least one slot registered; at least one slot pending ACK.
     *  publish() on registered handles returns ESP_OK;
     *  publish() on pending handles returns ESP_ERR_INVALID_STATE. */
    ESPNOW_MQTT_STATE_PARTIALLY_REGISTERED,

    /** All slots have valid topic_ids. Fully operational. */
    ESPNOW_MQTT_STATE_REGISTERED,
} espnow_mqtt_state_t;

/* =========================================================================
 * Callback types
 * ========================================================================= */

/**
 * Subscriber callback — invoked by broker_dispatch_task for every PUBLISH
 * frame whose topic matches the registered subscription pattern.
 *
 * Runs in dispatch task context. May block on NVS, network, or I2C without
 * starving recv_cb. Must NOT call espnow_mqtt_subscribe() or
 * espnow_mqtt_unsubscribe() from within this callback (deadlock).
 *
 * MANDATORY: guard against payload_len == 0 before accessing payload.
 * When the publisher sends a keepalive (zero-payload PUBLISH), this callback
 * fires with payload_len == 0. The payload pointer is never NULL, but
 * dereferencing it for payload_len bytes is only valid when payload_len > 0.
 *
 * @param topic       Null-terminated resolved topic string. Stable only for
 *                    the duration of this callback — copy if retention needed.
 * @param payload     Pointer to raw publish payload. Never NULL, but may be
 *                    empty (payload_len == 0 for keepalives).
 * @param payload_len Number of valid bytes at payload. 0 = keepalive.
 * @param sender_mac  6-byte MAC of the publishing node.
 * @param seq         Sequence counter from the PUBLISH frame (informational).
 * @param user_ctx    The user_ctx pointer passed to espnow_mqtt_subscribe().
 */
typedef void (*espnow_mqtt_cb_t)(
    const char    *topic,
    const uint8_t *payload,
    size_t         payload_len,
    const uint8_t  sender_mac[6],
    uint16_t       seq,
    void          *user_ctx
);

/**
 * Passthrough callback — invoked for ESP-NOW frames from known peers whose
 * type byte does not match any espnow_mqtt message type.
 *
 * Trust filter still applies: only frames from MACs in the ESP-NOW peer table
 * reach this callback. Treat data as untrusted input from a potentially
 * compromised node. If this callback is not needed, do not register one.
 *
 * @param src_mac   6-byte source MAC.
 * @param data      Raw frame bytes.
 * @param data_len  Frame length.
 * @param user_ctx  The user_ctx pointer passed to espnow_mqtt_set_passthrough_cb().
 */
typedef void (*espnow_mqtt_passthrough_cb_t)(
    const uint8_t *src_mac,
    const uint8_t *data,
    int            data_len,
    void          *user_ctx
);

/**
 * Timeout callback — invoked when a publisher has been radio-silent for
 * CONFIG_ESPNOW_MQTT_PUBLISHER_TIMEOUT_MS milliseconds.
 *
 * "Radio silent" means no frame (including keepalives) has been received.
 * A publisher sending keepalives but no data does NOT trigger this callback.
 * Fires from broker_dispatch_task context.
 *
 * @param sender_mac  6-byte MAC of the silent publisher.
 * @param last_topic  Last registered topic string for this publisher.
 * @param silence_ms  How long the publisher has been silent.
 * @param user_ctx    The user_ctx pointer passed to espnow_mqtt_set_timeout_cb().
 */
typedef void (*espnow_mqtt_timeout_cb_t)(
    const uint8_t  sender_mac[6],
    const char    *last_topic,
    uint32_t       silence_ms,
    void          *user_ctx
);

/**
 * Publisher lifecycle event types for espnow_mqtt_peer_event_cb_t.
 */
typedef enum {
    ESPNOW_MQTT_PEER_EVENT_REGISTERED,   /**< New publisher registered for first time. */
    ESPNOW_MQTT_PEER_EVENT_REREGISTERED, /**< Existing publisher re-registered (reboot or
                                              ID_UNKNOWN recovery). Component resets its
                                              seq baseline for this publisher automatically. */
    ESPNOW_MQTT_PEER_EVENT_TIMEOUT,      /**< Publisher silent for PUBLISHER_TIMEOUT_MS. */
} espnow_mqtt_peer_event_t;

/**
 * Publisher lifecycle callback — fired on REGISTERED, REREGISTERED, TIMEOUT.
 * Purely informational. Fires from broker_dispatch_task context.
 *
 * @param event  The lifecycle event type.
 * @param mac    6-byte MAC of the publisher.
 * @param topic  Topic string: the registered topic for (RE)REGISTERED events;
 *               last-known topic for TIMEOUT events.
 * @param ctx    The ctx pointer passed to espnow_mqtt_set_peer_event_cb().
 */
typedef void (*espnow_mqtt_peer_event_cb_t)(
    espnow_mqtt_peer_event_t  event,
    const uint8_t             mac[6],
    const char               *topic,
    void                     *ctx
);

/* =========================================================================
 * Statistics structs
 * ========================================================================= */

/**
 * Publisher-side runtime statistics. Read via espnow_mqtt_get_publisher_stats().
 *
 * Note on seq_gaps / seq_reordered: the broker tracks these per publisher MAC,
 * not per topic. A multi-topic publisher with unequal publish rates can show
 * gapless seq tracking even when individual topics are losing frames, because
 * higher-rate topics fill the sequence space between lower-rate topic frames.
 * Treat seq_gaps as a whole-publisher RF quality indicator only.
 */
typedef struct {
    uint32_t frames_sent;       /**< Total esp_now_send() calls returning ESP_OK. */
    uint32_t no_ack_count;      /**< L1 ACK not received (normal under RF loss). */
    uint32_t not_found_count;   /**< ESP_ERR_ESPNOW_NOT_FOUND from esp_now_send(). */
    uint32_t no_mem_count;      /**< ESP_ERR_ESPNOW_NO_MEM from esp_now_send(). */
    uint32_t queue_full_count;  /**< publish() returned ESP_ERR_NO_MEM (queue full). */
    uint32_t rediscover_count;  /**< Channel rediscover cycles triggered. */
    uint32_t register_count;    /**< REGISTER frames sent. */
} espnow_mqtt_publisher_stats_t;

/**
 * Broker-side runtime statistics. Read via espnow_mqtt_get_stats().
 */
typedef struct {
    uint32_t frames_received;        /**< Total frames received by recv_cb. */
    uint32_t frames_trusted;         /**< Frames passing the MAC trust filter. */
    uint32_t frames_rejected_trust;  /**< Frames dropped by the trust filter. */
    uint32_t frames_dispatched;      /**< Frames successfully queued for dispatch. */
    uint32_t frames_dropped_queue;   /**< Frames dropped because dispatch queue was full. */
    uint32_t frames_id_unknown;      /**< PUBLISH frames rejected with ID_UNKNOWN. */
    uint32_t registers_received;     /**< REGISTER frames processed. */
    uint32_t seq_gaps;               /**< Estimated frames lost (summed delta-1, per publisher). */
    uint32_t seq_reordered;          /**< Duplicate or out-of-order PUBLISHes (delta <= 0). */
    uint32_t announce_send_failures; /**< BROKER_ANNOUNCE esp_now_send() failures. */
    uint32_t id_unknown_send_failures;/**< ID_UNKNOWN esp_now_send() failures. */
    uint32_t hmac_failures;          /**< PUBLISH frames dropped due to HMAC tag mismatch.
                                       *  Only incremented when PAYLOAD_HMAC=y.
                                       *  Zero in disabled builds. */
} espnow_mqtt_stats_t;

/* =========================================================================
 * RTC state for SLEEP mode (publisher only)
 *
 * The application declares an instance of this struct with RTC_NOINIT_ATTR
 * so it survives deep sleep. The component only reads and writes the fields
 * via espnow_mqtt_restore_rtc() and espnow_mqtt_snapshot_rtc().
 * The component never declares RTC_NOINIT_ATTR variables itself.
 * ========================================================================= */

#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_SLEEP)

/** Magic value indicating a valid RTC state snapshot. */
#define ESPNOW_MQTT_RTC_MAGIC  0xE5701A42UL

/**
 * RTC state struct for wake-publish-sleep mode.
 *
 * Survives deep sleep and software reset but NOT a power-off cycle.
 * On power cycle, magic will not match ESPNOW_MQTT_RTC_MAGIC and the
 * application treats the state as a cold start.
 *
 * The application declares:
 *   RTC_NOINIT_ATTR static espnow_mqtt_rtc_state_t s_rtc;
 * and validates magic before calling espnow_mqtt_restore_rtc().
 */
typedef struct {
    uint32_t magic;         /**< ESPNOW_MQTT_RTC_MAGIC when valid; 0 after invalidation. */
    uint8_t  hint_channel;  /**< Channel on which broker was last found. Set by application
                              *  from espnow_mqtt_get_broker_channel() after register_sync(). */
    uint16_t seq;           /**< Rolling publish counter — persists across wakes to reduce
                              *  the naive-replay window. */
    uint8_t  topic_ids[CONFIG_ESPNOW_MQTT_MAX_PUBLISHER_TOPICS];
                            /**< Cached topic_ids. If valid, the wake cycle can attempt
                              *  PUBLISH directly without re-registering. If a PUBLISH
                              *  returns ID_UNKNOWN or INVALID_STATE, the caller must
                              *  invalidate and call register_sync(). */
} espnow_mqtt_rtc_state_t;

#endif /* CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_SLEEP */

/* =========================================================================
 * Shared API — always present regardless of role
 * ========================================================================= */

/**
 * Initialize the espnow_mqtt component for the configured role.
 *
 * Must be called after esp_now_init() and esp_wifi_start(). Registers the
 * ESP-NOW recv_cb and send_cb. Creates publisher_task, event queues, and
 * timers for CONTINUOUS mode; creates s_probe_eg only for SLEEP mode.
 *
 * Kconfig invariant checks (CONTINUOUS mode):
 *   KEEPALIVE_MS >= NO_REPLY_TIMEOUT_MS   -> ESP_ERR_INVALID_STATE
 *   ANNOUNCE_INTERVAL_MS >= NO_REPLY_TIMEOUT_MS -> ESP_ERR_INVALID_STATE (BOTH role)
 *
 * @return ESP_ERR_INVALID_STATE  esp_now_init() not called, or init already done,
 *                                or Kconfig invariant violated.
 * @return ESP_ERR_NO_MEM         FreeRTOS object creation failed.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_init(void);

/**
 * De-initialize the espnow_mqtt component.
 *
 * Stops timers, signals tasks to exit, waits for task exit, unregisters
 * callbacks, deletes all FreeRTOS objects, and clears all static state.
 * After this call the component is back to its pre-init state.
 *
 * Deinit order is critical: timers are stopped and deleted BEFORE posting
 * DEINIT events to queues, to prevent timer callbacks from posting to queues
 * that are being torn down.
 *
 * @return ESP_ERR_INVALID_STATE  espnow_mqtt_init() was not called.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_deinit(void);

/**
 * Return the component version string.
 *
 * Returns a null-terminated string of the form "MAJOR.MINOR.PATCH" (e.g.
 * "0.1.0"). Stored in flash; pointer is always valid and never NULL.
 */
const char *espnow_mqtt_get_version(void);

/**
 * Register a passthrough callback for non-espnow_mqtt ESP-NOW frames.
 *
 * The trust filter still applies: only frames from MACs in the ESP-NOW peer
 * table reach this callback. Treat received data as untrusted input.
 * Pass NULL to clear a previously registered callback.
 *
 * @param cb      Callback function, or NULL to clear.
 * @param ctx     Passed to each callback invocation.
 * @return ESP_OK always.
 */
esp_err_t espnow_mqtt_set_passthrough_cb(espnow_mqtt_passthrough_cb_t cb,
                                          void *ctx);

/* =========================================================================
 * Publisher API — absent in BROKER-only builds (linker error if called)
 * ========================================================================= */

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)

/**
 * Designate which peer MAC is the broker.
 *
 * The mac must have been added to the ESP-NOW peer table via esp_now_add_peer()
 * before this call. Validated with esp_now_is_peer_exist().
 *
 * Call-once constraint: must be called once per init/deinit cycle, before any
 * topics are registered. A second call in the same cycle is always rejected.
 *
 * @param mac  6-byte broker MAC address.
 * @return ESP_ERR_NOT_FOUND      mac not in ESP-NOW peer table.
 * @return ESP_ERR_INVALID_STATE  already called this cycle, or init not done.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_set_broker(const uint8_t mac[6]);

/**
 * Register a topic string, allocate a slot, and start the REGISTER handshake.
 *
 * CONTINUOUS mode: validates the topic string, allocates a slot, and posts an
 * event to publisher_task to begin the REGISTER handshake asynchronously.
 * Use espnow_mqtt_wait_registered() to block until all topics are registered.
 *
 * SLEEP mode: allocates the slot and validates the string only. Does NOT post
 * any event. Call espnow_mqtt_register_sync() afterward.
 *
 * @param topic_str   Null-terminated topic string. Max ESPNOW_MAX_TOPIC_LEN chars.
 * @param handle_out  Receives the topic handle. Must not be NULL.
 * @return ESP_ERR_INVALID_ARG    topic_str or handle_out is NULL, or topic invalid.
 * @return ESP_ERR_NO_MEM         topic slot array full.
 * @return ESP_ERR_INVALID_STATE  init not called, or set_broker() not called.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_register(const char *topic_str,
                                espnow_mqtt_topic_handle_t *handle_out);

/**
 * Block until all registered topics have valid topic_ids, or timeout.
 *
 * CONTINUOUS mode only. Returns ESP_ERR_INVALID_STATE in SLEEP mode.
 * The registered bit is NOT cleared on return — subsequent calls return
 * ESP_OK immediately until a recovery event clears it.
 *
 * @param timeout_ms  Max wait in milliseconds. 0 = check without blocking.
 * @return ESP_OK              All slots registered.
 * @return ESP_ERR_TIMEOUT     Timeout elapsed before all slots registered.
 * @return ESP_ERR_INVALID_STATE Called in SLEEP mode.
 */
esp_err_t espnow_mqtt_wait_registered(uint32_t timeout_ms);

/**
 * Publish payload on a specific registered topic.
 *
 * Routes through publisher_task (Single Sender Principle). Copies payload
 * internally — caller's buffer need not remain valid after return.
 *
 * Return values reflect the esp_now_send() queue-status result, NOT the L1
 * ACK result. L1 ACK failures arrive asynchronously via send_cb and are
 * counted in espnow_mqtt_publisher_stats_t.no_ack_count.
 *
 * Non-blocking fast-fail: if the publisher event queue is full, returns
 * ESP_ERR_NO_MEM immediately without blocking.
 *
 * @param handle      Topic handle from espnow_mqtt_register().
 * @param payload     Data to publish. May be NULL only when payload_len == 0.
 * @param payload_len Payload length. Max 246 bytes. 0 = keepalive.
 * @return ESP_ERR_INVALID_ARG    handle out of range, or payload NULL with len > 0.
 * @return ESP_ERR_INVALID_STATE  slot not registered, permanently rejected, or UNPROVISIONED.
 * @return ESP_ERR_INVALID_SIZE   payload_len > 246.
 * @return ESP_ERR_NO_MEM         publisher event queue full.
 * @return ESP_ERR_ESPNOW_NO_MEM  ESP-NOW TX buffer full — back off and retry.
 * @return ESP_ERR_ESPNOW_NOT_FOUND broker MAC not in peer table — triggers async rediscover.
 * @return ESP_OK                 Frame handed to ESP-NOW stack.
 */
esp_err_t espnow_mqtt_publish(espnow_mqtt_topic_handle_t handle,
                               const void *payload, size_t payload_len);

/**
 * Query the current publisher state (non-blocking, thread-safe).
 *
 * In SLEEP mode returns UNREGISTERED until register_sync() succeeds, then REGISTERED.
 */
espnow_mqtt_state_t espnow_mqtt_get_state(void);

/**
 * Force immediate channel re-discovery.
 *
 * Resets all topic slots to unregistered, zeros s_broker_channel, resets the
 * rediscover attempt counter, and posts PUBLISHER_EVENT_REDISCOVER.
 * Safe to call from any task.
 *
 * @return ESP_ERR_INVALID_STATE  init not called.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_rediscover(void);

/**
 * Return the currently locked broker channel (0 if not yet discovered).
 *
 * Reads the channel on which the last REGISTER_ACK was received.
 * Thread-safe.
 */
uint8_t espnow_mqtt_get_broker_channel(void);

/**
 * Clear the permanent-rejection flag on a topic slot.
 *
 * When perm_rejected is true, publish() returns ESP_ERR_INVALID_STATE for
 * that handle. Use this when the rejection cause is confirmed to be transient
 * (e.g. broker registry temporarily full). Does NOT trigger a REGISTER
 * handshake — call espnow_mqtt_rediscover() afterward if immediate retry is needed.
 *
 * @param handle  Topic handle from espnow_mqtt_register().
 * @return ESP_ERR_INVALID_ARG    handle out of range.
 * @return ESP_ERR_INVALID_STATE  init not called.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_clear_perm_rejected(espnow_mqtt_topic_handle_t handle);

/**
 * Read publisher-side runtime statistics (atomic snapshot).
 *
 * @param stats_out  Must not be NULL.
 * @return ESP_ERR_INVALID_ARG    stats_out is NULL.
 * @return ESP_ERR_INVALID_STATE  init not called.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_get_publisher_stats(espnow_mqtt_publisher_stats_t *stats_out);

/**
 * Reset all publisher-side statistics counters to zero.
 *
 * @return ESP_ERR_INVALID_STATE  init not called.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_reset_publisher_stats(void);

/* SLEEP mode API — only when SLEEP mode is selected in Kconfig */
#if defined(CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_SLEEP)

/**
 * Synchronous REGISTER handshake for SLEEP mode.
 *
 * Tries hint_channel first (if non-zero), then scans channels 1..MAX_CHANNEL.
 * Blocks until all allocated slots are registered or timeout_ms elapses.
 * CONTINUOUS mode: returns ESP_ERR_INVALID_STATE.
 *
 * @param hint_channel  Channel to try first (from RTC state), or 0 for full scan.
 * @param channel_out   Receives the channel on which broker was found (0 if none).
 * @param timeout_ms    Maximum total scan time in milliseconds.
 * @return ESP_OK              All slots registered.
 * @return ESP_ERR_TIMEOUT     Broker not found within timeout_ms.
 * @return ESP_ERR_INVALID_STATE CONTINUOUS mode, or no topics allocated.
 */
esp_err_t espnow_mqtt_register_sync(uint8_t  hint_channel,
                                     uint8_t *channel_out,
                                     uint32_t timeout_ms);

/**
 * Synchronous PUBLISH for SLEEP mode.
 *
 * Sends the frame and blocks until the ESP-NOW send callback fires or
 * timeout_ms elapses.
 * CONTINUOUS mode: returns ESP_ERR_INVALID_STATE.
 *
 * @param handle      Topic handle from espnow_mqtt_register().
 * @param payload     Data to publish (may be NULL when payload_len == 0).
 * @param payload_len Payload length. Max 246 bytes.
 * @param timeout_ms  Maximum wait for send callback in milliseconds.
 * @return ESP_ERR_INVALID_STATE  CONTINUOUS mode, or slot not registered.
 * @return ESP_ERR_ESPNOW_NO_ACK  L1 ACK not received within timeout.
 * @return other                  From esp_now_send().
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_publish_sync(espnow_mqtt_topic_handle_t handle,
                                    const void *payload, size_t payload_len,
                                    uint32_t timeout_ms);

/**
 * Pre-populate internal topic slots from a valid RTC state cache.
 *
 * Call after espnow_mqtt_register() slot allocation, before publish_sync()
 * on a warm wake. The caller must validate rtc->magic before calling.
 * CONTINUOUS mode: returns ESP_ERR_INVALID_STATE.
 *
 * @param rtc  Pointer to a validated RTC state struct.
 * @return ESP_ERR_INVALID_ARG    rtc is NULL.
 * @return ESP_ERR_INVALID_STATE  CONTINUOUS mode.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_restore_rtc(const espnow_mqtt_rtc_state_t *rtc);

/**
 * Snapshot current internal slot state into an RTC state struct.
 *
 * Sets rtc->magic = ESPNOW_MQTT_RTC_MAGIC only when ALL allocated slots have
 * registered == true. If any slot is unregistered, magic is set to 0 so the
 * next wake treats it as a cold start.
 * Does NOT set rtc->hint_channel — the caller sets this from
 * espnow_mqtt_get_broker_channel() after a successful register_sync().
 * CONTINUOUS mode: returns ESP_ERR_INVALID_STATE.
 *
 * @param rtc  Output RTC state struct. Must not be NULL.
 * @return ESP_ERR_INVALID_ARG    rtc is NULL.
 * @return ESP_ERR_INVALID_STATE  CONTINUOUS mode.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_snapshot_rtc(espnow_mqtt_rtc_state_t *rtc);

#endif /* CONFIG_ESPNOW_MQTT_PUBLISHER_MODE_SLEEP */

#endif /* !CONFIG_ESPNOW_MQTT_ROLE_BROKER */

/* =========================================================================
 * Broker API — absent in PUBLISHER-only builds (linker error if called)
 * ========================================================================= */

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)

/**
 * Register a subscription pattern and callback.
 *
 * Patterns support '+' as a single-segment wildcard. '#' is not supported.
 * Callbacks fire from broker_dispatch_task. Multiple subscriptions may match
 * the same PUBLISH frame; all matching callbacks are called in order.
 *
 * @param pattern     Null-terminated pattern string. '+' allowed as full segment.
 * @param cb          Callback to invoke on match. Must not be NULL.
 * @param user_ctx    Passed to cb on each invocation.
 * @param handle_out  Receives the subscription handle. Must not be NULL.
 * @return ESP_ERR_INVALID_ARG    pattern or cb or handle_out is NULL, or pattern invalid.
 * @return ESP_ERR_NO_MEM         subscription table full.
 * @return ESP_ERR_INVALID_STATE  broker_start() not called.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_subscribe(const char *pattern, espnow_mqtt_cb_t cb,
                                 void *user_ctx,
                                 espnow_mqtt_sub_handle_t *handle_out);

/**
 * Unregister a subscription.
 *
 * @param handle  Handle from espnow_mqtt_subscribe().
 * @return ESP_ERR_INVALID_ARG    handle out of range or not in use.
 * @return ESP_ERR_INVALID_STATE  broker_start() not called.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_unsubscribe(espnow_mqtt_sub_handle_t handle);

/**
 * Register a callback for publisher radio-silence timeouts.
 *
 * Pass NULL to clear. Fires from broker_dispatch_task.
 *
 * @param cb   Callback function, or NULL to clear.
 * @param ctx  Passed to each callback invocation.
 * @return ESP_OK always.
 */
esp_err_t espnow_mqtt_set_timeout_cb(espnow_mqtt_timeout_cb_t cb, void *ctx);

/**
 * Register a callback for publisher lifecycle events.
 *
 * Pass NULL to clear. Fires from broker_dispatch_task. Purely informational.
 *
 * @param cb   Callback function, or NULL to clear.
 * @param ctx  Passed to each callback invocation.
 * @return ESP_OK always.
 */
esp_err_t espnow_mqtt_set_peer_event_cb(espnow_mqtt_peer_event_cb_t cb, void *ctx);

/**
 * Phase 1 broker boot helper — pre-announce on last known channel.
 *
 * Call after wifi_init + wifi_start + esp_now_init, BEFORE esp_wifi_connect().
 * Reads last_channel from NVS and sends a best-effort BROKER_ANNOUNCE on that
 * channel. Adds and removes the broadcast peer internally. Non-fatal if NVS
 * has no stored channel — the call is a no-op.
 *
 * After this returns, call esp_now_deinit() before esp_wifi_connect().
 *
 * @return ESP_ERR_INVALID_STATE  WiFi STA is already associated.
 * @return ESP_OK                 Including when no channel was stored (no-op).
 */
esp_err_t espnow_mqtt_broker_prepare(void);

/**
 * Phase 3 broker boot helper — start the broker after WiFi connect.
 *
 * Call after esp_now_init() (second call) and after esp_now_add_peer() for
 * all known publisher peers. Starts broker_dispatch_task, creates the dispatch
 * queue, sends BROKER_ANNOUNCE on the current channel, writes last_channel to
 * NVS, starts the announce timer, and adds the broadcast peer permanently.
 *
 * @return ESP_ERR_INVALID_STATE  esp_now_init() not called, or broker already started.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_broker_start(void);

/**
 * Read the last known broker channel from NVS.
 *
 * The ONLY espnow_mqtt function that may be called before espnow_mqtt_init().
 * Returns the in-memory cached value if called after init.
 *
 * @param channel_out  Receives the stored channel (1-13), or 0 if not stored.
 * @return ESP_ERR_INVALID_ARG  channel_out is NULL.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_get_stored_channel(uint8_t *channel_out);

/**
 * Notify the broker that the ESP-NOW peer table has changed.
 *
 * Posts BROKER_EVENT_PEER_CHANGE to the dispatch queue (50 ms bounded block).
 * The dispatch task then purges orphaned registry entries and clears the
 * per-MAC seq table. Prefer the wrapper functions espnow_mqtt_add_peer() and
 * espnow_mqtt_del_peer() which call this automatically.
 *
 * Must NOT be called from recv_cb or an ISR — App task context only.
 *
 * @return ESP_ERR_INVALID_STATE  broker_start() not called.
 * @return ESP_ERR_TIMEOUT        Dispatch queue full after 50 ms.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_on_peer_list_changed(void);

/**
 * Immediately remove all registry entries for a specific publisher MAC.
 *
 * Posts a targeted BROKER_EVENT_PEER_CHANGE. The actual purge runs in the
 * dispatch task. The MAC does not need to be removed from the ESP-NOW peer
 * table before calling this.
 *
 * Primary use case: OTA rollouts where a publisher re-registers with new
 * topic strings after a firmware update.
 *
 * @param mac  6-byte MAC address of the publisher to purge.
 * @return ESP_ERR_INVALID_ARG    mac is NULL.
 * @return ESP_ERR_INVALID_STATE  broker_start() not called.
 * @return ESP_ERR_TIMEOUT        Dispatch queue full after 50 ms.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_purge_mac(const uint8_t mac[6]);

/**
 * Add a publisher peer and notify the broker in one atomic call.
 *
 * Calls esp_now_add_peer() then espnow_mqtt_on_peer_list_changed().
 * Use for runtime peer additions after broker_start(). For boot-time peer
 * setup, use raw esp_now_add_peer() before broker_start().
 *
 * @param peer  Peer info struct. peer->encrypt must be false for this component.
 * @return From esp_now_add_peer(), or ESP_ERR_TIMEOUT from on_peer_list_changed().
 */
esp_err_t espnow_mqtt_add_peer(const esp_now_peer_info_t *peer);

/**
 * Remove a publisher peer and notify the broker in one atomic call.
 *
 * Calls esp_now_del_peer() then espnow_mqtt_on_peer_list_changed().
 *
 * @param peer_addr  6-byte MAC of the peer to remove.
 * @return From esp_now_del_peer(), or ESP_ERR_TIMEOUT from on_peer_list_changed().
 */
esp_err_t espnow_mqtt_del_peer(const uint8_t peer_addr[6]);

/**
 * Read broker-side runtime statistics (atomic snapshot).
 *
 * @param stats_out  Must not be NULL.
 * @return ESP_ERR_INVALID_ARG    stats_out is NULL.
 * @return ESP_ERR_INVALID_STATE  broker_start() not called.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_get_stats(espnow_mqtt_stats_t *stats_out);

/**
 * Reset all broker-side statistics counters to zero.
 *
 * @return ESP_ERR_INVALID_STATE  broker_start() not called.
 * @return ESP_OK
 */
esp_err_t espnow_mqtt_reset_broker_stats(void);

#endif /* !CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER */

#ifdef __cplusplus
}
#endif
