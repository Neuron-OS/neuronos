/**
 * @file neuronos_hal.h
 * @brief NeuronOS Hardware Abstraction Layer — Public API
 *
 * Provides a runtime-selectable backend interface for ternary (I2_S)
 * matrix operations. Backends register themselves with a vtable of
 * kernel function pointers. The HAL probes hardware capabilities at
 * init time and selects the best available backend automatically.
 *
 * Design principles:
 *   - C-compatible API (extern "C") for maximum portability
 *   - Zero-cost when a single backend is compiled in
 *   - No dynamic allocation in the hot path
 *   - Runtime ISA detection → no recompilation needed
 *   - Fallback chain: best → next-best → scalar
 *
 * @copyright MIT License
 */

#ifndef NEURONOS_HAL_H
#define NEURONOS_HAL_H

#include <stddef.h>
#include <stdint.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────── Version ──────────────────────────── */

#define NEURONOS_HAL_VERSION_MAJOR 0
#define NEURONOS_HAL_VERSION_MINOR 2
#define NEURONOS_HAL_VERSION_PATCH 0

/* ──────────────────────────── Runtime profiles ──────────────────── */

/**
 * Runtime profile determines which features are available.
 *   FULL    — Desktop/Server: mmap, threads, KV-cache, server
 *   LITE    — Browser/Mobile: fetch+buffer, web workers, limited cache
 *   MINIMAL — MCU/Embedded: static alloc, no heap, single-thread
 *
 * Selected at compile time via -DNEURONOS_RUNTIME_PROFILE=FULL|LITE|MINIMAL
 */
#define NEURONOS_PROFILE_FULL 0
#define NEURONOS_PROFILE_LITE 1
#define NEURONOS_PROFILE_MINIMAL 2

#ifndef NEURONOS_RUNTIME_PROFILE
    #if defined(__EMSCRIPTEN__)
        #define NEURONOS_RUNTIME_PROFILE NEURONOS_PROFILE_LITE
    #elif defined(NEURONOS_MCU)
        #define NEURONOS_RUNTIME_PROFILE NEURONOS_PROFILE_MINIMAL
    #else
        #define NEURONOS_RUNTIME_PROFILE NEURONOS_PROFILE_FULL
    #endif
#endif

/* ──────────────────────────── Device tier ────────────────────────── */

/**
 * Device tiers for model auto-selection and resource management.
 *   S — Server (32GB+ RAM, multi-GPU)
 *   A — Desktop/Laptop/SBC (4-32GB RAM)
 *   B — Browser/Mobile (1-4GB usable)
 *   C — IoT/Embedded (8-64MB RAM, ESP32, Pi Zero)
 *   D — MCU/Bare-metal (<1MB RAM, Cortex-M)
 */
typedef enum {
    NEURONOS_TIER_S = 0, /* Server/Data Center */
    NEURONOS_TIER_A = 1, /* Desktop/Laptop/SBC */
    NEURONOS_TIER_B = 2, /* Browser/Mobile */
    NEURONOS_TIER_C = 3, /* IoT/Embedded */
    NEURONOS_TIER_D = 4, /* MCU/Bare-metal */
} neuronos_device_tier_t;

/**
 * Detect the device tier based on available memory and platform.
 */
neuronos_device_tier_t neuronos_detect_device_tier(void);

/* ──────────────────────────── Error codes ──────────────────────── */

typedef enum {
    NEURONOS_HAL_OK = 0,
    NEURONOS_HAL_ERR_INIT = -1,        /* Backend init failed */
    NEURONOS_HAL_ERR_NO_BACKEND = -2,  /* No suitable backend found */
    NEURONOS_HAL_ERR_INVALID = -3,     /* Invalid parameter */
    NEURONOS_HAL_ERR_UNSUPPORTED = -4, /* Operation not supported by backend */
} neuronos_hal_status_t;

/* ──────────────────────────── Hardware features ─────────────────── */

