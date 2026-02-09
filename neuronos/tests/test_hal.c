/**
 * @file test_hal.c
 * @brief NeuronOS HAL — Basic smoke test
 *
 * Tests:
 *   1. HAL initialization and hardware detection
 *   2. Backend registration and selection
 *   3. Scalar vec_dot correctness
 *   4. Scalar quantize correctness
 */

#include "neuronos/neuronos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(cond, msg)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);                                                    \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define PASS(msg) printf("  PASS: %s\n", msg)

/* ──────── Test 1: Init and hardware detection ──────── */
static int test_init(void) {
    neuronos_hal_status_t st = neuronos_hal_init();
    ASSERT(st == NEURONOS_HAL_OK, "neuronos_hal_init() should return OK");

    uint32_t features = neuronos_hal_get_features();
    printf("  Detected features: 0x%08x\n", features);

    const neuronos_backend_t * active = neuronos_hal_get_active_backend();
    ASSERT(active != NULL, "Should have an active backend");
    printf("  Active backend: %s (priority=%d)\n", active->name, active->priority);

    int count = neuronos_hal_get_backend_count();
    ASSERT(count >= 1, "Should have at least scalar backend");
    printf("  Registered backends: %d\n", count);

    PASS("HAL init + hardware detection");
    return 0;
}

/* ──────── Test 2: Backend enumeration ──────── */
static int test_backends(void) {
    int count = neuronos_hal_get_backend_count();
    for (int i = 0; i < count; i++) {
        const neuronos_backend_t * b = neuronos_hal_get_backend(i);
        ASSERT(b != NULL, "Backend should not be NULL");
        ASSERT(b->name != NULL, "Backend name should not be NULL");
        ASSERT(b->vec_dot_i2_i8 != NULL, "vec_dot must not be NULL");
        ASSERT(b->quantize_i2 != NULL, "quantize must not be NULL");
        printf("  Backend[%d]: %s type=%d priority=%d qk=%d\n", i, b->name, b->type, b->priority, b->config.qk_i2_s);
    }

    /* Test selecting scalar explicitly */
    neuronos_hal_status_t st = neuronos_hal_select_backend(NEURONOS_BACKEND_SCALAR);
    ASSERT(st == NEURONOS_HAL_OK, "Should be able to select scalar backend");

    const neuronos_backend_t * active = neuronos_hal_get_active_backend();
    ASSERT(active->type == NEURONOS_BACKEND_SCALAR, "Active should be scalar");

    /* Re-init to get best backend again */
    neuronos_hal_shutdown();
    st = neuronos_hal_init();
    ASSERT(st == NEURONOS_HAL_OK, "Re-init should succeed");

    PASS("Backend enumeration + selection");
    return 0;
}

/* ──────── Test 3: Scalar vec_dot correctness ──────── */
static int test_scalar_vec_dot(void) {
    /* Force scalar backend */
    neuronos_hal_select_backend(NEURONOS_BACKEND_SCALAR);

    /*
     * Create a simple test case:
     * 128 weights, packed in I2_S format.
     * We'll manually pack known ternary values and verify the dot product.
     *
     * Test: all weights = +1 (encoded as 2), activations = 1
     * Expected: sum = 128 * 2 * 1 = 256  (in raw u2 × s8 space)
     */
    const int n = 128;          /* Must be multiple of QK_I2_S=128 */
    const int packed_size = 32; /* 128 weights, 4 per byte = 32 bytes */

    uint8_t packed[32 + 4]; /* +4 for scale float */
    memset(packed, 0, sizeof(packed));

    /* Pack all weights as 2 (=+1 ternary) */
    /* group_idx=0: bits 6-7, group_idx=1: bits 4-5, etc. */
    for (int j = 0; j < n; j++) {
        int group_idx = j / 32;
        int group_pos = j % 32;
        packed[group_pos] |= (2 << (6 - 2 * group_idx));
    }

    /* Activations: all ones */
    int8_t act[128];
    for (int i = 0; i < 128; i++)
        act[i] = 1;

    float result = 0.0f;
    neuronos_vec_dot_i2_i8(n, &result, sizeof(float), packed, (size_t)(n / 4), act, 0, 1);

    /* Expected: each weight raw=2, act=1, so sum = 128 * 2 = 256 */
    ASSERT(fabsf(result - 256.0f) < 0.001f, "vec_dot: all +1 weights × all 1 activations should = 256");
    printf("  vec_dot result: %.1f (expected 256.0)\n", result);

    /*
     * Test 2: all weights = -1 (encoded as 0), activations = 1
     * Expected: sum = 128 * 0 * 1 = 0  (raw u2 = 0)
     */
    memset(packed, 0, 32); /* All zeros → ternary -1, encoded as 0 */
    result = 0.0f;
    neuronos_vec_dot_i2_i8(n, &result, sizeof(float), packed, (size_t)(n / 4), act, 0, 1);
    ASSERT(fabsf(result) < 0.001f, "vec_dot: all 0-encoded weights × all 1 activations should = 0");
    printf("  vec_dot result: %.1f (expected 0.0)\n", result);

    /*
     * Test 3: all weights = 0 (encoded as 1), activations = 5
     * Expected: sum = 128 * 1 * 5 = 640
     */
    memset(packed, 0, 32);
    for (int j = 0; j < n; j++) {
        int group_idx = j / 32;
        int group_pos = j % 32;
        packed[group_pos] |= (1 << (6 - 2 * group_idx));
    }
    for (int i = 0; i < 128; i++)
        act[i] = 5;

    result = 0.0f;
    neuronos_vec_dot_i2_i8(n, &result, sizeof(float), packed, (size_t)(n / 4), act, 0, 1);
    ASSERT(fabsf(result - 640.0f) < 0.001f, "vec_dot: all 1-encoded weights × all 5 activations should = 640");
    printf("  vec_dot result: %.1f (expected 640.0)\n", result);

    /* Restore best backend */
    neuronos_hal_shutdown();
    neuronos_hal_init();

    PASS("Scalar vec_dot correctness");
    return 0;
}

/* ──────── Test 4: Print info ──────── */
static int test_print_info(void) {
    printf("\n");
    neuronos_hal_print_info();
    PASS("HAL print_info");
    return 0;
}

/* ──────── Main ──────── */
int main(void) {
    printf("=== NeuronOS HAL Test Suite ===\n\n");

    int failures = 0;

    failures += test_init();
    failures += test_backends();
    failures += test_scalar_vec_dot();
    failures += test_print_info();

    printf("\n=== Results: %d failures ===\n", failures);
    return failures ? 1 : 0;
}
