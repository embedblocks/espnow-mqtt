/*
 * esp_now.h — ESP-NOW stub for test_app builds.
 *
 * This file is included INSTEAD OF the real <esp_now.h> in test_app builds.
 * It provides the same public API surface that espnow_mqtt uses, backed by
 * mock_transport.c rather than the WiFi/PHY stack.
 *
 * Struct definitions match ESP-IDF >= 5.5.0 layout exactly.
 * _Static_assert checks on field offsets catch any future layout divergence.
 *
 * DO NOT include the real <esp_now.h> from this file — the stub component
 * replaces the real esp_now component in the test_app build, so the real
 * header is not available in that context.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

#define ESP_NOW_MAX_DATA_LEN        250  /**< Maximum ESP-NOW payload (v1.0) */
#define ESP_NOW_ETH_ALEN              6  /**< MAC address length in bytes */
#define ESP_NOW_KEY_LEN              16  /**< LMK key length in bytes */

/* =========================================================================
 * Send status
 * ========================================================================= */

typedef enum {
    ESP_NOW_SEND_SUCCESS = 0, /**< Frame delivered to MAC layer */
    ESP_NOW_SEND_FAIL,        /**< Frame delivery failed */
} esp_now_send_status_t;

/* =========================================================================
 * Callback parameter structs — IDF >= 5.5.0 layout
 *
 * recv_cb parameter changed at IDF 5.0: const uint8_t *mac_addr ->
 *   const esp_now_recv_info_t *esp_now_info
 *
 * send_cb parameter changed at IDF 5.5: const uint8_t *mac_addr ->
 *   const esp_now_send_info_t *tx_info
 * ========================================================================= */

/**
 * Receive callback parameter struct (IDF >= 5.0).
 * src_addr replaces the old mac_addr parameter.
 * des_addr is the destination MAC (unicast or broadcast).
 * rx_ctrl points to wifi_pkt_rx_ctrl_t — NULL in the stub (not needed by component).
 */
typedef struct {
    uint8_t *src_addr;  /**< Source MAC address of the received frame */
    uint8_t *des_addr;  /**< Destination MAC address (unicast or broadcast) */
    void    *rx_ctrl;   /**< Rx control info (RSSI etc) — NULL in stub */
} esp_now_recv_info_t;

/* Layout check: src_addr must be the first field. */
_Static_assert(offsetof(esp_now_recv_info_t, src_addr) == 0,
               "esp_now_recv_info_t.src_addr must be at offset 0");

/**
 * Send callback parameter struct (IDF >= 5.5).
 * des_addr is the destination MAC of the sent frame.
 * tx_status carries the send result (same value as the status parameter).
 */
typedef struct {
    uint8_t              *des_addr;  /**< Destination MAC of the sent frame */
    esp_now_send_status_t tx_status; /**< Send result */
} esp_now_send_info_t;

/* Layout check: des_addr must be the first field. */
_Static_assert(offsetof(esp_now_send_info_t, des_addr) == 0,
               "esp_now_send_info_t.des_addr must be at offset 0");

/* =========================================================================
 * Callback typedefs — IDF >= 5.5.0 signatures
 * ========================================================================= */

/** Receive callback — first parameter is esp_now_recv_info_t* (IDF >= 5.0). */
typedef void (*esp_now_recv_cb_t)(const esp_now_recv_info_t *esp_now_info,
                                   const uint8_t *data, int data_len);

/** Send callback — first parameter is esp_now_send_info_t* (IDF >= 5.5). */
typedef void (*esp_now_send_cb_t)(const esp_now_send_info_t *tx_info,
                                   esp_now_send_status_t status);

/* =========================================================================
 * Peer info
 * ========================================================================= */

typedef struct {
    uint8_t  peer_addr[ESP_NOW_ETH_ALEN]; /**< Peer MAC address */
    uint8_t  lmk[ESP_NOW_KEY_LEN];        /**< LMK (unused; set encrypt=false) */
    uint8_t  channel;                      /**< 0 = use current channel */
    bool     encrypt;                      /**< Must be false for this component */
    void    *priv;                         /**< Private data (unused) */
} esp_now_peer_info_t;

typedef struct {
    int total_num;   /**< Total number of peers in the peer list */
    int encrypt_num; /**< Number of encrypted peers */
} esp_now_peer_num_t;

/* =========================================================================
 * Public API — matches real esp_now.h
 * ========================================================================= */

esp_err_t esp_now_init(void);
esp_err_t esp_now_deinit(void);

esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb);
esp_err_t esp_now_unregister_recv_cb(void);

esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb);
esp_err_t esp_now_unregister_send_cb(void);

esp_err_t esp_now_send(const uint8_t *peer_addr,
                        const uint8_t *data, size_t len);

esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer);
esp_err_t esp_now_del_peer(const uint8_t *peer_addr);
bool      esp_now_is_peer_exist(const uint8_t *peer_addr);
esp_err_t esp_now_get_peer_num(esp_now_peer_num_t *num);

/* =========================================================================
 * Stub-internal injection API
 *
 * NOT part of the real esp_now.h — called by mock_transport.c only.
 * Declared here so mock_transport.c can include this header and call them.
 * ========================================================================= */

/**
 * Inject a received frame directly into the component's recv_cb.
 * Called by mock_transport_pump() to simulate an incoming ESP-NOW frame.
 *
 * @param src_mac   Source MAC (6 bytes).
 * @param dst_mac   Destination MAC (6 bytes).
 * @param data      Frame bytes.
 * @param data_len  Frame length.
 */
void esp_now_stub_inject_recv(const uint8_t *src_mac,
                               const uint8_t *dst_mac,
                               const uint8_t *data, int data_len);

/**
 * Fire the send callback with a given result.
 * Called by mock_transport_pump() after esp_now_send() is intercepted.
 *
 * @param dst_mac  Destination MAC of the sent frame.
 * @param status   Send result to deliver to the callback.
 */
void esp_now_stub_fire_send_cb(const uint8_t *dst_mac,
                                esp_now_send_status_t status);

#ifdef __cplusplus
}
#endif
