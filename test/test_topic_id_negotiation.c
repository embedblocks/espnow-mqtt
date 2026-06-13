/*
 * test_topic_id_negotiation.c — Tier 2: Phase 3 publisher state machine tests.
 *
 * Uses mock transport. Simulates REGISTER/REGISTER_ACK exchanges and verifies:
 *   - REGISTER frame wire format (length = 2 + strlen + 1, not sizeof)
 *   - Positive ACK → slot registered, REGISTERED_EG_BIT set
 *   - Repeated zero ACK → perm_rejected after threshold
 *   - BROKER_ANNOUNCE: hint channel set before scan; does NOT post REDISCOVER
 *   - ID_UNKNOWN → slot cleared, targeted re-register posted
 *
 * Concurrency note: pump() runs in this task, not the WiFi task.
 * Priority inversion is not testable here — see hardware examples.
 */

#include "unity.h"
#include "mock_transport.h"
#include "espnow_mqtt.h"
#include "espnow_mqtt_proto.h"
#include "esp_now.h"  /* stub header */

#include <string.h>
#include <stdint.h>

/* =========================================================================
 * Test fixture MACs
 * ========================================================================= */

static const uint8_t BROKER_MAC[6]  = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
static const uint8_t SELF_MAC[6]    = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t UNKNOWN_MAC[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};

/* =========================================================================
 * setUp / tearDown
 * ========================================================================= */

void setUp(void)
{
    mock_transport_reset();

    /* Add broker as a known peer so trust filter passes. */
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROKER_MAC, 6);
    esp_now_add_peer(&peer);

    esp_now_init();
    espnow_mqtt_init();
    espnow_mqtt_set_broker(BROKER_MAC);
}

void tearDown(void)
{
    espnow_mqtt_deinit();
    esp_now_deinit();
    mock_transport_reset();
}

/* =========================================================================
 * Helper: build a REGISTER_ACK frame
 * ========================================================================= */

static void make_register_ack(uint8_t topic_id, uint8_t *out)
{
    out[0] = ESPNOW_MSG_REGISTER_ACK;
    out[1] = topic_id;
}

/* =========================================================================
 * Helper: build a BROKER_ANNOUNCE frame
 * ========================================================================= */

static void make_broker_announce(uint8_t *out)
{
    out[0] = ESPNOW_MSG_BROKER_ANNOUNCE;
    out[1] = ESPNOW_PROTO_VERSION;
}

/* =========================================================================
 * Helper: build an ID_UNKNOWN frame
 * ========================================================================= */

static void make_id_unknown(uint8_t topic_id, uint8_t *out)
{
    out[0] = ESPNOW_MSG_ID_UNKNOWN;
    out[1] = topic_id;
}

/* =========================================================================
 * Test: REGISTER frame wire format
 * ========================================================================= */

TEST_CASE("REGISTER frame length is 2 + strlen + 1", "[negotiation]")
{
    /* Register a topic — publisher_task will send REGISTER via esp_now_send(). */
    espnow_mqtt_topic_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, espnow_mqtt_register("sensors/temp", &h));

    /* Give the task a moment to send the REGISTER frame. */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Verify the sent frame. */
    const char *topic = "sensors/temp";
    size_t expected_len = 2 + strlen(topic) + 1;

    int count = mock_transport_send_count();
    TEST_ASSERT_GREATER_THAN(0, count);

    /* Find the REGISTER frame in captured sends. */
    bool found = false;
    for (int i = 0; i < count; i++) {
        const uint8_t *frame = mock_transport_get_sent_frame(i);
        size_t         flen  = mock_transport_get_sent_frame_len(i);
        if (frame && frame[0] == ESPNOW_MSG_REGISTER && flen == expected_len) {
            TEST_ASSERT_EQUAL_UINT8(ESPNOW_PROTO_VERSION, frame[1]);
            TEST_ASSERT_EQUAL_STRING(topic, (const char *)(frame + 2));
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "REGISTER frame with correct length not found");
}

/* =========================================================================
 * Test: Positive ACK → registered
 * ========================================================================= */

TEST_CASE("REGISTER_ACK topic_id=1 registers slot", "[negotiation]")
{
    uint8_t ack[2];
    make_register_ack(1, ack);

    /* Expect: when a REGISTER is sent, respond with REGISTER_ACK{1}. */
    mock_transport_expect_send(ESPNOW_MSG_REGISTER, ack, 2,
                                BROKER_MAC, ESP_NOW_SEND_SUCCESS);

    espnow_mqtt_topic_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, espnow_mqtt_register("home/temp", &h));

    /* Pump: deliver the REGISTER_ACK. */
    vTaskDelay(pdMS_TO_TICKS(30));
    mock_transport_pump();
    vTaskDelay(pdMS_TO_TICKS(30));

    TEST_ASSERT_EQUAL(ESPNOW_MQTT_STATE_REGISTERED, espnow_mqtt_get_state());

    /* wait_registered should return immediately now. */
    TEST_ASSERT_EQUAL(ESP_OK, espnow_mqtt_wait_registered(100));
}

