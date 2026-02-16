/* ============================================================
 * NeuronOS — JSON Parser Test Suite
 *
 * Tests the unified JSON parser (neuronos_json.h / neuronos_json.c):
 *  1.  nj_find_str — basic key lookup
 *  2.  nj_find_str — key inside string value (must NOT match)
 *  3.  nj_find_str — escaped quotes in value
 *  4.  nj_find_int — integer extraction
 *  5.  nj_find_bool — boolean extraction
 *  6.  nj_find_float — float extraction
 *  7.  nj_extract_object — nested object extraction
 *  8.  nj_extract_array — array extraction
 *  9.  nj_copy_str — buffer copy
 * 10.  nj_alloc_str — allocated string
 * 11.  nj_escape / nj_unescape — round-trip
 * 12.  nj_escape_n — truncated escape
 * 13.  nj_skip_value — skip complex values
 * 14.  NULL / malformed input handling
 * 15.  Recall GC — basic function
 *
 * Usage: ./test_json   (no model needed — pure unit tests)
 * ============================================================ */
#include "neuronos/neuronos_json.h"
#include "neuronos/neuronos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
 * TEST 1: nj_find_str — basic key lookup
 * ============================================================ */
static void test_find_str_basic(void) {
    TEST_START("nj_find_str basic");

    const char * json = "{\"name\":\"NeuronOS\",\"version\":\"0.9.1\"}";
    int len = 0;

    const char * val = nj_find_str(json, "name", &len);
    ASSERT(val != NULL, "name not found");
    ASSERT(len == 8, "name length wrong");
    ASSERT(strncmp(val, "NeuronOS", 8) == 0, "name value wrong");

    val = nj_find_str(json, "version", &len);
    ASSERT(val != NULL, "version not found");
    ASSERT(len == 5, "version length wrong");
    ASSERT(strncmp(val, "0.9.1", 5) == 0, "version value wrong");

    /* Key not present */
    val = nj_find_str(json, "missing", &len);
    ASSERT(val == NULL, "missing key should return NULL");

    TEST_PASS();
}

/* ============================================================
 * TEST 2: nj_find_str — key inside string value (must NOT match)
 * ============================================================ */
static void test_find_str_key_in_value(void) {
    TEST_START("nj_find_str key-in-value safety");

    /* The word "target" appears as a value of "decoy", and also as an actual key */
    const char * json = "{\"decoy\":\"the target is here\",\"target\":\"correct\"}";
    int len = 0;

    const char * val = nj_find_str(json, "target", &len);
    ASSERT(val != NULL, "target key not found");
    ASSERT(len == 7, "target length wrong");
    ASSERT(strncmp(val, "correct", 7) == 0, "matched key inside value instead of actual key");

    TEST_PASS();
}

/* ============================================================
 * TEST 3: nj_find_str — escaped quotes in value
 * ============================================================ */
static void test_find_str_escaped_quotes(void) {
    TEST_START("nj_find_str escaped quotes");

    const char * json = "{\"msg\":\"He said \\\"hello\\\" to me\",\"next\":\"ok\"}";
    int len = 0;

    const char * val = nj_find_str(json, "msg", &len);
    ASSERT(val != NULL, "msg not found");
    /* The raw value includes the backslash-quote sequences */
    ASSERT(strncmp(val, "He said \\\"hello\\\" to me", (size_t)len) == 0, "escaped content wrong");

    /* Make sure "next" key still works after escaped quotes */
    val = nj_find_str(json, "next", &len);
    ASSERT(val != NULL, "next not found after escaped quotes");
    ASSERT(len == 2, "next length wrong");
    ASSERT(strncmp(val, "ok", 2) == 0, "next value wrong");

    TEST_PASS();
}

/* ============================================================
 * TEST 4: nj_find_int — integer extraction
 * ============================================================ */
static void test_find_int(void) {
    TEST_START("nj_find_int");

    const char * json = "{\"id\":42,\"count\":-7,\"zero\":0}";

    ASSERT(nj_find_int(json, "id", -1) == 42, "id wrong");
    ASSERT(nj_find_int(json, "count", 0) == -7, "count wrong");
    ASSERT(nj_find_int(json, "zero", -1) == 0, "zero wrong");
    ASSERT(nj_find_int(json, "missing", 999) == 999, "missing should return fallback");

    TEST_PASS();
}

/* ============================================================
 * TEST 5: nj_find_bool — boolean extraction
 * ============================================================ */
static void test_find_bool(void) {
    TEST_START("nj_find_bool");

    const char * json = "{\"active\":true,\"debug\":false,\"name\":\"test\"}";

    ASSERT(nj_find_bool(json, "active", 0) == 1, "active should be true");
    ASSERT(nj_find_bool(json, "debug", 1) == 0, "debug should be false");
    ASSERT(nj_find_bool(json, "missing", 1) == 1, "missing should return fallback");
    /* "name" is a string, not a bool */
    ASSERT(nj_find_bool(json, "name", -1) == -1, "string value should return fallback");

    TEST_PASS();
}

/* ============================================================
 * TEST 6: nj_find_float — float extraction
 * ============================================================ */
static void test_find_float(void) {
    TEST_START("nj_find_float");

    const char * json = "{\"temp\":0.75,\"neg\":-1.5,\"int_like\":3}";

    float temp = nj_find_float(json, "temp", -1.0f);
    ASSERT(fabsf(temp - 0.75f) < 0.001f, "temp wrong");

    float neg = nj_find_float(json, "neg", 0.0f);
    ASSERT(fabsf(neg - (-1.5f)) < 0.001f, "neg wrong");

    float missing = nj_find_float(json, "missing", 99.0f);
    ASSERT(fabsf(missing - 99.0f) < 0.001f, "missing should return fallback");

    TEST_PASS();
}

/* ============================================================
 * TEST 7: nj_extract_object — nested object extraction
 * ============================================================ */
static void test_extract_object(void) {
    TEST_START("nj_extract_object");

    const char * json = "{\"config\":{\"threads\":4,\"mode\":\"fast\"},\"name\":\"test\"}";

    char * obj = nj_extract_object(json, "config");
    ASSERT(obj != NULL, "config object not found");
    ASSERT(strstr(obj, "\"threads\":4") != NULL, "threads missing in extracted object");
    ASSERT(strstr(obj, "\"mode\":\"fast\"") != NULL, "mode missing in extracted object");
    free(obj);

    /* Non-existent key */
    obj = nj_extract_object(json, "missing");
    ASSERT(obj == NULL, "missing key should return NULL");

    /* Key whose value is not an object */
    obj = nj_extract_object(json, "name");
    ASSERT(obj == NULL, "string value should not extract as object");

    TEST_PASS();
}

/* ============================================================
 * TEST 8: nj_extract_array — array extraction
 * ============================================================ */
static void test_extract_array(void) {
    TEST_START("nj_extract_array");

    const char * json = "{\"items\":[1,2,3],\"nested\":[{\"a\":1},{\"b\":2}]}";

    char * arr = nj_extract_array(json, "items");
    ASSERT(arr != NULL, "items array not found");
    ASSERT(strcmp(arr, "[1,2,3]") == 0, "items content wrong");
    free(arr);

    arr = nj_extract_array(json, "nested");
    ASSERT(arr != NULL, "nested array not found");
    ASSERT(strstr(arr, "{\"a\":1}") != NULL, "nested object missing");
    free(arr);

    arr = nj_extract_array(json, "missing");
    ASSERT(arr == NULL, "missing key should return NULL");

    TEST_PASS();
}

/* ============================================================
 * TEST 9: nj_copy_str — buffer copy
 * ============================================================ */
static void test_copy_str(void) {
    TEST_START("nj_copy_str");

    const char * json = "{\"greeting\":\"hello world\"}";
    char buf[32];

    int n = nj_copy_str(json, "greeting", buf, sizeof(buf));
    ASSERT(n == 11, "copy length wrong");
    ASSERT(strcmp(buf, "hello world") == 0, "copy content wrong");

    /* Buffer too small — should truncate safely */
    char tiny[6];
    n = nj_copy_str(json, "greeting", tiny, sizeof(tiny));
    ASSERT(n >= 0, "copy with small buffer should not fail");
    ASSERT(tiny[5] == '\0', "small buffer must be NUL-terminated");
    ASSERT(strncmp(tiny, "hello", 5) == 0, "truncated content wrong");

    /* Missing key */
    n = nj_copy_str(json, "missing", buf, sizeof(buf));
    ASSERT(n == -1, "missing key should return -1");

    TEST_PASS();
}

/* ============================================================
 * TEST 10: nj_alloc_str — allocated string
 * ============================================================ */
static void test_alloc_str(void) {
    TEST_START("nj_alloc_str");

    const char * json = "{\"tool\":\"calculator\",\"empty\":\"\"}";

    char * val = nj_alloc_str(json, "tool");
    ASSERT(val != NULL, "tool not found");
    ASSERT(strcmp(val, "calculator") == 0, "tool value wrong");
    free(val);

    /* Empty string value */
    val = nj_alloc_str(json, "empty");
    ASSERT(val != NULL, "empty string not found");
    ASSERT(strcmp(val, "") == 0, "empty string should be empty");
    free(val);

    /* Missing key */
    val = nj_alloc_str(json, "nope");
    ASSERT(val == NULL, "missing key should return NULL");

    TEST_PASS();
}

/* ============================================================
 * TEST 11: nj_escape / nj_unescape — round-trip
 * ============================================================ */
static void test_escape_roundtrip(void) {
    TEST_START("nj_escape / nj_unescape round-trip");

    const char * original = "Hello\n\"World\"\ttab\\slash";

    char * escaped = nj_escape(original);
    ASSERT(escaped != NULL, "escape failed");
    /* Escaped should contain \\n, \\\", \\t, \\\\ */
    ASSERT(strstr(escaped, "\\n") != NULL, "newline not escaped");
    ASSERT(strstr(escaped, "\\\"") != NULL, "quote not escaped");
    ASSERT(strstr(escaped, "\\t") != NULL, "tab not escaped");
    ASSERT(strstr(escaped, "\\\\") != NULL, "backslash not escaped");

    /* Round-trip: unescape should recover original */
    char * back = nj_unescape(escaped);
    ASSERT(back != NULL, "unescape failed");
    ASSERT(strcmp(back, original) == 0, "round-trip mismatch");

    free(escaped);
    free(back);

    /* NULL input */
    escaped = nj_escape(NULL);
    ASSERT(escaped != NULL, "nj_escape(NULL) should return \"null\"");
    ASSERT(strcmp(escaped, "null") == 0, "nj_escape(NULL) should be \"null\"");
    free(escaped);

    TEST_PASS();
}

/* ============================================================
 * TEST 12: nj_escape_n — truncated escape
 * ============================================================ */
static void test_escape_n(void) {
    TEST_START("nj_escape_n truncated");

    const char * input = "abcdefghij";

    /* Escape only first 5 chars */
    char * esc = nj_escape_n(input, 5);
    ASSERT(esc != NULL, "escape_n failed");
    ASSERT(strcmp(esc, "abcde") == 0, "truncated escape wrong");
    free(esc);

    /* With special chars: "ab\ncd" → first 4 chars = "ab\n" (escaped = "ab\\n") */
    const char * special = "ab\ncd";
    esc = nj_escape_n(special, 3);
    ASSERT(esc != NULL, "escape_n special failed");
    ASSERT(strcmp(esc, "ab\\n") == 0, "truncated escape with special wrong");
    free(esc);

    TEST_PASS();
}

/* ============================================================
 * TEST 13: nj_skip_value — skip complex values
 * ============================================================ */
static void test_skip_value(void) {
    TEST_START("nj_skip_value");

    /* Skip a string */
    const char * s1 = "\"hello\", next";
    const char * end = nj_skip_value(s1);
    ASSERT(end != NULL, "skip string failed");
    ASSERT(*end == ',', "should point after closing quote");

    /* Skip a number */
    const char * s2 = "42, next";
    end = nj_skip_value(s2);
    ASSERT(end != NULL, "skip number failed");
    ASSERT(*end == ',', "should point after number");

    /* Skip a nested object */
    const char * s3 = "{\"a\":{\"b\":1}}, next";
    end = nj_skip_value(s3);
    ASSERT(end != NULL, "skip object failed");
    ASSERT(*end == ',', "should point after closing brace");

    /* Skip an array */
    const char * s4 = "[1,[2,3],4], next";
    end = nj_skip_value(s4);
    ASSERT(end != NULL, "skip array failed");
    ASSERT(*end == ',', "should point after closing bracket");

    /* Skip true/false/null */
    end = nj_skip_value("true, x");
    ASSERT(end && *end == ',', "skip true failed");
    end = nj_skip_value("false, x");
    ASSERT(end && *end == ',', "skip false failed");
    end = nj_skip_value("null, x");
    ASSERT(end && *end == ',', "skip null failed");

    TEST_PASS();
}

/* ============================================================
 * TEST 14: NULL and malformed input handling
 * ============================================================ */
static void test_null_safety(void) {
    TEST_START("NULL / malformed input safety");

    /* All read functions should handle NULL gracefully */
    ASSERT(nj_find_str(NULL, "key", NULL) == NULL, "find_str(NULL) should return NULL");
    ASSERT(nj_find_str("{}", NULL, NULL) == NULL, "find_str(json, NULL) should return NULL");
    ASSERT(nj_find_int(NULL, "key", -1) == -1, "find_int(NULL) should return fallback");
    ASSERT(nj_find_bool(NULL, "key", 0) == 0, "find_bool(NULL) should return fallback");
    ASSERT(nj_extract_object(NULL, "key") == NULL, "extract_object(NULL) should return NULL");
    ASSERT(nj_extract_array(NULL, "key") == NULL, "extract_array(NULL) should return NULL");
    ASSERT(nj_alloc_str(NULL, "key") == NULL, "alloc_str(NULL) should return NULL");

    char buf[32];
    ASSERT(nj_copy_str(NULL, "key", buf, sizeof(buf)) == -1, "copy_str(NULL) should return -1");

    /* Empty JSON */
    ASSERT(nj_find_str("{}", "key", NULL) == NULL, "empty JSON should return NULL");
    ASSERT(nj_find_int("{}", "key", 42) == 42, "empty JSON int should return fallback");

    /* Malformed JSON — should not crash */
    ASSERT(nj_find_str("{broken", "key", NULL) == NULL, "broken JSON should return NULL");
    ASSERT(nj_extract_object("{\"a\":{unclosed", "a") == NULL, "unclosed object should return NULL");

    TEST_PASS();
}

/* ============================================================
 * TEST 15: Recall memory GC
 * ============================================================ */
static void test_recall_gc(void) {
    TEST_START("Recall memory GC");

    neuronos_memory_t * mem = neuronos_memory_open(":memory:");
    ASSERT(mem != NULL, "memory open failed");

    int64_t sid = neuronos_memory_session_create(mem);
    ASSERT(sid > 0, "session create failed");

    /* Add 10 messages */
    for (int i = 0; i < 10; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Message %d", i);
        int64_t id = neuronos_memory_recall_add(mem, sid, "user", msg, 10);
        ASSERT(id > 0, "recall add failed");
    }

    /* Verify 10 messages */
    int msg_count = 0, token_count = 0;
    neuronos_memory_recall_stats(mem, sid, &msg_count, &token_count);
    ASSERT(msg_count == 10, "should have 10 messages");

    /* GC: keep only 5 newest */
    int deleted = neuronos_memory_recall_gc(mem, sid, 5, 0);
    ASSERT(deleted == 5, "should delete 5 messages");

    /* Verify 5 remain */
    neuronos_memory_recall_stats(mem, sid, &msg_count, &token_count);
    ASSERT(msg_count == 5, "should have 5 messages after GC");

    /* GC with no effect (already <= limit) */
    deleted = neuronos_memory_recall_gc(mem, sid, 10, 0);
    ASSERT(deleted == 0, "should delete 0 when under limit");

    /* GC with 0 max_messages (no limit) should not delete */
    deleted = neuronos_memory_recall_gc(mem, sid, 0, 0);
    ASSERT(deleted == 0, "0/0 should delete nothing");

    neuronos_memory_close(mem);
    TEST_PASS();
}

/* ============================================================
 * MAIN
 * ============================================================ */
int main(void) {
    fprintf(stderr, "\n");
    fprintf(stderr, "═══════════════════════════════════════════\n");
    fprintf(stderr, " NeuronOS JSON Parser + GC Test Suite\n");
    fprintf(stderr, "═══════════════════════════════════════════\n");

    test_find_str_basic();
    test_find_str_key_in_value();
    test_find_str_escaped_quotes();
    test_find_int();
    test_find_bool();
    test_find_float();
    test_extract_object();
    test_extract_array();
    test_copy_str();
    test_alloc_str();
    test_escape_roundtrip();
    test_escape_n();
    test_skip_value();
    test_null_safety();
    test_recall_gc();

    fprintf(stderr, "\n");
    fprintf(stderr, "═══════════════════════════════════════════\n");
    fprintf(stderr, " Results: %d/%d passed\n", tests_passed, tests_run);
    fprintf(stderr, "═══════════════════════════════════════════\n\n");

    return tests_failed > 0 ? 1 : 0;
}
