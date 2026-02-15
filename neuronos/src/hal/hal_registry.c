/**
 * @file hal_registry.c
 * @brief NeuronOS HAL — Backend registry and dispatch
 *
 * Manages the lifecycle of backends: registration, hardware probing,
 * automatic selection by priority, and kernel dispatch.
 */

#include "neuronos/neuronos_hal.h"

#include <stdio.h>
#include <string.h>

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

/* ──────────────────────────── Internal state ─────────────────────── */

static struct {
    neuronos_backend_t backends[NEURONOS_MAX_BACKENDS];
    int count;
    int active_index;     /* -1 = none selected */
    uint32_t hw_features; /* Detected hardware features */
    bool initialized;
} g_hal = {
    .count = 0,
    .active_index = -1,
    .hw_features = 0,
    .initialized = false,
};

/* ──────────────────────────── Hardware detection ────────────────── */

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)

    #ifdef _MSC_VER
        #include <intrin.h>
static void cpuid(int info[4], int leaf) {
    __cpuid(info, leaf);
}
static void cpuidex(int info[4], int leaf, int sub) {
    __cpuidex(info, leaf, sub);
}
    #else
        #include <cpuid.h>
static void cpuid(int info[4], int leaf) {
    __cpuid_count(leaf, 0, info[0], info[1], info[2], info[3]);
}
static void cpuidex(int info[4], int leaf, int sub) {
    __cpuid_count(leaf, sub, info[0], info[1], info[2], info[3]);
}
    #endif

static uint32_t detect_x86_features(void) {
    uint32_t features = 0;
    int info[4];

    cpuid(info, 0);
    int max_leaf = info[0];

    if (max_leaf >= 1) {
        cpuid(info, 1);
        if (info[2] & (1 << 0))
            features |= NEURONOS_FEAT_SSE3;
        if (info[2] & (1 << 9))
            features |= NEURONOS_FEAT_SSSE3;
        if (info[2] & (1 << 28))
            features |= NEURONOS_FEAT_AVX;
        if (info[2] & (1 << 12))
            features |= NEURONOS_FEAT_FMA;
    }

    if (max_leaf >= 7) {
        cpuidex(info, 7, 0);
        if (info[1] & (1 << 5))
            features |= NEURONOS_FEAT_AVX2;
        if (info[1] & (1 << 16))
            features |= NEURONOS_FEAT_AVX512F;

        /* AVX-VNNI: CPUID.7.1:EAX[4] (0x10) */
        cpuidex(info, 7, 1);
        if (info[0] & (1 << 4))
            features |= NEURONOS_FEAT_AVX_VNNI;

        /* AVX512-VNNI: CPUID.7.0:ECX[11] */
        cpuidex(info, 7, 0);
        if (info[2] & (1 << 11))
            features |= NEURONOS_FEAT_AVX512VNNI;
    }

    return features;
}

#elif defined(__aarch64__) || defined(_M_ARM64)

    #ifdef __linux__
        #include <asm/hwcap.h>
        #include <sys/auxv.h>
    #endif

static uint32_t detect_arm_features(void) {
    uint32_t features = NEURONOS_FEAT_NEON; /* Always available on AArch64 */

    #ifdef __linux__
    unsigned long hwcap = getauxval(AT_HWCAP);
    unsigned long hwcap2 = getauxval(AT_HWCAP2);

    if (hwcap & HWCAP_ASIMDDP)
        features |= NEURONOS_FEAT_DOTPROD;
    if (hwcap & HWCAP_SVE)
        features |= NEURONOS_FEAT_SVE;
        #ifdef HWCAP2_SVE2
    if (hwcap2 & HWCAP2_SVE2)
        features |= NEURONOS_FEAT_SVE2;
        #endif
        #ifdef HWCAP2_I8MM
    if (hwcap2 & HWCAP2_I8MM)
        features |= NEURONOS_FEAT_I8MM;
        #endif
    #endif /* __linux__ */

    #ifdef __APPLE__
    /* Apple Silicon always has NEON + dot product */
    features |= NEURONOS_FEAT_DOTPROD;
    #endif

    return features;
}

#else

static uint32_t detect_generic_features(void) {
    /* RISC-V, WASM, or unknown — just scalar for now */
    #if defined(__riscv_vector)
    return NEURONOS_FEAT_RVV;
    #elif defined(__wasm_simd128__)
    return NEURONOS_FEAT_WASM_SIMD;
    #else
    return 0;
    #endif
}

