/* ============================================================
 * NeuronOS — Inference Engine
 * Wraps llama.cpp's C API into the clean NeuronOS interface.
 *
 * Phase 2A: engine init, model loading, text generation.
 * ============================================================ */
#include "neuronos/neuronos.h"
#include "neuronos/neuronos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* llama.cpp public C API */
#include "llama.h"

/* ---- Internal structs ---- */
struct neuronos_engine {
    int n_threads;
    int n_gpu_layers;
    bool verbose;
    bool initialized;
};

struct neuronos_model {
    neuronos_engine_t * engine;
    struct llama_model * llama_model;
    struct llama_context * llama_ctx;
    int context_size;
    char desc_buf[256];
};

/* ---- Helpers ---- */
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int detect_n_threads(void) {
    /* Use sysconf if available, fallback to 4 */
#ifdef _SC_NPROCESSORS_ONLN
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc > 0) {
        /* Use most cores: on hybrid CPUs (P+E), using ~75% of
         * logical cores gives best throughput for ternary inference.
         * Benchmarked: 8-10 threads optimal on 16-thread i7-12650H. */
        int n = (int)(nproc * 3 / 4);
        if (n < 2) n = 2;
        if (n > 16) n = 16;
        return n;
    }
#endif
    return 4;
}

/* ============================================================
 * ENGINE
 * ============================================================ */

neuronos_engine_t * neuronos_init(neuronos_engine_params_t params) {
    neuronos_engine_t * engine = calloc(1, sizeof(neuronos_engine_t));
    if (!engine)
        return NULL;

    engine->n_threads = params.n_threads > 0 ? params.n_threads : detect_n_threads();
    engine->n_gpu_layers = params.n_gpu_layers;
    engine->verbose = params.verbose;

    /* Initialize llama.cpp backend */
    llama_backend_init();

    /* Initialize NeuronOS HAL */
    neuronos_hal_init();

    engine->initialized = true;

    if (engine->verbose) {
        fprintf(stderr, "[neuronos] Engine initialized (v%s, threads=%d, gpu_layers=%d)\n", NEURONOS_VERSION_STRING,
                engine->n_threads, engine->n_gpu_layers);
    }

    return engine;
}

void neuronos_shutdown(neuronos_engine_t * engine) {
    if (!engine)
        return;
    if (engine->initialized) {
        neuronos_hal_shutdown();
        llama_backend_free();
        engine->initialized = false;
    }
    free(engine);
}

const char * neuronos_version(void) {
    return NEURONOS_VERSION_STRING;
}

/* ============================================================
 * MODEL
 * ============================================================ */

neuronos_model_t * neuronos_model_load(neuronos_engine_t * engine, neuronos_model_params_t params) {
    if (!engine || !params.model_path)
        return NULL;

    neuronos_model_t * model = calloc(1, sizeof(neuronos_model_t));
    if (!model)
        return NULL;
    model->engine = engine;

    /* --- Load model --- */
    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = engine->n_gpu_layers;
    mparams.use_mmap = params.use_mmap;

    if (engine->verbose) {
        fprintf(stderr, "[neuronos] Loading model: %s\n", params.model_path);
    }

    model->llama_model = llama_load_model_from_file(params.model_path, mparams);
    if (!model->llama_model) {
        if (engine->verbose) {
            fprintf(stderr, "[neuronos] ERROR: Failed to load model\n");
        }
        free(model);
        return NULL;
    }

    /* --- Create context --- */
    int ctx_size = params.context_size > 0 ? params.context_size : 2048;
    model->context_size = ctx_size;

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = (uint32_t)ctx_size;
    cparams.n_batch = 512;
    cparams.n_threads = engine->n_threads;
    cparams.n_threads_batch = engine->n_threads;
    cparams.flash_attn = true;

    model->llama_ctx = llama_new_context_with_model(model->llama_model, cparams);
    if (!model->llama_ctx) {
        if (engine->verbose) {
            fprintf(stderr, "[neuronos] ERROR: Failed to create context\n");
        }
        llama_free_model(model->llama_model);
        free(model);
        return NULL;
    }

    /* Store description */
    llama_model_desc(model->llama_model, model->desc_buf, sizeof(model->desc_buf));

    if (engine->verbose) {
        fprintf(stderr, "[neuronos] Model loaded: %s (ctx=%d, params=%lldM)\n", model->desc_buf, ctx_size,
                (long long)(llama_model_n_params(model->llama_model) / 1000000));
    }

    return model;
}

void neuronos_model_free(neuronos_model_t * model) {
    if (!model)
        return;
    if (model->llama_ctx) {
        llama_free(model->llama_ctx);
    }
    if (model->llama_model) {
        llama_free_model(model->llama_model);
    }
    free(model);
}

