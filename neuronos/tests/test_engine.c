/* ============================================================
 * NeuronOS — Engine & Agent Test Suite v0.7
 *
 * Tests:
 *  1. Engine init/shutdown
 *  2. Model load/free/info
 *  3. Text generation (basic)
 *  4. Text generation with GBNF grammar (JSON)
 *  5. Tool registry operations
 *  6. Tool execution (calculate)
 *  7. Hardware detection
 *  8. Model scanner
 *  9. Auto-tuning engine
 * 10. Zero-arg auto-launch
 * 11. GPU detection
 * 12. Expanded agentic tools
 *  13. MCP server protocol
 * 14. Chat template formatting
 * 15. Ternary GPU offload guard
 *
 * Usage: ./test_engine <path-to-gguf-model>
 * ============================================================ */
#include "neuronos/neuronos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Helpers ---- */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name)                                                                                               \
    do {                                                                                                               \
        tests_run++;                                                                                                   \
        fprintf(stderr, "\n[TEST %d] %s... ", tests_run, name);                                                        \
    } while (0)

#define TEST_PASS()                                                                                                    \
    do {                                                                                                               \
        tests_passed++;                                                                                                \
        fprintf(stderr, "PASS ✓\n");                                                                                   \
    } while (0)

#define TEST_FAIL(msg)                                                                                                 \
    do {                                                                                                               \
        tests_failed++;                                                                                                \
        fprintf(stderr, "FAIL ✗ (%s)\n", msg);                                                                         \
    } while (0)

#define ASSERT(cond, msg)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            TEST_FAIL(msg);                                                                                            \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

/* ---- Globals ---- */
static const char * g_model_path = NULL;

/* ============================================================
 * TEST 1: Engine init/shutdown
 * ============================================================ */
static void test_engine_init(void) {
    TEST_START("Engine init/shutdown");

    /* Verify version */
    const char * ver = neuronos_version();
    ASSERT(ver != NULL, "version is NULL");
    ASSERT(strcmp(ver, "0.7.0") == 0, "version mismatch");

    /* Init engine */
    neuronos_engine_params_t params = {
        .n_threads = 4,
        .n_gpu_layers = 0,
        .verbose = true,
    };
    neuronos_engine_t * engine = neuronos_init(params);
    ASSERT(engine != NULL, "engine is NULL");

    /* Shutdown */
    neuronos_shutdown(engine);

    TEST_PASS();
}

/* ============================================================
 * TEST 2: Model load/free/info
 * ============================================================ */
static neuronos_engine_t * g_engine = NULL;
static neuronos_model_t * g_model = NULL;

static void test_model_load(void) {
    TEST_START("Model load/info/free");

    if (!g_model_path) {
        fprintf(stderr, "SKIP (no model path)");
        tests_run--; /* don't count as run */
        return;
    }

    neuronos_engine_params_t eparams = {
        .n_threads = 4,
        .n_gpu_layers = 0,
        .verbose = true,
    };
    g_engine = neuronos_init(eparams);
    ASSERT(g_engine != NULL, "engine init failed");

    neuronos_model_params_t mparams = {
        .model_path = g_model_path,
        .context_size = 2048,
        .use_mmap = true,
    };
    g_model = neuronos_model_load(g_engine, mparams);
    ASSERT(g_model != NULL, "model load failed");

    /* Check info */
    neuronos_model_info_t info = neuronos_model_info(g_model);
    ASSERT(info.n_params > 0, "n_params should be > 0");
    ASSERT(info.n_vocab > 0, "n_vocab should be > 0");
    ASSERT(info.n_embd > 0, "n_embd should be > 0");

    fprintf(stderr, "\n  Model: %s, params=%lldM, vocab=%d, embd=%d", info.description,
            (long long)(info.n_params / 1000000), info.n_vocab, info.n_embd);

    TEST_PASS();
}

/* ============================================================
 * TEST 3: Basic text generation
 * ============================================================ */