#endif /* arch detection */

static uint32_t detect_hardware_features(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    return detect_x86_features();
#elif defined(__aarch64__) || defined(_M_ARM64)
    return detect_arm_features();
#else
    return detect_generic_features();
#endif
}

/* ──────────── Built-in backend declarations ────────────────────── */

/* These are defined in the per-ISA source files */
extern const neuronos_backend_t neuronos_backend_scalar;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
extern const neuronos_backend_t neuronos_backend_x86_avx2;
extern const neuronos_backend_t neuronos_backend_x86_avxvnni;
#endif
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
extern const neuronos_backend_t neuronos_backend_arm_neon;
#endif

/* Vulkan GPU detection (from hal_vulkan.c) */
extern neuronos_hal_status_t neuronos_hal_vulkan_init(void);
extern void neuronos_hal_vulkan_print_info(void);


/* ──────────────────────────── HAL API implementation ────────────── */

neuronos_hal_status_t neuronos_hal_init(void) {
    if (g_hal.initialized) {
        return NEURONOS_HAL_OK;
    }

    g_hal.count = 0;
    g_hal.active_index = -1;
    g_hal.hw_features = detect_hardware_features();
    printf("[HAL] Detected features: 0x%08X\n", g_hal.hw_features);

    /* Register built-in backends */
    printf("[HAL] Registering backends...\n");
    neuronos_hal_register_backend(&neuronos_backend_scalar);

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    neuronos_hal_register_backend(&neuronos_backend_x86_avx2);
    neuronos_hal_register_backend(&neuronos_backend_x86_avxvnni);
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
    neuronos_hal_register_backend(&neuronos_backend_arm_neon);
#endif

    /* Initialize Vulkan GPU detection (independent of CPU backends) */
#ifdef NEURONOS_HAS_VULKAN
    neuronos_hal_vulkan_init();
#endif

    /* Select best backend: highest priority that satisfies feature requirements */
    int best_index = -1;
    int best_priority = -1;

    for (int i = 0; i < g_hal.count; i++) {
        const neuronos_backend_t * b = &g_hal.backends[i];

        /* Check if all required features are available */
        if ((b->required_features & g_hal.hw_features) != b->required_features) {
            continue;
        }

        if (b->priority > best_priority) {
            best_priority = b->priority;
            best_index = i;
        }
    }

    if (best_index < 0) {
        printf("[HAL] Error: No suitable backend found (features 0x%X)\n", g_hal.hw_features);
        return NEURONOS_HAL_ERR_NO_BACKEND;
    }
    printf("[HAL] Selected best backend: index %d (%s)\n", best_index, g_hal.backends[best_index].name);

    /* Initialize the selected backend */
    g_hal.active_index = best_index;
    neuronos_backend_t * active = &g_hal.backends[best_index];

    if (active->init) {
        neuronos_hal_status_t st = active->init();
        if (st != NEURONOS_HAL_OK) {
            g_hal.active_index = -1;
            return st;
        }
    }

    g_hal.initialized = true;
    return NEURONOS_HAL_OK;
}

void neuronos_hal_shutdown(void) {
    for (int i = 0; i < g_hal.count; i++) {
        if (g_hal.backends[i].shutdown) {
            g_hal.backends[i].shutdown();
        }
    }
    g_hal.count = 0;
    g_hal.active_index = -1;
    g_hal.hw_features = 0;
    g_hal.initialized = false;
}

uint32_t neuronos_hal_get_features(void) {
    return g_hal.hw_features;
}

neuronos_hal_status_t neuronos_hal_register_backend(const neuronos_backend_t * backend) {
    if (!backend || !backend->name || !backend->vec_dot_i2_i8 || !backend->quantize_i2) {
        return NEURONOS_HAL_ERR_INVALID;
    }
    if (g_hal.count >= NEURONOS_MAX_BACKENDS) {
        return NEURONOS_HAL_ERR_INVALID;
    }

    /* Copy descriptor */
    memcpy(&g_hal.backends[g_hal.count], backend, sizeof(neuronos_backend_t));
    printf("[HAL] Registered backend [%d]: %s (feat: 0x%X)\n", g_hal.count, backend->name, backend->required_features);
    g_hal.count++;

    return NEURONOS_HAL_OK;
}