neuronos_model_info_t neuronos_model_info(const neuronos_model_t * model) {
    neuronos_model_info_t info = {0};
    if (!model || !model->llama_model)
        return info;

    info.description = model->desc_buf;
    info.n_params = (int64_t)llama_model_n_params(model->llama_model);
    info.model_size = (int64_t)llama_model_size(model->llama_model);
    info.n_vocab = llama_n_vocab(model->llama_model);
    info.n_ctx_train = llama_n_ctx_train(model->llama_model);
    info.n_embd = llama_n_embd(model->llama_model);

    return info;
}

/* ============================================================
 * GENERATE
 * ============================================================ */

neuronos_gen_result_t neuronos_generate(neuronos_model_t * model, neuronos_gen_params_t params) {
    neuronos_gen_result_t result = {0};

    if (!model || !params.prompt) {
        result.status = NEURONOS_ERROR_INVALID_PARAM;
        return result;
    }

    double t_start = get_time_ms();
    struct llama_model * lmodel = model->llama_model;
    struct llama_context * ctx = model->llama_ctx;

    /* --- Defaults --- */
    int max_tokens = params.max_tokens > 0 ? params.max_tokens : 256;
    float temp = params.temperature >= 0.0f ? params.temperature : 0.7f;
    float top_p = params.top_p > 0.0f ? params.top_p : 0.95f;
    int top_k = params.top_k > 0 ? params.top_k : 40;
    float repeat_penalty = params.repeat_penalty > 0.0f ? params.repeat_penalty : 1.1f;
    int repeat_last_n = params.repeat_last_n > 0 ? params.repeat_last_n : 64;
    uint32_t seed = params.seed > 0 ? params.seed : (uint32_t)time(NULL);
    const char * grammar_root = params.grammar_root ? params.grammar_root : "root";

    /* --- Tokenize prompt --- */
    int prompt_len = (int)strlen(params.prompt);
    int n_prompt = -llama_tokenize(lmodel, params.prompt, prompt_len, NULL, 0, true, true);
    if (n_prompt <= 0) {
        result.status = NEURONOS_ERROR_GENERATE;
        return result;
    }

    llama_token * prompt_tokens = malloc((size_t)n_prompt * sizeof(llama_token));
    if (!prompt_tokens) {
        result.status = NEURONOS_ERROR_GENERATE;
        return result;
    }
    llama_tokenize(lmodel, params.prompt, prompt_len, prompt_tokens, n_prompt, true, true);

    /* --- Check context size --- */
    if (n_prompt + max_tokens > model->context_size) {
        max_tokens = model->context_size - n_prompt;
        if (max_tokens <= 0) {
            free(prompt_tokens);
            result.status = NEURONOS_ERROR_CONTEXT_FULL;
            return result;
        }
    }

    /* --- Clear KV cache for fresh generation --- */
    llama_kv_cache_clear(ctx);

    /* --- Create sampler chain --- */
    struct llama_sampler * smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());

    /* Add grammar sampler if grammar provided */
    if (params.grammar && params.grammar[0]) {
        struct llama_sampler * grammar_smpl = llama_sampler_init_grammar(lmodel, params.grammar, grammar_root);
        if (grammar_smpl) {
            llama_sampler_chain_add(smpl, grammar_smpl);
        }
    }

    /* Standard sampling: penalties → top-k → top-p → temperature → dist */
    if (repeat_penalty != 1.0f) {
        int32_t n_vocab = llama_n_vocab(lmodel);
        llama_token eos_id = llama_token_eos(lmodel);
        llama_token nl_id  = llama_token_nl(lmodel);
        llama_sampler_chain_add(smpl,
            llama_sampler_init_penalties(n_vocab, eos_id, nl_id,
                                        repeat_last_n, repeat_penalty,
                                        0.0f, 0.0f,   /* freq_penalty, presence_penalty */
                                        false, false)); /* penalize_nl, ignore_eos */
    }
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(top_k));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(top_p, 1));

    if (temp > 0.0f) {
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(temp));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(seed));
    } else {
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    }

    /* --- Evaluate prompt --- */
    struct llama_batch batch = llama_batch_get_one(prompt_tokens, n_prompt, 0, 0);
    int rc = llama_decode(ctx, batch);
    if (rc != 0) {
        free(prompt_tokens);
        llama_sampler_free(smpl);
        result.status = NEURONOS_ERROR_GENERATE;
        return result;
    }

    /* --- Generation loop --- */
    /* Output buffer: start with 4KB, grow as needed */
    size_t out_cap = 4096;
    size_t out_len = 0;
    char * out_buf = malloc(out_cap);
    if (!out_buf) {
        free(prompt_tokens);
        llama_sampler_free(smpl);
        result.status = NEURONOS_ERROR_GENERATE;
        return result;
    }

    char piece_buf[256];
    int n_generated = 0;
    bool stop_requested = false;

    for (int i = 0; i < max_tokens && !stop_requested; i++) {
        /* Sample next token */
        llama_token id = llama_sampler_sample(smpl, ctx, -1);

        /* Check end of generation */
        if (llama_token_is_eog(lmodel, id)) {
            break;
        }

        /* Detokenize */
        int piece_len = llama_token_to_piece(lmodel, id, piece_buf, (int)sizeof(piece_buf) - 1, 0, true);
        if (piece_len < 0)
            piece_len = 0;
        piece_buf[piece_len] = '\0';

        /* Append to output buffer (grow if needed) */
        while (out_len + (size_t)piece_len + 1 > out_cap) {
            out_cap *= 2;
            char * new_buf = realloc(out_buf, out_cap);
            if (!new_buf) {
                free(out_buf);
                free(prompt_tokens);
                llama_sampler_free(smpl);
                result.status = NEURONOS_ERROR_GENERATE;
                return result;
            }
            out_buf = new_buf;
        }
        memcpy(out_buf + out_len, piece_buf, (size_t)piece_len);
        out_len += (size_t)piece_len;

        n_generated++;

        /* Streaming callback */
        if (params.on_token) {
            if (!params.on_token(piece_buf, params.user_data)) {
                stop_requested = true;
                break;
            }
        }

        /* Prepare next batch (single token) */
        batch = llama_batch_get_one(&id, 1, n_prompt + i, 0);
        rc = llama_decode(ctx, batch);
        if (rc != 0) {
            break;
        }
    }

    /* Null-terminate output */
    out_buf[out_len] = '\0';

    double t_end = get_time_ms();
    double elapsed = t_end - t_start;

    /* --- Build result --- */
    result.text = out_buf;
    result.n_tokens = n_generated;
    result.elapsed_ms = elapsed;
    result.tokens_per_s = elapsed > 0.0 ? (double)n_generated / (elapsed / 1000.0) : 0.0;
    result.status = NEURONOS_OK;

    if (model->engine->verbose) {
        fprintf(stderr, "[neuronos] Generated %d tokens in %.1f ms (%.2f t/s)\n", n_generated, elapsed,
                result.tokens_per_s);
    }

    /* --- Cleanup --- */
    free(prompt_tokens);
    llama_sampler_free(smpl);

    return result;
}

