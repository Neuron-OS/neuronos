/* ============================================================
 * NeuronOS — Hardware Detection & Model Auto-Selection
 *
 * Phase 2D: Detect hardware → scan models → score → select best.
 *
 * Inspired by:
 *   - Claude Code: "do the simple thing first"
 *   - OpenClaw: minimal architecture (triggering + state + glue)
 *   - ArXiv 2601.01743: budgeted, tool-augmented systems
 *
 * Algorithm:
 *   score = fits_in_ram * 1000
 *         + quality_tier(params) * 100
 *         + speed_estimate * 10
 *         + format_bonus * 5
 * ============================================================ */
#include "neuronos/neuronos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#ifdef __linux__
    #include <dirent.h>
    #include <sys/sysinfo.h>
    #include <unistd.h>
#elif defined(__APPLE__)
    #include <dirent.h>
    #include <sys/sysctl.h>
    #include <unistd.h>
#elif defined(_WIN32)
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #define popen _popen
    #define pclose _pclose
#else
    #include <dirent.h>
    #include <unistd.h>
#endif

/* ============================================================
 * HARDWARE DETECTION
 * ============================================================ */

/* Read a line from a file, return 0 on success */
static int read_proc_line(const char * path, const char * key, char * buf, size_t buflen) {
    FILE * fp = fopen(path, "r");
    if (!fp)
        return -1;

    char line[512];
    while (fgets(line, (int)sizeof(line), fp)) {
        if (strstr(line, key)) {
            /* Extract value after ':' */
            const char * val = strchr(line, ':');
            if (val) {
                val++;
                while (*val == ' ' || *val == '\t')
                    val++;
                size_t len = strlen(val);
                while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r'))
                    len--;
                if (len >= buflen)
                    len = buflen - 1;
                memcpy(buf, val, len);
                buf[len] = '\0';
                fclose(fp);
                return 0;
            }
        }
    }
    fclose(fp);
    return -1;
}

static int64_t read_meminfo_kb(const char * key) {
    char val[64] = {0};
    if (read_proc_line("/proc/meminfo", key, val, sizeof(val)) == 0) {
        /* Value is in kB, e.g. "16384000 kB" */
        return atoll(val);
    }
    return 0;
}

