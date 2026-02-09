/**
 * @file hal_x86_avx2.c
 * @brief NeuronOS HAL — x86 AVX2 backend
 *
 * Wraps the existing AVX2 SIMD kernels from ggml-bitnet-mad.cpp
 * into the NeuronOS HAL backend interface. This is a thin adapter
 * layer — the actual SIMD code lives in ggml-bitnet-mad.cpp and
 * is called through the forward declarations below.
 *
 * Requirements: AVX2 + SSSE3 (for _mm256_maddubs_epi16)
 *
 * Compile with: -mavx2 -mssse3 (clang/gcc)
 */

/* Savage Mode: Force Enable */
//#if defined(__AVX2__)

    #include "neuronos/neuronos_hal.h"

    #include <stdint.h>

/* ──────── Forward declarations of existing kernel functions ─────── */

/*
 * These functions are defined in src/ggml-bitnet-mad.cpp.
 * We declare them here with C linkage to call from our C backend.
 * The actual dispatch logic (1x1 vs 1xN vs Nx1) is inside
 * ggml_vec_dot_i2_i8_s().
 */

    #ifdef __cplusplus
extern "C" {
    #endif

extern void ggml_vec_dot_i2_i8_s(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by,
                                 int nrc);

extern size_t quantize_i2_s(const float * src, void * dst, int64_t nrow, int64_t n_per_row,
                            const float * quant_weights);

/*
 * Note: ggml_gemv_i2_i8_s / ggml_gemm_i2_i8_s are defined in
 * ggml-aarch64.c (ARM only). On x86, ggml dispatches gemv/gemm
 * through type_traits which calls vec_dot in a loop.
 * Our HAL gemv/gemm for x86 uses vec_dot directly.
 */

    #ifdef __cplusplus
}
    #endif

/* ──────── HAL wrapper functions ────────────────────────────────── */

/**
 * AVX2 vec_dot: delegates directly to the BitNet MAD dispatcher.
 * ggml_vec_dot_i2_i8_s internally selects between 1x1, 1xN, Nx1
 * based on nrc and PARALLEL_SIZE.
 */
static void avx2_vec_dot_i2_i8(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by,
                               int nrc) {
    ggml_vec_dot_i2_i8_s(n, s, bs, vx, bx, vy, by, nrc);
}

/**
 * AVX2 quantize: delegates to the existing BitNet quantizer.
 */
static size_t avx2_quantize_i2(const float * src, void * dst, int64_t nrow, int64_t n_per_row,
                               const float * quant_weights) {
    return quantize_i2_s(src, dst, nrow, n_per_row, quant_weights);
}

/**
 * AVX2 gemv: On x86, there's no dedicated gemv — we use vec_dot per row.
 * This matches how ggml dispatches on x86 via type_traits.
 */
static void avx2_gemv_i2_i8(int n, float * s, size_t bs, const void * vx, const void * vy, int nr, int nc) {
    (void)nc;
    /* Process one row at a time using vec_dot */
    const uint8_t * x = (const uint8_t *)vx;
    const size_t row_bytes_packed = (size_t)(n / 4); /* packed bytes per row */

    for (int row = 0; row < nr; row++) {
        ggml_vec_dot_i2_i8_s(n, s + row, 0, x + row * row_bytes_packed, row_bytes_packed, vy, 0, 1);
    }
}

/**
 * AVX2 gemm: Uses vec_dot in a loop.
 */
static void avx2_gemm_i2_i8(int n, float * s, size_t bs, const void * vx, const void * vy, int nr, int nc) {
    avx2_gemv_i2_i8(n, s, bs, vx, vy, nr, nc);
}

/* ──────── Backend descriptor ───────────────────────────────────── */

const neuronos_backend_t neuronos_backend_x86_avx2 = {
    .name = "x86_avx2",
    .type = NEURONOS_BACKEND_X86_AVX2,
    .priority = 50, /* High priority for SIMD — above scalar(0) */
    .required_features = NEURONOS_FEAT_AVX2 | NEURONOS_FEAT_SSSE3,
    .config =
        {
            /*
             * These match gemm-config.h for x86 AVX2 + ACT_PARALLEL:
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
    .vec_dot_i2_i8 = avx2_vec_dot_i2_i8,
    .quantize_i2 = avx2_quantize_i2,
    .gemv_i2_i8 = avx2_gemv_i2_i8,
    .gemm_i2_i8 = avx2_gemm_i2_i8,
    .init = NULL,
    .shutdown = NULL,
};

//#endif /* __AVX2__ */
