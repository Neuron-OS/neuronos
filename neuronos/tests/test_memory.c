/* ============================================================
 * NeuronOS — Memory Test Suite
 *
 * Tests the SQLite-backed persistent memory system:
 *  1. Open/close (in-memory)
 *  2. Core memory set/get
 *  3. Core memory append
 *  4. Core memory dump
 *  5. Archival store/recall
 *  6. Archival search (FTS5)
 *  7. Archival stats
 *  8. Recall memory add/recent
 *  9. Recall memory search (FTS5)
 * 10. Recall memory stats
 * 11. Session management
 * 12. Legacy API (store/recall/search)
 *
 * Usage: ./test_memory   (no model needed — pure SQLite)
 * ============================================================ */
#include "neuronos/neuronos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Helpers ---- */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name)                                                                                               \
    do {                                                                                                               \
        tests_run++;                                                                                                   \
        fprintf(stderr, "\n[TEST %d] %s... ", tests_run, name);                                                        \
    } while (0)

#define TEST_PASS()                                                                                                    \
    do {                                                                                                               \
        tests_passed++;                                                                                                \
        fprintf(stderr, "PASS ✓\n");                                                                                   \
    } while (0)

#define TEST_FAIL(msg)                                                                                                 \
    do {                                                                                                               \
        tests_failed++;                                                                                                \
        fprintf(stderr, "FAIL ✗ (%s)\n", msg);                                                                         \
    } while (0)

#define ASSERT(cond, msg)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            TEST_FAIL(msg);                                                                                            \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

/* ============================================================
 * TEST 1: Open / Close (in-memory)
 * ============================================================ */