neuronos_hw_info_t neuronos_detect_hardware(void) {
    neuronos_hw_info_t hw = {0};

    /* ---- CPU name ---- */
#ifdef __linux__
    if (read_proc_line("/proc/cpuinfo", "model name", hw.cpu_name, sizeof(hw.cpu_name)) != 0) {
        /* ARM or other */
        if (read_proc_line("/proc/cpuinfo", "Hardware", hw.cpu_name, sizeof(hw.cpu_name)) != 0) {
            snprintf(hw.cpu_name, sizeof(hw.cpu_name), "Unknown CPU");
        }
    }
#elif defined(__APPLE__)
    size_t len = sizeof(hw.cpu_name);
    sysctlbyname("machdep.cpu.brand_string", hw.cpu_name, &len, NULL, 0);
#else
    snprintf(hw.cpu_name, sizeof(hw.cpu_name), "Unknown CPU");
#endif

    /* ---- Architecture ---- */
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    snprintf(hw.arch, sizeof(hw.arch), "x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    snprintf(hw.arch, sizeof(hw.arch), "aarch64");
#elif defined(__riscv) && (__riscv_xlen == 64)
    snprintf(hw.arch, sizeof(hw.arch), "riscv64");
#elif defined(__EMSCRIPTEN__)
    snprintf(hw.arch, sizeof(hw.arch), "wasm");
#elif defined(__arm__)
    snprintf(hw.arch, sizeof(hw.arch), "arm32");
#else
    snprintf(hw.arch, sizeof(hw.arch), "unknown");
#endif

    /* ---- Cores ---- */
#ifdef _SC_NPROCESSORS_ONLN
    hw.n_cores_logical = (int)sysconf(_SC_NPROCESSORS_ONLN);
#else
    hw.n_cores_logical = 4;
#endif
    /* Heuristic: assume ~60% are physical on hybrid CPUs */
    hw.n_cores_physical = hw.n_cores_logical > 8 ? (int)(hw.n_cores_logical * 0.6) : hw.n_cores_logical;

    /* ---- RAM ---- */
#ifdef __linux__
    hw.ram_total_mb = read_meminfo_kb("MemTotal") / 1024;
    hw.ram_available_mb = read_meminfo_kb("MemAvailable") / 1024;
    if (hw.ram_available_mb <= 0) {
        /* Fallback: free + buffers + cached */
        hw.ram_available_mb =
            (read_meminfo_kb("MemFree") + read_meminfo_kb("Buffers") + read_meminfo_kb("Cached")) / 1024;
    }
#elif defined(__APPLE__)
    {
        int64_t memsize = 0;
        size_t len = sizeof(memsize);
        sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
        hw.ram_total_mb = memsize / (1024 * 1024);
        /* macOS: estimate available as 60% of total */
        hw.ram_available_mb = hw.ram_total_mb * 60 / 100;
    }
#elif defined(_WIN32)
    {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        hw.ram_total_mb = (int64_t)(ms.ullTotalPhys / (1024 * 1024));
        hw.ram_available_mb = (int64_t)(ms.ullAvailPhys / (1024 * 1024));
    }
#else
    /* POSIX fallback */
    {
        long pages = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && page_size > 0) {
            hw.ram_total_mb = (int64_t)(pages * page_size / (1024 * 1024));
        } else {
            hw.ram_total_mb = 2048; /* assume 2GB */
        }
        hw.ram_available_mb = hw.ram_total_mb * 50 / 100;
    }
#endif

    /* ---- Model budget: available RAM - 500MB safety margin ---- */
    hw.model_budget_mb = hw.ram_available_mb - 500;
    if (hw.model_budget_mb < 256) {
        hw.model_budget_mb = 256; /* minimum */
    }

    /* ---- CPU features from HAL ---- */
    /* We'll detect inline instead of depending on HAL init */
#if defined(__x86_64__) || defined(_M_X64)
    hw.features = 0;
    /* Use cpuid if available */
    #ifdef __GNUC__
    {
        unsigned int eax, ebx, ecx, edx;
        __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        if (ecx & (1 << 0))
            hw.features |= (1 << 0); /* SSE3 */
        if (ecx & (1 << 9))
            hw.features |= (1 << 1); /* SSSE3 */
        if (ecx & (1 << 28))
            hw.features |= (1 << 2); /* AVX */

        __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
        if (ebx & (1 << 5))
            hw.features |= (1 << 3); /* AVX2 */
        if (ebx & (1 << 16))
            hw.features |= (1 << 5); /* AVX512F */
    }
    #endif
#elif defined(__aarch64__)
    hw.features = (1 << 8); /* NEON is always available on aarch64 */
#endif

    /* ---- GPU Detection ---- */
    /* Try multiple methods to detect GPU and VRAM */
    hw.gpu_vram_mb = 0;
    hw.gpu_name[0] = '\0';

#ifdef __linux__
    /* Method 1: NVIDIA GPU via nvidia-smi */
    {
        FILE * fp = popen("nvidia-smi --query-gpu=name,memory.total "
                          "--format=csv,noheader,nounits 2>/dev/null",
                          "r");
        if (fp) {
            char line[256] = {0};
            if (fgets(line, sizeof(line), fp)) {
                /* Format: "NVIDIA GeForce RTX 3050 Laptop GPU, 4096" */
                char * comma = strchr(line, ',');
                if (comma) {
                    *comma = '\0';
                    snprintf(hw.gpu_name, sizeof(hw.gpu_name), "%s", line);
                    /* Trim trailing whitespace from name */
                    size_t nlen = strlen(hw.gpu_name);
                    while (nlen > 0 && (hw.gpu_name[nlen - 1] == ' ' || hw.gpu_name[nlen - 1] == '\n'))
                        hw.gpu_name[--nlen] = '\0';
                    hw.gpu_vram_mb = atoll(comma + 1);
                }
            }
            pclose(fp);
        }
    }

    /* Method 2: AMD GPU via sysfs (if NVIDIA not found) */
    if (hw.gpu_vram_mb == 0) {
        /* Check /sys/class/drm/cardN/device/ for AMD GPU */
        DIR * drm_dir = opendir("/sys/class/drm");
        if (drm_dir) {
            struct dirent * de;
            while ((de = readdir(drm_dir)) != NULL) {
                if (strncmp(de->d_name, "card", 4) != 0)
                    continue;
                /* Skip render nodes */
                if (strchr(de->d_name, '-'))
                    continue;

                char vram_path[512];
                snprintf(vram_path, sizeof(vram_path), "/sys/class/drm/%s/device/mem_info_vram_total", de->d_name);
                FILE * fp = fopen(vram_path, "r");
                if (fp) {
                    char val[64] = {0};
                    if (fgets(val, sizeof(val), fp)) {
                        int64_t vram_bytes = atoll(val);
                        hw.gpu_vram_mb = vram_bytes / (1024 * 1024);
                    }
                    fclose(fp);

                    /* Try to get GPU name from product file */
                    char name_path[512];
                    snprintf(name_path, sizeof(name_path), "/sys/class/drm/%s/device/product_name", de->d_name);
                    FILE * nfp = fopen(name_path, "r");
                    if (nfp) {
                        if (fgets(hw.gpu_name, (int)sizeof(hw.gpu_name), nfp)) {
                            size_t nlen = strlen(hw.gpu_name);
                            while (nlen > 0 && (hw.gpu_name[nlen - 1] == '\n' || hw.gpu_name[nlen - 1] == ' '))
                                hw.gpu_name[--nlen] = '\0';
                        }
                        fclose(nfp);
                    }
                    if (hw.gpu_vram_mb > 0)
                        break; /* Found a GPU with VRAM */
                }
            }
            closedir(drm_dir);
        }
    }

    /* Method 3: Intel GPU via lspci (if no dedicated GPU)  */
    if (hw.gpu_vram_mb == 0) {
        FILE * fp = popen("lspci 2>/dev/null | grep -i 'vga\\|3d\\|display' | head -1", "r");
        if (fp) {
            char line[256] = {0};
            if (fgets(line, sizeof(line), fp)) {
                /* Just store the name, no VRAM info available for integrated */
                char * colon = strrchr(line, ':');
                if (colon) {
                    colon++;
                    while (*colon == ' ')
                        colon++;
                    size_t nlen = strlen(colon);
                    while (nlen > 0 && (colon[nlen - 1] == '\n' || colon[nlen - 1] == ' '))
                        nlen--;
                    if (nlen >= sizeof(hw.gpu_name))
                        nlen = sizeof(hw.gpu_name) - 1;
                    memcpy(hw.gpu_name, colon, nlen);
                    hw.gpu_name[nlen] = '\0';
                    /* Integrated GPUs share system RAM — estimate 25% */
                    /* hw.gpu_vram_mb stays 0 (no dedicated VRAM) */
                }
            }
            pclose(fp);
        }
    }
#elif defined(__APPLE__)
    /* macOS: Metal GPU detection via system_profiler */
    {
        FILE * fp = popen("system_profiler SPDisplaysDataType 2>/dev/null "
                          "| grep -A2 'Chipset\\|VRAM' | head -6",
                          "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "Chipset") || strstr(line, "Chip")) {
                    char * val = strchr(line, ':');
                    if (val) {
                        val++;
                        while (*val == ' ')
                            val++;
                        size_t nlen = strlen(val);
                        while (nlen > 0 && (val[nlen - 1] == '\n' || val[nlen - 1] == ' '))
                            nlen--;
                        if (nlen >= sizeof(hw.gpu_name))
                            nlen = sizeof(hw.gpu_name) - 1;
                        memcpy(hw.gpu_name, val, nlen);
                        hw.gpu_name[nlen] = '\0';
                    }
                }
                if (strstr(line, "VRAM") || strstr(line, "Memory")) {
                    char * val = strchr(line, ':');
                    if (val) {
                        hw.gpu_vram_mb = atoll(val + 1);
                        /* Could be in GB */
                        if (hw.gpu_vram_mb < 64)
                            hw.gpu_vram_mb *= 1024;
                    }
                }
            }
            pclose(fp);
        }
    }
