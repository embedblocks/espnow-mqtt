/*
 * test_wire_protocol.c — Tier 1: Phase 1 wire protocol tests.
 *
 * Tests all struct sizes, constants, and seq encode/decode helpers.
 * No radio, no recv_cb, no state machine. Pure computation.
 *
 * _Static_assert checks run at compile time. The corresponding Unity runtime
 * checks confirm the values are correct from the test runner's perspective
 * (useful for catching any conditional compilation surprises).
 */

#include "unity.h"
#include "espnow_mqtt_proto.h"
#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Test group setup / teardown
 * ========================================================================= */

void setUp(void)   { /* nothing */ }
void tearDown(void){ /* nothing */ }

/* =========================================================================
 * Constants
 * ========================================================================= */

TEST_CASE("ESPNOW_MAX_TOPIC_LEN is 247", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(247, ESPNOW_MAX_TOPIC_LEN);
}

TEST_CASE("ESPNOW_MAX_PAYLOAD_LEN is 246", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(246, ESPNOW_MAX_PAYLOAD_LEN);
}

TEST_CASE("ESPNOW_MAX_PAYLOAD_HMAC_LEN is 230", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(230, ESPNOW_MAX_PAYLOAD_HMAC_LEN);
}

TEST_CASE("ESPNOW_PROTO_VERSION is 0x01", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_HEX8(0x01, ESPNOW_PROTO_VERSION);
}

/* =========================================================================
 * Message type constants
 * ========================================================================= */

TEST_CASE("espnow_msg_type_t is 1 byte", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(1, sizeof(espnow_msg_type_t));
}

TEST_CASE("Message type values are correct", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_HEX8(0x01, ESPNOW_MSG_REGISTER);
    TEST_ASSERT_EQUAL_HEX8(0x02, ESPNOW_MSG_REGISTER_ACK);
    TEST_ASSERT_EQUAL_HEX8(0x03, ESPNOW_MSG_PUBLISH);
    TEST_ASSERT_EQUAL_HEX8(0x04, ESPNOW_MSG_ID_UNKNOWN);
    TEST_ASSERT_EQUAL_HEX8(0x05, ESPNOW_MSG_BROKER_ANNOUNCE);
}

/* =========================================================================
 * Frame struct sizes
 * (Also verified at compile time via _Static_assert in espnow_mqtt_proto.h)
 * ========================================================================= */

TEST_CASE("espnow_register_frame_t is 250 bytes", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(250, sizeof(espnow_register_frame_t));
}

TEST_CASE("espnow_register_ack_frame_t is 2 bytes", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(2, sizeof(espnow_register_ack_frame_t));
}

TEST_CASE("espnow_publish_frame_t is 4 bytes", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(4, sizeof(espnow_publish_frame_t));
}

TEST_CASE("espnow_id_unknown_frame_t is 2 bytes", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(2, sizeof(espnow_id_unknown_frame_t));
}

TEST_CASE("espnow_broker_announce_frame_t is 2 bytes", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(2, sizeof(espnow_broker_announce_frame_t));
}

/* =========================================================================
 * Frame struct field offsets
 * ========================================================================= */

TEST_CASE("espnow_register_frame_t field offsets", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(0, offsetof(espnow_register_frame_t, type));
    TEST_ASSERT_EQUAL_INT(1, offsetof(espnow_register_frame_t, version));
    TEST_ASSERT_EQUAL_INT(2, offsetof(espnow_register_frame_t, topic));
}

TEST_CASE("espnow_register_ack_frame_t field offsets", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(0, offsetof(espnow_register_ack_frame_t, type));
    TEST_ASSERT_EQUAL_INT(1, offsetof(espnow_register_ack_frame_t, topic_id));
}

TEST_CASE("espnow_publish_frame_t field offsets", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(0, offsetof(espnow_publish_frame_t, type));
    TEST_ASSERT_EQUAL_INT(1, offsetof(espnow_publish_frame_t, topic_id));
    TEST_ASSERT_EQUAL_INT(2, offsetof(espnow_publish_frame_t, seq_lo));
    TEST_ASSERT_EQUAL_INT(3, offsetof(espnow_publish_frame_t, seq_hi));
}

TEST_CASE("espnow_id_unknown_frame_t field offsets", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(0, offsetof(espnow_id_unknown_frame_t, type));
    TEST_ASSERT_EQUAL_INT(1, offsetof(espnow_id_unknown_frame_t, topic_id));
}

TEST_CASE("espnow_broker_announce_frame_t field offsets", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(0, offsetof(espnow_broker_announce_frame_t, type));
    TEST_ASSERT_EQUAL_INT(1, offsetof(espnow_broker_announce_frame_t, version));
}

/* =========================================================================
 * Minimum frame length constants
 * ========================================================================= */

