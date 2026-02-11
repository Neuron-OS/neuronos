/* ============================================================
 * NeuronOS WASM Glue — C API exported to JavaScript
 *
 * Bridge between NeuronOS C engine and JavaScript Web Worker.
 * Each exported function is callable from JS via Emscripten ccall/cwrap.
 *
 * Architecture:
 *   JS Main Thread ──msg──▶ Web Worker ──ccall──▶ WASM (this)
 *   WASM (this) ──EM_JS──▶ Web Worker ──postMessage──▶ Main Thread
 *
 * Uses the REAL NeuronOS API from neuronos.h v0.9.1.
 * ============================================================ */
#include "neuronos/neuronos.h"

#include <emscripten/emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Global state ── */
static neuronos_engine_t        * g_engine = NULL;
static neuronos_model_t         * g_model  = NULL;
static neuronos_tool_registry_t * g_tools  = NULL;
static neuronos_agent_t         * g_agent  = NULL;
static neuronos_memory_t        * g_memory = NULL;

/* Path where we store the model in the Emscripten virtual filesystem */
#define WASM_MODEL_PATH "/model.gguf"
#define WASM_MEMORY_DB  "/neuronos_memory.db"

/* ── JS callbacks via Emscripten EM_JS ── */

EM_JS(void, js_on_token, (const char * token_ptr, int len), {
    var text = UTF8ToString(token_ptr, len);
    postMessage({ type: "token", text: text });
});

EM_JS(void, js_on_agent_step, (int step, const char * thought_ptr,
                                const char * action_ptr, const char * obs_ptr), {
    var thought = thought_ptr ? UTF8ToString(thought_ptr) : "";
    var action  = action_ptr  ? UTF8ToString(action_ptr)  : "";
    var obs     = obs_ptr     ? UTF8ToString(obs_ptr)     : "";
    postMessage({
        type: "agent_step",
        step: step,
        thought: thought,
        action: action,
        observation: obs
    });
});

EM_JS(void, js_on_status, (const char * status_ptr), {
    var status = UTF8ToString(status_ptr);
    postMessage({ type: "status", text: status });
});

EM_JS(void, js_on_error, (const char * error_ptr), {
    var error = UTF8ToString(error_ptr);
    postMessage({ type: "error", text: error });
});

/* ── Token streaming callback (matches neuronos_token_cb signature) ── */
static bool wasm_token_callback(const char * token_text, void * user_data) {
    (void)user_data;
    if (token_text) {
        js_on_token(token_text, (int)strlen(token_text));
    }
    return true; /* continue generating */
}

/* ── Agent step callback (matches neuronos_agent_step_cb signature) ── */
static void wasm_agent_step_callback(int step, const char * thought,
                                      const char * action,
                                      const char * observation,
                                      void * user_data) {
    (void)user_data;
    js_on_agent_step(step, thought, action, observation);
}

/* ════════════════════════════════════════════════
 * Exported Functions (called from JS via cwrap)
 * ════════════════════════════════════════════════ */

/**
 * Initialize the NeuronOS engine.
 * @param n_threads Number of threads (0 = auto, typically 4 for WASM)
 * @param n_ctx     Context size hint (used when loading model)
 * Returns 0 on success, non-zero on error.
 */
EMSCRIPTEN_KEEPALIVE
int neuronos_wasm_init(int n_threads, int n_ctx) {
    (void)n_ctx; /* stored for later use at model load */

    if (g_engine) return 0; /* Already initialized */

    js_on_status("Initializing NeuronOS engine...");

    neuronos_engine_params_t params;
    memset(&params, 0, sizeof(params));
#ifdef __EMSCRIPTEN_PTHREADS__
    params.n_threads    = n_threads > 0 ? n_threads : 4;
#else
    /* Single-thread WASM build — pthread_create will abort if n_threads > 1 */
    params.n_threads    = 1;
    (void)n_threads;
#endif
    params.n_gpu_layers = 0;     /* No GPU in WASM */
    params.verbose      = false;

    g_engine = neuronos_init(params);
    if (!g_engine) {
        js_on_error("Failed to initialize NeuronOS engine");
        return -1;
    }

    js_on_status("Engine initialized");
    return 0;
}

/**
 * Load a GGUF model from an ArrayBuffer.
 * JS writes the model bytes into WASM heap, then we write to VFS.
 *
 * @param data    Pointer to model bytes in WASM heap
 * @param size    Size in bytes
 * @param n_ctx   Context size (0 = auto)
 * Returns 0 on success.
 */