#elif defined(_WIN32)
    /* Windows: try wmic */
    {
        FILE * fp = _popen("wmic path win32_VideoController get Name,AdapterRAM /format:csv 2>nul", "r");
        if (fp) {
            char line[512];
            /* Skip header */
            fgets(line, sizeof(line), fp);
            fgets(line, sizeof(line), fp);
            if (fgets(line, sizeof(line), fp)) {
                /* CSV: Node,AdapterRAM,Name */
                char * tok = strtok(line, ",");
                tok = strtok(NULL, ","); /* AdapterRAM */
                if (tok) {
                    int64_t adapter_ram = atoll(tok);
                    hw.gpu_vram_mb = adapter_ram / (1024 * 1024);
                }
                tok = strtok(NULL, ","); /* Name */
                if (tok) {
                    snprintf(hw.gpu_name, sizeof(hw.gpu_name), "%s", tok);
                    size_t nlen = strlen(hw.gpu_name);
                    while (nlen > 0 && (hw.gpu_name[nlen - 1] == '\n' || hw.gpu_name[nlen - 1] == '\r'))
                        hw.gpu_name[--nlen] = '\0';
                }
            }
            _pclose(fp);
        }
    }
#endif

    return hw;
}

void neuronos_hw_print_info(const neuronos_hw_info_t * hw) {
    if (!hw)
        return;
    fprintf(stderr, "╔══════════════════════════════════════════╗\n");
    fprintf(stderr, "║  NeuronOS Hardware Detection v%s     ║\n", NEURONOS_VERSION_STRING);
    fprintf(stderr, "╠══════════════════════════════════════════╣\n");
    fprintf(stderr, "║  CPU:    %-32s║\n", hw->cpu_name);
    fprintf(stderr, "║  Arch:   %-32s║\n", hw->arch);
    fprintf(stderr, "║  Cores:  %d physical / %d logical        ║\n", hw->n_cores_physical, hw->n_cores_logical);
    fprintf(stderr, "║  RAM:    %lld MB total / %lld MB available ║\n", (long long)hw->ram_total_mb,
            (long long)hw->ram_available_mb);
    fprintf(stderr, "║  Budget: %lld MB for models               ║\n", (long long)hw->model_budget_mb);
    if (hw->gpu_vram_mb > 0) {
        fprintf(stderr, "║  GPU:    %s (%lld MB) ║\n", hw->gpu_name, (long long)hw->gpu_vram_mb);
    } else {
        fprintf(stderr, "║  GPU:    None detected (CPU-only)        ║\n");
    }
    fprintf(stderr, "║  Features: 0x%08X                     ║\n", hw->features);
    fprintf(stderr, "╚══════════════════════════════════════════╝\n");
}

