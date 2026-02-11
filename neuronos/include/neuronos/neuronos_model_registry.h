/* ============================================================
 * NeuronOS — Model Registry & Auto-Download
 *
 * Static catalog of known ternary GGUF models with HuggingFace
 * download URLs. Enables zero-config model selection:
 *   1. Detect hardware (RAM, CPU, GPU)
 *   2. Filter registry → rank by fit
 *   3. Download best model automatically
 *   4. Verify SHA256 integrity
 *
 * All models use the I2_S (1.58-bit ternary) quantization
 * format, which gives NeuronOS its unique CPU efficiency edge.
 *
 * URL pattern: https://huggingface.co/{repo}/resolve/main/{file}
 * No API token needed for public models.
 *
 * Copyright (c) 2025-2026 NeuronOS Project
 * SPDX-License-Identifier: MIT
 * ============================================================ */
#ifndef NEURONOS_MODEL_REGISTRY_H
#define NEURONOS_MODEL_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Registry entry for a known model ---- */
typedef struct {
    const char * id;           /* Short ID: "bitnet-2b", "falcon3-7b"       */
    const char * display_name; /* "BitNet b1.58 2B (Microsoft)"             */
    const char * hf_repo;      /* "microsoft/bitnet-b1.58-2B-4T-gguf"       */
    const char * filename;     /* "ggml-model-i2_s.gguf"                    */
    const char * url;          /* Full download URL                          */
    const char * sha256;       /* SHA256 of the file (NULL if unknown)       */
    int64_t size_mb;           /* Download size in MB                        */
    int64_t min_ram_mb;        /* Absolute minimum RAM to run                */
    int64_t rec_ram_mb;        /* Recommended RAM for good performance       */
    int64_t n_params_b;        /* Parameters in billions (e.g. 2, 7, 10)    */
    int n_ctx_max;             /* Max context length                         */
    bool is_ternary;           /* True = native 1.58-bit (I2_S)             */
    bool is_instruct;          /* True = instruction-tuned (not base)        */
    const char * family;       /* "bitnet", "falcon3", "falcon-e"            */
    const char * languages;    /* "en", "en,fr,es,pt", etc.                  */
    int quality_stars;         /* 1-5 quality rating for display             */
} neuronos_registry_entry_t;

/* ---- Registry API ---- */

/* Get the full model registry (static array, do NOT free).
 * Returns pointer to first entry and count via out_count. */
const neuronos_registry_entry_t * neuronos_registry_get_all(int * out_count);

/* Find a model by short ID (e.g. "falcon3-7b"). Returns NULL if not found. */
const neuronos_registry_entry_t * neuronos_registry_find(const char * model_id);

/* Get all models that fit in the given RAM budget (MB).
 * Returns count of compatible models. Fills indices[] with
 * registry indices sorted by recommendation (best first).
 * indices must have space for at least max_results entries. */
int neuronos_registry_recommend(int64_t available_ram_mb, int * indices, int max_results);

/* Get the single best recommended model for the given RAM.
 * Returns NULL if no model fits. */
const neuronos_registry_entry_t * neuronos_registry_best_for_ram(int64_t available_ram_mb);

/* ---- Download API ---- */

/* Download progress callback.
 * downloaded_bytes: bytes received so far
 * total_bytes: total file size (0 if unknown)
 * Return false to cancel download. */
typedef bool (*neuronos_download_progress_cb)(int64_t downloaded_bytes, int64_t total_bytes, void * user_data);

/* Download a model from the registry to ~/.neuronos/models/.
 * Uses curl (must be in PATH). Shows progress bar in TTY.
 *
 * @param entry      Registry entry to download
 * @param dest_dir   Destination directory (NULL = ~/.neuronos/models/)
 * @param on_progress  Progress callback or NULL
 * @param user_data    Passed to callback
 *
 * @return 0 on success, -1 on download error, -2 on checksum mismatch */
int neuronos_model_download(const neuronos_registry_entry_t * entry,
                            const char * dest_dir,
                            neuronos_download_progress_cb on_progress,
                            void * user_data);

/* Check if a registry model is already downloaded.
 * Returns full path if found, NULL if not. Caller must free. */
char * neuronos_model_find_downloaded(const neuronos_registry_entry_t * entry);

/* Remove a downloaded model. Returns 0 on success. */
int neuronos_model_remove(const neuronos_registry_entry_t * entry);

/* Get the default models directory (~/.neuronos/models/).
 * Writes path to buf. Returns 0 on success. */
int neuronos_models_dir(char * buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* NEURONOS_MODEL_REGISTRY_H */
