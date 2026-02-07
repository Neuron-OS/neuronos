/**
 * @file hal_arm_neon.c
 * @brief NeuronOS HAL — ARM NEON backend
 *
 * Wraps the existing ARM NEON SIMD kernels from ggml-bitnet-mad.cpp
 * and ggml-aarch64.c into the NeuronOS HAL backend interface.
 * This is a thin adapter layer — the actual SIMD code lives in the
 * parent BitNet repository.
 *
 * Requirements: NEON (always available on AArch64)
 *
 * Compile with: -march=armv8-a+simd (gcc/clang on ARM)
 */

#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)

    #include "neuronos/neuronos_hal.h"

    #include <stdint.h>

/* ──────── Forward declarations of existing kernel functions ─────── */

/*
 * These functions are defined in:
 *   - src/ggml-bitnet-mad.cpp (vec_dot, quantize)
 *   - 3rdparty/llama.cpp/ggml/src/ggml-aarch64.c (gemv, gemm)
 *
 * We declare them here with C linkage to call from our C backend.
 */

    #ifdef __cplusplus
extern "C" {
    #endif

/* From ggml-bitnet-mad.cpp */
extern void ggml_vec_dot_i2_i8_s(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by,
                                 int nrc);

extern size_t quantize_i2_s(const float * src, void * dst, int64_t nrow, int64_t n_per_row,
                            const float * quant_weights);

/* From ggml-aarch64.c */
extern void ggml_gemv_i2_i8_s(int n, float * restrict s, size_t bs, const void * restrict vx, const void * restrict vy,
                              int nr, int nc);

extern void ggml_gemm_i2_i8_s(int n, float * restrict s, size_t bs, const void * restrict vx, const void * restrict vy,
                              int nr, int nc);

    #ifdef __cplusplus
}
    #endif

/* ──────── HAL wrapper functions ────────────────────────────────── */

/**
 * ARM NEON vec_dot: delegates directly to the BitNet MAD dispatcher.
 */
static void neon_vec_dot_i2_i8(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by,
                               int nrc) {
    ggml_vec_dot_i2_i8_s(n, s, bs, vx, bx, vy, by, nrc);
}

/**
 * ARM NEON quantize: delegates to the existing BitNet quantizer.
 */
static size_t neon_quantize_i2(const float * src, void * dst, int64_t nrow, int64_t n_per_row,
                               const float * quant_weights) {
    return quantize_i2_s(src, dst, nrow, n_per_row, quant_weights);
}

/**
 * ARM NEON gemv: delegates to the optimized kernel in ggml-aarch64.c
 */
static void neon_gemv_i2_i8(int n, float * s, size_t bs, const void * vx, const void * vy, int nr, int nc) {
    ggml_gemv_i2_i8_s(n, s, bs, vx, vy, nr, nc);
}

/**
 * ARM NEON gemm: delegates to the optimized kernel in ggml-aarch64.c
 */
static void neon_gemm_i2_i8(int n, float * s, size_t bs, const void * vx, const void * vy, int nr, int nc) {
    ggml_gemm_i2_i8_s(n, s, bs, vx, vy, nr, nc);
}

/* ──────── Backend descriptor ───────────────────────────────────── */

const neuronos_backend_t neuronos_backend_arm_neon = {
    .name = "arm_neon",
    .type = NEURONOS_BACKEND_ARM_NEON,
    .priority = 50, /* High priority for SIMD — above scalar(0) */
    .required_features = NEURONOS_FEAT_NEON,
    .config =
        {
            /*
             * ARM NEON configuration from gemm-config.h:
             *   ROW_BLOCK_SIZE  = 4
             *   COL_BLOCK_SIZE  = 128
             *   PARALLEL_SIZE   = 4
             *   QK_I2_S         = 128
             */
            .row_block_size = 4,
            .col_block_size = 128,
            .parallel_size = 4,
            .qk_i2_s = 128,
        },
    .vec_dot_i2_i8 = neon_vec_dot_i2_i8,
    .quantize_i2 = neon_quantize_i2,
    .gemv_i2_i8 = neon_gemv_i2_i8,
    .gemm_i2_i8 = neon_gemm_i2_i8,
    .init = NULL,
    .shutdown = NULL,
};

#endif /* __ARM_NEON || __aarch64__ || _M_ARM64 */