typedef enum {
    /* x86 features */
    NEURONOS_FEAT_SSE3 = (1 << 0),
    NEURONOS_FEAT_SSSE3 = (1 << 1),
    NEURONOS_FEAT_AVX = (1 << 2),
    NEURONOS_FEAT_AVX2 = (1 << 3),
    NEURONOS_FEAT_AVX_VNNI = (1 << 4),
    NEURONOS_FEAT_AVX512F = (1 << 5),
    NEURONOS_FEAT_AVX512VNNI = (1 << 6),
    NEURONOS_FEAT_FMA = (1 << 7),

    /* ARM features */
    NEURONOS_FEAT_NEON = (1 << 8),
    NEURONOS_FEAT_DOTPROD = (1 << 9), /* ARMv8.2 dot product */
    NEURONOS_FEAT_SVE = (1 << 10),
    NEURONOS_FEAT_SVE2 = (1 << 11),
    NEURONOS_FEAT_I8MM = (1 << 12), /* ARMv8.6 int8 matmul */

    /* RISC-V features */
    NEURONOS_FEAT_RVV = (1 << 16), /* RISC-V Vector */

    /* WASM features */
    NEURONOS_FEAT_WASM_SIMD = (1 << 20),

    /* GPU features */
    NEURONOS_FEAT_CUDA = (1 << 24),
    NEURONOS_FEAT_VULKAN = (1 << 25),
    NEURONOS_FEAT_METAL = (1 << 26),
    NEURONOS_FEAT_OPENCL = (1 << 27),
} neuronos_feature_t;

/* ──────────────────────────── Backend type ──────────────────────── */

typedef enum {
    NEURONOS_BACKEND_SCALAR = 0,    /* Portable C fallback */
    NEURONOS_BACKEND_X86_AVX2 = 10, /* x86-64 with AVX2 */
    NEURONOS_BACKEND_X86_AVXVNNI = 11, /* x86-64 with AVX-VNNI (Alder Lake+) */
    NEURONOS_BACKEND_X86_AVX512 = 12,
    NEURONOS_BACKEND_ARM_NEON = 20, /* ARM with NEON */
    NEURONOS_BACKEND_ARM_SVE = 21,
    NEURONOS_BACKEND_WASM = 30,   /* WebAssembly SIMD */
    NEURONOS_BACKEND_CUDA = 40,   /* NVIDIA CUDA */
    NEURONOS_BACKEND_VULKAN = 41, /* Vulkan compute */
    NEURONOS_BACKEND_METAL = 42,  /* Apple Metal */
} neuronos_backend_type_t;

/* ──────────────────────────── Kernel configuration ──────────────── */

/**
 * Runtime-configurable kernel parameters.
 * Replaces the compile-time macros in gemm-config.h.
 */
typedef struct {
    int row_block_size; /* Number of weight rows per kernel invocation */
    int col_block_size; /* Number of columns processed per inner loop */
    int parallel_size;  /* SIMD parallelism factor */
    int qk_i2_s;        /* Quantization block size (128 for x86, 64 for ARM) */
} neuronos_kernel_config_t;

/* ──────────────────────────── Kernel function types ─────────────── */

/**
 * Ternary vector dot product: computes dot(weights_i2, activations_i8).
 *
 * @param n     Number of elements
 * @param s     Output: array of dot-product results
 * @param bs    Stride between output elements (bytes)
 * @param vx    Packed I2_S ternary weights
 * @param bx    Stride between weight rows (bytes)
 * @param vy    Quantized I8_S activations
 * @param by    Stride between activation rows (bytes)
 * @param nrc   Number of rows/columns to process
 */
typedef void (*neuronos_vec_dot_i2_i8_fn)(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy,
                                          size_t by, int nrc);

/**
 * Quantize f32 weights to I2_S packed ternary format.
 *
 * @param src          Source f32 weights
 * @param dst          Destination packed buffer
 * @param nrow         Number of rows
 * @param n_per_row    Elements per row
 * @param quant_weights Optional importance weights (can be NULL)
 * @return             Number of bytes written
 */
typedef size_t (*neuronos_quantize_i2_fn)(const float * src, void * dst, int64_t nrow, int64_t n_per_row,
                                          const float * quant_weights);

/**
 * GEMV: matrix-vector product for ternary weights.
 * y = W * x  where W is I2_S, x is I8_S
 */
typedef void (*neuronos_gemv_i2_i8_fn)(int n, float * s, size_t bs, const void * vx, const void * vy, int nr, int nc);

/**
 * GEMM: matrix-matrix product for ternary weights.
 * C = W * A  where W is I2_S, A is I8_S
 */
typedef void (*neuronos_gemm_i2_i8_fn)(int n, float * s, size_t bs, const void * vx, const void * vy, int nr, int nc);

