/*
 * espnow_mqtt_hmac.h — Optional HMAC-SHA256 payload integrity.
 *
 * PRIVATE header. Included only by espnow_mqtt_publisher.c and
 * espnow_mqtt_broker.c when CONFIG_ESPNOW_MQTT_PAYLOAD_HMAC=y.
 *
 * Wire format when enabled:
 *   [0..15]  HMAC tag   — truncated SHA256, 16 bytes
 *   [16..]   payload    — application data (0–230 bytes)
 *
 * HMAC input (to mbedTLS): topic_id || seq_lo || seq_hi || payload_data
 * Key: CONFIG_ESPNOW_MQTT_HMAC_SECRET (null-terminated string, compiled in)
 *
 * This is an integrity mechanism for physically-controlled deployments,
 * NOT a security mechanism against active attackers. Frames are still
 * transmitted in plaintext. See 08_optional_payload_integrity.md.
 *
 * Software path only: mbedtls/md.h. Never esp_hmac.h (absent on plain ESP32).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build an HMAC-protected payload buffer.
 *
 * Computes HMAC-SHA256 over (topic_id || seq_lo || seq_hi || data), truncates
 * to 16 bytes, prepends the tag to data, and writes the result to out_buf.
 *
 * Fails closed: on any mbedTLS error, returns the error code and does NOT
 * write a partial frame. The caller must not transmit on non-ESP_OK return.
 *
 * @param topic_id   Negotiated topic ID for this slot.
 * @param seq        Current sequence counter value.
 * @param data       Application payload (may be NULL when data_len == 0).
 * @param data_len   Payload length. Must be <= ESPNOW_MAX_PAYLOAD_HMAC_LEN (230).
 * @param out_buf    Output buffer. Must be at least (16 + data_len) bytes.
 * @param out_len    Set to (16 + data_len) on success.
 * @return ESP_ERR_INVALID_SIZE  data_len > 230.
 * @return ESP_ERR_INVALID_ARG   out_buf or out_len is NULL.
 * @return ESP_FAIL              mbedTLS HMAC computation failed (fail closed).
 * @return ESP_OK
 */
esp_err_t espnow_hmac_build(uint8_t topic_id, uint16_t seq,
                             const void *data, size_t data_len,
                             uint8_t *out_buf, size_t *out_len);

/**
 * Verify an HMAC-protected payload buffer.
 *
 * Recomputes the expected HMAC tag over the payload content and compares
 * it to the received tag using a constant-time XOR-accumulate (never memcmp).
 *
 * @param topic_id    Negotiated topic ID (from the PUBLISH frame header).
 * @param seq         Sequence counter (from the PUBLISH frame header).
 * @param payload     Full wire payload: [0..15] = tag, [16..] = data.
 * @param payload_len Total payload length including the 16-byte tag.
 *                    Must be >= 16 and <= 246.
 * @return true   Tag matches — frame is intact.
 * @return false  Tag mismatch, or payload_len out of range, or mbedTLS error.
 */
bool espnow_hmac_verify(uint8_t topic_id, uint16_t seq,
                         const uint8_t *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif
