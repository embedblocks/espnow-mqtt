/*
 * esp_now_stub.c — ESP-NOW stub implementation for test_app builds.
 *
 * Provides the same public symbols as the real esp_now component but routes
 * all calls through mock_transport.c instead of the WiFi/PHY stack.
 *
 * Uses IDF >= 5.5.0 callback signatures exclusively. No compatibility shims.
 */

#include "esp_now.h"     /* stub header from this component */
#include "mock_transport.h"
#include <string.h>
#include <stdint.h>
#include "esp_err.h"

/* =========================================================================
 * Registered callback pointers
 *
 * Captured by mock_transport_init() so pump() can deliver frames into the
 * real component recv_cb without going through the WiFi stack.
 * ========================================================================= */

static esp_now_recv_cb_t s_recv_cb = NULL;
static esp_now_send_cb_t s_send_cb = NULL;

/* =========================================================================
 * Peer list — simple fixed-size table for test use
 * ========================================================================= */

#define STUB_MAX_PEERS 32

static uint8_t s_peers[STUB_MAX_PEERS][6];
static int     s_peer_count = 0;

static bool peer_find(const uint8_t *mac)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(s_peers[i], mac, 6) == 0) return true;
    }
    return false;
}

/* =========================================================================
 * ESP-NOW lifecycle stubs
 * ========================================================================= */

esp_err_t esp_now_init(void)
{
    s_recv_cb    = NULL;
    s_send_cb    = NULL;
    s_peer_count = 0;
    memset(s_peers, 0, sizeof(s_peers));
    return ESP_OK;
}

esp_err_t esp_now_deinit(void)
{
    s_recv_cb    = NULL;
    s_send_cb    = NULL;
    s_peer_count = 0;
    return ESP_OK;
}

/* =========================================================================
 * Callback registration
 * ========================================================================= */

esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb)
{
    s_recv_cb = cb;
    /* Expose to mock_transport so pump() can call it. */
    mock_transport_set_recv_cb(cb);
    return ESP_OK;
}

esp_err_t esp_now_unregister_recv_cb(void)
{
    s_recv_cb = NULL;
    mock_transport_set_recv_cb(NULL);
    return ESP_OK;
}

esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb)
{
    s_send_cb = cb;
    mock_transport_set_send_cb(cb);
    return ESP_OK;
}

esp_err_t esp_now_unregister_send_cb(void)
{
    s_send_cb = NULL;
    mock_transport_set_send_cb(NULL);
    return ESP_OK;
}

/* =========================================================================
 * Send — intercepted by mock_transport
 * ========================================================================= */

esp_err_t esp_now_send(const uint8_t *peer_addr,
                        const uint8_t *data, size_t len)
{
    return mock_transport_intercept_send(peer_addr, data, len);
}

/* =========================================================================
 * Peer management
 * ========================================================================= */

esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer)
{
    if (!peer) return ESP_ERR_INVALID_ARG;
    if (peer_find(peer->peer_addr)) return ESP_OK;  /* idempotent */
    if (s_peer_count >= STUB_MAX_PEERS) return ESP_ERR_NO_MEM;
    memcpy(s_peers[s_peer_count], peer->peer_addr, 6);
    s_peer_count++;
    return ESP_OK;
}

esp_err_t esp_now_del_peer(const uint8_t *peer_addr)
{
    if (!peer_addr) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(s_peers[i], peer_addr, 6) == 0) {
            /* Compact the table. */
            s_peer_count--;
            if (i < s_peer_count) {
                memcpy(s_peers[i], s_peers[s_peer_count], 6);
            }
            memset(s_peers[s_peer_count], 0, 6);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

bool esp_now_is_peer_exist(const uint8_t *peer_addr)
{
    if (!peer_addr) return false;
    return peer_find(peer_addr);
}

esp_err_t esp_now_get_peer_num(esp_now_peer_num_t *num)
{
    if (!num) return ESP_ERR_INVALID_ARG;
    num->total_num   = s_peer_count;
    num->encrypt_num = 0;  /* stub: no encrypted peers */
    return ESP_OK;
}

/* =========================================================================
 * Stub-internal injection helpers
 * Called by mock_transport_pump() — not part of the real esp_now.h API.
 * ========================================================================= */

void esp_now_stub_inject_recv(const uint8_t *src_mac,
                               const uint8_t *dst_mac,
                               const uint8_t *data, int data_len)
{
    if (!s_recv_cb || !src_mac || !dst_mac || !data) return;

    /* Build the esp_now_recv_info_t on the stack as mock_transport would. */
    esp_now_recv_info_t info = {
        .src_addr = (uint8_t *)src_mac,  /* cast away const: matches IDF API */
        .des_addr = (uint8_t *)dst_mac,
        .rx_ctrl  = NULL,                /* not used by espnow_mqtt */
    };
    s_recv_cb(&info, data, data_len);
}

void esp_now_stub_fire_send_cb(const uint8_t *dst_mac,
                                esp_now_send_status_t status)
{
    if (!s_send_cb || !dst_mac) return;

    esp_now_send_info_t tx_info = {
        .des_addr  = (uint8_t *)dst_mac,
        .tx_status = status,
    };
    s_send_cb(&tx_info, status);
}