const neuronos_backend_t * neuronos_hal_get_active_backend(void) {
    if (g_hal.active_index < 0)
        return NULL;
    return &g_hal.backends[g_hal.active_index];
}

neuronos_hal_status_t neuronos_hal_select_backend(neuronos_backend_type_t type) {
    for (int i = 0; i < g_hal.count; i++) {
        if (g_hal.backends[i].type == type) {
            /* Check features */
            if ((g_hal.backends[i].required_features & g_hal.hw_features) != g_hal.backends[i].required_features) {
                return NEURONOS_HAL_ERR_UNSUPPORTED;
            }

            /* Shutdown old backend */
            if (g_hal.active_index >= 0 && g_hal.backends[g_hal.active_index].shutdown) {
                g_hal.backends[g_hal.active_index].shutdown();
            }

            /* Init new backend */
            if (g_hal.backends[i].init) {
                neuronos_hal_status_t st = g_hal.backends[i].init();
                if (st != NEURONOS_HAL_OK)
                    return st;
            }

            g_hal.active_index = i;
            return NEURONOS_HAL_OK;
        }
    }
    return NEURONOS_HAL_ERR_NO_BACKEND;
}

int neuronos_hal_get_backend_count(void) {
    return g_hal.count;
}

const neuronos_backend_t * neuronos_hal_get_backend(int index) {
    if (index < 0 || index >= g_hal.count)
        return NULL;
    return &g_hal.backends[index];
}

/* ──────────── Dispatch functions (hot path) ─────────────────────── */

void neuronos_vec_dot_i2_i8(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by,
                            int nrc) {
    const neuronos_backend_t * b = neuronos_hal_get_active_backend();
    if (b && b->vec_dot_i2_i8) {
        b->vec_dot_i2_i8(n, s, bs, vx, bx, vy, by, nrc);
    }
}

size_t neuronos_quantize_i2(const float * src, void * dst, int64_t nrow, int64_t n_per_row,
                            const float * quant_weights) {
    const neuronos_backend_t * b = neuronos_hal_get_active_backend();
    if (b && b->quantize_i2) {
        return b->quantize_i2(src, dst, nrow, n_per_row, quant_weights);
    }
    return 0;
}

void neuronos_gemv_i2_i8(int n, float * s, size_t bs, const void * vx, const void * vy, int nr, int nc) {
    const neuronos_backend_t * b = neuronos_hal_get_active_backend();
    if (b && b->gemv_i2_i8) {
        b->gemv_i2_i8(n, s, bs, vx, vy, nr, nc);
    }
}

void neuronos_gemm_i2_i8(int n, float * s, size_t bs, const void * vx, const void * vy, int nr, int nc) {
    const neuronos_backend_t * b = neuronos_hal_get_active_backend();
    if (b && b->gemm_i2_i8) {
        b->gemm_i2_i8(n, s, bs, vx, vy, nr, nc);
    }
}

const neuronos_kernel_config_t * neuronos_hal_get_kernel_config(void) {
    const neuronos_backend_t * b = neuronos_hal_get_active_backend();
    if (!b)
        return NULL;
    return &b->config;
}

/* ──────────── Diagnostic output ────────────────────────────────── */

