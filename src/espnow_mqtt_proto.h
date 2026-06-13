/*
 * espnow_mqtt_proto.h — Wire protocol constants and frame definitions.
 *
 * PRIVATE header. Included by espnow_mqtt.c, espnow_mqtt_publisher.c,
 * espnow_mqtt_broker.c, and test code only. Never included by application code.
 *
 * This is the single authoritative source of truth for:
 *   - All frame struct definitions
 *   - ESPNOW_MAX_TOPIC_LEN and related size constants
 *   - Message type constants (typedef + #define, NOT plain enum)
 *   - Minimum frame length constants
 *   - Seq encode/decode inline helpers
 *
 * Never duplicate these constants elsewhere. Never use raw integers where a
 * named constant exists.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Protocol version
 * ========================================================================= */

#define ESPNOW_PROTO_VERSION    ((uint8_t)0x01)

/* =========================================================================
 * Frame size limits
 *
 * ESP-NOW maximum payload: 250 bytes.
 *
 * REGISTER:  2-byte header (type + version) + topic field
 *            topic field = ESPNOW_MAX_TOPIC_LEN chars + 1 null byte = 248 bytes
 *            Total: 2 + 248 = 250
 *
 * PUBLISH:   4-byte header (type + topic_id + seq_lo + seq_hi) + payload
 *            payload <= ESPNOW_MAX_PAYLOAD_LEN = 246 bytes
 *            Total: 4 + 246 = 250
 *
 * ESPNOW_MAX_TOPIC_LEN: maximum topic string length in characters, excluding
 *   the null terminator. Derivation: 250 - 2 (header) - 1 (null) = 247.
 *   This is the SINGLE authoritative definition. Every validation path, API
 *   docstring, and Kconfig help text must reference this constant.
 * ========================================================================= */

/** Maximum topic string length (characters, excluding null terminator). */
#define ESPNOW_MAX_TOPIC_LEN        247

/** Maximum PUBLISH payload length in bytes. */
#define ESPNOW_MAX_PAYLOAD_LEN      246

/**
 * Maximum PUBLISH payload length when HMAC is enabled.
 * 16 bytes of HMAC tag are prepended, consuming that space from the budget.
 */
#define ESPNOW_MAX_PAYLOAD_HMAC_LEN 230

/* =========================================================================
 * Message types
 *
 * DO NOT use a plain enum. On ESP-IDF's default GCC configuration, plain
 * enums are int-sized (4 bytes) because -fshort-enums is not set. Using a
 * plain enum causes _Static_assert(sizeof == 1) to fire immediately.
 * DO NOT add -fshort-enums to work around this — it silently breaks the ABI
 * of every other enum in the project (esp_err_t, wifi_mode_t, etc.).
 *
 * The typedef + #define pattern below gives a named type with a guaranteed
 * 1-byte size without touching the compiler ABI.
 * ========================================================================= */

typedef uint8_t espnow_msg_type_t;

#define ESPNOW_MSG_REGISTER         ((espnow_msg_type_t)0x01)
#define ESPNOW_MSG_REGISTER_ACK     ((espnow_msg_type_t)0x02)
#define ESPNOW_MSG_PUBLISH          ((espnow_msg_type_t)0x03)
#define ESPNOW_MSG_ID_UNKNOWN       ((espnow_msg_type_t)0x04)
#define ESPNOW_MSG_BROKER_ANNOUNCE  ((espnow_msg_type_t)0x05)

/* Verify the typedef is actually 1 byte. */
_Static_assert(sizeof(espnow_msg_type_t) == 1,
               "espnow_msg_type_t must be 1 byte");

/* =========================================================================
 * Frame structs — all packed, no padding.
 *
 * Frame lengths on the wire are always computed as ACTUAL byte counts, never
 * as sizeof(struct). For example:
 *   REGISTER send length = 2 + strlen(topic_str) + 1  (not sizeof register frame)
 *   PUBLISH  send length = 4 + payload_len             (not sizeof publish frame)
 * ========================================================================= */

/**
 * REGISTER — Publisher to Broker
 *
 * Sent on boot before any PUBLISH. Contains full topic string.
 * Also serves as the channel discovery probe (sent on each candidate channel).
 *
 * Wire layout (250 bytes maximum):
 *   [0]     type    = ESPNOW_MSG_REGISTER
 *   [1]     version = ESPNOW_PROTO_VERSION
 *   [2..N]  topic   = null-terminated topic string (max 248 bytes incl. null)
 *
 * Send length on wire: 2 + strlen(topic_str) + 1   (NOT sizeof this struct)
 */
typedef struct {
    uint8_t type;       /* ESPNOW_MSG_REGISTER */
    uint8_t version;    /* ESPNOW_PROTO_VERSION */
    char    topic[248]; /* topic string + null terminator (full field capacity) */
} __attribute__((packed)) espnow_register_frame_t;

_Static_assert(sizeof(espnow_register_frame_t) == 250,
               "espnow_register_frame_t must be 250 bytes");

/**
 * REGISTER_ACK — Broker to Publisher
 *
 * Assigns topic_id for this session. topic_id == 0 means rejected.
 *
 * Wire layout (always exactly 2 bytes):
 *   [0]  type     = ESPNOW_MSG_REGISTER_ACK
 *   [1]  topic_id = assigned ID (1-254) or 0 for rejection
 *
 * Send length: sizeof(espnow_register_ack_frame_t) == 2 (always exact)
 */
