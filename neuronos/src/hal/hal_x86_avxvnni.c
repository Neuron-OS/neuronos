/**
 * @file hal_x86_avxvnni.c
 * @brief NeuronOS HAL — x86 AVX-VNNI backend (Savage Mode 2.4 - Ultimate Parallelism)
 */

/* Savage Mode: Force Enable */
#include "neuronos/neuronos_hal.h"
#include <immintrin.h>
#include <stdint.h>
#include <string.h>

extern size_t quantize_i2_s(const float * src, void * dst, int64_t nrow, int64_t n_per_row, const float * quant_weights);

static inline int hsum_i32_8(const __m256i a) {
    const __m128i sum128 = _mm_add_epi32(_mm256_castsi256_si128(a), _mm256_extractf128_si256(a, 1));
    const __m128i hi64 = _mm_unpackhi_epi64(sum128, sum128);
    const __m128i sum64 = _mm_add_epi32(hi64, sum128);
    const __m128i hi32  = _mm_shuffle_epi32(sum64, _MM_SHUFFLE(2, 3, 0, 1));
    return _mm_cvtsi128_si32(_mm_add_epi32(sum64, hi32));
}

static void avxvnni_vec_dot_i2_i8(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by, int nrc) {
    const uint8_t * x = (const uint8_t *)vx;
    const int8_t  * y = (const int8_t *)vy;
    const int qk = 128;
    const int nb = n / qk;

    __m256i ones = _mm256_set1_epi8(1);
    __m256i mask = _mm256_set1_epi8(0x03);

    int row = 0;
    // Process 8 rows at a time for ULTIMATE throughput
    for (; row <= nrc - 8; row += 8) {
        __m256i acc0 = _mm256_setzero_si256();
        __m256i acc1 = _mm256_setzero_si256();
        __m256i acc2 = _mm256_setzero_si256();
        __m256i acc3 = _mm256_setzero_si256();
        __m256i acc4 = _mm256_setzero_si256();
        __m256i acc5 = _mm256_setzero_si256();
        __m256i acc6 = _mm256_setzero_si256();
        __m256i acc7 = _mm256_setzero_si256();

        __m256i sum_y = _mm256_setzero_si256();

        const uint8_t * x_base = x + (row * bx / 4);

        for (int i = 0; i < nb; i++) {
            const int8_t * py = y + i * 128;
            _mm_prefetch((const char*)(py + 128), _MM_HINT_T0);

            __m256i v0 = _mm256_loadu_si256((const __m256i*)(py + 0));
            __m256i v1 = _mm256_loadu_si256((const __m256i*)(py + 32));
            __m256i v2 = _mm256_loadu_si256((const __m256i*)(py + 64));
            __m256i v3 = _mm256_loadu_si256((const __m256i*)(py + 96));

            sum_y = _mm256_add_epi32(sum_y, _mm256_madd_epi16(_mm256_maddubs_epi16(ones, v0), ones));
            sum_y = _mm256_add_epi32(sum_y, _mm256_madd_epi16(_mm256_maddubs_epi16(ones, v1), ones));
            sum_y = _mm256_add_epi32(sum_y, _mm256_madd_epi16(_mm256_maddubs_epi16(ones, v2), ones));
            sum_y = _mm256_add_epi32(sum_y, _mm256_madd_epi16(_mm256_maddubs_epi16(ones, v3), ones));

#define PROC_ROW(IDX, ACC) \
            { \
                __m256i b = _mm256_loadu_si256((const __m256i*)(x_base + (IDX * bx / 4) + i * 32)); \
                ACC = _mm256_dpbusd_epi32(ACC, _mm256_and_si256(_mm256_srli_epi16(b, 6), mask), v0); \
                ACC = _mm256_dpbusd_epi32(ACC, _mm256_and_si256(_mm256_srli_epi16(b, 4), mask), v1); \
                ACC = _mm256_dpbusd_epi32(ACC, _mm256_and_si256(_mm256_srli_epi16(b, 2), mask), v2); \
                ACC = _mm256_dpbusd_epi32(ACC, _mm256_and_si256(b, mask), v3); \
            }

            PROC_ROW(0, acc0); PROC_ROW(1, acc1);
            PROC_ROW(2, acc2); PROC_ROW(3, acc3);
            PROC_ROW(4, acc4); PROC_ROW(5, acc5);
            PROC_ROW(6, acc6); PROC_ROW(7, acc7);
#undef PROC_ROW
        }

        int sy = hsum_i32_8(sum_y);
        s[row+0] = (float)(hsum_i32_8(acc0) - sy);
        s[row+1] = (float)(hsum_i32_8(acc1) - sy);
        s[row+2] = (float)(hsum_i32_8(acc2) - sy);
        s[row+3] = (float)(hsum_i32_8(acc3) - sy);
        s[row+4] = (float)(hsum_i32_8(acc4) - sy);
        s[row+5] = (float)(hsum_i32_8(acc5) - sy);
        s[row+6] = (float)(hsum_i32_8(acc6) - sy);
        s[row+7] = (float)(hsum_i32_8(acc7) - sy);
    }

    // Fallback for remaining rows
    for (; row < nrc; row++) {
        __m256i acc0 = _mm256_setzero_si256();
        __m256i sum_y = _mm256_setzero_si256();
        const uint8_t * x_row = x + (row * bx / 4);
        for (int i = 0; i < nb; i++) {
            const int8_t * py = y + i * 128;
            __m256i v0 = _mm256_loadu_si256((const __m256i*)(py + 0));
            __m256i v1 = _mm256_loadu_si256((const __m256i*)(py + 32));
            __m256i v2 = _mm256_loadu_si256((const __m256i*)(py + 64));
            __m256i v3 = _mm256_loadu_si256((const __m256i*)(py + 96));
            sum_y = _mm256_add_epi32(sum_y, _mm256_madd_epi16(_mm256_maddubs_epi16(ones, v0), ones));
            sum_y = _mm256_add_epi32(sum_y, _mm256_madd_epi16(_mm256_maddubs_epi16(ones, v1), ones));
            sum_y = _mm256_add_epi32(sum_y, _mm256_madd_epi16(_mm256_maddubs_epi16(ones, v2), ones));
            sum_y = _mm256_add_epi32(sum_y, _mm256_madd_epi16(_mm256_maddubs_epi16(ones, v3), ones));
            __m256i b0 = _mm256_loadu_si256((const __m256i*)(x_row + i * 32));
            acc0 = _mm256_dpbusd_epi32(acc0, _mm256_and_si256(_mm256_srli_epi16(b0, 6), mask), v0);
            acc0 = _mm256_dpbusd_epi32(acc0, _mm256_and_si256(_mm256_srli_epi16(b0, 4), mask), v1);
            acc0 = _mm256_dpbusd_epi32(acc0, _mm256_and_si256(_mm256_srli_epi16(b0, 2), mask), v2);
            acc0 = _mm256_dpbusd_epi32(acc0, _mm256_and_si256(b0, mask), v3);
        }
        s[row] = (float)(hsum_i32_8(acc0) - hsum_i32_8(sum_y));
    }
}

const neuronos_backend_t neuronos_backend_x86_avxvnni = {
    .name = "x86_avxvnni",
    .type = NEURONOS_BACKEND_X86_AVXVNNI,
    .priority = 75,
    .required_features = NEURONOS_FEAT_AVX2 | NEURONOS_FEAT_AVX_VNNI,
    .config = {
        .row_block_size = 8,
        .col_block_size = 128,
        .parallel_size = 8,
        .qk_i2_s = 128,
    },
    .vec_dot_i2_i8 = avxvnni_vec_dot_i2_i8,
    .quantize_i2 = quantize_i2_s,
    .gemv_i2_i8 = NULL,
    .gemm_i2_i8 = NULL,
    .init = NULL,
    .shutdown = NULL,
};