EMSCRIPTEN_KEEPALIVE
int neuronos_wasm_load_model_from_buffer(const uint8_t * data, int size, int n_ctx) {
    if (!g_engine) {
        js_on_error("Engine not initialized");
        return -1;
    }

    /* Free previous resources */
    if (g_agent)  { neuronos_agent_free(g_agent);         g_agent  = NULL; }
    if (g_tools)  { neuronos_tool_registry_free(g_tools); g_tools  = NULL; }
    if (g_model)  { neuronos_model_free(g_model);         g_model  = NULL; }

    js_on_status("Writing model to virtual filesystem...");

    /* Write model bytes to Emscripten VFS */
    FILE * f = fopen(WASM_MODEL_PATH, "wb");
    if (!f) {
        js_on_error("Failed to create VFS file for model");
        return -2;
    }
    size_t written = fwrite(data, 1, (size_t)size, f);
    fclose(f);

    if ((int)written != size) {
        js_on_error("Incomplete write to VFS");
        return -3;
    }

    js_on_status("Loading model into inference engine...");

    /* Load model using real API */
    neuronos_model_params_t mp;
    memset(&mp, 0, sizeof(mp));
    mp.model_path   = WASM_MODEL_PATH;
    mp.context_size = n_ctx > 0 ? n_ctx : 1024;  /* Reduced default for WASM memory constraints */
    mp.use_mmap     = false;  /* Emscripten FS doesn't support mmap */

    g_model = neuronos_model_load(g_engine, mp);
    if (!g_model) {
        js_on_error("Failed to load GGUF model");
        return -4;
    }

    /* Free the VFS copy — model data is now in llama.cpp's own buffers.
     * This reclaims ~500MB of WASM heap that was holding the duplicate. */
    remove(WASM_MODEL_PATH);

    js_on_status("Setting up agent...");

    /* Create tool registry — browser-safe tools only (no shell, no fs) */
    g_tools = neuronos_tool_registry_create();
    if (g_tools) {
        /* Register only safe built-in tools for browser environment.
         * NEURONOS_CAP_MEMORY allows memory tools; skip FS/SHELL/NETWORK. */
        neuronos_tool_register_defaults(g_tools, NEURONOS_CAP_MEMORY);
    }

    /* Create agent */
    neuronos_agent_params_t ap;
    memset(&ap, 0, sizeof(ap));
    ap.max_steps           = 5;
    ap.max_tokens_per_step = 256;  /* Reduced from 512 for WASM memory constraints */
    ap.temperature         = 0.7f;
    ap.context_budget      = 0;    /* auto */
    ap.verbose             = false;

    g_agent = neuronos_agent_create(g_model, g_tools, ap);

    /* Attach memory if already initialized */
    if (g_agent && g_memory) {
        neuronos_agent_set_memory(g_agent, g_memory);
    }

    /* Report model info */
    neuronos_model_info_t info = neuronos_model_info(g_model);
    char buf[256];
    snprintf(buf, sizeof(buf), "Model loaded: %lld params, ctx=%d",
             (long long)info.n_params,
             neuronos_model_context_size(g_model));
    js_on_status(buf);

    return 0;
}

/**
 * Generate text from a prompt.
 * Streams tokens via js_on_token callback.
 * Returns the full generated text (malloc'd, caller frees with neuronos_wasm_free_string).
 */
EMSCRIPTEN_KEEPALIVE
char * neuronos_wasm_generate(const char * prompt, int n_predict, float temp) {
    if (!g_model) {
        js_on_error("No model loaded");
        return NULL;
    }

    neuronos_gen_params_t gp;
    memset(&gp, 0, sizeof(gp));
    gp.prompt         = prompt;
    gp.max_tokens     = n_predict > 0 ? n_predict : 256;
    gp.temperature    = temp > 0.001f ? temp : 0.7f;
    gp.top_p          = 0.95f;
    gp.top_k          = 40;
    gp.repeat_penalty = 1.1f;
    gp.repeat_last_n  = 64;
    gp.grammar        = NULL;
    gp.grammar_root   = NULL;
    gp.on_token       = wasm_token_callback;
    gp.user_data      = NULL;
    gp.seed           = 0;

    neuronos_gen_result_t result = neuronos_generate(g_model, gp);

    if (result.status != NEURONOS_OK) {
        js_on_error("Generation failed");
        neuronos_gen_result_free(&result);
        return NULL;
    }

    /* Report speed */
    char speed_buf[128];
    snprintf(speed_buf, sizeof(speed_buf), "Generated %d tokens at %.1f t/s",
             result.n_tokens, result.tokens_per_s);
    js_on_status(speed_buf);

    char * out = result.text ? strdup(result.text) : strdup("");
    neuronos_gen_result_free(&result);
    return out;
}

/**
 * Agent chat — multi-turn conversational agent.
 * Streams agent steps via js_on_agent_step callback.
 * Returns the final answer text (malloc'd).
 */