typedef struct {
    uint8_t type;       /* ESPNOW_MSG_REGISTER_ACK */
    uint8_t topic_id;   /* 0 = rejected; 1-254 = assigned */
} __attribute__((packed)) espnow_register_ack_frame_t;

_Static_assert(sizeof(espnow_register_ack_frame_t) == 2,
               "espnow_register_ack_frame_t must be 2 bytes");

/**
 * PUBLISH header — Publisher to Broker
 *
 * The payload follows immediately after this header in the wire buffer.
 * payload_len == 0 is a valid keepalive (zero bytes after the header).
 *
 * Wire layout:
 *   [0]      type     = ESPNOW_MSG_PUBLISH
 *   [1]      topic_id = negotiated topic ID
 *   [2]      seq_lo   = seq & 0xFF
 *   [3]      seq_hi   = (seq >> 8) & 0xFF
 *   [4..N]   payload  = application data (0-246 bytes)
 *
 * Send length on wire: 4 + payload_len   (NOT sizeof this struct)
 */
typedef struct {
    uint8_t type;       /* ESPNOW_MSG_PUBLISH */
    uint8_t topic_id;   /* negotiated topic ID */
    uint8_t seq_lo;     /* sequence counter low byte */
    uint8_t seq_hi;     /* sequence counter high byte */
} __attribute__((packed)) espnow_publish_frame_t;

_Static_assert(sizeof(espnow_publish_frame_t) == 4,
               "espnow_publish_frame_t header must be 4 bytes");

/**
 * ID_UNKNOWN — Broker to Publisher
 *
 * Sent when a PUBLISH arrives with an unrecognised (topic_id, sender_mac) pair.
 * Triggers publisher re-registration for the affected slot.
 *
 * Wire layout (always exactly 2 bytes):
 *   [0]  type     = ESPNOW_MSG_ID_UNKNOWN
 *   [1]  topic_id = the unrecognised topic_id from the received PUBLISH frame
 */
typedef struct {
    uint8_t type;       /* ESPNOW_MSG_ID_UNKNOWN */
    uint8_t topic_id;   /* the unrecognised topic_id */
} __attribute__((packed)) espnow_id_unknown_frame_t;

_Static_assert(sizeof(espnow_id_unknown_frame_t) == 2,
               "espnow_id_unknown_frame_t must be 2 bytes");

/**
 * BROKER_ANNOUNCE — Broker to broadcast (FF:FF:FF:FF:FF:FF)
 *
 * Sent on broker boot and periodically at ANNOUNCE_INTERVAL_MS.
 * Publishers re-register on receipt; used as a channel hint.
 *
 * Wire layout (always exactly 2 bytes):
 *   [0]  type    = ESPNOW_MSG_BROKER_ANNOUNCE
 *   [1]  version = ESPNOW_PROTO_VERSION
 */
typedef struct {
    uint8_t type;       /* ESPNOW_MSG_BROKER_ANNOUNCE */
    uint8_t version;    /* ESPNOW_PROTO_VERSION */
} __attribute__((packed)) espnow_broker_announce_frame_t;

_Static_assert(sizeof(espnow_broker_announce_frame_t) == 2,
               "espnow_broker_announce_frame_t must be 2 bytes");

/* =========================================================================
 * Minimum frame lengths for recv_cb validation
 *
 * recv_cb checks data_len >= ESPNOW_MIN_LEN_* before accessing any field.
 * This prevents out-of-bounds reads on truncated or malformed frames.
 * ========================================================================= */

/** Minimum valid REGISTER frame: type + version (topic validated separately). */
#define ESPNOW_MIN_LEN_REGISTER         2

/** Minimum valid REGISTER_ACK frame: type + topic_id. */
#define ESPNOW_MIN_LEN_REGISTER_ACK     2

/** Minimum valid PUBLISH frame: type + topic_id + seq_lo + seq_hi. */
#define ESPNOW_MIN_LEN_PUBLISH          4

/** Minimum valid ID_UNKNOWN frame: type + topic_id. */
#define ESPNOW_MIN_LEN_ID_UNKNOWN       2

/** Minimum valid BROKER_ANNOUNCE frame: type + version. */
#define ESPNOW_MIN_LEN_BROKER_ANNOUNCE  2

/* =========================================================================
 * Sequence counter encode / decode
 *
 * The sequence counter is a uint16_t stored little-endian in the PUBLISH frame
 * (seq_lo = low byte, seq_hi = high byte).
 *
 * Inline so they compile to zero overhead. Used in both publisher (encoding)
 * and broker (decoding) paths without a function call.
 * ========================================================================= */

/**
 * Encode a uint16_t sequence counter into two wire bytes.
 *
 * @param seq  The sequence counter value to encode.
 * @param lo   Output: low byte  (seq & 0xFF).
 * @param hi   Output: high byte ((seq >> 8) & 0xFF).
 */
static inline void espnow_proto_encode_seq(uint16_t seq,
                                            uint8_t *lo, uint8_t *hi)
{
    *lo = (uint8_t)(seq & 0xFF);
    *hi = (uint8_t)((seq >> 8) & 0xFF);
}

/**
 * Decode two wire bytes from a PUBLISH frame into a uint16_t sequence counter.
 *
 * @param lo  Low byte  (data[2] from the PUBLISH frame).
 * @param hi  High byte (data[3] from the PUBLISH frame).
 * @return    The reconstructed sequence counter.
 */
static inline uint16_t espnow_proto_decode_seq(uint8_t lo, uint8_t hi)
{
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

#ifdef __cplusplus
}
#endif
