/*
 * test_topic_validation.c — Tier 1: Phase 2 topic_valid() tests.
 *
 * Tests every validation rule and boundary. No radio, no recv_cb.
 * Runs fast — pure string operations.
 */

#include "unity.h"
#include "espnow_mqtt_internal.h"  /* espnow_mqtt_topic_valid() */
#include "espnow_mqtt_proto.h"     /* ESPNOW_MAX_TOPIC_LEN */
#include <string.h>
#include <stdint.h>

void setUp(void)    { /* nothing */ }
void tearDown(void) { /* nothing */ }

/* =========================================================================
 * Helper: build a string of exactly N chars (all 'a')
 * ========================================================================= */
static char s_buf[300];

static const char *make_topic(size_t len)
{
    if (len >= sizeof(s_buf)) len = sizeof(s_buf) - 1;
    memset(s_buf, 'a', len);
    s_buf[len] = '\0';
    return s_buf;
}

/* =========================================================================
 * NULL and empty
 * ========================================================================= */

TEST_CASE("NULL topic is invalid", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid(NULL, false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid(NULL, true));
}

TEST_CASE("Empty topic is invalid", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("", false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("", true));
}

/* =========================================================================
 * Length boundaries
 * ========================================================================= */

TEST_CASE("Topic of length 1 is valid", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("a", false));
}

TEST_CASE("Topic of length 247 (MAX) is valid", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid(make_topic(ESPNOW_MAX_TOPIC_LEN), false));
    TEST_ASSERT_EQUAL_INT(247, ESPNOW_MAX_TOPIC_LEN);
}

TEST_CASE("Topic of length 248 (MAX+1) is invalid", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid(make_topic(ESPNOW_MAX_TOPIC_LEN + 1), false));
}

/* =========================================================================
 * Reserved prefix
 * ========================================================================= */

TEST_CASE("espnow/ prefix is reserved and invalid", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("espnow/data",   false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("espnow/",       false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("espnow/status", true));
}

TEST_CASE("Topics starting with espnow but not espnow/ are valid", "[topic_valid]")
{
    /* 'espnow' without the slash is not reserved. */
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("espnowdata", false));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("espnow2",    false));
}

/* =========================================================================
 * ASCII printable range
 * ========================================================================= */

TEST_CASE("Non-ASCII byte (0x80) is invalid", "[topic_valid]")
{
    char bad[4] = {'a', '/', (char)0x80, '\0'};
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid(bad, false));
}

TEST_CASE("Control character (0x01) is invalid", "[topic_valid]")
{
    char bad[4] = {'a', (char)0x01, 'b', '\0'};
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid(bad, false));
}

TEST_CASE("Space (0x20) is valid ASCII printable", "[topic_valid]")
{
    /* Space is technically in the printable range 0x20-0x7E. */
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("a b", false));
}

TEST_CASE("DEL (0x7F) is invalid", "[topic_valid]")
{
    char bad[3] = {'a', (char)0x7F, '\0'};
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid(bad, false));
}

/* =========================================================================
 * Slash rules
 * ========================================================================= */

TEST_CASE("Leading slash is invalid", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("/a/b",  false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("/",     false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("/a/b",  true));
}

TEST_CASE("Trailing slash is invalid", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/b/",  false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/",    false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/b/",  true));
}

TEST_CASE("Double slash is invalid", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a//b",  false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a//b",  true));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("//b",   false));
}

TEST_CASE("Single-segment topic with no slashes is valid", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("sensors", false));
}

TEST_CASE("Multi-segment topic is valid", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("sensors/node1/temp", false));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("a/b/c/d/e",          false));
}

/* =========================================================================
 * Wildcard '#'
 * ========================================================================= */

TEST_CASE("Hash is invalid in publisher topics", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("#",      false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/#",    false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/b/#",  false));
}

TEST_CASE("Hash is always invalid in patterns (out of scope)", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("#",      true));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/#",    true));
}

/* =========================================================================
 * Wildcard '+' — publisher topics
 * ========================================================================= */

TEST_CASE("Plus is invalid in publisher topics", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("+",       false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/+/b",   false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("+/b",     false));
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/+",     false));
}

/* =========================================================================
 * Wildcard '+' — subscription patterns
 * ========================================================================= */

TEST_CASE("Plus as sole topic is valid pattern", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("+", true));
}

TEST_CASE("Plus as full mid-segment is valid pattern", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("a/+/b",     true));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("a/+/b/c",   true));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("sensors/+/temp", true));
}

TEST_CASE("Plus as leading segment is valid pattern", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("+/b",   true));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("+/b/c", true));
}

TEST_CASE("Plus as trailing segment is valid pattern", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("a/+",   true));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("a/b/+", true));
}

TEST_CASE("Plus combined with other chars in segment is invalid pattern", "[topic_valid]")
{
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a+b",   true));  /* not a full segment */
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/+b",  true));  /* '+' not alone */
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/b+",  true));  /* '+' not alone */
    TEST_ASSERT_FALSE(espnow_mqtt_topic_valid("a/b+/c",true));  /* '+' not alone */
}

TEST_CASE("Multiple plus segments are valid patterns", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("+/+",     true));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("a/+/+/b", true));
}

/* =========================================================================
 * Realistic valid topic strings
 * ========================================================================= */

TEST_CASE("Realistic sensor topics are valid", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("sensors/node1/temp",         false));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("sensors/node1/humidity",     false));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("home/floor1/room3/motion",   false));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("devices/abc123/status",      false));
}

TEST_CASE("Realistic subscription patterns are valid", "[topic_valid]")
{
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("sensors/+/temp",     true));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("sensors/+/humidity", true));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("+/floor1/+",         true));
    TEST_ASSERT_TRUE(espnow_mqtt_topic_valid("home/+/+/motion",    true));
}
