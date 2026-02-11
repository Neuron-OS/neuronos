/* ============================================================
 * NeuronOS — Model Registry & Auto-Download Implementation
 *
 * Static catalog of ternary GGUF models + download engine.
 * Zero runtime dependencies: uses curl subprocess for HTTP.
 *
 * Model selection algorithm:
 *   score = fits_in_ram * 1000
 *         + quality(params) * 100
 *         + is_instruct * 50
 *         + headroom_ratio * 30
 *
 * Copyright (c) 2025-2026 NeuronOS Project
 * SPDX-License-Identifier: MIT
 * ============================================================ */
#include "neuronos/neuronos_model_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <errno.h>

#ifdef _WIN32
    #include <direct.h>
    #include <io.h>
    #define isatty _isatty
    #define fileno _fileno
    #define mkdir(p, m) _mkdir(p)
#else
    #include <unistd.h>
#endif

/* ============================================================
 * MODEL REGISTRY — Static catalog of known ternary models
 *
 * All models use ggml-model-i2_s.gguf (I2_S ternary format).
 * URLs are direct HuggingFace resolve links (no API needed).
 *
 * Size estimates based on HuggingFace file listings.
 * SHA256 set to NULL — will be populated as we verify them.
 * ============================================================ */

static const neuronos_registry_entry_t REGISTRY[] = {
    /* ── BitNet (Microsoft) ── */
    {
        .id           = "bitnet-2b",
        .display_name = "BitNet b1.58 2B (Microsoft)",
        .hf_repo      = "microsoft/bitnet-b1.58-2B-4T-gguf",
        .filename     = "ggml-model-i2_s.gguf",
        .url          = "https://huggingface.co/microsoft/bitnet-b1.58-2B-4T-gguf/resolve/main/ggml-model-i2_s.gguf",
        .sha256       = NULL,
        .size_mb      = 780,
        .min_ram_mb   = 1500,
        .rec_ram_mb   = 3000,
        .n_params_b   = 2,
        .n_ctx_max    = 4096,
        .is_ternary   = true,
        .is_instruct  = true,
        .family       = "bitnet",
        .languages    = "en",
        .quality_stars = 3,
    },

    /* ── Falcon3 1B (TII) ── */
    {
        .id           = "falcon3-1b",
        .display_name = "Falcon3 1B Instruct 1.58-bit (TII)",
        .hf_repo      = "tiiuae/Falcon3-1B-Instruct-1.58bit-GGUF",
        .filename     = "ggml-model-i2_s.gguf",
        .url          = "https://huggingface.co/tiiuae/Falcon3-1B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf",
        .sha256       = NULL,
        .size_mb      = 420,
        .min_ram_mb   = 800,
        .rec_ram_mb   = 2000,
        .n_params_b   = 1,
        .n_ctx_max    = 8192,
        .is_ternary   = true,
        .is_instruct  = true,
        .family       = "falcon3",
        .languages    = "en,fr,es,pt",
        .quality_stars = 2,
    },

    /* ── Falcon3 3B (TII) ── */
    {
        .id           = "falcon3-3b",
        .display_name = "Falcon3 3B Instruct 1.58-bit (TII)",
        .hf_repo      = "tiiuae/Falcon3-3B-Instruct-1.58bit-GGUF",
        .filename     = "ggml-model-i2_s.gguf",
        .url          = "https://huggingface.co/tiiuae/Falcon3-3B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf",
        .sha256       = NULL,
        .size_mb      = 1100,
        .min_ram_mb   = 2000,
        .rec_ram_mb   = 4000,
        .n_params_b   = 3,
        .n_ctx_max    = 8192,
        .is_ternary   = true,
        .is_instruct  = true,
        .family       = "falcon3",
        .languages    = "en,fr,es,pt",
        .quality_stars = 3,
    },

    /* ── Falcon3 7B (TII) ── */
    {
        .id           = "falcon3-7b",
        .display_name = "Falcon3 7B Instruct 1.58-bit (TII)",
        .hf_repo      = "tiiuae/Falcon3-7B-Instruct-1.58bit-GGUF",
        .filename     = "ggml-model-i2_s.gguf",
        .url          = "https://huggingface.co/tiiuae/Falcon3-7B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf",
        .sha256       = NULL,
        .size_mb      = 2600,
        .min_ram_mb   = 4000,
        .rec_ram_mb   = 8000,
        .n_params_b   = 7,
        .n_ctx_max    = 32768,
        .is_ternary   = true,
        .is_instruct  = true,
        .family       = "falcon3",
        .languages    = "en,fr,es,pt",
        .quality_stars = 4,
    },

    /* ── Falcon3 10B (TII) ── */
    {
        .id           = "falcon3-10b",
        .display_name = "Falcon3 10B Instruct 1.58-bit (TII)",
        .hf_repo      = "tiiuae/Falcon3-10B-Instruct-1.58bit-GGUF",
        .filename     = "ggml-model-i2_s.gguf",
        .url          = "https://huggingface.co/tiiuae/Falcon3-10B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf",
        .sha256       = NULL,
        .size_mb      = 3800,
        .min_ram_mb   = 6000,
        .rec_ram_mb   = 12000,
        .n_params_b   = 10,
        .n_ctx_max    = 32768,
        .is_ternary   = true,
        .is_instruct  = true,
        .family       = "falcon3",
        .languages    = "en,fr,es,pt",
        .quality_stars = 5,
    },

    /* ── Falcon-E 1B (TII, newer architecture) ── */
    {
        .id           = "falcon-e-1b",
        .display_name = "Falcon-E 1B Instruct 1.58-bit (TII)",
        .hf_repo      = "tiiuae/Falcon-E-1B-Instruct-GGUF",
        .filename     = "ggml-model-i2_s.gguf",
        .url          = "https://huggingface.co/tiiuae/Falcon-E-1B-Instruct-GGUF/resolve/main/ggml-model-i2_s.gguf",
        .sha256       = NULL,
        .size_mb      = 450,
        .min_ram_mb   = 900,
        .rec_ram_mb   = 2000,
        .n_params_b   = 1,
        .n_ctx_max    = 8192,
        .is_ternary   = true,
        .is_instruct  = true,
        .family       = "falcon-e",
        .languages    = "en",
        .quality_stars = 3,
    },

    /* ── Falcon-E 3B (TII, newer architecture) ── */
    {
        .id           = "falcon-e-3b",
        .display_name = "Falcon-E 3B Instruct 1.58-bit (TII)",
        .hf_repo      = "tiiuae/Falcon-E-3B-Instruct-GGUF",
        .filename     = "ggml-model-i2_s.gguf",
        .url          = "https://huggingface.co/tiiuae/Falcon-E-3B-Instruct-GGUF/resolve/main/ggml-model-i2_s.gguf",
        .sha256       = NULL,
        .size_mb      = 1000,
        .min_ram_mb   = 2000,
        .rec_ram_mb   = 4000,
        .n_params_b   = 3,
        .n_ctx_max    = 8192,
        .is_ternary   = true,
        .is_instruct  = true,
        .family       = "falcon-e",
        .languages    = "en",
        .quality_stars = 4,
    },
};