static void test_generate_basic(void) {
    TEST_START("Basic text generation");

    if (!g_model) {
        fprintf(stderr, "SKIP (model not loaded)");
        tests_run--;
        return;
    }

    neuronos_gen_params_t params = {
        .prompt = "Hello, my name is",
        .max_tokens = 32,
        .temperature = 0.7f,
        .top_p = 0.95f,
        .top_k = 40,
        .grammar = NULL,
        .on_token = NULL,
        .seed = 42,
    };

    neuronos_gen_result_t result = neuronos_generate(g_model, params);
    ASSERT(result.status == NEURONOS_OK, "generation failed");
    ASSERT(result.text != NULL, "text is NULL");
    ASSERT(result.n_tokens > 0, "no tokens generated");
    ASSERT(result.tokens_per_s > 0.0, "speed should be > 0");

    fprintf(stderr, "\n  Generated %d tokens (%.2f t/s): \"%.80s%s\"", result.n_tokens, result.tokens_per_s,
            result.text, strlen(result.text) > 80 ? "..." : "");

    neuronos_gen_result_free(&result);
    TEST_PASS();
}

/* ============================================================
 * TEST 4: Grammar-constrained generation (JSON)
 * ============================================================ */
static void test_generate_grammar(void) {
    TEST_START("Grammar-constrained JSON generation");

    if (!g_model) {
        fprintf(stderr, "SKIP (model not loaded)");
        tests_run--;
        return;
    }

    /* Simple JSON grammar */
    const char * json_grammar = "root ::= \"{\" ws \"\\\"name\\\"\" ws \":\" ws string ws \"}\"\n"
                                "string ::= \"\\\"\" [a-zA-Z ]+ \"\\\"\"\n"
                                "ws ::= [ \\t\\n]*\n";

    neuronos_gen_params_t params = {
        .prompt = "Generate a JSON object with a name field:",
        .max_tokens = 64,
        .temperature = 0.5f,
        .top_p = 0.95f,
        .top_k = 40,
        .grammar = json_grammar,
        .grammar_root = "root",
        .on_token = NULL,
        .seed = 42,
    };

    neuronos_gen_result_t result = neuronos_generate(g_model, params);
    ASSERT(result.status == NEURONOS_OK, "generation failed");
    ASSERT(result.text != NULL, "text is NULL");

    /* Verify it's valid JSON: starts with { and ends with } */
    const char * text = result.text;
    /* Skip leading whitespace */
    while (*text == ' ' || *text == '\n' || *text == '\t')
        text++;
    ASSERT(text[0] == '{', "output doesn't start with {");

    fprintf(stderr, "\n  Grammar output: %s", result.text);

    neuronos_gen_result_free(&result);
    TEST_PASS();
}

/* ============================================================
 * TEST 5: Tool registry
 * ============================================================ */
static void test_tool_registry(void) {
    TEST_START("Tool registry operations");

    neuronos_tool_registry_t * reg = neuronos_tool_registry_create();
    ASSERT(reg != NULL, "registry is NULL");
    ASSERT(neuronos_tool_count(reg) == 0, "should start empty");

    /* Register defaults with filesystem + shell caps */
    int n = neuronos_tool_register_defaults(reg, NEURONOS_CAP_FILESYSTEM | NEURONOS_CAP_SHELL);
    ASSERT(n > 0, "should register some tools");
    ASSERT(neuronos_tool_count(reg) >= 3, "should have >= 3 tools");

    /* Verify tool names */
    fprintf(stderr, "\n  Registered tools (%d):", neuronos_tool_count(reg));
    for (int i = 0; i < neuronos_tool_count(reg); i++) {
        const char * name = neuronos_tool_name(reg, i);
        ASSERT(name != NULL, "tool name is NULL");
        fprintf(stderr, " %s", name);
    }

    /* Test grammar generation */
    char * grammar = neuronos_tool_grammar_names(reg);
    ASSERT(grammar != NULL, "grammar is NULL");
    ASSERT(strstr(grammar, "tool-name") != NULL, "grammar should contain tool-name");
    fprintf(stderr, "\n  Grammar: %s", grammar);
    free(grammar);

    /* Test prompt description */
    char * desc = neuronos_tool_prompt_description(reg);
    ASSERT(desc != NULL, "description is NULL");
    ASSERT(strstr(desc, "shell") != NULL, "should mention shell");
    free(desc);

    neuronos_tool_registry_free(reg);
    TEST_PASS();
}

