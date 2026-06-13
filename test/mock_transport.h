/*
 * mock_transport.h — Mock ESP-NOW transport for Tier 2 state-machine tests.
 *
 * Provides a canned-response mechanism: tests register expected sends and the
 * responses that should be delivered when they occur. mock_transport_pump()
 * delivers all queued responses synchronously into the component's recv_cb.
 *
 * Also exposes the internal wiring functions called by esp_now_stub.c.
 *
 * Concurrency note: pump() runs in the Unity/main task, not the WiFi task.
 * Priority inversion and scheduling jitter are not testable with this mock.
 * System-level timing issues are validated on hardware via the two examples.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_now.h"   /* for esp_now_recv_cb_t, esp_now_send_cb_t, esp_now_send_status_t */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Maximum pending expectations
 * ========================================================================= */

#define MOCK_TRANSPORT_MAX_EXPECTATIONS 16

/* =========================================================================
 * Public mock API — used by test cases
 * ========================================================================= */

/**
 * Reset all state: clear expectations, queued responses, and captured frames.
 * Call at the start of each test case (typically in setUp()).
 */
void mock_transport_reset(void);

/**
 * Register a canned response.
 *
 * When esp_now_send() is called with a frame whose first byte matches
 * trigger_type, the response frame is queued for delivery via pump().
 *
 * Multiple expectations may be registered; they are matched in order.
 * Once an expectation is consumed it is removed from the queue.
 *
 * @param trigger_type   First byte of the outgoing frame that triggers this response.
 * @param response_frame Frame bytes to deliver into recv_cb.
 * @param response_len   Length of response_frame in bytes.
 * @param src_mac        Source MAC to use when delivering the response (6 bytes).
 * @param send_result    ESP_NOW_SEND_SUCCESS or FAIL to fire in the send_cb.
 */
void mock_transport_expect_send(uint8_t        trigger_type,
                                 const uint8_t *response_frame,
                                 size_t         response_len,
                                 const uint8_t *src_mac,
                                 esp_now_send_status_t send_result);

/**
 * Deliver all queued responses synchronously.
 *
 * For each pending response:
 *   1. Fire send_cb with the registered send_result.
 *   2. Call esp_now_stub_inject_recv() to deliver the response frame.
 *
 * After pump() returns, all pending expectations have been consumed.
 * Call pump() after each esp_now_send() call that is expected to trigger a response.
 */
void mock_transport_pump(void);

/**
 * Return the number of times esp_now_send() was called since the last reset.
 * Useful for verifying frame counts in Tier 2 tests.
 */
int mock_transport_send_count(void);

/**
 * Return a pointer to the Nth captured send frame (0-indexed).
 * Returns NULL if n is out of range.
 * The returned pointer is valid until the next mock_transport_reset().
 */
const uint8_t *mock_transport_get_sent_frame(int n);

/**
 * Return the length of the Nth captured send frame.
 * Returns 0 if n is out of range.
 */
size_t mock_transport_get_sent_frame_len(int n);

/* =========================================================================
 * Internal wiring — called by esp_now_stub.c only
 * ========================================================================= */

/**
 * Store the recv_cb pointer registered by the component via
 * esp_now_register_recv_cb(). Called by esp_now_stub.c.
 */
void mock_transport_set_recv_cb(esp_now_recv_cb_t cb);

/**
 * Store the send_cb pointer registered by the component via
 * esp_now_register_send_cb(). Called by esp_now_stub.c.
 */
void mock_transport_set_send_cb(esp_now_send_cb_t cb);

/**
 * Intercept an outgoing esp_now_send() call.
 * Captures the frame, matches against pending expectations, and queues
 * responses for delivery by pump(). Called by esp_now_stub.c esp_now_send().
 *
 * @param peer_addr  Destination MAC (6 bytes).
 * @param data       Frame bytes.
 * @param len        Frame length.
 * @return ESP_OK always (stub always succeeds at the transport layer).
 */
esp_err_t mock_transport_intercept_send(const uint8_t *peer_addr,
                                         const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