#define REGISTRY_COUNT ((int)(sizeof(REGISTRY) / sizeof(REGISTRY[0])))

/* ============================================================
 * REGISTRY API
 * ============================================================ */

const neuronos_registry_entry_t * neuronos_registry_get_all(int * out_count) {
    if (out_count)
        *out_count = REGISTRY_COUNT;
    return REGISTRY;
}

const neuronos_registry_entry_t * neuronos_registry_find(const char * model_id) {
    if (!model_id)
        return NULL;
    for (int i = 0; i < REGISTRY_COUNT; i++) {
        if (strcmp(REGISTRY[i].id, model_id) == 0)
            return &REGISTRY[i];
    }
    return NULL;
}

/* Score a registry model for recommendation */
static float registry_score(const neuronos_registry_entry_t * e, int64_t available_ram_mb) {
    if (e->min_ram_mb > available_ram_mb)
        return -1.0f; /* doesn't fit */

    float score = 0.0f;

    /* Fits in RAM: base bonus */
    score += 1000.0f;

    /* Quality: bigger = smarter */
    score += (float)(e->n_params_b) * 50.0f;

    /* Instruct bonus: better for agents */
    if (e->is_instruct)
        score += 50.0f;

    /* Headroom: prefer models that leave breathing room */
    float headroom = (float)(available_ram_mb - e->min_ram_mb) / (float)available_ram_mb;
    if (headroom < 0.15f)
        score -= 100.0f; /* too tight */
    else
        score += headroom * 30.0f;

    /* Comfortable fit bonus: RAM >= recommended */
    if (available_ram_mb >= e->rec_ram_mb)
        score += 100.0f;

    /* Multilingual bonus */
    if (e->languages && strchr(e->languages, ','))
        score += 20.0f;

    return score;
}