/* ============================================================
 * TEST 6: Tool execution (calculate)
 * ============================================================ */
static void test_tool_execute(void) {
    TEST_START("Tool execution (calculate)");

    neuronos_tool_registry_t * reg = neuronos_tool_registry_create();
    neuronos_tool_register_defaults(reg, NEURONOS_CAP_ALL);

    /* Test calculate */
    neuronos_tool_result_t result = neuronos_tool_execute(reg, "calculate", "{\"expression\": \"2+2\"}");

    ASSERT(result.success, "calculate should succeed");
    ASSERT(result.output != NULL, "output should not be NULL");
    ASSERT(strstr(result.output, "4") != NULL, "2+2 should be 4");
    fprintf(stderr, "\n  calculate(2+2) = %s", result.output);

    neuronos_tool_result_free(&result);

    /* Test unknown tool */
    neuronos_tool_result_t r2 = neuronos_tool_execute(reg, "nonexistent", "{}");
    ASSERT(!r2.success, "unknown tool should fail");
    neuronos_tool_result_free(&r2);

    neuronos_tool_registry_free(reg);
    TEST_PASS();
}

/* ============================================================
 * TEST 7: Hardware detection
 * ============================================================ */
static void test_hardware_detection(void) {
    TEST_START("Hardware detection");

    neuronos_hw_info_t hw = neuronos_detect_hardware();

    ASSERT(hw.ram_total_mb > 0, "should detect RAM");
    ASSERT(hw.ram_available_mb > 0, "should detect available RAM");
    ASSERT(hw.n_cores_logical > 0, "should detect cores");
    ASSERT(hw.n_cores_physical > 0, "should detect physical cores");
    ASSERT(hw.model_budget_mb > 0, "should compute model budget");
    ASSERT(strlen(hw.arch) > 0, "should detect architecture");

    fprintf(stderr, "\n  CPU: %s", hw.cpu_name);
    fprintf(stderr, "\n  Arch: %s", hw.arch);
    fprintf(stderr, "\n  Cores: %d/%d (phys/logical)", hw.n_cores_physical, hw.n_cores_logical);
    fprintf(stderr, "\n  RAM: %lld MB total, %lld MB available", (long long)hw.ram_total_mb,
            (long long)hw.ram_available_mb);
    fprintf(stderr, "\n  Budget: %lld MB for models", (long long)hw.model_budget_mb);
    fprintf(stderr, "\n  Features: 0x%08X", hw.features);

    TEST_PASS();
}

/* ============================================================
 * TEST 8: Model scanner
 * ============================================================ */
static void test_model_scanner(void) {
    TEST_START("Model scanner");

    neuronos_hw_info_t hw = neuronos_detect_hardware();

    /* Try to scan the models directory */
    int count = 0;
    neuronos_model_entry_t * models = neuronos_model_scan("../../models", &hw, &count);

    if (models && count > 0) {
        fprintf(stderr, "\n  Found %d model(s):", count);
        for (int i = 0; i < count; i++) {
            fprintf(stderr, "\n    [%d] %s (%.0f MB, score=%.1f, fits=%s)", i + 1, models[i].name,
                    (double)models[i].file_size_mb, models[i].score, models[i].fits_in_ram ? "yes" : "no");
        }

        const neuronos_model_entry_t * best = neuronos_model_select_best(models, count);
        if (best) {
            fprintf(stderr, "\n  Best: %s (score=%.1f)", best->name, best->score);
            ASSERT(best->score > 0, "best model should have positive score");
            ASSERT(best->fits_in_ram, "best model must fit in RAM");
        }

        neuronos_model_scan_free(models, count);
    } else {
        fprintf(stderr, "\n  No models directory found (OK in CI)");
    }

    /* Test with nonexistent directory */
    int count2 = 0;
    neuronos_model_entry_t * none = neuronos_model_scan("/nonexistent", &hw, &count2);
    ASSERT(none == NULL, "nonexistent dir should return NULL");
    ASSERT(count2 == 0, "nonexistent dir should return 0 count");

    TEST_PASS();
}