/* ============================================================
 * MODEL SCANNER
 * ============================================================ */

/* Maximum models we'll scan */
#define MAX_SCAN_MODELS 128

/* Get file size in MB */
static int64_t file_size_mb(const char * path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return (int64_t)(st.st_size / (1024 * 1024));
    }
    return 0;
}

/* Extract model name from file path (just the filename without .gguf) */
static void extract_model_name(const char * path, char * name, size_t name_len) {
    const char * base = strrchr(path, '/');
    if (!base)
        base = strrchr(path, '\\');
    base = base ? base + 1 : path;

    size_t len = strlen(base);
    /* Remove .gguf extension */
    if (len > 5 && strcmp(base + len - 5, ".gguf") == 0) {
        len -= 5;
    }
    if (len >= name_len)
        len = name_len - 1;
    memcpy(name, base, len);
    name[len] = '\0';
}

/* Estimate RAM needed: file size + ~30% overhead for context/KV cache */
static int64_t estimate_ram_needed(int64_t file_size_mb_val) {
    return file_size_mb_val + (file_size_mb_val * 30 / 100) + 100; /* +100MB for context */
}

/* Detect quantization type from filename */
static neuronos_quant_type_t detect_quant_type(const char * name) {
    if (strstr(name, "i2_s") || strstr(name, "I2_S"))
        return NEURONOS_QUANT_I2_S;
    if (strstr(name, "tl1") || strstr(name, "TL1"))
        return NEURONOS_QUANT_TL1;
    /* Check k-quants (case insensitive matching via common patterns) */
    if (strstr(name, "Q8_0") || strstr(name, "q8_0"))
        return NEURONOS_QUANT_Q8_0;
    if (strstr(name, "Q6_K") || strstr(name, "q6_k"))
        return NEURONOS_QUANT_Q6_K;
    if (strstr(name, "Q5_K_M") || strstr(name, "q5_k_m") || strstr(name, "Q5_K") || strstr(name, "q5_k"))
        return NEURONOS_QUANT_Q5_K_M;
    if (strstr(name, "Q4_K_M") || strstr(name, "q4_k_m") || strstr(name, "Q4_K") || strstr(name, "q4_k"))
        return NEURONOS_QUANT_Q4_K_M;
    if (strstr(name, "Q4_0") || strstr(name, "q4_0"))
        return NEURONOS_QUANT_Q4_0;
    if (strstr(name, "Q3_K") || strstr(name, "q3_k"))
        return NEURONOS_QUANT_Q3_K;
    if (strstr(name, "Q2_K") || strstr(name, "q2_k"))
        return NEURONOS_QUANT_Q2_K;
    if (strstr(name, "f16") || strstr(name, "F16") || strstr(name, "fp16"))
        return NEURONOS_QUANT_F16;
    /* Ternary by name convention */
    if (strstr(name, "1.58") || strstr(name, "bitnet") || strstr(name, "BitNet") || strstr(name, "ternary"))
        return NEURONOS_QUANT_I2_S;
    return NEURONOS_QUANT_UNKNOWN;
}

