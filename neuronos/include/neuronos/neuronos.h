/* ============================================================
 * NeuronOS Agent Engine — Public API
 * Version 0.7.0
 *
 * The fastest AI agent engine in the world.
 * Universal, offline, runs on any device.
 *
 * Copyright (c) 2025 NeuronOS Project
 * SPDX-License-Identifier: MIT
 * ============================================================ */
#ifndef NEURONOS_H
#define NEURONOS_H

#include <stddef.h>
#include <stdint.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Version ---- */
#define NEURONOS_VERSION_MAJOR 0
#define NEURONOS_VERSION_MINOR 7
#define NEURONOS_VERSION_PATCH 0
#define NEURONOS_VERSION_STRING "0.7.0"

/* ---- Opaque types ---- */
typedef struct neuronos_engine neuronos_engine_t;
typedef struct neuronos_model neuronos_model_t;
typedef struct neuronos_agent neuronos_agent_t;
typedef struct neuronos_tool_reg neuronos_tool_registry_t;
typedef struct neuronos_memory neuronos_memory_t;

/* ---- Status codes ---- */
typedef enum {
    NEURONOS_OK = 0,
    NEURONOS_ERROR_INIT = -1,
    NEURONOS_ERROR_MODEL_LOAD = -2,
    NEURONOS_ERROR_GENERATE = -3,
    NEURONOS_ERROR_TOOL_NOT_FOUND = -4,
    NEURONOS_ERROR_TOOL_EXEC = -5,
    NEURONOS_ERROR_GRAMMAR = -6,
    NEURONOS_ERROR_MEMORY = -7,
    NEURONOS_ERROR_MAX_STEPS = -8,
    NEURONOS_ERROR_CONTEXT_FULL = -9,
    NEURONOS_ERROR_INVALID_PARAM = -10,
} neuronos_status_t;

/* ============================================================
 * ENGINE: Init / Shutdown
 * ============================================================ */
typedef struct {
    int n_threads;    /* 0 = auto-detect (physical cores)        */
    int n_gpu_layers; /* 0 = CPU only; >0 = offload N layers     */
    bool verbose;     /* print info to stderr                     */
} neuronos_engine_params_t;

/* Create engine (initializes llama.cpp backend) */
neuronos_engine_t * neuronos_init(neuronos_engine_params_t params);

/* Shutdown and free all resources */
void neuronos_shutdown(neuronos_engine_t * engine);

/* Version string */
const char * neuronos_version(void);

/* ============================================================
 * MODEL: Load / Free / Info
 * ============================================================ */
typedef struct {
    const char * model_path; /* path to GGUF file                    */
    int context_size;        /* 0 = model default (typically 2048)   */
    bool use_mmap;           /* memory-map model (default: true)     */
} neuronos_model_params_t;

neuronos_model_t * neuronos_model_load(neuronos_engine_t * engine, neuronos_model_params_t params);

void neuronos_model_free(neuronos_model_t * model);

/* Model info */
typedef struct {
    const char * description; /* model description string             */
    int64_t n_params;         /* number of parameters                 */
    int64_t model_size;       /* size in bytes                        */
    int n_vocab;              /* vocabulary size                      */
    int n_ctx_train;          /* training context length              */
    int n_embd;               /* embedding dimension                  */
} neuronos_model_info_t;

neuronos_model_info_t neuronos_model_info(const neuronos_model_t * model);

/* ============================================================
 * GENERATE: Text generation (inference)
 * ============================================================ */

/* Streaming callback: called for each generated token.
 * Return false to stop generation early. */
typedef bool (*neuronos_token_cb)(const char * token_text, void * user_data);

typedef struct {
    const char * prompt;        /* input text                   */
    int max_tokens;             /* max tokens to generate (256) */
    float temperature;          /* 0.0 = greedy (default: 0.7)  */
    float top_p;                /* nucleus sampling (0.95)      */
    int top_k;                  /* top-k sampling (40)          */
    float repeat_penalty;       /* repetition penalty (1.1); 1.0 = off */
    int repeat_last_n;          /* window for repeat penalty (64)      */
    const char * grammar;       /* GBNF grammar or NULL         */
    const char * grammar_root;  /* grammar root rule ("root")   */
    neuronos_token_cb on_token; /* stream callback or NULL      */
    void * user_data;           /* passed to callback           */
    uint32_t seed;              /* RNG seed; 0 = random         */
} neuronos_gen_params_t;