void neuronos_gen_result_free(neuronos_gen_result_t * result) {
    if (!result)
        return;
    free(result->text);
    result->text = NULL;
}

void neuronos_free(void * ptr) {
    free(ptr);
}

/* ============================================================
 * CHAT TEMPLATE
 * ============================================================ */

neuronos_status_t neuronos_chat_format(const neuronos_model_t * model, const char * tmpl,
                                       const neuronos_chat_msg_t * messages, size_t n_messages,
                                       bool add_generation_prompt, char ** out_text) {
    if (!model || !model->llama_model || !messages || n_messages == 0 || !out_text) {
        return NEURONOS_ERROR_INVALID_PARAM;
    }

    /* llama_chat_message is layout-compatible with neuronos_chat_msg_t
     * (two const char * fields), but we copy explicitly for safety. */
    struct llama_chat_message * msgs = malloc(n_messages * sizeof(struct llama_chat_message));
    if (!msgs) {
        return NEURONOS_ERROR_GENERATE;
    }
    for (size_t i = 0; i < n_messages; i++) {
        msgs[i].role    = messages[i].role;
        msgs[i].content = messages[i].content;
    }

    /* First call: measure required buffer size */
    int32_t needed = llama_chat_apply_template(
        model->llama_model, tmpl, msgs, n_messages, add_generation_prompt, NULL, 0);

    if (needed < 0) {
        free(msgs);
        return NEURONOS_ERROR_INVALID_PARAM;
    }

    /* Allocate and format */
    size_t buf_size = (size_t)needed + 1;
    char * buf = malloc(buf_size);
    if (!buf) {
        free(msgs);
        return NEURONOS_ERROR_GENERATE;
    }

    llama_chat_apply_template(
        model->llama_model, tmpl, msgs, n_messages, add_generation_prompt, buf, (int32_t)buf_size);
    buf[needed] = '\0';

    free(msgs);
    *out_text = buf;
    return NEURONOS_OK;
}