/* Bytes per parameter for each quantization type (approximate) */
static float bytes_per_param(neuronos_quant_type_t qt) {
    switch (qt) {
        case NEURONOS_QUANT_I2_S:   return 0.35f;  /* ~2 bpw + overhead */
        case NEURONOS_QUANT_TL1:    return 0.35f;
        case NEURONOS_QUANT_Q2_K:   return 0.40f;  /* ~2.6 bpw */
        case NEURONOS_QUANT_Q3_K:   return 0.50f;  /* ~3.4 bpw */
        case NEURONOS_QUANT_Q4_0:   return 0.56f;  /* ~4.5 bpw */
        case NEURONOS_QUANT_Q4_K_M: return 0.62f;  /* ~4.9 bpw */
        case NEURONOS_QUANT_Q5_K_M: return 0.72f;  /* ~5.7 bpw */
        case NEURONOS_QUANT_Q6_K:   return 0.82f;  /* ~6.6 bpw */
        case NEURONOS_QUANT_Q8_0:   return 1.10f;  /* ~8.5 bpw */
        case NEURONOS_QUANT_F16:    return 2.00f;  /* 16 bpw */
        default:                    return 0.62f;  /* safe Q4 estimate */
    }
}

/* Estimate parameters from file size using detected quant type */
static int64_t estimate_params_from_quant(int64_t file_size_mb_val, neuronos_quant_type_t qt) {
    float bpp = bytes_per_param(qt);
    /* file_size_bytes / bytes_per_param = total params */
    return (int64_t)((double)(file_size_mb_val) * 1024.0 * 1024.0 / (double)bpp);
}

/* Legacy wrapper */
static int64_t estimate_params(int64_t file_size_mb_val) {
    return estimate_params_from_quant(file_size_mb_val, NEURONOS_QUANT_I2_S);
}

/* Score a model based on hardware fit */
static float score_model(const neuronos_model_entry_t * entry, const neuronos_hw_info_t * hw) {
    float score = 0.0f;

    /* Hard constraint: must fit in RAM */
    if (entry->est_ram_mb > hw->model_budget_mb) {
        return -1.0f; /* doesn't fit */
    }

    /* Fits in RAM: huge bonus */
    score += 1000.0f;

    /* Quality tier: prefer larger models (more params = smarter) */
    /* Scale: 0-500M=10, 500M-2B=30, 2B-4B=60, 4B-8B=80, 8B+=100 */
    int64_t params_b = entry->n_params_est / 1000000000LL;
    if (params_b >= 8)
        score += 100.0f;
    else if (params_b >= 4)
        score += 80.0f;
    else if (params_b >= 2)
        score += 60.0f;
    else if (params_b >= 1)
        score += 30.0f;
    else
        score += 10.0f;

    /* Speed estimate: smaller models are faster */
    /* Inverse relationship: budget_headroom → more speed */
    float headroom = (float)(hw->model_budget_mb - entry->est_ram_mb) / (float)hw->model_budget_mb;
    score += headroom * 50.0f;

    /* Format bonus: ternary models get speed bonus, higher quants get quality bonus */
    if (entry->is_ternary) {
        score += 25.0f;  /* Ternary: 2-3x faster on CPU */
    } else {
        /* Higher quality quantization bonus (Q8 > Q6 > Q5 > Q4 > Q3 > Q2) */
        switch (entry->quant) {
            case NEURONOS_QUANT_Q8_0:   score += 20.0f; break;
            case NEURONOS_QUANT_Q6_K:   score += 18.0f; break;
            case NEURONOS_QUANT_Q5_K_M: score += 16.0f; break;
            case NEURONOS_QUANT_Q4_K_M: score += 14.0f; break;
            case NEURONOS_QUANT_Q4_0:   score += 12.0f; break;
            case NEURONOS_QUANT_Q3_K:   score += 8.0f;  break;
            case NEURONOS_QUANT_Q2_K:   score += 5.0f;  break;
            case NEURONOS_QUANT_F16:    score += 22.0f; break;
            default:                    score += 10.0f; break;
        }
    }

    /* Instruct model bonus (better for agents) */
    if (strstr(entry->name, "nstruct") || strstr(entry->name, "chat") || strstr(entry->name, "Chat")) {
        score += 15.0f;
    }

    return score;
}