typedef struct {
    char * text;              /* generated text (caller must free) */
    int n_tokens;             /* tokens generated                  */
    double elapsed_ms;        /* total generation time             */
    double tokens_per_s;      /* tokens/second                     */
    neuronos_status_t status; /* NEURONOS_OK or error              */
} neuronos_gen_result_t;

/* Generate text from a prompt */
neuronos_gen_result_t neuronos_generate(neuronos_model_t * model, neuronos_gen_params_t params);

/* Free a generation result */
void neuronos_gen_result_free(neuronos_gen_result_t * result);

/* ============================================================
 * CHAT TEMPLATE: Format messages using model's chat template
 *
 * Wraps llama_chat_apply_template() to produce correctly
 * formatted prompts (e.g. Llama-3 <|start_header_id|> format).
 *
 * The template is auto-detected from the model's GGUF metadata.
 * Pass a custom tmpl string to override (e.g. "chatml", "llama3").
 * ============================================================ */

typedef struct {
    const char * role;    /* "system", "user", or "assistant"     */
    const char * content; /* message text                          */
} neuronos_chat_msg_t;

/* Format an array of chat messages into a prompt string.
 *
 * @param model                  Loaded model (reads chat template from GGUF)
 * @param tmpl                   Custom template name or NULL for auto-detect
 * @param messages               Array of chat messages
 * @param n_messages             Number of messages
 * @param add_generation_prompt  Append assistant turn prefix at the end
 * @param out_text               Output: caller must free with neuronos_free()
 *
 * @return NEURONOS_OK on success.
 */
neuronos_status_t neuronos_chat_format(const neuronos_model_t * model, const char * tmpl,
                                       const neuronos_chat_msg_t * messages, size_t n_messages,
                                       bool add_generation_prompt, char ** out_text);

/* ============================================================
 * TOOL SYSTEM: Register and execute tools
 * ============================================================ */

/* Tool result */
typedef struct {
    char * output; /* tool output text (caller must free with neuronos_free) */
    bool success;
    char * error; /* error message if !success (caller must free)           */
} neuronos_tool_result_t;

/* Tool function signature */
typedef neuronos_tool_result_t (*neuronos_tool_fn_t)(const char * args_json, void * user_data);

/* Capability flags for sandboxing */
#define NEURONOS_CAP_FILESYSTEM (1u << 0)
#define NEURONOS_CAP_NETWORK (1u << 1)
#define NEURONOS_CAP_SHELL (1u << 2)
#define NEURONOS_CAP_MEMORY (1u << 3)
#define NEURONOS_CAP_SENSOR (1u << 4)
#define NEURONOS_CAP_GPIO (1u << 5)
#define NEURONOS_CAP_ALL (0xFFFFFFFFu)

/* Tool descriptor */
typedef struct {
    const char * name;             /* e.g. "shell", "read_file"    */
    const char * description;      /* human description for prompt */
    const char * args_schema_json; /* JSON Schema for arguments    */
    neuronos_tool_fn_t execute;    /* function pointer             */
    void * user_data;              /* passed to execute()          */
    uint32_t required_caps;        /* NEURONOS_CAP_* flags         */
} neuronos_tool_desc_t;

/* Create/free tool registry */
neuronos_tool_registry_t * neuronos_tool_registry_create(void);
void neuronos_tool_registry_free(neuronos_tool_registry_t * reg);

/* Register a tool. Returns 0 on success. */
int neuronos_tool_register(neuronos_tool_registry_t * reg, const neuronos_tool_desc_t * desc);

/* Register default built-in tools (shell, read_file, write_file, calculate).
 * Only registers tools whose required_caps are within allowed_caps. */