TEST_CASE("ESPNOW_MIN_LEN constants match struct sizes", "[wire_protocol]")
{
    TEST_ASSERT_EQUAL_INT(2, ESPNOW_MIN_LEN_REGISTER);
    TEST_ASSERT_EQUAL_INT(2, ESPNOW_MIN_LEN_REGISTER_ACK);
    TEST_ASSERT_EQUAL_INT(4, ESPNOW_MIN_LEN_PUBLISH);
    TEST_ASSERT_EQUAL_INT(2, ESPNOW_MIN_LEN_ID_UNKNOWN);
    TEST_ASSERT_EQUAL_INT(2, ESPNOW_MIN_LEN_BROKER_ANNOUNCE);

    /* Cross-check: min lens must equal the corresponding struct sizes
     * for the fixed-size frames. */
    TEST_ASSERT_EQUAL_INT(sizeof(espnow_register_ack_frame_t),
                          ESPNOW_MIN_LEN_REGISTER_ACK);
    TEST_ASSERT_EQUAL_INT(sizeof(espnow_publish_frame_t),
                          ESPNOW_MIN_LEN_PUBLISH);
    TEST_ASSERT_EQUAL_INT(sizeof(espnow_id_unknown_frame_t),
                          ESPNOW_MIN_LEN_ID_UNKNOWN);
    TEST_ASSERT_EQUAL_INT(sizeof(espnow_broker_announce_frame_t),
                          ESPNOW_MIN_LEN_BROKER_ANNOUNCE);
}

/* =========================================================================
 * Sequence counter encode / decode round-trip
 * ========================================================================= */

static void seq_roundtrip(uint16_t seq)
{
    uint8_t lo, hi;
    espnow_proto_encode_seq(seq, &lo, &hi);
    uint16_t decoded = espnow_proto_decode_seq(lo, hi);
    TEST_ASSERT_EQUAL_UINT16(seq, decoded);
}

TEST_CASE("Seq round-trip: 0", "[wire_protocol]")
{
    seq_roundtrip(0);
}

TEST_CASE("Seq round-trip: 1", "[wire_protocol]")
{
    seq_roundtrip(1);
}

TEST_CASE("Seq round-trip: 255 (low byte max)", "[wire_protocol]")
{
    seq_roundtrip(255);
}

TEST_CASE("Seq round-trip: 256 (first carry into high byte)", "[wire_protocol]")
{
    seq_roundtrip(256);
}

TEST_CASE("Seq round-trip: 65535 (uint16_t max)", "[wire_protocol]")
{
    seq_roundtrip(65535);
}

TEST_CASE("Seq encode: 65535 wraps to 0 on increment", "[wire_protocol]")
{
    /* Verify the wrap-around uint16_t arithmetic used by the publisher seq counter. */
    uint16_t seq = 65535;
    seq++;  /* natural uint16_t overflow -> 0 */
    TEST_ASSERT_EQUAL_UINT16(0, seq);

    uint8_t lo, hi;
    espnow_proto_encode_seq(seq, &lo, &hi);
    TEST_ASSERT_EQUAL_UINT8(0, lo);
    TEST_ASSERT_EQUAL_UINT8(0, hi);
}

TEST_CASE("Seq encode byte values for known inputs", "[wire_protocol]")
{
    uint8_t lo, hi;

    /* seq = 0x0100 = 256: lo=0x00, hi=0x01 */
    espnow_proto_encode_seq(0x0100, &lo, &hi);
    TEST_ASSERT_EQUAL_HEX8(0x00, lo);
    TEST_ASSERT_EQUAL_HEX8(0x01, hi);

    /* seq = 0xABCD: lo=0xCD, hi=0xAB */
    espnow_proto_encode_seq(0xABCD, &lo, &hi);
    TEST_ASSERT_EQUAL_HEX8(0xCD, lo);
    TEST_ASSERT_EQUAL_HEX8(0xAB, hi);
}

TEST_CASE("Seq wrap-aware delta is correct for broker gap detection", "[wire_protocol]")
{
    /* Verify the int16_t delta arithmetic used by the broker seq tracker.
     * This is the core of the wrap-aware gap/duplicate detection. */

    /* Normal advance: delta = +1 */
    uint16_t last = 100;
    uint16_t next = 101;
    int16_t  delta = (int16_t)(next - last);
    TEST_ASSERT_EQUAL_INT(1, delta);

    /* Gap: delta = +3 -> 2 frames lost */
    last = 100; next = 103;
    delta = (int16_t)(next - last);
    TEST_ASSERT_EQUAL_INT(3, delta);

    /* Duplicate/reorder: delta <= 0 */
    last = 100; next = 100;
    delta = (int16_t)(next - last);
    TEST_ASSERT_EQUAL_INT(0, delta);

    last = 100; next = 99;
    delta = (int16_t)(next - last);
    TEST_ASSERT_EQUAL_INT(-1, delta);

    /* Wrap-around: last=65535, next=1 -> delta = +2 (1 frame lost)
     * uint16_t subtraction: 1 - 65535 = 2 (mod 65536) -> int16_t(2) = +2 */
    last = 65535; next = 1;
    delta = (int16_t)(next - last);
    TEST_ASSERT_EQUAL_INT(2, delta);

    /* Wrap-around normal: last=65535, next=0 -> delta = +1 (no loss) */
    last = 65535; next = 0;
    delta = (int16_t)(next - last);
    TEST_ASSERT_EQUAL_INT(1, delta);
}