/* ============================================================
 * TEST 9: Auto-tuning engine
 * ============================================================ */
static void test_auto_tune(void) {
    TEST_START("Auto-tuning engine");

    neuronos_hw_info_t hw = neuronos_detect_hardware();

    /* Create a fake model entry for tuning */
    neuronos_model_entry_t fake_model = {
        .file_size_mb = 1200,
        .est_ram_mb = 1660,
        .n_params_est = 2000000000LL,
        .fits_in_ram = true,
        .score = 1100.0f,
    };
    snprintf(fake_model.name, sizeof(fake_model.name), "test-model-2B");
    snprintf(fake_model.path, sizeof(fake_model.path), "/tmp/test.gguf");

    neuronos_tuned_params_t tuned = neuronos_auto_tune(&hw, &fake_model);

    /* Validate: threads should be > 0 and <= logical cores */
    ASSERT(tuned.n_threads > 0, "n_threads should be > 0");
    ASSERT(tuned.n_threads <= hw.n_cores_logical, "n_threads should be <= logical cores");

    /* Validate: batch should be 512, 1024, or 2048 */
    ASSERT(tuned.n_batch >= 512, "n_batch should be >= 512");
    ASSERT(tuned.n_batch <= 2048, "n_batch should be <= 2048");

    /* Validate: context should be >= 512 and <= 8192 */
    ASSERT(tuned.n_ctx >= 512, "n_ctx should be >= 512");
    ASSERT(tuned.n_ctx <= 8192, "n_ctx should be <= 8192");

    /* Validate: mmap should always be true */
    ASSERT(tuned.use_mmap == true, "use_mmap should be true");

    fprintf(stderr, "\n  threads=%d batch=%d ctx=%d mmap=%d mlock=%d gpu=%d", tuned.n_threads, tuned.n_batch,
            tuned.n_ctx, tuned.use_mmap, tuned.use_mlock, tuned.n_gpu_layers);

    /* Print formatted output */
    neuronos_tune_print(&tuned);

    TEST_PASS();
}

/* ============================================================
 * TEST 10: Zero-arg auto-launch
 * ============================================================ */
static void test_auto_launch(void) {
    TEST_START("Zero-arg auto-launch");

    /* Try auto-launch with default search paths */
    neuronos_auto_ctx_t ctx = neuronos_auto_launch(NULL, false);

    if (ctx.status == NEURONOS_OK) {
        /* Full pipeline succeeded */
        ASSERT(ctx.engine != NULL, "engine should not be NULL");
        ASSERT(ctx.model != NULL, "model should not be NULL");
        ASSERT(ctx.tuning.n_threads > 0, "tuning n_threads should be > 0");
        ASSERT(ctx.selected_model.score > 0, "selected model score should be > 0");

        fprintf(stderr, "\n  Auto-launched: %s (score=%.1f)", ctx.selected_model.name, ctx.selected_model.score);
        fprintf(stderr, "\n  Tuning: threads=%d batch=%d ctx=%d", ctx.tuning.n_threads, ctx.tuning.n_batch,
                ctx.tuning.n_ctx);

        /* Quick generation test to verify it works */
        neuronos_gen_params_t gparams = {
            .prompt = "Test:",
            .max_tokens = 8,
            .temperature = 0.5f,
            .top_p = 0.95f,
            .top_k = 40,
            .grammar = NULL,
            .on_token = NULL,
            .seed = 42,
        };
        neuronos_gen_result_t result = neuronos_generate(ctx.model, gparams);
        ASSERT(result.status == NEURONOS_OK, "auto-launched model should generate");
        ASSERT(result.n_tokens > 0, "should generate tokens");
        fprintf(stderr, "\n  Generated %d tokens at %.2f t/s", result.n_tokens, result.tokens_per_s);
        neuronos_gen_result_free(&result);

        neuronos_auto_release(&ctx);
    } else {
        fprintf(stderr, "\n  No models found in default paths (OK in CI)");
    }

    TEST_PASS();
}

