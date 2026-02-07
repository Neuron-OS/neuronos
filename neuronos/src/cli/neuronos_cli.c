/* ============================================================
 * NeuronOS CLI v0.5.0 — Universal AI Agent Interface
 *
 * ZERO-ARG: Just run `neuronos` → auto-configures everything.
 * Detects hardware, finds best model, tunes params, starts REPL.
 *
 * Modes:
 *   neuronos                        Interactive REPL (auto-config)
 *   neuronos run "prompt"           One-shot generation
 *   neuronos agent "task"           One-shot agent with tools
 *   neuronos serve                  HTTP server (OpenAI-compatible)
 *   neuronos hwinfo                 Show hardware capabilities
 *   neuronos scan [dir]             Scan for models
 *   neuronos <model.gguf> generate  Legacy mode
 *   neuronos <model.gguf> agent     Legacy mode
 * ============================================================ */
#include "neuronos/neuronos.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <io.h>
    #define isatty _isatty
    #define fileno _fileno
#else
    #include <unistd.h>
#endif

/* ---- Streaming callback: print tokens as they arrive ---- */
static bool stream_token(const char * text, void * user_data) {
    (void)user_data;
    printf("%s", text);
    fflush(stdout);
    return true;
}

/* ---- Agent step callback: show each step ---- */
static void agent_step(int step, const char * thought, const char * action, const char * observation,
                       void * user_data) {
    (void)user_data;
    fprintf(stderr, "\n── Step %d ──\n", step + 1);
    if (thought)
        fprintf(stderr, "  Thought: %s\n", thought);
    if (action)
        fprintf(stderr, "  Action:  %s\n", action);
    if (observation)
        fprintf(stderr, "  Observe: %.200s%s\n", observation, strlen(observation) > 200 ? "..." : "");
}

/* ---- Print banner ---- */
static void print_banner(void) {
    fprintf(stderr,
            "\033[36m"
            "╔══════════════════════════════════════════════╗\n"
            "║  NeuronOS v%-6s — AI Agent Engine          ║\n"
            "║  The fastest AI runtime. Any device.         ║\n"
            "║  Type /help for commands, /quit to exit.     ║\n"
            "╚══════════════════════════════════════════════╝\n"
            "\033[0m",
            NEURONOS_VERSION_STRING);
}