int neuronos_registry_recommend(int64_t available_ram_mb, int * indices, int max_results) {
    if (!indices || max_results <= 0)
        return 0;

    /* Score all models */
    typedef struct {
        int index;
        float score;
    } scored_t;

    scored_t scored[REGISTRY_COUNT];
    int n_fit = 0;

    for (int i = 0; i < REGISTRY_COUNT; i++) {
        float s = registry_score(&REGISTRY[i], available_ram_mb);
        if (s > 0.0f) {
            scored[n_fit].index = i;
            scored[n_fit].score = s;
            n_fit++;
        }
    }

    /* Sort descending by score (simple insertion sort, small N) */
    for (int i = 1; i < n_fit; i++) {
        scored_t tmp = scored[i];
        int j = i - 1;
        while (j >= 0 && scored[j].score < tmp.score) {
            scored[j + 1] = scored[j];
            j--;
        }
        scored[j + 1] = tmp;
    }

    int count = n_fit < max_results ? n_fit : max_results;
    for (int i = 0; i < count; i++)
        indices[i] = scored[i].index;

    return count;
}

const neuronos_registry_entry_t * neuronos_registry_best_for_ram(int64_t available_ram_mb) {
    int idx = -1;
    float best_score = -1.0f;

    for (int i = 0; i < REGISTRY_COUNT; i++) {
        float s = registry_score(&REGISTRY[i], available_ram_mb);
        if (s > best_score) {
            best_score = s;
            idx = i;
        }
    }

    return (idx >= 0) ? &REGISTRY[idx] : NULL;
}

/* ============================================================
 * FILE UTILITIES
 * ============================================================ */

int neuronos_models_dir(char * buf, size_t buflen) {
    const char * home = getenv("HOME");
#ifdef _WIN32
    if (!home)
        home = getenv("USERPROFILE");
#endif
    if (!home)
        return -1;
    int n = snprintf(buf, buflen, "%s/.neuronos/models", home);
    return (n > 0 && (size_t)n < buflen) ? 0 : -1;
}

/* Ensure directory exists (recursive) */
static int ensure_dir(const char * path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0)
        return -1;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char * p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* errno.h already included at top */

/* ============================================================
 * DOWNLOAD ENGINE
 *
 * Strategy: Use curl subprocess (available on 99.9% of systems).
 * This avoids adding libcurl as a dependency while providing:
 *   - HTTPS support
 *   - Redirect following (-L)
 *   - Resume support (-C -)
 *   - Progress bar (--progress-bar)
 *   - Timeout handling
 *
 * Fallback: wget if curl not found.
 * ============================================================ */

/* Check if a command exists */
static bool cmd_exists(const char * cmd) {
    char check[256];
    snprintf(check, sizeof(check), "command -v %s >/dev/null 2>&1", cmd);
    return system(check) == 0;
}

char * neuronos_model_find_downloaded(const neuronos_registry_entry_t * entry) {
    if (!entry)
        return NULL;

    char dir[512];
    if (neuronos_models_dir(dir, sizeof(dir)) != 0)
        return NULL;

    /* Check under ~/.neuronos/models/<id>/filename */
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%s", dir, entry->id, entry->filename);

    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) {
        char * result = malloc(strlen(path) + 1);
        if (result)
            strcpy(result, path);
        return result;
    }

    /* Also check directly in models dir (legacy layout) */
    snprintf(path, sizeof(path), "%s/%s", dir, entry->filename);
    if (stat(path, &st) == 0 && st.st_size > 0) {
        char * result = malloc(strlen(path) + 1);
        if (result)
            strcpy(result, path);
        return result;
    }

    return NULL;
}