int neuronos_tool_register_defaults(neuronos_tool_registry_t * reg, uint32_t allowed_caps);

/* Execute a tool by name */
neuronos_tool_result_t neuronos_tool_execute(neuronos_tool_registry_t * reg, const char * tool_name,
                                             const char * args_json);

/* Free tool result strings */
void neuronos_tool_result_free(neuronos_tool_result_t * result);

/* Get number of registered tools */
int neuronos_tool_count(const neuronos_tool_registry_t * reg);

/* Get tool name by index (for grammar generation) */
const char * neuronos_tool_name(const neuronos_tool_registry_t * reg, int index);

/* Get tool description by index (for MCP) */
const char * neuronos_tool_description(const neuronos_tool_registry_t * reg, int index);

/* Get tool JSON schema by index (for MCP) */
const char * neuronos_tool_schema(const neuronos_tool_registry_t * reg, int index);

/* Generate GBNF grammar rule for registered tool names */
char * neuronos_tool_grammar_names(const neuronos_tool_registry_t * reg);

/* Generate tool descriptions for injection into system prompt */
char * neuronos_tool_prompt_description(const neuronos_tool_registry_t * reg);

/* ============================================================
 * AGENT: ReAct agent loop
 * ============================================================ */

typedef struct {
    int max_steps;           /* max think-act-observe cycles (10) */
    int max_tokens_per_step; /* max tokens per gen step (512)     */
    float temperature;       /* sampling temperature (0.7)        */
    int context_budget;      /* max context tokens before compress */
    bool verbose;            /* print steps to stderr             */
} neuronos_agent_params_t;

/* Step callback: called after each think-act-observe cycle */
typedef void (*neuronos_agent_step_cb)(int step, const char * thought,
                                       const char * action, /* tool name or "final_answer" */
                                       const char * observation, void * user_data);

typedef struct {
    char * text; /* final answer (caller frees) */
    int steps_taken;
    double total_ms;
    neuronos_status_t status;
} neuronos_agent_result_t;

/* Create an agent with a model, tools, and params */
neuronos_agent_t * neuronos_agent_create(neuronos_model_t * model, neuronos_tool_registry_t * tools,
                                         neuronos_agent_params_t params);

void neuronos_agent_free(neuronos_agent_t * agent);

/* Run the agent on a user query */
neuronos_agent_result_t neuronos_agent_run(neuronos_agent_t * agent, const char * user_input,
                                           neuronos_agent_step_cb on_step, void * user_data);

void neuronos_agent_result_free(neuronos_agent_result_t * result);

/* Set system prompt (default is built-in ReAct prompt) */
void neuronos_agent_set_system_prompt(neuronos_agent_t * agent, const char * system_prompt);

/* ============================================================
 * MEMORY: Persistent key-value store
 * ============================================================ */

/* Open memory store (NULL path = in-memory only) */
neuronos_memory_t * neuronos_memory_open(const char * db_path);
void neuronos_memory_close(neuronos_memory_t * mem);

int neuronos_memory_store(neuronos_memory_t * mem, const char * key, const char * value);
char * neuronos_memory_recall(neuronos_memory_t * mem, const char * key); /* caller must free */
int neuronos_memory_search(neuronos_memory_t * mem, const char * query, char *** results, int * n_results,
                           int max_results);
void neuronos_memory_free_results(char ** results, int n);

/* ============================================================
 * CONVENIENCE: One-shot agent
 * ============================================================ */

/* Quick agent: init + load + register defaults + run + cleanup.
 * Caller must free returned string. */
char * neuronos_quick_agent(const char * model_path, const char * prompt, int max_steps);

/* Generic free (for any neuronos-allocated string) */
void neuronos_free(void * ptr);

/* ============================================================
 * HARDWARE DETECTION
 * ============================================================ */