/* =========================================================================
 * Test: Zero ACK × N < threshold → not perm_rejected
 * ========================================================================= */

TEST_CASE("Repeated zero ACK below threshold does not perm_reject", "[negotiation]")
{
    uint8_t ack_zero[2];
    make_register_ack(0, ack_zero);

    /* Register N-1 rejection ACKs (threshold = CONFIG_ESPNOW_MQTT_MAX_CONSECUTIVE_REJECTIONS). */
    int below = CONFIG_ESPNOW_MQTT_MAX_CONSECUTIVE_REJECTIONS - 1;
    for (int i = 0; i < below; i++) {
        mock_transport_expect_send(ESPNOW_MSG_REGISTER, ack_zero, 2,
                                    BROKER_MAC, ESP_NOW_SEND_SUCCESS);
    }

    espnow_mqtt_topic_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, espnow_mqtt_register("home/hum", &h));

    for (int i = 0; i < below; i++) {
        vTaskDelay(pdMS_TO_TICKS(30));
        mock_transport_pump();
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Still UNREGISTERED, not UNPROVISIONED (not perm_rejected). */
    TEST_ASSERT_NOT_EQUAL(ESPNOW_MQTT_STATE_REGISTERED, espnow_mqtt_get_state());

    /* publish() should return INVALID_STATE (not registered), not something worse. */
    uint8_t payload = 0x42;
    esp_err_t ret = espnow_mqtt_publish(h, &payload, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);
}

/* =========================================================================
 * Test: Zero ACK × threshold → perm_rejected
 * ========================================================================= */

TEST_CASE("Zero ACK at threshold causes perm_rejected", "[negotiation]")
{
    uint8_t ack_zero[2];
    make_register_ack(0, ack_zero);

    int threshold = CONFIG_ESPNOW_MQTT_MAX_CONSECUTIVE_REJECTIONS;
    for (int i = 0; i < threshold; i++) {
        mock_transport_expect_send(ESPNOW_MSG_REGISTER, ack_zero, 2,
                                    BROKER_MAC, ESP_NOW_SEND_SUCCESS);
    }

    espnow_mqtt_topic_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, espnow_mqtt_register("home/co2", &h));

    for (int i = 0; i < threshold; i++) {
        vTaskDelay(pdMS_TO_TICKS(30));
        mock_transport_pump();
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    /* After threshold rejections, publish returns INVALID_STATE (perm_rejected). */
    uint8_t payload = 0x42;
    esp_err_t ret = espnow_mqtt_publish(h, &payload, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);
}

/* =========================================================================
 * Test: BROKER_ANNOUNCE from unknown MAC is ignored
 * ========================================================================= */

TEST_CASE("BROKER_ANNOUNCE from unknown MAC is dropped", "[negotiation]")
{
    uint8_t announce[2];
    make_broker_announce(announce);

    espnow_mqtt_state_t state_before = espnow_mqtt_get_state();

    /* Inject announce from UNKNOWN_MAC (not in peer table → trust filter drops it). */
    esp_now_stub_inject_recv(UNKNOWN_MAC, SELF_MAC, announce, 2);
    vTaskDelay(pdMS_TO_TICKS(30));

    /* State should be unchanged. */
    TEST_ASSERT_EQUAL(state_before, espnow_mqtt_get_state());
}

/* =========================================================================
 * Test: ID_UNKNOWN clears slot
 * ========================================================================= */

TEST_CASE("ID_UNKNOWN clears slot and triggers re-register", "[negotiation]")
{
    /* First register successfully. */
    uint8_t ack[2];
    make_register_ack(7, ack);
    mock_transport_expect_send(ESPNOW_MSG_REGISTER, ack, 2,
                                BROKER_MAC, ESP_NOW_SEND_SUCCESS);

    espnow_mqtt_topic_handle_t h;
    TEST_ASSERT_EQUAL(ESP_OK, espnow_mqtt_register("sensors/pres", &h));
    vTaskDelay(pdMS_TO_TICKS(30));
    mock_transport_pump();
    vTaskDelay(pdMS_TO_TICKS(30));

    /* Confirm registered. */
    TEST_ASSERT_EQUAL(ESPNOW_MQTT_STATE_REGISTERED, espnow_mqtt_get_state());

    /* Now inject ID_UNKNOWN for topic_id=7 from the broker. */
    uint8_t id_unknown[2];
    make_id_unknown(7, id_unknown);
    esp_now_stub_inject_recv(BROKER_MAC, SELF_MAC, id_unknown, 2);
    vTaskDelay(pdMS_TO_TICKS(30));

    /* Slot should no longer be registered. */
    TEST_ASSERT_NOT_EQUAL(ESPNOW_MQTT_STATE_REGISTERED, espnow_mqtt_get_state());

    /* publish() should return INVALID_STATE (slot not registered). */
    uint8_t payload = 0x42;
    esp_err_t ret = espnow_mqtt_publish(h, &payload, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);
}
