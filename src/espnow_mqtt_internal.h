/*
 * espnow_mqtt_internal.h — Cross-module internal declarations.
 *
 * PRIVATE header. Included by espnow_mqtt.c, espnow_mqtt_publisher.c, and
 * espnow_mqtt_broker.c only. Never included by application code.
 *
 * Contains:
 *   - espnow_mqtt_topic_valid() — defined in espnow_mqtt.c, called from all
 *     three .c files. Cannot be static because all three modules need it.
 *   - publisher_trigger_rediscover_async() — defined in espnow_mqtt_publisher.c,
 *     called from espnow_mqtt.c send_cb. Guarded so broker-only builds that
 *     exclude espnow_mqtt_publisher.c do not get an unresolved symbol.
 *   - s_broker_stats extern — defined in espnow_mqtt_broker.c, accessed from
 *     espnow_mqtt.c recv_cb for frame counting. Guarded so publisher-only builds
 *     that exclude espnow_mqtt_broker.c do not get an unresolved symbol.
 *
 * Do NOT put wire-protocol constants here — those belong in espnow_mqtt_proto.h.
 */

#pragma once

#include <stdbool.h>
#include "espnow_mqtt.h"  /* for espnow_mqtt_stats_t type */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Shared validation — defined once in espnow_mqtt.c
 * ========================================================================= */

/**
 * Validate a topic string against the component rules.
 *
 * Rules (publisher topics, is_pattern == false):
 *   - Not NULL, not empty
 *   - Length <= ESPNOW_MAX_TOPIC_LEN (247 chars, excluding null)
 *   - ASCII printable characters only (0x20-0x7E)
 *   - No '+' or '#' characters
 *   - No leading or trailing '/'
 *   - No '//' (double slash)
 *   - No "espnow/" reserved prefix
 *
 * Additional rules for subscription patterns (is_pattern == true):
 *   - '+' is allowed only as a complete path segment (between '/' delimiters,
 *     or at start/end of string bounded by '/')
 *   - '#' is always rejected (out of scope for this version)
 *
 * @param topic_str  Null-terminated topic string to validate. May be NULL.
 * @param is_pattern true for subscription patterns ('+' allowed as segment);
 *                   false for publisher topics ('+' and '#' always rejected).
 * @return true if the topic string is valid; false otherwise.
 */
bool espnow_mqtt_topic_valid(const char *topic_str, bool is_pattern);

/* =========================================================================
 * Publisher internals — defined in espnow_mqtt_publisher.c
 * Guarded: not declared when building BROKER-only (publisher.c excluded).
 * ========================================================================= */

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_BROKER)

/**
 * Post PUBLISHER_EVENT_REDISCOVER to the publisher task queue asynchronously.
 *
 * Non-blocking: if the queue is full, the event is dropped (a rediscover is
 * already pending). Called from espnow_mqtt.c send_cb when consecutive
 * SEND_FAILs exceed INTERNAL_ERR_THRESHOLD, and from espnow_mqtt_rediscover().
 *
 * DO NOT call from publisher_handle_broker_announce() — that path uses
 * PUBLISHER_EVENT_BROKER_ANNOUNCE to preserve the channel hint.
 */
void publisher_trigger_rediscover_async(void);

#endif /* !CONFIG_ESPNOW_MQTT_ROLE_BROKER */

/* =========================================================================
 * Broker internals — defined in espnow_mqtt_broker.c
 * Guarded: not declared when building PUBLISHER-only (broker.c excluded).
 *
 * Note: the design doc uses '#if CONFIG_ESPNOW_MQTT_ROLE != 1' for this guard.
 * Both forms are equivalent after Kconfig generates symbols. The boolean form
 * is used here for consistency with the rest of this file.
 * ========================================================================= */

#if !defined(CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER)

/**
 * Broker runtime statistics. Defined in espnow_mqtt_broker.c.
 * Accessed from espnow_mqtt.c recv_cb for frame_received and trust filter
 * counters, which must be incremented before the type dispatch switch.
 */
extern espnow_mqtt_stats_t s_broker_stats;

#endif /* !CONFIG_ESPNOW_MQTT_ROLE_PUBLISHER */

#ifdef __cplusplus
}
#endif