typedef struct {
    /* CPU */
    char cpu_name[128];   /* CPU model string                    */
    char arch[32];        /* "x86_64", "aarch64", "riscv64"...   */
    int n_cores_physical; /* Physical cores                      */
    int n_cores_logical;  /* Logical cores (with HT)             */
    uint32_t features;    /* Bitmask of HAL features             */

    /* Memory */
    int64_t ram_total_mb;     /* Total system RAM in MB              */
    int64_t ram_available_mb; /* Available RAM in MB                 */

    /* GPU (future) */
    int64_t gpu_vram_mb; /* 0 = no GPU detected                */
    char gpu_name[128];  /* GPU model string                    */

    /* Budget for model loading (RAM - OS overhead - safety margin) */
    int64_t model_budget_mb; /* Max MB available for model          */
} neuronos_hw_info_t;

/* Detect hardware capabilities */
neuronos_hw_info_t neuronos_detect_hardware(void);

/* Print hardware info to stderr */
void neuronos_hw_print_info(const neuronos_hw_info_t * hw);

/* ============================================================
 * MODEL SCANNER & AUTO-SELECTION
 * ============================================================ */

/* Quantization type detected from filename heuristics */
typedef enum {
    NEURONOS_QUANT_UNKNOWN = 0,
    NEURONOS_QUANT_I2_S,       /* BitNet ternary 1.58-bit           */
    NEURONOS_QUANT_TL1,        /* BitNet TL1 LUT kernel             */
    NEURONOS_QUANT_Q2_K,       /* 2-bit k-quant                     */
    NEURONOS_QUANT_Q3_K,       /* 3-bit k-quant                     */
    NEURONOS_QUANT_Q4_0,       /* 4-bit legacy                      */
    NEURONOS_QUANT_Q4_K_M,     /* 4-bit k-quant medium              */
    NEURONOS_QUANT_Q5_K_M,     /* 5-bit k-quant medium              */
    NEURONOS_QUANT_Q6_K,       /* 6-bit k-quant                     */
    NEURONOS_QUANT_Q8_0,       /* 8-bit                             */
    NEURONOS_QUANT_F16,        /* float16                           */
} neuronos_quant_type_t;

typedef struct {
    char path[512];              /* Absolute path to .gguf file        */
    char name[128];              /* Model name (from filename)         */
    int64_t file_size_mb;        /* File size in MB                    */
    int64_t est_ram_mb;          /* Estimated RAM needed (file + ctx)  */
    int64_t n_params_est;        /* Estimated params (from file size)  */
    float score;                 /* Auto-computed suitability score     */
    bool fits_in_ram;            /* Can load with available RAM?       */
    neuronos_quant_type_t quant; /* Detected quantization type         */
    bool is_ternary;             /* True if I2_S / TL1 / 1.58-bit     */
} neuronos_model_entry_t;

/* Scan a directory recursively for .gguf model files.
 * Returns array of entries sorted by score (best first).
 * Caller must free with neuronos_model_scan_free(). */
neuronos_model_entry_t * neuronos_model_scan(const char * dir_path, const neuronos_hw_info_t * hw, int * out_count);

void neuronos_model_scan_free(neuronos_model_entry_t * entries, int count);

/* Select the best model from a scan result.
 * Returns pointer into the entries array (do not free separately).
 * Returns NULL if no model fits. */
const neuronos_model_entry_t * neuronos_model_select_best(const neuronos_model_entry_t * entries, int count);

/* ============================================================
 * CONTEXT COMPACTION (inspired by Claude Code / OpenClaw)
 *
 * When context usage exceeds threshold, older conversation
 * exchanges are summarized to free tokens.
 * Pattern: "treat LLM context as cache, disk as source of truth"
 * ============================================================ */

typedef struct {
    float trigger_ratio;    /* Compact when ctx usage > ratio (0.85)  */
    int retention_window;   /* Keep last N exchanges verbatim (6)     */
    int max_summary_tokens; /* Max tokens for the summary (256)       */
    bool auto_compact;      /* Auto-trigger during agent_run (true)   */
} neuronos_compact_params_t;

/* Get current context token usage */
int neuronos_context_token_count(const neuronos_agent_t * agent);

/* Get context capacity */
int neuronos_context_capacity(const neuronos_agent_t * agent);