void neuronos_hal_print_info(void) {
    uint32_t f = g_hal.hw_features;

    printf("=== NeuronOS HAL v%d.%d.%d ===\n", NEURONOS_HAL_VERSION_MAJOR, NEURONOS_HAL_VERSION_MINOR,
           NEURONOS_HAL_VERSION_PATCH);

    /* Runtime profile */
    const char * profile_name = "UNKNOWN";
#if NEURONOS_RUNTIME_PROFILE == NEURONOS_PROFILE_FULL
    profile_name = "FULL";
#elif NEURONOS_RUNTIME_PROFILE == NEURONOS_PROFILE_LITE
    profile_name = "LITE";
#elif NEURONOS_RUNTIME_PROFILE == NEURONOS_PROFILE_MINIMAL
    profile_name = "MINIMAL";
#endif
    printf("Runtime profile: %s\n", profile_name);

    /* Device tier */
    neuronos_device_tier_t tier = neuronos_detect_device_tier();
    const char * tier_names[] = {"S (Server)", "A (Desktop)", "B (Browser/Mobile)", "C (IoT/Embedded)",
                                 "D (MCU/Bare-metal)"};
    printf("Device tier: %s\n", tier_names[tier]);

    printf("Hardware features:");
    if (f & NEURONOS_FEAT_SSE3)
        printf(" SSE3");
    if (f & NEURONOS_FEAT_SSSE3)
        printf(" SSSE3");
    if (f & NEURONOS_FEAT_AVX)
        printf(" AVX");
    if (f & NEURONOS_FEAT_AVX2)
        printf(" AVX2");
    if (f & NEURONOS_FEAT_AVX_VNNI)
        printf(" AVX-VNNI");
    if (f & NEURONOS_FEAT_AVX512F)
        printf(" AVX512F");
    if (f & NEURONOS_FEAT_AVX512VNNI)
        printf(" AVX512-VNNI");
    if (f & NEURONOS_FEAT_FMA)
        printf(" FMA");
    if (f & NEURONOS_FEAT_NEON)
        printf(" NEON");
    if (f & NEURONOS_FEAT_DOTPROD)
        printf(" DOTPROD");
    if (f & NEURONOS_FEAT_SVE)
        printf(" SVE");
    if (f & NEURONOS_FEAT_SVE2)
        printf(" SVE2");
    if (f & NEURONOS_FEAT_I8MM)
        printf(" I8MM");
    if (f & NEURONOS_FEAT_RVV)
        printf(" RVV");
    if (f & NEURONOS_FEAT_WASM_SIMD)
        printf(" WASM-SIMD");
    if (f == 0)
        printf(" (none)");
    printf("\n");

    printf("Registered backends: %d\n", g_hal.count);
    for (int i = 0; i < g_hal.count; i++) {
        const neuronos_backend_t * b = &g_hal.backends[i];
        bool is_active = (i == g_hal.active_index);
        bool feasible = (b->required_features & f) == b->required_features;
        printf("  [%d] %-16s  priority=%3d  feasible=%s%s\n", i, b->name, b->priority, feasible ? "yes" : "no ",
               is_active ? "  ← ACTIVE" : "");
    }

    const neuronos_backend_t * active = neuronos_hal_get_active_backend();
    if (active) {
        printf("Active backend: %s\n", active->name);
        printf("  row_block=%d  col_block=%d  parallel=%d  qk=%d\n", active->config.row_block_size,
               active->config.col_block_size, active->config.parallel_size, active->config.qk_i2_s);
    } else {
        printf("Active backend: NONE\n");
    }

    /* Vulkan GPU info (if compiled with Vulkan support) */
    printf("\n");
    neuronos_hal_vulkan_print_info();
}

/* ──────────────── Device tier detection ──────────────── */

neuronos_device_tier_t neuronos_detect_device_tier(void) {
#if NEURONOS_RUNTIME_PROFILE == NEURONOS_PROFILE_MINIMAL
    return NEURONOS_TIER_D;
#elif defined(__EMSCRIPTEN__)
    return NEURONOS_TIER_B;
#else
    /* Detect based on available RAM */
    size_t total_ram_mb = 0;

    #if defined(__linux__)
    FILE * meminfo = fopen("/proc/meminfo", "r");
    if (meminfo) {
        char line[256];
        while (fgets(line, sizeof(line), meminfo)) {
            unsigned long val;
            if (sscanf(line, "MemTotal: %lu kB", &val) == 1) {
                total_ram_mb = val / 1024;
                break;
            }
        }
        fclose(meminfo);
    }
    #elif defined(__APPLE__)
    /* macOS: sysctl hw.memsize */
    size_t len = sizeof(total_ram_mb);
    uint64_t memsize = 0;
    len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0) {
        total_ram_mb = (size_t)(memsize / (1024 * 1024));
    }
    #elif defined(_WIN32)
    /* Windows: GlobalMemoryStatusEx */
    /* Simplified; actual impl would use Windows API */
    total_ram_mb = 8192; /* Assume 8GB if we can't detect */
    #endif

    if (total_ram_mb == 0) {
        return NEURONOS_TIER_A; /* Default assumption */
    } else if (total_ram_mb >= 32768) {
        return NEURONOS_TIER_S; /* 32GB+ = Server */
    } else if (total_ram_mb >= 2048) {
        return NEURONOS_TIER_A; /* 2GB+ = Desktop */
    } else if (total_ram_mb >= 64) {
        return NEURONOS_TIER_C; /* 64MB-2GB = IoT */
    } else {
        return NEURONOS_TIER_D; /* <64MB = MCU */
    }
#endif
}