static void test_open_close(void) {
    TEST_START("Memory open/close (in-memory)");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 2: Core memory set/get
 * ============================================================ */
static void test_core_set_get(void) {
    TEST_START("Core memory set/get");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    /* Default persona should exist */
    char * persona = neuronos_memory_core_get(mem, "persona");
    ASSERT(persona != NULL, "default persona missing");
    ASSERT(strstr(persona, "NeuronOS") != NULL, "default persona doesn't mention NeuronOS");
    free(persona);

    /* Set custom value */
    int rc = neuronos_memory_core_set(mem, "persona", "I am a test agent.");
    ASSERT(rc == 0, "core_set failed");

    char * val = neuronos_memory_core_get(mem, "persona");
    ASSERT(val != NULL, "core_get returned NULL");
    ASSERT(strcmp(val, "I am a test agent.") == 0, "core_get value mismatch");
    free(val);

    /* Update existing */
    rc = neuronos_memory_core_set(mem, "persona", "Updated persona.");
    ASSERT(rc == 0, "core_set update failed");

    val = neuronos_memory_core_get(mem, "persona");
    ASSERT(val != NULL, "core_get after update returned NULL");
    ASSERT(strcmp(val, "Updated persona.") == 0, "update value mismatch");
    free(val);

    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 3: Core memory append
 * ============================================================ */
static void test_core_append(void) {
    TEST_START("Core memory append");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    neuronos_memory_core_set(mem, "human", "Name: Alice");
    int rc = neuronos_memory_core_append(mem, "human", "Likes: cats");
    ASSERT(rc == 0, "core_append failed");

    char * val = neuronos_memory_core_get(mem, "human");
    ASSERT(val != NULL, "core_get failed");
    ASSERT(strstr(val, "Name: Alice") != NULL, "original content lost");
    ASSERT(strstr(val, "Likes: cats") != NULL, "appended content missing");
    free(val);

    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 4: Core memory dump
 * ============================================================ */
static void test_core_dump(void) {
    TEST_START("Core memory dump");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    char * dump = neuronos_memory_core_dump(mem);
    ASSERT(dump != NULL, "core_dump returned NULL");
    /* Should contain default blocks: persona, human, instructions */
    ASSERT(strstr(dump, "<persona>") != NULL || strstr(dump, "persona") != NULL,
           "dump missing persona block");
    ASSERT(strstr(dump, "<instructions>") != NULL || strstr(dump, "instructions") != NULL,
           "dump missing instructions block");
    free(dump);

    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 5: Archival store / recall
 * ============================================================ */
static void test_archival_store_recall(void) {
    TEST_START("Archival store/recall");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    int64_t id = neuronos_memory_archival_store(mem, "user_name", "Alice", "user_info", 0.9f);
    ASSERT(id >= 0, "archival_store failed");

    char * val = neuronos_memory_archival_recall(mem, "user_name");
    ASSERT(val != NULL, "archival_recall returned NULL");
    ASSERT(strcmp(val, "Alice") == 0, "archival recall value mismatch");
    free(val);

    /* Update existing key */
    int64_t id2 = neuronos_memory_archival_store(mem, "user_name", "Bob", "user_info", 0.9f);
    ASSERT(id2 == id, "store should return same id for update");

    val = neuronos_memory_archival_recall(mem, "user_name");
    ASSERT(val != NULL, "recall after update failed");
    ASSERT(strcmp(val, "Bob") == 0, "updated value mismatch");
    free(val);

    /* Non-existent key */
    val = neuronos_memory_archival_recall(mem, "nonexistent");
    ASSERT(val == NULL, "recall of nonexistent should be NULL");

    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 6: Archival search (FTS5)
 * ============================================================ */
static void test_archival_search(void) {
    TEST_START("Archival search (FTS5)");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    /* Store several facts */
    neuronos_memory_archival_store(mem, "fav_color", "User's favorite color is blue", "preferences", 0.7f);
    neuronos_memory_archival_store(mem, "fav_food", "User enjoys Italian pasta", "preferences", 0.6f);
    neuronos_memory_archival_store(mem, "work", "User works as a software engineer", "personal", 0.8f);

    /* Search */
    neuronos_archival_entry_t * entries = NULL;
    int count = 0;
    int rc = neuronos_memory_archival_search(mem, "color blue", 10, &entries, &count);
    ASSERT(rc == 0, "archival_search failed");
    ASSERT(count >= 1, "archival_search found no results for 'color blue'");
    ASSERT(strstr(entries[0].value, "blue") != NULL, "first result doesn't contain 'blue'");

    neuronos_memory_archival_free(entries, count);

    /* Search for "engineer" */
    rc = neuronos_memory_archival_search(mem, "software engineer", 10, &entries, &count);
    ASSERT(rc == 0, "archival_search for 'software engineer' failed");
    ASSERT(count >= 1, "no results for 'software engineer'");
    neuronos_memory_archival_free(entries, count);

    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 7: Archival stats
 * ============================================================ */
static void test_archival_stats(void) {
    TEST_START("Archival stats");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    neuronos_memory_archival_store(mem, "fact1", "value1", NULL, 0.5f);
    neuronos_memory_archival_store(mem, "fact2", "value2", NULL, 0.5f);

    int fact_count = 0;
    int rc = neuronos_memory_archival_stats(mem, &fact_count);
    ASSERT(rc == 0, "archival_stats failed");
    ASSERT(fact_count == 2, "expected 2 facts");

    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 8: Recall memory add / recent
 * ============================================================ */
static void test_recall_add_recent(void) {
    TEST_START("Recall add/recent");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    int64_t id1 = neuronos_memory_recall_add(mem, 1, "user", "Hello there!", 3);
    ASSERT(id1 >= 0, "recall_add user failed");

    int64_t id2 = neuronos_memory_recall_add(mem, 1, "assistant", "Hi! How can I help?", 5);
    ASSERT(id2 > id1, "recall_add assistant should have higher id");

    int64_t id3 = neuronos_memory_recall_add(mem, 1, "user", "What is 2+2?", 4);
    ASSERT(id3 >= 0, "recall_add second user msg failed");

    /* Get recent (most recent first) */
    neuronos_recall_entry_t * entries = NULL;
    int count = 0;
    int rc = neuronos_memory_recall_recent(mem, 1, 10, &entries, &count);
    ASSERT(rc == 0, "recall_recent failed");
    ASSERT(count == 3, "expected 3 recall entries");
    /* Most recent first */
    ASSERT(strcmp(entries[0].content, "What is 2+2?") == 0, "first entry should be most recent");
    ASSERT(strcmp(entries[0].role, "user") == 0, "role mismatch");

    neuronos_memory_recall_free(entries, count);
    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 9: Recall memory search (FTS5)
 * ============================================================ */
static void test_recall_search(void) {
    TEST_START("Recall search (FTS5)");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    neuronos_memory_recall_add(mem, 1, "user", "Tell me about quantum computing", 6);
    neuronos_memory_recall_add(mem, 1, "assistant", "Quantum computing uses qubits for computation", 8);
    neuronos_memory_recall_add(mem, 1, "user", "What about classical computers?", 5);

    neuronos_recall_entry_t * entries = NULL;
    int count = 0;
    int rc = neuronos_memory_recall_search(mem, "quantum", 10, &entries, &count);
    ASSERT(rc == 0, "recall_search failed");
    ASSERT(count >= 1, "recall_search found no results for 'quantum'");

    neuronos_memory_recall_free(entries, count);
    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 10: Recall stats
 * ============================================================ */
static void test_recall_stats(void) {
    TEST_START("Recall stats");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    neuronos_memory_recall_add(mem, 1, "user", "Hello", 2);
    neuronos_memory_recall_add(mem, 1, "assistant", "Hi there", 3);

    int msg_count = 0, token_count = 0;
    int rc = neuronos_memory_recall_stats(mem, 1, &msg_count, &token_count);
    ASSERT(rc == 0, "recall_stats failed");
    ASSERT(msg_count == 2, "expected 2 messages");
    ASSERT(token_count == 5, "expected 5 tokens total");

    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 11: Session management
 * ============================================================ */
static void test_sessions(void) {
    TEST_START("Session management");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    int64_t s1 = neuronos_memory_session_create(mem);
    ASSERT(s1 > 0, "session_create failed");

    int64_t s2 = neuronos_memory_session_create(mem);
    ASSERT(s2 > s1, "second session should have higher id");

    /* Messages in different sessions are separate */
    neuronos_memory_recall_add(mem, s1, "user", "Session 1 message", 4);
    neuronos_memory_recall_add(mem, s2, "user", "Session 2 message", 4);

    int msg1 = 0, msg2 = 0;
    neuronos_memory_recall_stats(mem, s1, &msg1, NULL);
    neuronos_memory_recall_stats(mem, s2, &msg2, NULL);
    ASSERT(msg1 == 1, "session 1 should have 1 message");
    ASSERT(msg2 == 1, "session 2 should have 1 message");

    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * TEST 12: Legacy API (store / recall / search)
 * ============================================================ */
static void test_legacy_api(void) {
    TEST_START("Legacy API (store/recall/search)");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    int rc = neuronos_memory_store(mem, "project", "NeuronOS agent engine");
    ASSERT(rc == 0, "legacy store failed");

    char * val = neuronos_memory_recall(mem, "project");
    ASSERT(val != NULL, "legacy recall returned NULL");
    ASSERT(strcmp(val, "NeuronOS agent engine") == 0, "legacy recall value mismatch");
    free(val);

    /* Store more and search */
    neuronos_memory_store(mem, "language", "C11 for core runtime");
    neuronos_memory_store(mem, "model", "BitNet b1.58 ternary");

    char ** results = NULL;
    int n_results = 0;
    rc = neuronos_memory_search(mem, "C11 runtime", &results, &n_results, 10);
    ASSERT(rc == 0, "legacy search failed");
    ASSERT(n_results >= 1, "legacy search found no results");

    neuronos_memory_free_results(results, n_results);
    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * MAIN
 * ============================================================ */
int main(void) {
    fprintf(stderr, "═══════════════════════════════════════════\n");
    fprintf(stderr, " NeuronOS Memory Test Suite\n");
    fprintf(stderr, "═══════════════════════════════════════════\n");

    test_open_close();
    test_core_set_get();
    test_core_append();
    test_core_dump();
    test_archival_store_recall();
    test_archival_search();
    test_archival_stats();
    test_recall_add_recent();
    test_recall_search();
    test_recall_stats();
    test_sessions();
    test_legacy_api();

    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, " Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        fprintf(stderr, " (%d FAILED)", tests_failed);
    }
    fprintf(stderr, "\n═══════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