/* Recursive directory walker for .gguf files */
static int scan_dir_recursive(const char * dir_path, const neuronos_hw_info_t * hw, neuronos_model_entry_t * entries,
                              int max_entries, int current_count) {
#ifdef _WIN32
    char search_path[1024];
    snprintf(search_path, sizeof(search_path), "%s\\*", dir_path);

    WIN32_FIND_DATAA fdata;
    HANDLE hFind = FindFirstFileA(search_path, &fdata);
    if (hFind == INVALID_HANDLE_VALUE)
        return current_count;

    do {
        if (fdata.cFileName[0] == '.')
            continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s\\%s", dir_path, fdata.cFileName);

        if (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            current_count = scan_dir_recursive(full_path, hw, entries, max_entries, current_count);
        } else if (current_count < max_entries) {
            size_t name_len = strlen(fdata.cFileName);
            if (name_len > 5 && strcmp(fdata.cFileName + name_len - 5, ".gguf") == 0) {
                neuronos_model_entry_t * e = &entries[current_count];
                snprintf(e->path, sizeof(e->path), "%s", full_path);
                extract_model_name(full_path, e->name, sizeof(e->name));
                e->file_size_mb = file_size_mb(full_path);
                e->est_ram_mb = estimate_ram_needed(e->file_size_mb);
                e->quant = detect_quant_type(e->name);
                e->is_ternary = (e->quant == NEURONOS_QUANT_I2_S || e->quant == NEURONOS_QUANT_TL1);
                e->n_params_est = estimate_params_from_quant(e->file_size_mb, e->quant);
                e->fits_in_ram = (e->est_ram_mb <= hw->model_budget_mb);
                e->score = score_model(e, hw);
                current_count++;
            }
        }
    } while (FindNextFileA(hFind, &fdata) && current_count < max_entries);
    FindClose(hFind);
#else
    DIR * dir = opendir(dir_path);
    if (!dir)
        return current_count;

    struct dirent * ent;
    while ((ent = readdir(dir)) != NULL && current_count < max_entries) {
        /* Skip . and .. */
        if (ent->d_name[0] == '.')
            continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            current_count = scan_dir_recursive(full_path, hw, entries, max_entries, current_count);
        } else if (S_ISREG(st.st_mode)) {
            /* Check for .gguf extension */
            size_t name_len = strlen(ent->d_name);
            if (name_len > 5 && strcmp(ent->d_name + name_len - 5, ".gguf") == 0) {
                neuronos_model_entry_t * e = &entries[current_count];

                snprintf(e->path, sizeof(e->path), "%s", full_path);
                extract_model_name(full_path, e->name, sizeof(e->name));
                e->file_size_mb = file_size_mb(full_path);
                e->est_ram_mb = estimate_ram_needed(e->file_size_mb);
                e->quant = detect_quant_type(e->name);
                e->is_ternary = (e->quant == NEURONOS_QUANT_I2_S || e->quant == NEURONOS_QUANT_TL1);
                e->n_params_est = estimate_params_from_quant(e->file_size_mb, e->quant);
                e->fits_in_ram = (e->est_ram_mb <= hw->model_budget_mb);
                e->score = score_model(e, hw);

                current_count++;
            }
        }
    }

    closedir(dir);
#endif
    return current_count;
}

/* Comparison function for qsort: descending by score */
static int compare_models_desc(const void * a, const void * b) {
    const neuronos_model_entry_t * ma = (const neuronos_model_entry_t *)a;
    const neuronos_model_entry_t * mb = (const neuronos_model_entry_t *)b;
    if (mb->score > ma->score)
        return 1;
    if (mb->score < ma->score)
        return -1;
    return 0;
}

neuronos_model_entry_t * neuronos_model_scan(const char * dir_path, const neuronos_hw_info_t * hw, int * out_count) {
    if (!dir_path || !hw || !out_count)
        return NULL;

    /* Allocate temporary buffer */
    neuronos_model_entry_t * entries = calloc(MAX_SCAN_MODELS, sizeof(neuronos_model_entry_t));
    if (!entries)
        return NULL;

    int count = scan_dir_recursive(dir_path, hw, entries, MAX_SCAN_MODELS, 0);

    if (count == 0) {
        free(entries);
        *out_count = 0;
        return NULL;
    }

    /* Sort by score descending (best first) */
    qsort(entries, (size_t)count, sizeof(neuronos_model_entry_t), compare_models_desc);

    *out_count = count;
    return entries;
}

void neuronos_model_scan_free(neuronos_model_entry_t * entries, int count) {
    (void)count;
    free(entries);
}

const neuronos_model_entry_t * neuronos_model_select_best(const neuronos_model_entry_t * entries, int count) {
    if (!entries || count == 0)
        return NULL;

    /* Already sorted by score desc, return first that fits */
    for (int i = 0; i < count; i++) {
        if (entries[i].fits_in_ram && entries[i].score > 0.0f) {
            return &entries[i];
        }
    }

    return NULL;
}

/* ============================================================
 * CONTEXT TRACKING
 *
 * Implementations moved to neuronos_agent.c where the agent
 * struct internals are accessible.
 * ============================================================ */

/* ============================================================
 * AUTO-TUNING ENGINE
 *
 * Computes optimal inference parameters for max performance.
 * Rules derived from llama.cpp benchmarks + community knowledge:
 *   - n_threads = physical cores (HT adds 0% for matmul)
 *   - n_batch scales with available RAM
 *   - n_ctx maximized within free memory budget
 *   - mmap always, mlock when headroom available
 * ============================================================ */