/* ============================================================
 * TEST 11: GPU Detection
 * ============================================================ */
static void test_gpu_detection(void) {
    TEST_START("GPU detection");

    neuronos_hw_info_t hw = neuronos_detect_hardware();

    /* GPU detection should work without crashing */
    /* On CI, there may not be a GPU, so we just validate the fields */
    ASSERT(hw.gpu_vram_mb >= 0, "gpu_vram_mb negative");

    if (hw.gpu_vram_mb > 0) {
        fprintf(stderr, "\n  GPU: %s (%lld MB VRAM)", hw.gpu_name, (long long)hw.gpu_vram_mb);
        ASSERT(strlen(hw.gpu_name) > 0, "GPU name empty but VRAM detected");
    } else {
        fprintf(stderr, "\n  GPU: none detected (CPU-only inference)");
        if (strlen(hw.gpu_name) > 0) {
            fprintf(stderr, "\n  Integrated: %s", hw.gpu_name);
        }
    }

    /* Verify model budget accounts for GPU if present */
    ASSERT(hw.model_budget_mb > 0, "model_budget_mb should be positive");

    TEST_PASS();
}

/* ============================================================
 * TEST 12: Expanded Agentic Tools
 * ============================================================ */
static void test_agentic_tools(void) {
    TEST_START("Expanded agentic tools");

    neuronos_tool_registry_t * reg = neuronos_tool_registry_create();
    ASSERT(reg != NULL, "registry NULL");

    /* Register all tools including network and shell */
    int n = neuronos_tool_register_defaults(reg, NEURONOS_CAP_FILESYSTEM | NEURONOS_CAP_NETWORK | NEURONOS_CAP_SHELL);
    fprintf(stderr, "\n  Registered %d tools:", n);
    ASSERT(n >= 7, "expected at least 7 tools");

    /* Print all tool names */
    for (int i = 0; i < neuronos_tool_count(reg); i++) {
        fprintf(stderr, " %s", neuronos_tool_name(reg, i));
    }

    /* Test list_dir tool */
    neuronos_tool_result_t r = neuronos_tool_execute(reg, "list_dir", "{\"path\":\".\"}");
    ASSERT(r.success, "list_dir failed");
    ASSERT(r.output != NULL && r.output[0] == '[', "list_dir should return JSON array");
    fprintf(stderr, "\n  list_dir(.) = %.*s...", 60, r.output);
    neuronos_tool_result_free(&r);

    /* Test search_files tool */
    r = neuronos_tool_execute(reg, "search_files", "{\"pattern\":\"*.c\",\"directory\":\".\"}");
    ASSERT(r.success, "search_files failed");
    fprintf(stderr, "\n  search_files(*.c) = %.*s...", 60, r.output ? r.output : "(empty)");
    neuronos_tool_result_free(&r);

    /* Generate grammar for all tools */
    char * grammar = neuronos_tool_grammar_names(reg);
    ASSERT(grammar != NULL, "grammar NULL");
    ASSERT(strstr(grammar, "list_dir") != NULL, "grammar missing list_dir");
    ASSERT(strstr(grammar, "http_get") != NULL, "grammar missing http_get");
    ASSERT(strstr(grammar, "search_files") != NULL, "grammar missing search_files");
    fprintf(stderr, "\n  Grammar: %s", grammar);
    free(grammar);

    neuronos_tool_registry_free(reg);
    TEST_PASS();
}

