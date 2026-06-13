/*
 * test_seq_tracking.c — Tier 1: Phase 4 sequence tracking tests.
 *
 * Tests the broker's wrap-aware per-MAC seq tracking logic.
 * No radio, no tasks, no queues. Pure arithmetic.
 */

#include "unity.h"
#include "espnow_mqtt_proto.h"
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * Mirror the broker seq tracking logic in isolation.
 * Avoids linking the full broker for a pure-logic test.
 * ========================================================================= */

typedef struct {
    uint8_t  mac[6];
    uint16_t last_seq;
    bool     first_frame;
    bool     valid;
} test_seq_entry_t;

#define MAX_PEERS 4
static test_seq_entry_t s_entries[MAX_PEERS];
static uint32_t s_seq_gaps      = 0;
static uint32_t s_seq_reordered = 0;

static void seq_test_reset(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_seq_gaps      = 0;
    s_seq_reordered = 0;
}

static test_seq_entry_t *find_or_create(const uint8_t *mac)
{
    test_seq_entry_t *free_slot = NULL;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_entries[i].valid && memcmp(s_entries[i].mac, mac, 6) == 0)
            return &s_entries[i];
        if (!s_entries[i].valid && !free_slot) free_slot = &s_entries[i];
    }
    if (free_slot) {
        memcpy(free_slot->mac, mac, 6);
        free_slot->first_frame = true;
        free_slot->valid       = true;
    }
    return free_slot;
}

/* Process one frame from mac with given seq.
 * Returns: delta (for testing); updates gap/reorder counters. */
static int16_t process_seq(const uint8_t *mac, uint16_t seq)
{
    test_seq_entry_t *e = find_or_create(mac);
    if (!e) return 0;

    if (e->first_frame) {
        e->first_frame = false;
        e->last_seq    = seq;
        return 1; /* no anomaly on first frame */
    }

    int16_t delta = (int16_t)(seq - e->last_seq);
    if (delta > 1) {
        s_seq_gaps += (uint32_t)(delta - 1);
    } else if (delta <= 0) {
        s_seq_reordered++;
    }
    e->last_seq = seq;
    return delta;
}

static const uint8_t MAC_A[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static const uint8_t MAC_B[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

/* =========================================================================
 * Tests
 * ========================================================================= */

TEST_CASE("Normal consecutive frames: no anomaly", "[seq_tracking]")
{
    seq_test_reset();
    process_seq(MAC_A, 1);
    process_seq(MAC_A, 2);
    process_seq(MAC_A, 3);
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_gaps);
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_reordered);
}

TEST_CASE("Gap detection: 1,2,5 -> seq_gaps=2", "[seq_tracking]")
{
    seq_test_reset();
    process_seq(MAC_A, 1);
    process_seq(MAC_A, 2);
    process_seq(MAC_A, 5);
    TEST_ASSERT_EQUAL_UINT32(2, s_seq_gaps);
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_reordered);
}

TEST_CASE("Duplicate detection: 5,5 -> seq_reordered=1", "[seq_tracking]")
{
    seq_test_reset();
    process_seq(MAC_A, 5);
    process_seq(MAC_A, 5);
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_gaps);
    TEST_ASSERT_EQUAL_UINT32(1, s_seq_reordered);
}

TEST_CASE("Out-of-order: 5,3 -> seq_reordered=1", "[seq_tracking]")
{
    seq_test_reset();
    process_seq(MAC_A, 5);
    process_seq(MAC_A, 3);
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_gaps);
    TEST_ASSERT_EQUAL_UINT32(1, s_seq_reordered);
}

TEST_CASE("Wrap 65535->0: delta=+1, no anomaly", "[seq_tracking]")
{
    seq_test_reset();
    process_seq(MAC_A, 65534);
    process_seq(MAC_A, 65535);
    int16_t delta = process_seq(MAC_A, 0); /* wrap */
    TEST_ASSERT_EQUAL_INT(1, delta);
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_gaps);
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_reordered);
}

TEST_CASE("Wrap 65535->1: delta=+2, seq_gaps=1", "[seq_tracking]")
{
    seq_test_reset();
    process_seq(MAC_A, 65535);
    int16_t delta = process_seq(MAC_A, 1);
    TEST_ASSERT_EQUAL_INT(2, delta);
    TEST_ASSERT_EQUAL_UINT32(1, s_seq_gaps);
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_reordered);
}

TEST_CASE("First frame: no anomaly regardless of seq value", "[seq_tracking]")
{
    seq_test_reset();
    int16_t delta = process_seq(MAC_A, 50000); /* cold start at arbitrary value */
    TEST_ASSERT_EQUAL_INT(1, delta); /* first_frame guard: returns 1 */
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_gaps);
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_reordered);
}

TEST_CASE("Two publishers tracked independently with same seq values", "[seq_tracking]")
{
    seq_test_reset();
    /* Both publishers start at seq=1 — should not cross-contaminate. */
    process_seq(MAC_A, 1);
    process_seq(MAC_B, 1);
    process_seq(MAC_A, 2);
    process_seq(MAC_B, 2);
    process_seq(MAC_A, 5); /* gap for A only */
    process_seq(MAC_B, 3); /* consecutive for B */

    TEST_ASSERT_EQUAL_UINT32(2, s_seq_gaps);      /* A's gap only */
    TEST_ASSERT_EQUAL_UINT32(0, s_seq_reordered);
}

TEST_CASE("Large gap accumulates correctly", "[seq_tracking]")
{
    seq_test_reset();
    process_seq(MAC_A, 0);
    process_seq(MAC_A, 100); /* gap of 99 */
    TEST_ASSERT_EQUAL_UINT32(99, s_seq_gaps);
}

TEST_CASE("Multiple reorders accumulate correctly", "[seq_tracking]")
{
    seq_test_reset();
    process_seq(MAC_A, 10);
    process_seq(MAC_A, 9);   /* reorder */
    process_seq(MAC_A, 10);  /* duplicate (still <= 0 delta) */
    TEST_ASSERT_EQUAL_UINT32(2, s_seq_reordered);
}
