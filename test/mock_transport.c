/*
 * mock_transport.c — Mock ESP-NOW transport implementation.
 *
 * See mock_transport.h for the public API contract.
 */

#include "mock_transport.h"
#include "esp_now.h"   /* stub header: esp_now_stub_inject_recv, esp_now_stub_fire_send_cb */
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Internal state
 * ========================================================================= */

#define MOCK_MAX_FRAME_LEN   250
#define MOCK_MAX_SENT_FRAMES 32

/* A pending canned response entry. */
typedef struct {
    uint8_t               trigger_type;
    uint8_t               response_frame[MOCK_MAX_FRAME_LEN];
    size_t                response_len;
    uint8_t               src_mac[6];
    esp_now_send_status_t send_result;
    bool                  used;
} mock_expectation_t;

/* A captured outgoing frame. */
typedef struct {
    uint8_t data[MOCK_MAX_FRAME_LEN];
    size_t  len;
} mock_sent_frame_t;

static esp_now_recv_cb_t   s_recv_cb    = NULL;
static esp_now_send_cb_t   s_send_cb    = NULL;

static mock_expectation_t  s_expectations[MOCK_TRANSPORT_MAX_EXPECTATIONS];
static int                 s_expect_count  = 0;

/* Queued responses waiting for pump(). */
typedef struct {
    mock_expectation_t exp;
    uint8_t            dst_mac[6];   /* peer_addr from the triggering send */
} mock_queued_response_t;

static mock_queued_response_t s_queued[MOCK_TRANSPORT_MAX_EXPECTATIONS];
static int                    s_queued_count = 0;

static mock_sent_frame_t   s_sent_frames[MOCK_MAX_SENT_FRAMES];
static int                 s_send_count    = 0;

/* =========================================================================
 * Internal wiring (called by esp_now_stub.c)
 * ========================================================================= */

void mock_transport_set_recv_cb(esp_now_recv_cb_t cb)
{
    s_recv_cb = cb;
}

void mock_transport_set_send_cb(esp_now_send_cb_t cb)
{
    s_send_cb = cb;
}

/* =========================================================================
 * Public mock API
 * ========================================================================= */

void mock_transport_reset(void)
{
    s_recv_cb      = NULL;
    s_send_cb      = NULL;
    s_expect_count = 0;
    s_queued_count = 0;
    s_send_count   = 0;
    memset(s_expectations, 0, sizeof(s_expectations));
    memset(s_queued,        0, sizeof(s_queued));
    memset(s_sent_frames,   0, sizeof(s_sent_frames));
}

void mock_transport_expect_send(uint8_t        trigger_type,
                                 const uint8_t *response_frame,
                                 size_t         response_len,
                                 const uint8_t *src_mac,
                                 esp_now_send_status_t send_result)
{
    if (s_expect_count >= MOCK_TRANSPORT_MAX_EXPECTATIONS) return;
    if (!response_frame || !src_mac) return;
    if (response_len > MOCK_MAX_FRAME_LEN) response_len = MOCK_MAX_FRAME_LEN;

    mock_expectation_t *e = &s_expectations[s_expect_count++];
    e->trigger_type = trigger_type;
    memcpy(e->response_frame, response_frame, response_len);
    e->response_len = response_len;
    memcpy(e->src_mac, src_mac, 6);
    e->send_result  = send_result;
    e->used         = false;
}

esp_err_t mock_transport_intercept_send(const uint8_t *peer_addr,
                                         const uint8_t *data, size_t len)
{
    /* Capture the outgoing frame. */
    if (s_send_count < MOCK_MAX_SENT_FRAMES && data && len > 0) {
        size_t cap = len < MOCK_MAX_FRAME_LEN ? len : MOCK_MAX_FRAME_LEN;
        memcpy(s_sent_frames[s_send_count].data, data, cap);
        s_sent_frames[s_send_count].len = cap;
        s_send_count++;
    }

    /* Match against the first unused expectation whose trigger_type matches. */
    if (!data || len == 0) return ESP_OK;

    uint8_t type = data[0];
    for (int i = 0; i < s_expect_count; i++) {
        if (!s_expectations[i].used &&
            s_expectations[i].trigger_type == type) {
            /* Queue the response for delivery via pump(). */
            if (s_queued_count < MOCK_TRANSPORT_MAX_EXPECTATIONS) {
                s_queued[s_queued_count].exp = s_expectations[i];
                if (peer_addr) {
                    memcpy(s_queued[s_queued_count].dst_mac, peer_addr, 6);
                }
                s_queued_count++;
            }
            s_expectations[i].used = true;
            break;
        }
    }

    return ESP_OK;
}

void mock_transport_pump(void)
{
    static const uint8_t zero_mac[6] = {0};

    for (int i = 0; i < s_queued_count; i++) {
        mock_queued_response_t *q = &s_queued[i];

        /* 1. Fire the send_cb with the registered result. */
        if (s_send_cb) {
            esp_now_stub_fire_send_cb(q->dst_mac, q->exp.send_result);
        }

        /* 2. Deliver the canned response frame into the component's recv_cb. */
        if (s_recv_cb && q->exp.response_len > 0) {
            esp_now_stub_inject_recv(q->exp.src_mac,
                                      zero_mac,    /* dst_mac: not used by component */
                                      q->exp.response_frame,
                                      (int)q->exp.response_len);
        }
    }
    s_queued_count = 0;
}

int mock_transport_send_count(void)
{
    return s_send_count;
}

const uint8_t *mock_transport_get_sent_frame(int n)
{
    if (n < 0 || n >= s_send_count) return NULL;
    return s_sent_frames[n].data;
}

size_t mock_transport_get_sent_frame_len(int n)
{
    if (n < 0 || n >= s_send_count) return 0;
    return s_sent_frames[n].len;
}