int neuronos_model_download(const neuronos_registry_entry_t * entry,
                            const char * dest_dir,
                            neuronos_download_progress_cb on_progress,
                            void * user_data) {
    (void)on_progress;
    (void)user_data;

    if (!entry || !entry->url)
        return -1;

    /* Determine destination directory */
    char dir[512];
    if (dest_dir) {
        snprintf(dir, sizeof(dir), "%s", dest_dir);
    } else {
        if (neuronos_models_dir(dir, sizeof(dir)) != 0) {
            fprintf(stderr, "Error: Cannot determine models directory\n");
            return -1;
        }
    }

    /* Create model subdirectory: ~/.neuronos/models/<id>/ */
    char model_dir[1024];
    snprintf(model_dir, sizeof(model_dir), "%s/%s", dir, entry->id);
    ensure_dir(model_dir);

    /* Full path for the model file */
    char dest_path[1024];
    snprintf(dest_path, sizeof(dest_path), "%s/%s", model_dir, entry->filename);

    /* Check if already downloaded */
    struct stat st;
    if (stat(dest_path, &st) == 0 && st.st_size > 0) {
        /* Verify size roughly matches expected */
        int64_t actual_mb = (int64_t)(st.st_size / (1024 * 1024));
        if (actual_mb >= entry->size_mb * 90 / 100) {
            fprintf(stderr, "  Model already downloaded: %s\n", dest_path);
            return 0;
        }
        /* Partial download — will resume */
    }

    /* Display download banner */
    fprintf(stderr,
            "\033[36m"
            "  ┌────────────────────────────────────────────┐\n"
            "  │  Downloading: %-29s│\n"
            "  │  Size: ~%lld MB                              │\n"
            "  │  From: HuggingFace                         │\n"
            "  │  To:   ~/.neuronos/models/%-16s│\n"
            "  └────────────────────────────────────────────┘\n"
            "\033[0m\n",
            entry->display_name,
            (long long)entry->size_mb,
            entry->id);

    /* Build download command */
    char cmd[4096];
    bool is_tty = isatty(fileno(stderr));

    if (cmd_exists("curl")) {
        if (is_tty) {
            snprintf(cmd, sizeof(cmd),
                     "curl -fL --progress-bar -C - "
                     "-o \"%s\" \"%s\"",
                     dest_path, entry->url);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "curl -fL -s -C - "
                     "-o \"%s\" \"%s\"",
                     dest_path, entry->url);
        }
    } else if (cmd_exists("wget")) {
        snprintf(cmd, sizeof(cmd),
                 "wget -c -q --show-progress "
                 "-O \"%s\" \"%s\"",
                 dest_path, entry->url);
    } else {
        fprintf(stderr,
                "\033[31mError: Neither curl nor wget found.\033[0m\n"
                "Please install curl:  sudo apt install curl\n"
                "Or download manually:\n  %s\n",
                entry->url);
        return -1;
    }

    /* Execute download */
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr,
                "\n\033[31mDownload failed.\033[0m\n"
                "Try manually:\n"
                "  curl -L -o \"%s\" \"%s\"\n",
                dest_path, entry->url);
        /* Clean up partial file on failure (not resume-related) */
        return -1;
    }

    /* Verify file was written */
    if (stat(dest_path, &st) != 0 || st.st_size == 0) {
        fprintf(stderr, "\033[31mError: Downloaded file is empty or missing.\033[0m\n");
        return -1;
    }

    /* SHA256 verification (if checksum known) */
    if (entry->sha256) {
        char sha_cmd[2048];
        char expected_line[256];
        snprintf(expected_line, sizeof(expected_line), "%s", entry->sha256);

        if (cmd_exists("sha256sum")) {
            snprintf(sha_cmd, sizeof(sha_cmd),
                     "sha256sum \"%s\" | cut -d' ' -f1", dest_path);
        } else if (cmd_exists("shasum")) {
            snprintf(sha_cmd, sizeof(sha_cmd),
                     "shasum -a 256 \"%s\" | cut -d' ' -f1", dest_path);
        } else {
            fprintf(stderr, "  [skipping SHA256 check: no sha256sum/shasum found]\n");
            goto done;
        }

        FILE * fp = popen(sha_cmd, "r");
        if (fp) {
            char actual[128] = {0};
            if (fgets(actual, sizeof(actual), fp)) {
                /* Trim newline */
                size_t len = strlen(actual);
                while (len > 0 && (actual[len - 1] == '\n' || actual[len - 1] == '\r'))
                    actual[--len] = '\0';

                if (strcmp(actual, expected_line) != 0) {
                    fprintf(stderr,
                            "\033[31mSHA256 mismatch!\033[0m\n"
                            "  Expected: %s\n"
                            "  Got:      %s\n"
                            "  File may be corrupt. Delete and retry.\n",
                            expected_line, actual);
                    pclose(fp);
                    return -2;
                }
            }
            pclose(fp);
        }
        fprintf(stderr, "  \033[32m✓ SHA256 verified\033[0m\n");
    }

done:
    fprintf(stderr, "  \033[32m✓ Model ready: %s\033[0m\n\n", dest_path);
    return 0;
}

int neuronos_model_remove(const neuronos_registry_entry_t * entry) {
    if (!entry)
        return -1;

    char * path = neuronos_model_find_downloaded(entry);
    if (!path) {
        fprintf(stderr, "Model '%s' is not installed.\n", entry->id);
        return -1;
    }

    int ret = remove(path);
    if (ret == 0) {
        fprintf(stderr, "  \033[32m✓ Removed: %s\033[0m\n", path);
    } else {
        fprintf(stderr, "  \033[31mError removing: %s\033[0m\n", path);
    }

    free(path);
    return ret;
}