neuronos_tuned_params_t neuronos_auto_tune(const neuronos_hw_info_t * hw, const neuronos_model_entry_t * model) {
    neuronos_tuned_params_t t = {0};

    /* Threads: physical cores only (HT hurts matmul throughput) */
    t.n_threads = hw->n_cores_physical;
    if (t.n_threads <= 0)
        t.n_threads = 4;

    /* Batch size: scales with available RAM
     * ≤4GB: 512, ≤16GB: 1024, >16GB: 2048 */
    if (hw->ram_available_mb <= 4096)
        t.n_batch = 512;
    else if (hw->ram_available_mb <= 16384)
        t.n_batch = 1024;
    else
        t.n_batch = 2048;

    /* Context size: max tokens we can afford after model is loaded
     * KV cache ≈ 2 × n_layers × d_model × sizeof(f16) × n_ctx / 1024²
     * Rough estimate: 1 token ≈ 0.1MB for a 2B model */
    int64_t free_after_model = hw->model_budget_mb - model->est_ram_mb;
    if (free_after_model < 256)
        free_after_model = 256;

    /* Heuristic: each 1K context costs ~75MB for a 2B model */
    int ctx_capacity = (int)(free_after_model * 1024 / 75);
    if (ctx_capacity > 8192)
        ctx_capacity = 8192; /* cap at 8K for now */
    if (ctx_capacity < 512)
        ctx_capacity = 512;
    /* Round to nearest 512 */
    t.n_ctx = (ctx_capacity / 512) * 512;

    /* Flash attention: enable if we're on a capable build */
    t.flash_attn = false; /* TODO: detect from llama.cpp build flags */

    /* mmap: always true (lazy page loading, reduces RSS) */
    t.use_mmap = true;

    /* mlock: lock model in RAM if we have 2× headroom
     * Prevents OS from swapping model pages during inference */
    t.use_mlock = (hw->ram_available_mb > model->est_ram_mb * 2 + 1024);

    /* GPU layers: offload to GPU if available and model supports it.
     * BitNet I2_S ternary models use CPU-only MAD kernels — GPU offload
     * would bypass the ternary GEMM and fall back to slow dequant path.
     * Only offload non-ternary (standard GGUF Q4/Q8/F16) models. */
    t.n_gpu_layers = 0;
    bool is_ternary = model->is_ternary;
    if (hw->gpu_vram_mb > 0 && !is_ternary) {
        /* Estimate: each layer ≈ model_size / n_layers
         * Try to offload all if VRAM fits */
        int64_t est_model_vram = model->file_size_mb + 256; /* file + overhead */
        if (hw->gpu_vram_mb >= est_model_vram) {
            t.n_gpu_layers = 999; /* all layers */
        } else {
            /* Partial offload: proportion that fits */
            t.n_gpu_layers = (int)(30 * hw->gpu_vram_mb / est_model_vram);
        }
    } else if (hw->gpu_vram_mb > 0 && is_ternary) {
        /* Ternary model: CPU-only inference (MAD kernel).
         * GPU VRAM noted for future when BitNet adds GPU kernels. */
        t.n_gpu_layers = 0;
    }

    return t;
}

void neuronos_tune_print(const neuronos_tuned_params_t * params) {
    if (!params)
        return;
    fprintf(stderr, "╔══════════════════════════════════════════╗\n");
    fprintf(stderr, "║  NeuronOS Auto-Tuning                    ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════╣\n");
    fprintf(stderr, "║  Threads:     %-4d (physical cores only)  ║\n", params->n_threads);
    fprintf(stderr, "║  Batch size:  %-4d                        ║\n", params->n_batch);
    fprintf(stderr, "║  Context:     %-4d tokens                 ║\n", params->n_ctx);
    fprintf(stderr, "║  Flash attn:  %-3s                         ║\n", params->flash_attn ? "yes" : "no");
    fprintf(stderr, "║  Memory map:  %-3s                         ║\n", params->use_mmap ? "yes" : "no");
    fprintf(stderr, "║  Memory lock: %-3s                         ║\n", params->use_mlock ? "yes" : "no");
    fprintf(stderr, "║  GPU layers:  %-4d                        ║\n", params->n_gpu_layers);
    fprintf(stderr, "╚══════════════════════════════════════════╝\n");
}

/* ============================================================
 * ZERO-ARG LAUNCHER
 *
 * The entire auto-config pipeline in one call:
 *   detect → scan → select → tune → load → ready
 * ============================================================ */

/* Default model search paths (all non-NULL; home dir added at runtime) */
static const char * default_search_paths[] = {
    "./models",
    "../../models",              /* relative to build dir */
    "./neuronos/models",         /* relative to workspace root */
    "/usr/share/neuronos/models",
    "/usr/local/share/neuronos/models",
    NULL, /* sentinel */
};