/* ──────────────────────────── Backend descriptor ────────────────── */

/**
 * A backend is a named collection of kernel implementations.
 * Each backend targets a specific ISA or accelerator.
 */
typedef struct neuronos_backend {
    /* Identity */
    const char * name; /* e.g. "x86_avx2", "arm_neon", "scalar" */
    neuronos_backend_type_t type;
    int priority; /* Higher = preferred (100=GPU, 50=SIMD, 0=scalar) */

    /* Required HW features (bitmask of neuronos_feature_t) */
    uint32_t required_features;

    /* Kernel configuration for this backend */
    neuronos_kernel_config_t config;

    /* Kernel function pointers (set by backend at registration) */
    neuronos_vec_dot_i2_i8_fn vec_dot_i2_i8;
    neuronos_quantize_i2_fn quantize_i2;
    neuronos_gemv_i2_i8_fn gemv_i2_i8; /* Optional: can be NULL */
    neuronos_gemm_i2_i8_fn gemm_i2_i8; /* Optional: can be NULL */

    /* Lifecycle */
    neuronos_hal_status_t (*init)(void); /* Called once at registration */
    void (*shutdown)(void);          /* Called at neuronos_hal_shutdown() */
} neuronos_backend_t;

/* ──────────────────────────── HAL Public API ────────────────────── */

#define NEURONOS_MAX_BACKENDS 16

/**
 * Initialize the HAL: probe hardware, register built-in backends,
 * select the best one. Must be called before any kernel operations.
 *
 * @return NEURONOS_HAL_OK on success
 */
neuronos_hal_status_t neuronos_hal_init(void);

/**
 * Shutdown the HAL and all registered backends.
 */
void neuronos_hal_shutdown(void);

/**
 * Get detected hardware features (bitmask).
 */
uint32_t neuronos_hal_get_features(void);

/**
 * Register a custom backend. The HAL takes a copy of the descriptor.
 * Can be called before or after neuronos_hal_init().
 *
 * @param backend  Pointer to backend descriptor (copied internally)
 * @return NEURONOS_HAL_OK on success
 */
neuronos_hal_status_t neuronos_hal_register_backend(const neuronos_backend_t * backend);

/**
 * Get the currently active backend.
 *
 * @return Pointer to active backend, or NULL if not initialized
 */
const neuronos_backend_t * neuronos_hal_get_active_backend(void);

/**
 * Force selection of a specific backend type.
 * Returns ERR_NO_BACKEND if the requested type isn't registered
 * or its required features aren't available.
 *
 * @param type  Backend type to activate
 * @return NEURONOS_HAL_OK on success
 */
neuronos_hal_status_t neuronos_hal_select_backend(neuronos_backend_type_t type);

/**
 * Get the number of registered backends.
 */
int neuronos_hal_get_backend_count(void);

/**
 * Get a registered backend by index.
 *
 * @param index  0-based index
 * @return Pointer to backend, or NULL if index out of range
 */
const neuronos_backend_t * neuronos_hal_get_backend(int index);

/* ──────── Convenience: dispatch to active backend ──────── */

/**
 * Dispatch vec_dot to the active backend.
 * This is the hot-path function called by ggml during inference.
 */
void neuronos_vec_dot_i2_i8(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by,
                            int nrc);

/**
 * Dispatch quantize_i2 to the active backend.
 */
size_t neuronos_quantize_i2(const float * src, void * dst, int64_t nrow, int64_t n_per_row,
                            const float * quant_weights);

/**
 * Dispatch gemv to the active backend.
 */
void neuronos_gemv_i2_i8(int n, float * s, size_t bs, const void * vx, const void * vy, int nr, int nc);

/**
 * Dispatch gemm to the active backend.
 */
void neuronos_gemm_i2_i8(int n, float * s, size_t bs, const void * vx, const void * vy, int nr, int nc);

/**
 * Get the kernel config of the active backend.
 * Useful for ggml integration (e.g., setting ncols, nrows in type_traits).
 */
const neuronos_kernel_config_t * neuronos_hal_get_kernel_config(void);

/* ──────── Hardware detection utilities ──────── */

/**
 * Print detected hardware capabilities to stdout.
 * Useful for diagnostics and benchmarking.
 */
void neuronos_hal_print_info(void);

#ifdef __cplusplus
}
#endif

#endif /* NEURONOS_HAL_H */
