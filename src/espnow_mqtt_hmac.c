/*
 * espnow_mqtt_hmac.c — Optional HMAC-SHA256 payload integrity.
 *
 * Compiled only when CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC=y (CMakeLists.txt).
 *
 * Design constraints:
 *   - Software path exclusively: mbedtls/md.h. Never esp_hmac.h (hardware
 *     HMAC peripheral is absent on plain ESP32 which hosts the broker).
 *   - Fail closed: any mbedTLS error aborts the operation; no partial output.
 *   - Constant-time compare: XOR-accumulate over all 16 tag bytes; never
 *     short-circuit memcmp. Prevents timing side-channels on the broker.
 *   - HMAC input binding: tag covers topic_id + seq_lo + seq_hi + payload,
 *     so replaying a frame with a different topic_id or seq is detectable.
 */

#include "espnow_mqtt_hmac.h"
#include "espnow_mqtt_proto.h"   /* ESPNOW_MAX_PAYLOAD_HMAC_LEN */
#include "sdkconfig.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "espnow_mqtt_hmac";

/* HMAC tag length — always 16 bytes (truncated SHA256). */
#define HMAC_TAG_LEN 16

/* Full SHA256 output before truncation. */
#define SHA256_LEN   32

/*
 * Secret key — compiled from Kconfig string.
 * For production: read from eFuse at runtime and pass as a parameter.
 * See 08_optional_payload_integrity.md §Production Key Storage.
 */
static const char *s_secret     = CONFIG_ESPNOW_MQTT_HMAC_SECRET;
static size_t      s_secret_len; /* set once in hmac_compute() */

/* =========================================================================
 * Internal: compute HMAC-SHA256 over the AAD + data
 *
 * AAD (additional authenticated data bound into the tag):
 *   [0]  topic_id
 *   [1]  seq_lo
 *   [2]  seq_hi
 *   [3+] data bytes (0-230 bytes)
 *
 * Total HMAC input: 3 + data_len bytes.
 * Output: full 32-byte SHA256 in out32[]. Caller truncates to HMAC_TAG_LEN.
 * ========================================================================= */

static esp_err_t hmac_compute(uint8_t topic_id, uint16_t seq,
                               const uint8_t *data, size_t data_len,
                               uint8_t out32[SHA256_LEN])
{
    /* Lazy-init secret length (strlen is safe; s_secret is a compile-time
     * string literal that cannot be NULL). */
    if (s_secret_len == 0) {
        s_secret_len = strlen(s_secret);
    }

    const mbedtls_md_info_t *md_info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) {
        ESP_LOGE(TAG, "mbedtls_md_info_from_type failed — SHA256 unavailable");
        return ESP_FAIL;
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    int rc = mbedtls_md_setup(&ctx, md_info, /*hmac=*/1);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_md_setup failed: -0x%04x", (unsigned)(-rc));
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }

    rc = mbedtls_md_hmac_starts(&ctx,
                                  (const unsigned char *)s_secret,
                                  s_secret_len);
    if (rc != 0) { goto fail; }

    /* Feed AAD: topic_id + seq_lo + seq_hi */
    uint8_t aad[3];
    aad[0] = topic_id;
    espnow_proto_encode_seq(seq, &aad[1], &aad[2]);

    rc = mbedtls_md_hmac_update(&ctx, aad, sizeof(aad));
    if (rc != 0) { goto fail; }

    /* Feed payload data (may be zero length for keepalives). */
    if (data && data_len > 0) {
        rc = mbedtls_md_hmac_update(&ctx, data, data_len);
        if (rc != 0) { goto fail; }
    }

    rc = mbedtls_md_hmac_finish(&ctx, out32);
    if (rc != 0) { goto fail; }

    mbedtls_md_free(&ctx);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "HMAC computation failed: -0x%04x", (unsigned)(-rc));
    mbedtls_md_free(&ctx);
    return ESP_FAIL;
}

/* =========================================================================
 * espnow_hmac_build
 * ========================================================================= */

esp_err_t espnow_hmac_build(uint8_t topic_id, uint16_t seq,
                             const void *data, size_t data_len,
                             uint8_t *out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > ESPNOW_MAX_PAYLOAD_HMAC_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t full_hmac[SHA256_LEN];
    esp_err_t ret = hmac_compute(topic_id, seq,
                                  (const uint8_t *)data, data_len,
                                  full_hmac);
    if (ret != ESP_OK) {
        /* Fail closed: zero out_len so caller cannot accidentally send. */
        *out_len = 0;
        return ret;
    }

    /* Prepend truncated 16-byte tag, then copy payload. */
    memcpy(out_buf, full_hmac, HMAC_TAG_LEN);
    if (data && data_len > 0) {
        memcpy(out_buf + HMAC_TAG_LEN, data, data_len);
    }
    *out_len = HMAC_TAG_LEN + data_len;

    return ESP_OK;
}

/* =========================================================================
 * espnow_hmac_verify
 * ========================================================================= */

bool espnow_hmac_verify(uint8_t topic_id, uint16_t seq,
                         const uint8_t *payload, size_t payload_len)
{
    /* Bounds: must have at least the 16-byte tag; max is 16 tag + 230 data. */
    if (!payload || payload_len < HMAC_TAG_LEN) {
        return false;
    }
    if (payload_len > (HMAC_TAG_LEN + ESPNOW_MAX_PAYLOAD_HMAC_LEN)) {
        return false;
    }

    const uint8_t *received_tag = payload;
    const uint8_t *data         = payload + HMAC_TAG_LEN;
    size_t         data_len     = payload_len - HMAC_TAG_LEN;

    uint8_t expected_full[SHA256_LEN];
    esp_err_t ret = hmac_compute(topic_id, seq,
                                  data_len > 0 ? data : NULL, data_len,
                                  expected_full);
    if (ret != ESP_OK) {
        /* Fail closed: mbedTLS error → treat as verification failure. */
        return false;
    }

    /*
     * Constant-time comparison: XOR-accumulate all 16 tag bytes.
     * Never short-circuit. Never memcmp().
     * This prevents timing attacks where an attacker can determine how many
     * leading bytes of a forged tag are correct.
     */
    uint8_t diff = 0;
    for (int i = 0; i < HMAC_TAG_LEN; i++) {
        diff |= received_tag[i] ^ expected_full[i];
    }
    return (diff == 0);
}