/* ---- Usage ---- */
static void print_usage(const char * prog) {
    fprintf(stderr,
            "NeuronOS v%s — Universal AI Agent Engine\n\n"
            "Usage:\n"
            "  %s                              Auto-config + interactive REPL\n"
            "  %s run \"prompt\"                  One-shot text generation\n"
            "  %s agent \"task\"                  One-shot agent with tools\n"
            "  %s serve [--port 8080]           HTTP server (OpenAI API)\n"
            "  %s hwinfo                        Show hardware capabilities\n"
            "  %s scan [dir]                    Scan for GGUF models\n"
            "\n"
            "Legacy mode:\n"
            "  %s <model.gguf> generate \"text\"  Generate with specific model\n"
            "  %s <model.gguf> agent \"task\"     Agent with specific model\n"
            "  %s <model.gguf> info             Show model info\n"
            "\n"
            "Options:\n"
            "  -t <threads>     Number of threads (default: auto)\n"
            "  -n <tokens>      Max tokens to generate (default: 256)\n"
            "  -s <steps>       Max agent steps (default: 10)\n"
            "  --temp <float>   Temperature (default: 0.7)\n"
            "  --grammar <file> GBNF grammar file\n"
            "  --models <dir>   Additional model search directory\n"
            "  --host <addr>    Server bind address (default: 127.0.0.1)\n"
            "  --port <port>    Server port (default: 8080)\n"
            "  --verbose        Show debug info\n"
            "  --help           Show this help\n",
            NEURONOS_VERSION_STRING, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ---- Load grammar file ---- */
static char * load_grammar_file(const char * path) {
    if (!path)
        return NULL;
    FILE * fp = fopen(path, "r");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char * buf = malloc((size_t)size + 1);
    if (buf) {
        fread(buf, 1, (size_t)size, fp);
        buf[size] = '\0';
    }
    fclose(fp);
    return buf;
}

/* ---- Run generate command ---- */
static int cmd_generate(neuronos_model_t * model, const char * prompt, int max_tokens, float temperature,
                        const char * grammar_file, bool verbose) {
    if (!prompt) {
        fprintf(stderr, "Error: No prompt provided\n");
        return 1;
    }

    char * grammar = load_grammar_file(grammar_file);

    neuronos_gen_params_t gparams = {
        .prompt = prompt,
        .max_tokens = max_tokens,
        .temperature = temperature,
        .top_p = 0.95f,
        .top_k = 40,
        .grammar = grammar,
        .on_token = stream_token,
        .user_data = NULL,
        .seed = 0,
    };

    neuronos_gen_result_t result = neuronos_generate(model, gparams);
    printf("\n");

    if (verbose) {
        fprintf(stderr, "[%d tokens, %.1f ms, %.2f t/s]\n", result.n_tokens, result.elapsed_ms, result.tokens_per_s);
    }

    int rc = (result.status == NEURONOS_OK) ? 0 : 1;
    free(grammar);
    neuronos_gen_result_free(&result);
    return rc;
}

/* ---- Run agent command ---- */
static int cmd_agent(neuronos_model_t * model, const char * prompt, int max_tokens, int max_steps, float temperature,
                     bool verbose) {
    if (!prompt) {
        fprintf(stderr, "Error: No task provided\n");
        return 1;
    }

    neuronos_tool_registry_t * tools = neuronos_tool_registry_create();
    neuronos_tool_register_defaults(tools, NEURONOS_CAP_FILESYSTEM | NEURONOS_CAP_NETWORK | NEURONOS_CAP_SHELL);

    neuronos_agent_params_t aparams = {
        .max_steps = max_steps,
        .max_tokens_per_step = max_tokens,
        .temperature = temperature,
        .verbose = verbose,
    };

    neuronos_agent_t * agent = neuronos_agent_create(model, tools, aparams);
    if (!agent) {
        fprintf(stderr, "Error: Failed to create agent\n");
        neuronos_tool_registry_free(tools);
        return 1;
    }

    fprintf(stderr, "NeuronOS Agent v%s\n", neuronos_version());
    fprintf(stderr, "Task: %s\n", prompt);
    fprintf(stderr, "Tools: %d registered\n", neuronos_tool_count(tools));
    fprintf(stderr, "Running...\n");

    neuronos_agent_result_t result = neuronos_agent_run(agent, prompt, agent_step, NULL);

    if (result.status == NEURONOS_OK && result.text) {
        printf("\n══ Answer ══\n%s\n", result.text);
    } else {
        fprintf(stderr, "\nAgent stopped (status=%d, steps=%d)\n", result.status, result.steps_taken);
    }

    if (verbose) {
        fprintf(stderr, "[%d steps, %.1f ms]\n", result.steps_taken, result.total_ms);
    }

    int rc = (result.status == NEURONOS_OK) ? 0 : 1;
    neuronos_agent_result_free(&result);
    neuronos_agent_free(agent);
    neuronos_tool_registry_free(tools);
    return rc;
}

/* ---- REPL: Interactive Read-Eval-Print Loop ---- */
static void print_repl_help(void) {
    fprintf(stderr, "\033[33m"
                    "Commands:\n"
                    "  /help          Show this help\n"
                    "  /status        Show model & hardware info\n"
                    "  /tools         List registered tools\n"
                    "  /agent <task>  Run agent mode for one task\n"
                    "  /mode <gen|agent>  Switch default mode\n"
                    "  /temp <float>  Set temperature\n"
                    "  /tokens <n>    Set max tokens\n"
                    "  /quit          Exit NeuronOS\n"
                    "\n"
                    "  Any other input → text generation\n"
                    "\033[0m");
}

static int cmd_repl(neuronos_auto_ctx_t * ctx, int max_tokens, int max_steps, float temperature,
                    const char * grammar_file, bool verbose) {
    print_banner();

    fprintf(stderr, "Model: %s (%.1f score)\n", ctx->selected_model.name, ctx->selected_model.score);
    fprintf(stderr, "Threads: %d | Batch: %d | Context: %d\n\n", ctx->tuning.n_threads, ctx->tuning.n_batch,
            ctx->tuning.n_ctx);

    /* Tool registry for agent mode */
    neuronos_tool_registry_t * tools = neuronos_tool_registry_create();
    neuronos_tool_register_defaults(tools, NEURONOS_CAP_FILESYSTEM | NEURONOS_CAP_NETWORK | NEURONOS_CAP_SHELL);

    bool agent_mode = false;
    char line[4096];

    while (1) {
        /* Print prompt */
        if (isatty(fileno(stdin))) {
            if (agent_mode)
                fprintf(stderr, "\033[35mneuronos:agent> \033[0m");
            else
                fprintf(stderr, "\033[32mneuronos> \033[0m");
        }

        /* Read input */
        if (!fgets(line, sizeof(line), stdin)) {
            break; /* EOF */
        }

        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip empty lines */
        if (len == 0)
            continue;

        /* ---- REPL commands ---- */
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0 || strcmp(line, "/q") == 0) {
            fprintf(stderr, "Goodbye.\n");
            break;
        }

        if (strcmp(line, "/help") == 0 || strcmp(line, "/?") == 0) {
            print_repl_help();
            continue;
        }

        if (strcmp(line, "/status") == 0) {
            neuronos_hw_print_info(&ctx->hw);
            neuronos_tune_print(&ctx->tuning);
            neuronos_model_info_t info = neuronos_model_info(ctx->model);
            fprintf(stderr, "Model: %s\n", info.description);
            fprintf(stderr, "Params: %lldM | Vocab: %d | Embd: %d\n", (long long)(info.n_params / 1000000),
                    info.n_vocab, info.n_embd);
            continue;
        }

        if (strcmp(line, "/tools") == 0) {
            int tc = neuronos_tool_count(tools);
            fprintf(stderr, "Registered tools (%d):\n", tc);
            for (int i = 0; i < tc; i++) {
                fprintf(stderr, "  - %s\n", neuronos_tool_name(tools, i));
            }
            continue;
        }

        if (strncmp(line, "/mode ", 6) == 0) {
            const char * mode = line + 6;
            while (*mode == ' ')
                mode++;
            if (strcmp(mode, "agent") == 0) {
                agent_mode = true;
                fprintf(stderr, "Switched to agent mode\n");
            } else if (strcmp(mode, "gen") == 0 || strcmp(mode, "generate") == 0) {
                agent_mode = false;
                fprintf(stderr, "Switched to generation mode\n");
            } else {
                fprintf(stderr, "Unknown mode: %s (use gen or agent)\n", mode);
            }
            continue;
        }

        if (strncmp(line, "/temp ", 6) == 0) {
            temperature = (float)atof(line + 6);
            fprintf(stderr, "Temperature set to %.2f\n", temperature);
            continue;
        }

        if (strncmp(line, "/tokens ", 8) == 0) {
            max_tokens = atoi(line + 8);
            if (max_tokens < 1)
                max_tokens = 1;
            fprintf(stderr, "Max tokens set to %d\n", max_tokens);
            continue;
        }

        /* /agent command: run agent for one task */
        if (strncmp(line, "/agent ", 7) == 0) {
            const char * task = line + 7;
            while (*task == ' ')
                task++;
            cmd_agent(ctx->model, task, max_tokens, max_steps, temperature, verbose);
            continue;
        }

        /* ---- Default: generate or agent mode ---- */
        if (agent_mode) {
            cmd_agent(ctx->model, line, max_tokens, max_steps, temperature, verbose);
        } else {
            char * grammar = load_grammar_file(grammar_file);
            neuronos_gen_params_t gparams = {
                .prompt = line,
                .max_tokens = max_tokens,
                .temperature = temperature,
                .top_p = 0.95f,
                .top_k = 40,
                .grammar = grammar,
                .on_token = stream_token,
                .user_data = NULL,
                .seed = 0,
            };
            neuronos_gen_result_t result = neuronos_generate(ctx->model, gparams);
            printf("\n");
            if (verbose) {
                fprintf(stderr, "[%d tokens, %.1f ms, %.2f t/s]\n", result.n_tokens, result.elapsed_ms,
                        result.tokens_per_s);
            }
            free(grammar);
            neuronos_gen_result_free(&result);
        }
    }

    neuronos_tool_registry_free(tools);
    return 0;
}

/* ============================================================
 * MAIN
 * ============================================================ */
int main(int argc, char * argv[]) {
    /* ---- Parse global options ---- */
    int n_threads = 0;
    int max_tokens = 256;
    int max_steps = 10;
    float temperature = 0.7f;
    const char * grammar_file = NULL;
    const char * extra_models = NULL;
    const char * host = "127.0.0.1";
    int port = 8080;
    bool verbose = false;

    (void)n_threads; /* used in legacy mode */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            n_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_tokens = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            max_steps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--temp") == 0 && i + 1 < argc) {
            temperature = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--grammar") == 0 && i + 1 < argc) {
            grammar_file = argv[++i];
        } else if (strcmp(argv[i], "--models") == 0 && i + 1 < argc) {
            extra_models = argv[++i];
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Find the first positional argument (not an option) */
    const char * command = NULL;
    const char * positional2 = NULL; /* prompt or sub-command */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            /* Skip option + its value */
            if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "-s") == 0 ||
                strcmp(argv[i], "--temp") == 0 || strcmp(argv[i], "--grammar") == 0 ||
                strcmp(argv[i], "--models") == 0 || strcmp(argv[i], "--host") == 0 || strcmp(argv[i], "--port") == 0) {
                i++; /* skip value */
            }
            continue;
        }
        if (!command) {
            command = argv[i];
        } else if (!positional2) {
            positional2 = argv[i];
        } else {
            break;
        }
    }

    /* ════════════════════════════════════════════════════════
     * HWINFO — Hardware detection (no model needed)
     * ════════════════════════════════════════════════════════ */
    if (command && strcmp(command, "hwinfo") == 0) {
        neuronos_hw_info_t hw = neuronos_detect_hardware();
        neuronos_hw_print_info(&hw);
        return 0;
    }

    /* ════════════════════════════════════════════════════════
     * SCAN — Scan models directory
     * ════════════════════════════════════════════════════════ */
    if (command && strcmp(command, "scan") == 0) {
        const char * scan_dir = positional2 ? positional2 : "../../models";

        neuronos_hw_info_t hw = neuronos_detect_hardware();
        fprintf(stderr, "Scanning: %s\n", scan_dir);
        fprintf(stderr, "RAM budget: %lld MB\n\n", (long long)hw.model_budget_mb);

        int count = 0;
        neuronos_model_entry_t * models = neuronos_model_scan(scan_dir, &hw, &count);

        if (!models || count == 0) {
            fprintf(stderr, "No .gguf models found in %s\n", scan_dir);
            return 1;
        }

        printf("%-4s %-40s %8s %8s %10s %7s  %s\n", "Rank", "Name", "Size MB", "RAM MB", "Params", "Score", "Fits?");
        printf("──── ──────────────────────────────────────── ────────"
               " ──────── ────────── ───────  ─────\n");

        for (int i = 0; i < count; i++) {
            const neuronos_model_entry_t * m = &models[i];
            printf("%-4d %-40.40s %7lld %7lld %8lldM %7.1f  %s\n", i + 1, m->name, (long long)m->file_size_mb,
                   (long long)m->est_ram_mb, (long long)(m->n_params_est / 1000000), m->score,
                   m->fits_in_ram ? "YES" : "NO");
        }

        const neuronos_model_entry_t * best = neuronos_model_select_best(models, count);
        if (best) {
            printf("\n★ Best model: %s (score=%.1f)\n", best->name, best->score);
            printf("  Path: %s\n", best->path);
        }

        neuronos_model_scan_free(models, count);
        return 0;
    }

    /* ════════════════════════════════════════════════════════
     * Check if first arg is a .gguf file → Legacy mode
     * ════════════════════════════════════════════════════════ */
    if (command && strlen(command) > 5 && strcmp(command + strlen(command) - 5, ".gguf") == 0) {
        /* Legacy: neuronos <model.gguf> <command> [prompt] */
        const char * model_path = command;
        const char * sub_cmd = positional2 ? positional2 : "info";
        const char * prompt = NULL;

        /* Find prompt: third positional */
        int pcount = 0;
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "-s") == 0 ||
                    strcmp(argv[i], "--temp") == 0 || strcmp(argv[i], "--grammar") == 0 ||
                    strcmp(argv[i], "--models") == 0 || strcmp(argv[i], "--host") == 0 ||
                    strcmp(argv[i], "--port") == 0) {
                    i++;
                }
                continue;
            }
            pcount++;
            if (pcount == 3) {
                prompt = argv[i];
                break;
            }
        }

        neuronos_engine_params_t eparams = {
            .n_threads = n_threads,
            .n_gpu_layers = 0,
            .verbose = verbose,
        };
        neuronos_engine_t * engine = neuronos_init(eparams);
        if (!engine) {
            fprintf(stderr, "Error: Failed to initialize engine\n");
            return 1;
        }

        if (strcmp(sub_cmd, "info") == 0) {
            neuronos_model_params_t mparams = {.model_path = model_path, .context_size = 512, .use_mmap = true};
            neuronos_model_t * model = neuronos_model_load(engine, mparams);
            if (!model) {
                fprintf(stderr, "Error: Failed to load model\n");
                neuronos_shutdown(engine);
                return 1;
            }
            neuronos_model_info_t info = neuronos_model_info(model);
            printf("NeuronOS v%s\n", neuronos_version());
            printf("Model: %s\n", info.description);
            printf("Parameters: %lldM\n", (long long)(info.n_params / 1000000));
            printf("Size: %.1f MB\n", (double)info.model_size / (1024.0 * 1024.0));
            printf("Vocabulary: %d\n", info.n_vocab);
            printf("Context: %d\n", info.n_ctx_train);
            printf("Embedding: %d\n", info.n_embd);
            neuronos_model_free(model);
            neuronos_shutdown(engine);
            return 0;
        }

        neuronos_model_params_t mparams = {.model_path = model_path, .context_size = 2048, .use_mmap = true};
        neuronos_model_t * model = neuronos_model_load(engine, mparams);
        if (!model) {
            fprintf(stderr, "Error: Failed to load model\n");
            neuronos_shutdown(engine);
            return 1;
        }

        int rc = 1;
        if (strcmp(sub_cmd, "generate") == 0)
            rc = cmd_generate(model, prompt, max_tokens, temperature, grammar_file, verbose);
        else if (strcmp(sub_cmd, "agent") == 0)
            rc = cmd_agent(model, prompt, max_tokens, max_steps, temperature, verbose);
        else
            fprintf(stderr, "Unknown command: %s\n", sub_cmd);

        neuronos_model_free(model);
        neuronos_shutdown(engine);
        return rc;
    }

    /* ════════════════════════════════════════════════════════
     * AUTO-CONFIG modes: run, agent, serve, or REPL (default)
     *
     * All of these use neuronos_auto_launch() for zero-config.
     * ════════════════════════════════════════════════════════ */

    /* Build extra search paths */
    const char * extra_dirs[2] = {extra_models, NULL};

    neuronos_auto_ctx_t ctx = neuronos_auto_launch(extra_models ? extra_dirs : NULL, verbose);

    if (ctx.status != NEURONOS_OK) {
        fprintf(stderr, "\033[31m"
                        "Error: Could not auto-configure NeuronOS.\n"
                        "No suitable .gguf model found.\n\n"
                        "Place a .gguf model in one of these paths:\n"
                        "  ./models/\n"
                        "  ~/.neuronos/models/\n"
                        "  /usr/share/neuronos/models/\n"
                        "  or set NEURONOS_MODELS=/path/to/models\n"
                        "\033[0m");
        return 1;
    }

    int rc = 0;

    /* ── RUN: one-shot text generation ── */
    if (command && strcmp(command, "run") == 0) {
        rc = cmd_generate(ctx.model, positional2, max_tokens, temperature, grammar_file, verbose);
    }
    /* ── AGENT: one-shot agent ── */
    else if (command && strcmp(command, "agent") == 0) {
        rc = cmd_agent(ctx.model, positional2, max_tokens, max_steps, temperature, verbose);
    }
    /* ── SERVE: HTTP server ── */
    else if (command && strcmp(command, "serve") == 0) {
        neuronos_server_params_t sparams = {
            .host = host,
            .port = port,
            .cors = true,
        };
        neuronos_status_t status = neuronos_server_start(ctx.model, NULL, sparams);
        rc = (status == NEURONOS_OK) ? 0 : 1;
    }
    /* ── AUTO (legacy compat): auto generate/agent ── */
    else if (command && strcmp(command, "auto") == 0) {
        if (positional2) {
            /* Find prompt after sub-command */
            const char * auto_prompt = NULL;
            int pcount = 0;
            for (int i = 1; i < argc; i++) {
                if (argv[i][0] == '-') {
                    if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "-s") == 0 ||
                        strcmp(argv[i], "--temp") == 0 || strcmp(argv[i], "--grammar") == 0 ||
                        strcmp(argv[i], "--models") == 0 || strcmp(argv[i], "--host") == 0 ||
                        strcmp(argv[i], "--port") == 0) {
                        i++;
                    }
                    continue;
                }
                pcount++;
                if (pcount == 3) {
                    auto_prompt = argv[i];
                    break;
                }
            }

            if (strcmp(positional2, "generate") == 0)
                rc = cmd_generate(ctx.model, auto_prompt, max_tokens, temperature, grammar_file, verbose);
            else if (strcmp(positional2, "agent") == 0)
                rc = cmd_agent(ctx.model, auto_prompt, max_tokens, max_steps, temperature, verbose);
            else
                fprintf(stderr, "Unknown auto sub-command: %s\n", positional2);
        } else {
            fprintf(stderr, "Usage: %s auto <generate|agent> \"prompt\"\n", argv[0]);
            rc = 1;
        }
    }
    /* ── DEFAULT: Interactive REPL (zero args or unknown command) ── */
    else if (!command) {
        rc = cmd_repl(&ctx, max_tokens, max_steps, temperature, grammar_file, verbose);
    } else {
        fprintf(stderr, "Unknown command: %s\n\n", command);
        print_usage(argv[0]);
        rc = 1;
    }

    neuronos_auto_release(&ctx);
    return rc;
}