/* Get context usage ratio (0.0 - 1.0) */
float neuronos_context_usage_ratio(const neuronos_agent_t * agent);

/* ============================================================
 * AUTO-TUNING: Optimal parameters for maximum performance
 *
 * Detects hardware → computes optimal n_threads, n_batch,
 * n_ctx, flash_attn, mmap, mlock for the fastest inference.
 * ============================================================ */

typedef struct {
    int n_threads;    /* Optimal thread count (physical cores)    */
    int n_batch;      /* Batch size for prompt processing          */
    int n_ctx;        /* Context size (max tokens in conversation) */
    bool flash_attn;  /* Enable flash attention if supported       */
    bool use_mmap;    /* Memory-map model file (always true)       */
    bool use_mlock;   /* Lock model in RAM (if enough headroom)    */
    int n_gpu_layers; /* GPU layers to offload (0 = CPU only)      */
} neuronos_tuned_params_t;

/* Auto-compute optimal parameters for a given model+hardware combo */
neuronos_tuned_params_t neuronos_auto_tune(const neuronos_hw_info_t * hw, const neuronos_model_entry_t * model);

/* Print tuned parameters to stderr */
void neuronos_tune_print(const neuronos_tuned_params_t * params);

/* ============================================================
 * ZERO-ARG LAUNCHER: Full auto-config pipeline
 *
 * neuronos_auto_launch() does everything:
 *   1. Detect hardware
 *   2. Scan multiple model search paths
 *   3. Select best model for hardware
 *   4. Compute optimal parameters
 *   5. Initialize engine + load model
 *
 * Returns a fully ready model+engine pair.
 * ============================================================ */

typedef struct {
    neuronos_engine_t * engine;
    neuronos_model_t * model;
    neuronos_hw_info_t hw;
    neuronos_tuned_params_t tuning;
    neuronos_model_entry_t selected_model;
    neuronos_status_t status;
} neuronos_auto_ctx_t;

/* Model search path list (NULL-terminated) */
#define NEURONOS_MAX_SEARCH_PATHS 8

/* Auto-launch: detect → scan → select → tune → load → ready.
 * extra_model_dirs is a NULL-terminated list of additional search paths.
 * Pass NULL to use only default search paths.                       */
neuronos_auto_ctx_t neuronos_auto_launch(const char ** extra_model_dirs, bool verbose);

/* Release auto context */
void neuronos_auto_release(neuronos_auto_ctx_t * ctx);

/* ============================================================
 * HTTP SERVER (OpenAI-compatible API)
 *
 * Enables: desktop (Tauri/Electron), browser, mobile,
 *          VSCode Copilot, any OpenAI client.
 *
 * Endpoints:
 *   POST /v1/chat/completions
 *   POST /v1/completions
 *   GET  /v1/models
 *   GET  /health
 * ============================================================ */

typedef struct {
    const char * host; /* "0.0.0.0" or "127.0.0.1" (default)     */
    int port;          /* Default: 8080                           */
    bool cors;         /* Enable CORS for browser clients          */
} neuronos_server_params_t;

/* Start HTTP server (blocking). Returns status on exit. */
neuronos_status_t neuronos_server_start(neuronos_model_t * model, neuronos_tool_registry_t * tools,
                                        neuronos_server_params_t params);

/* ============================================================
 * MCP SERVER (Model Context Protocol — STDIO transport)
 *
 * Exposes NeuronOS tools to any MCP client:
 *   - Claude Desktop
 *   - VS Code / GitHub Copilot
 *   - Cursor, Windsurf, etc.
 *
 * Protocol: JSON-RPC 2.0 over stdin/stdout
 * Spec: 2025-11-25
 *
 * First MCP server in pure C.
 * ============================================================ */

/* Start MCP server on STDIO (blocking). Reads JSON-RPC from stdin,
 * writes responses to stdout. Logging goes to stderr.
 * Returns NEURONOS_OK when stdin is closed. */
neuronos_status_t neuronos_mcp_serve_stdio(neuronos_tool_registry_t * tools);

#ifdef __cplusplus
}
#endif

#endif /* NEURONOS_H */