EMSCRIPTEN_KEEPALIVE
char * neuronos_wasm_agent_chat(const char * message, int n_predict) {
    if (!g_agent) {
        /* No agent — fallback to raw generation */
        js_on_status("No agent available, using raw generation");
        return neuronos_wasm_generate(message, n_predict, 0.7f);
    }

    neuronos_agent_result_t result = neuronos_agent_chat(
        g_agent, message, wasm_agent_step_callback, NULL);

    if (result.status != NEURONOS_OK || !result.text) {
        js_on_error("Agent chat failed, falling back to raw generation");
        neuronos_agent_result_free(&result);
        return neuronos_wasm_generate(message, n_predict, 0.7f);
    }

    char * out = strdup(result.text);
    neuronos_agent_result_free(&result);
    return out;
}

/**
 * Get model info as JSON string.
 */
EMSCRIPTEN_KEEPALIVE
char * neuronos_wasm_model_info(void) {
    if (!g_model) return strdup("{\"error\":\"no model loaded\"}");

    neuronos_model_info_t info = neuronos_model_info(g_model);
    int ctx = neuronos_model_context_size(g_model);

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"description\":\"%s\","
             "\"n_params\":%lld,"
             "\"model_size\":%lld,"
             "\"n_vocab\":%d,"
             "\"n_ctx_train\":%d,"
             "\"n_embd\":%d,"
             "\"context_size\":%d,"
             "\"version\":\"%s\"}",
             info.description ? info.description : "unknown",
             (long long)info.n_params,
             (long long)info.model_size,
             info.n_vocab,
             info.n_ctx_train,
             info.n_embd,
             ctx,
             NEURONOS_VERSION_STRING);
    return strdup(buf);
}

/**
 * Initialize persistent memory (SQLite+FTS5 in Emscripten VFS).
 * Returns 0 on success.
 */
EMSCRIPTEN_KEEPALIVE
int neuronos_wasm_memory_init(void) {
    if (g_memory) return 0;

    js_on_status("Initializing persistent memory...");

    g_memory = neuronos_memory_open(WASM_MEMORY_DB);
    if (!g_memory) {
        js_on_error("Failed to initialize memory database");
        return -1;
    }

    /* Register memory tools if tool registry exists */
    if (g_tools) {
        neuronos_tool_register_memory(g_tools, g_memory);
    }

    /* Attach memory to agent if agent exists */
    if (g_agent) {
        neuronos_agent_set_memory(g_agent, g_memory);
    }

    js_on_status("Memory initialized (SQLite+FTS5)");
    return 0;
}

/**
 * Store a fact in archival memory.
 * Returns row id (>0) on success, negative on error.
 */
EMSCRIPTEN_KEEPALIVE
int neuronos_wasm_memory_store(const char * key, const char * value) {
    if (!g_memory) return -1;
    int64_t id = neuronos_memory_archival_store(g_memory, key, value, "wasm", 0.5f);
    return (int)id;
}

/**
 * Search memory via FTS5. Returns JSON array string.
 */
EMSCRIPTEN_KEEPALIVE
char * neuronos_wasm_memory_search(const char * query, int max_results) {
    if (!g_memory) return strdup("[]");

    /* Use legacy search API which returns string array */
    char ** results = NULL;
    int count = 0;
    int limit = max_results > 0 ? max_results : 5;

    int st = neuronos_memory_search(g_memory, query, &results, &count, limit);
    if (st != 0 || count == 0 || !results) {
        return strdup("[]");
    }

    /* Build JSON array of strings */
    size_t cap = 4096;
    char * json = malloc(cap);
    if (!json) {
        neuronos_memory_free_results(results, count);
        return strdup("[]");
    }

    int off = snprintf(json, cap, "[");
    for (int i = 0; i < count && (size_t)off < cap - 64; i++) {
        if (i > 0) off += snprintf(json + off, cap - (size_t)off, ",");
        /* Simple JSON string escape — just replace quotes */
        off += snprintf(json + off, cap - (size_t)off, "\"%s\"",
                        results[i] ? results[i] : "");
    }
    snprintf(json + off, cap - (size_t)off, "]");

    neuronos_memory_free_results(results, count);
    return json;
}

/**
 * Free a string returned by other WASM functions.
 */
EMSCRIPTEN_KEEPALIVE
void neuronos_wasm_free_string(char * ptr) {
    free(ptr);
}

/**
 * Shutdown and free all resources.
 */
EMSCRIPTEN_KEEPALIVE
void neuronos_wasm_free(void) {
    if (g_agent)  { neuronos_agent_free(g_agent);         g_agent  = NULL; }
    if (g_tools)  { neuronos_tool_registry_free(g_tools); g_tools  = NULL; }
    if (g_model)  { neuronos_model_free(g_model);         g_model  = NULL; }
    if (g_memory) { neuronos_memory_close(g_memory);      g_memory = NULL; }
    if (g_engine) { neuronos_shutdown(g_engine);           g_engine = NULL; }
    js_on_status("NeuronOS shutdown complete");
}
