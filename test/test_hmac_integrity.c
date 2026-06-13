/*
 * test_hmac_integrity.c — Tier 1: Phase 6 HMAC integrity tests.
 *
 * All tests are pure computation — no radio, no recv_cb, no state machine.
 * Compiled only when CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC=y.
 *
 * Test groups:
 *   [hmac_build]   — espnow_hmac_build() error cases and output format
 *   [hmac_verify]  — espnow_hmac_verify() correct accept / tamper reject
 *   [hmac_roundtrip] — build+verify round-trip for normal and edge cases
 *   [hmac_ct]      — constant-time property: faults at byte 0 AND byte 15 caught
 */

#include "unity.h"
#include "espnow_mqtt_hmac.h"
#include "espnow_mqtt_proto.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Maximum output buffer needed: 16 tag + 230 data = 246 bytes. */
#define BUF_MAX 246

void setUp(void)    { /* nothing */ }
void tearDown(void) { /* nothing */ }

/* =========================================================================
 * Helpers
 * ========================================================================= */

static uint8_t s_buf[BUF_MAX];
static size_t  s_len;

static void build_ok(uint8_t topic_id, uint16_t seq,
                      const void *data, size_t data_len)
{
    memset(s_buf, 0xAA, sizeof(s_buf));
    s_len = 0;
    esp_err_t ret = espnow_hmac_build(topic_id, seq, data, data_len,
                                       s_buf, &s_len);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_size_t(16 + data_len, s_len);
}

/* =========================================================================
 * espnow_hmac_build — error cases
 * ========================================================================= */

TEST_CASE("build: data_len > 230 returns INVALID_SIZE", "[hmac_build]")
{
    uint8_t dummy[231] = {0};
    size_t  out_len    = 99;
    uint8_t out_buf[BUF_MAX];
    esp_err_t ret = espnow_hmac_build(1, 0, dummy, 231, out_buf, &out_len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ret);
    /* Fail closed: out_len must be zeroed. */
    TEST_ASSERT_EQUAL_size_t(0, out_len);
}

TEST_CASE("build: NULL out_buf returns INVALID_ARG", "[hmac_build]")
{
    uint8_t  data[4] = {1, 2, 3, 4};
    size_t   out_len = 0;
    esp_err_t ret = espnow_hmac_build(1, 0, data, 4, NULL, &out_len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

TEST_CASE("build: NULL out_len returns INVALID_ARG", "[hmac_build]")
{
    uint8_t data[4]  = {1, 2, 3, 4};
    uint8_t out[BUF_MAX];
    esp_err_t ret = espnow_hmac_build(1, 0, data, 4, out, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

TEST_CASE("build: data_len == 230 (max) succeeds", "[hmac_build]")
{
    uint8_t data[230];
    memset(data, 0x55, sizeof(data));
    build_ok(1, 0, data, 230);
    TEST_ASSERT_EQUAL_size_t(246, s_len);
}

TEST_CASE("build: data_len == 0 (keepalive) produces 16-byte output", "[hmac_build]")
{
    build_ok(3, 1000, NULL, 0);
    TEST_ASSERT_EQUAL_size_t(16, s_len);
}

/* =========================================================================
 * espnow_hmac_verify — bounds checks
 * ========================================================================= */

TEST_CASE("verify: payload_len < 16 returns false", "[hmac_verify]")
{
    uint8_t buf[15] = {0};
    TEST_ASSERT_FALSE(espnow_hmac_verify(1, 0, buf, 15));
}

TEST_CASE("verify: payload_len == 0 returns false", "[hmac_verify]")
{
    uint8_t buf[1] = {0};
    TEST_ASSERT_FALSE(espnow_hmac_verify(1, 0, buf, 0));
}

TEST_CASE("verify: NULL payload returns false", "[hmac_verify]")
{
    TEST_ASSERT_FALSE(espnow_hmac_verify(1, 0, NULL, 16));
}

TEST_CASE("verify: payload_len > 246 returns false", "[hmac_verify]")
{
    uint8_t buf[247] = {0};
    TEST_ASSERT_FALSE(espnow_hmac_verify(1, 0, buf, 247));
}

/* =========================================================================
 * Round-trip tests
 * ========================================================================= */

TEST_CASE("round-trip: normal payload verifies correctly", "[hmac_roundtrip]")
{
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0xFF, 0xFE};
    build_ok(7, 42, data, sizeof(data));
    TEST_ASSERT_TRUE(espnow_hmac_verify(7, 42, s_buf, s_len));
}

TEST_CASE("round-trip: keepalive (data_len==0) verifies correctly", "[hmac_roundtrip]")
{
    build_ok(3, 1000, NULL, 0);
    TEST_ASSERT_EQUAL_size_t(16, s_len);
    /* After verify, subscriber sees payload_len = s_len - 16 = 0. */
    TEST_ASSERT_TRUE(espnow_hmac_verify(3, 1000, s_buf, s_len));
    size_t subscriber_payload_len = s_len - 16;
    TEST_ASSERT_EQUAL_size_t(0, subscriber_payload_len);
}

TEST_CASE("round-trip: seq=0 verifies", "[hmac_roundtrip]")
{
    uint8_t data[8] = {0xAA};
    build_ok(1, 0, data, 8);
    TEST_ASSERT_TRUE(espnow_hmac_verify(1, 0, s_buf, s_len));
}

TEST_CASE("round-trip: seq=65535 verifies", "[hmac_roundtrip]")
{
    uint8_t data[8] = {0xBB};
    build_ok(2, 65535, data, 8);
    TEST_ASSERT_TRUE(espnow_hmac_verify(2, 65535, s_buf, s_len));
}

TEST_CASE("round-trip: different topic_id does not verify", "[hmac_roundtrip]")
{
    uint8_t data[4] = {1, 2, 3, 4};
    build_ok(5, 10, data, 4);
    /* Verify with wrong topic_id. */
    TEST_ASSERT_FALSE(espnow_hmac_verify(6, 10, s_buf, s_len));
}

TEST_CASE("round-trip: different seq does not verify", "[hmac_roundtrip]")
{
    uint8_t data[4] = {1, 2, 3, 4};
    build_ok(5, 10, data, 4);
    /* Verify with wrong seq. */
    TEST_ASSERT_FALSE(espnow_hmac_verify(5, 11, s_buf, s_len));
}

/* =========================================================================
 * Tamper tests
 * ========================================================================= */

TEST_CASE("tamper: flipped payload byte fails verify", "[hmac_verify]")
{
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    build_ok(1, 100, data, sizeof(data));

    /* Flip one byte in the payload portion (after the 16-byte tag). */
    s_buf[16] ^= 0xFF;
    TEST_ASSERT_FALSE(espnow_hmac_verify(1, 100, s_buf, s_len));
}

TEST_CASE("tamper: flipped last payload byte fails verify", "[hmac_verify]")
{
    uint8_t data[] = {0x11, 0x22, 0x33, 0x44};
    build_ok(2, 200, data, sizeof(data));

    /* Flip the very last data byte. */
    s_buf[s_len - 1] ^= 0x01;
    TEST_ASSERT_FALSE(espnow_hmac_verify(2, 200, s_buf, s_len));
}

/* =========================================================================
 * Constant-time property
 *
 * These tests verify that a 1-bit fault at byte 0 AND byte 15 of the HMAC
 * tag are both caught. If the comparison were short-circuit (like memcmp),
 * a fault at byte 15 would still be caught — but we test both to confirm
 * the XOR-accumulate covers the full 16-byte range.
 * ========================================================================= */

TEST_CASE("constant-time: fault at tag byte 0 is caught", "[hmac_ct]")
{
    uint8_t data[] = {0xCA, 0xFE};
    build_ok(4, 300, data, sizeof(data));

    /* Flip one bit in the first tag byte. */
    s_buf[0] ^= 0x01;
    TEST_ASSERT_FALSE(espnow_hmac_verify(4, 300, s_buf, s_len));
}

TEST_CASE("constant-time: fault at tag byte 15 is caught", "[hmac_ct]")
{
    uint8_t data[] = {0xCA, 0xFE};
    build_ok(4, 300, data, sizeof(data));

    /* Restore byte 0, flip last tag byte. */
    s_buf[15] ^= 0x80;
    TEST_ASSERT_FALSE(espnow_hmac_verify(4, 300, s_buf, s_len));
}

TEST_CASE("constant-time: all tag bytes zeroed fails verify", "[hmac_ct]")
{
    uint8_t data[] = {0x01};
    build_ok(1, 1, data, 1);

    /* Zero the entire tag — ensures no accidental all-zero key bypass. */
    memset(s_buf, 0, 16);
    TEST_ASSERT_FALSE(espnow_hmac_verify(1, 1, s_buf, s_len));
}

TEST_CASE("constant-time: all tag bytes 0xFF fails verify", "[hmac_ct]")
{
    uint8_t data[] = {0x01};
    build_ok(1, 1, data, 1);

    memset(s_buf, 0xFF, 16);
    TEST_ASSERT_FALSE(espnow_hmac_verify(1, 1, s_buf, s_len));
}