neuronos_auto_ctx_t neuronos_auto_launch(const char ** extra_model_dirs, bool verbose) {
    neuronos_auto_ctx_t ctx = {0};

    /* Step 1: Detect hardware */
    ctx.hw = neuronos_detect_hardware();
    if (verbose)
        neuronos_hw_print_info(&ctx.hw);

    /* Step 2: Build search path list */
    const char * search_paths[16] = {0};
    int sp = 0;

    /* Add extra dirs first (highest priority) */
    if (extra_model_dirs) {
        for (int i = 0; extra_model_dirs[i] && sp < 14; i++) {
            search_paths[sp++] = extra_model_dirs[i];
        }
    }

    /* Add $HOME/.neuronos/models FIRST (highest default priority — where auto-download puts models) */
    char home_models[512] = {0};
    const char * home = getenv("HOME");
    if (!home)
        home = getenv("USERPROFILE"); /* Windows */
    if (home) {
        snprintf(home_models, sizeof(home_models), "%s/.neuronos/models", home);
        search_paths[sp++] = home_models;
    }

    /* Add default paths */
    for (int i = 0; default_search_paths[i] && sp < 14; i++) {
        search_paths[sp++] = default_search_paths[i];
    }

    /* Add $NEURONOS_MODELS env var */
    const char * env_models = getenv("NEURONOS_MODELS");
    if (env_models && sp < 15) {
        search_paths[sp++] = env_models;
    }

    /* Step 3: Scan all paths for models */
    neuronos_model_entry_t * best_overall = NULL;
    neuronos_model_entry_t * all_models = NULL;
    int best_count = 0;

    for (int i = 0; i < sp; i++) {
        int count = 0;
        neuronos_model_entry_t * models = neuronos_model_scan(search_paths[i], &ctx.hw, &count);
        if (models && count > 0) {
            const neuronos_model_entry_t * best = neuronos_model_select_best(models, count);
            if (best && (!best_overall || best->score > best_overall->score)) {
                if (all_models)
                    neuronos_model_scan_free(all_models, best_count);
                all_models = models;
                best_count = count;
                best_overall = (neuronos_model_entry_t *)best;
            } else {
                neuronos_model_scan_free(models, count);
            }
        }
    }

    if (!best_overall) {
        ctx.status = NEURONOS_ERROR_MODEL_LOAD;
        if (verbose) {
            fprintf(stderr, "Error: No .gguf models found in any search path:\n");
            for (int i = 0; i < sp; i++)
                fprintf(stderr, "  - %s\n", search_paths[i]);
        }
        return ctx;
    }

    ctx.selected_model = *best_overall;
    if (verbose)
        fprintf(stderr, "★ Auto-selected: %s (%.1f score, %lld MB)\n", best_overall->name, best_overall->score,
                (long long)best_overall->file_size_mb);

    /* Step 4: Auto-tune parameters */
    ctx.tuning = neuronos_auto_tune(&ctx.hw, best_overall);
    if (verbose)
        neuronos_tune_print(&ctx.tuning);

    /* Step 5: Initialize engine with tuned params */
    neuronos_engine_params_t eparams = {
        .n_threads = ctx.tuning.n_threads,
        .n_gpu_layers = ctx.tuning.n_gpu_layers,
        .verbose = verbose,
    };
    ctx.engine = neuronos_init(eparams);
    if (!ctx.engine) {
        ctx.status = NEURONOS_ERROR_INIT;
        neuronos_model_scan_free(all_models, best_count);
        return ctx;
    }

    /* Step 6: Load model with optimal context */
    neuronos_model_params_t mparams = {
        .model_path = best_overall->path,
        .context_size = ctx.tuning.n_ctx,
        .use_mmap = ctx.tuning.use_mmap,
    };
    ctx.model = neuronos_model_load(ctx.engine, mparams);
    if (!ctx.model) {
        ctx.status = NEURONOS_ERROR_MODEL_LOAD;
        neuronos_shutdown(ctx.engine);
        ctx.engine = NULL;
        neuronos_model_scan_free(all_models, best_count);
        return ctx;
    }

    ctx.status = NEURONOS_OK;
    neuronos_model_scan_free(all_models, best_count);
    return ctx;
}

void neuronos_auto_release(neuronos_auto_ctx_t * ctx) {
    if (!ctx)
        return;
    if (ctx->model) {
        neuronos_model_free(ctx->model);
        ctx->model = NULL;
    }
    if (ctx->engine) {
        neuronos_shutdown(ctx->engine);
        ctx->engine = NULL;
    }
}