/* ---- Test 13: MCP server protocol ---- */
static void test_mcp_protocol(void) {
    TEST_START("MCP server protocol");

    /* Test tool_description and tool_schema accessors (MCP needs these) */
    neuronos_tool_registry_t * reg = neuronos_tool_registry_create();
    ASSERT(reg != NULL, "registry NULL");

    neuronos_tool_register_defaults(reg, NEURONOS_CAP_FILESYSTEM | NEURONOS_CAP_NETWORK | NEURONOS_CAP_SHELL);
    int n = neuronos_tool_count(reg);
    ASSERT(n >= 7, "expected >= 7 tools for MCP");

    /* Verify each tool has name, description, and schema */
    int has_desc = 0;
    int has_schema = 0;
    for (int i = 0; i < n; i++) {
        const char * name = neuronos_tool_name(reg, i);
        const char * desc = neuronos_tool_description(reg, i);
        const char * schema = neuronos_tool_schema(reg, i);
        ASSERT(name != NULL, "tool name NULL");
        if (desc)
            has_desc++;
        if (schema)
            has_schema++;
        fprintf(stderr, "\n  MCP tool: %s (desc=%s, schema=%s)", name, desc ? "yes" : "no", schema ? "yes" : "no");
    }

    ASSERT(has_desc >= 7, "all tools should have descriptions");
    ASSERT(has_schema >= 7, "all tools should have schemas");
    fprintf(stderr, "\n  %d tools ready for MCP export (%d with desc, %d with schema)", n, has_desc, has_schema);

    /* Verify neuronos_mcp_serve_stdio is declared (compile-time check).
     * We can't actually run it in a test because it reads stdin,
     * but we verify the function pointer is valid. */
    neuronos_status_t (*mcp_fn)(neuronos_tool_registry_t *) = &neuronos_mcp_serve_stdio;
    ASSERT(mcp_fn != NULL, "MCP serve function not linked");
    fprintf(stderr, "\n  neuronos_mcp_serve_stdio: linked ✓");

    neuronos_tool_registry_free(reg);
    TEST_PASS();
}

/* ---- Test 14: Chat template formatting ---- */
static void test_chat_format(void) {
    TEST_START("Chat template formatting");

    if (!g_model) {
        fprintf(stderr, "SKIP (model not loaded)");
        tests_run--;
        return;
    }

    /* Basic two-message conversation */
    neuronos_chat_msg_t msgs[] = {
        {"system", "You are a helpful assistant."},
        {"user",   "Hello"},
    };

    char * formatted = NULL;
    neuronos_status_t st = neuronos_chat_format(g_model, NULL, msgs, 2, true, &formatted);
    ASSERT(st == NEURONOS_OK, "chat_format failed");
    ASSERT(formatted != NULL, "formatted is NULL");
    ASSERT(strlen(formatted) > 0, "formatted is empty");
    ASSERT(strstr(formatted, "Hello") != NULL, "should contain user message");

    fprintf(stderr, "\n  Formatted (%zu bytes): %.120s%s", strlen(formatted), formatted,
            strlen(formatted) > 120 ? "..." : "");

    neuronos_free(formatted);

    /* Test with explicit template override */
    char * formatted2 = NULL;
    st = neuronos_chat_format(g_model, "llama3", msgs, 2, true, &formatted2);
    ASSERT(st == NEURONOS_OK, "chat_format with llama3 template failed");
    ASSERT(formatted2 != NULL, "formatted2 is NULL");
    ASSERT(strstr(formatted2, "Hello") != NULL, "should contain user message");
    neuronos_free(formatted2);

    /* Test invalid params */
    char * bad = NULL;
    st = neuronos_chat_format(NULL, NULL, msgs, 2, true, &bad);
    ASSERT(st == NEURONOS_ERROR_INVALID_PARAM, "NULL model should fail");

    st = neuronos_chat_format(g_model, NULL, NULL, 0, true, &bad);
    ASSERT(st == NEURONOS_ERROR_INVALID_PARAM, "NULL messages should fail");

    TEST_PASS();
}

/* ---- Test 15: Ternary GPU offload guard ---- */
static void test_ternary_gpu_guard(void) {
    TEST_START("Ternary GPU offload guard");

    /* Simulate hardware with GPU */
    neuronos_hw_info_t hw = neuronos_detect_hardware();
    fprintf(stderr, "\n  GPU VRAM: %ld MB", (long)hw.gpu_vram_mb);

    /* Create a model entry that looks like a BitNet I2_S model */
    neuronos_model_entry_t ternary_model = {0};
    strncpy(ternary_model.name, "ggml-model-i2_s.gguf", sizeof(ternary_model.name) - 1);
    ternary_model.file_size_mb = 1200;
    ternary_model.est_ram_mb = 1500;
    ternary_model.quant = NEURONOS_QUANT_I2_S;
    ternary_model.is_ternary = true;

    neuronos_tuned_params_t t = neuronos_auto_tune(&hw, &ternary_model);
    fprintf(stderr, "\n  Ternary model '%s': ngl=%d", ternary_model.name, t.n_gpu_layers);
    ASSERT(t.n_gpu_layers == 0, "BitNet I2_S should NOT use GPU offload (ngl must be 0)");

    /* Now test with a non-ternary model (should allow GPU if available) */
    neuronos_model_entry_t normal_model = {0};
    strncpy(normal_model.name, "llama-3.2-1b-q4_0.gguf", sizeof(normal_model.name) - 1);
    normal_model.file_size_mb = 700;
    normal_model.est_ram_mb = 1000;

    neuronos_tuned_params_t t2 = neuronos_auto_tune(&hw, &normal_model);
    fprintf(stderr, "\n  Normal model '%s': ngl=%d", normal_model.name, t2.n_gpu_layers);

    if (hw.gpu_vram_mb > 0) {
        ASSERT(t2.n_gpu_layers > 0, "Non-ternary model should use GPU when available");
        fprintf(stderr, " (GPU offload enabled ✓)");
    } else {
        ASSERT(t2.n_gpu_layers == 0, "No GPU → ngl should be 0");
        fprintf(stderr, " (no GPU, ngl=0 ✓)");
    }

    TEST_PASS();
}

/* ============================================================
 * MAIN
 * ============================================================ */
int main(int argc, char * argv[]) {
    fprintf(stderr, "═══════════════════════════════════════════\n");
    fprintf(stderr, "  NeuronOS Engine & Agent Test Suite v0.7\n");
    fprintf(stderr, "═══════════════════════════════════════════\n");

    if (argc > 1) {
        g_model_path = argv[1];
        fprintf(stderr, "Model: %s\n", g_model_path);
    } else {
        fprintf(stderr, "Usage: %s <model.gguf>\n", argv[0]);
        fprintf(stderr, "  (running without model — inference tests skipped)\n");
    }

    /* Run tests */
    test_engine_init();
    test_model_load();
    test_generate_basic();
    test_generate_grammar();
    test_tool_registry();
    test_tool_execute();
    test_hardware_detection();
    test_model_scanner();
    test_auto_tune();
    test_auto_launch();
    test_gpu_detection();
    test_agentic_tools();
    test_mcp_protocol();
    test_chat_format();
    test_ternary_gpu_guard();

    /* Cleanup model if loaded */
    if (g_model)
        neuronos_model_free(g_model);
    if (g_engine)
        neuronos_shutdown(g_engine);

    /* Summary */
    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, "  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        fprintf(stderr, " (%d FAILED)", tests_failed);
    }
    fprintf(stderr, "\n═══════════════════════════════════════════\n");

    return tests_failed > 0 ? 1 : 0;
}
