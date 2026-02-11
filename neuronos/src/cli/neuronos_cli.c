/* ============================================================
 * NeuronOS CLI v0.7.0 — Universal AI Agent Interface
 *
 * ZERO-ARG: Just run `neuronos` → auto-configures everything.
 * Detects hardware, finds best model, tunes params, starts REPL.
 *
 * Modes:
 *   neuronos                        Interactive REPL (auto-config)
 *   neuronos run "prompt"           One-shot generation
 *   neuronos agent "task"           One-shot agent with tools
 *   neuronos serve                  HTTP server (OpenAI-compatible)
 *   neuronos mcp                    MCP server (STDIO transport)
 *   neuronos hwinfo                 Show hardware capabilities
 *   neuronos scan [dir]             Scan for models
 *   neuronos <model.gguf> generate  Legacy mode
 *   neuronos <model.gguf> agent     Legacy mode
 * ============================================================ */
#include "neuronos/neuronos.h"
#include "neuronos/neuronos_hal.h"

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
            "║  NeuronOS v%-6s — Interactive AI Agent     ║\n"
            "║  Tools + Memory + Conversation. Any device.  ║\n"
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
            "  %s mcp                           MCP server (STDIO transport)\n"
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
            "  --mcp <file>     MCP client config (default: ~/.neuronos/mcp.json)\n"
            "  --verbose        Show debug info\n"
            "  --help           Show this help\n",
            NEURONOS_VERSION_STRING, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ---- Auto-download model when none found ---- */
#define MODEL_DOWNLOAD_URL                                                                                             \
    "https://huggingface.co/microsoft/bitnet-b1.58-2B-4T-gguf/resolve/main/ggml-model-i2_s.gguf"
#define MODEL_DOWNLOAD_NAME "ggml-model-i2_s.gguf"
#define MODEL_DOWNLOAD_SIZE_MB 780

static int auto_download_model(bool verbose) {
    /* Determine model directory: ~/.neuronos/models/ */
    char models_dir[512] = {0};
    char model_path[1024] = {0};
    const char * home = getenv("HOME");
#ifdef _WIN32
    if (!home)
        home = getenv("USERPROFILE");
#endif
    if (!home) {
        fprintf(stderr, "Cannot determine home directory.\n");
        return -1;
    }

    snprintf(models_dir, sizeof(models_dir), "%s/.neuronos/models", home);
    snprintf(model_path, sizeof(model_path), "%s/%s", models_dir, MODEL_DOWNLOAD_NAME);

    /* Check if already exists */
    FILE * check = fopen(model_path, "r");
    if (check) {
        fclose(check);
        if (verbose)
            fprintf(stderr, "[model already at %s]\n", model_path);
        return 0;
    }

    /* Show download prompt */
    fprintf(stderr,
            "\033[36m"
            "╔══════════════════════════════════════════════╗\n"
            "║  NeuronOS — First Run Setup                  ║\n"
            "╠══════════════════════════════════════════════╣\n"
            "║  No AI model found on this device.           ║\n"
            "║                                              ║\n"
            "║  Recommended: BitNet b1.58 2B (~%d MB)      ║\n"
            "║  • Runs on any CPU, no GPU needed            ║\n"
            "║  • 1.58-bit ternary — ultra-efficient        ║\n"
            "║  • Full agent capabilities                   ║\n"
            "╚══════════════════════════════════════════════╝\n"
            "\033[0m",
            MODEL_DOWNLOAD_SIZE_MB);

    /* Auto-download if running interactively, ask first */
    if (isatty(fileno(stdin))) {
        fprintf(stderr, "  Download now? [Y/n] ");
        char answer[16] = {0};
        if (fgets(answer, sizeof(answer), stdin)) {
            if (answer[0] == 'n' || answer[0] == 'N') {
                fprintf(stderr, "\n  Download manually:\n"
                                "    mkdir -p %s\n"
                                "    curl -L -o %s \\\n"
                                "      %s\n\n",
                        models_dir, model_path, MODEL_DOWNLOAD_URL);
                return -1;
            }
        }
    }

    /* Create directory */
    char mkdir_cmd[1024];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", models_dir);
    if (system(mkdir_cmd) != 0) {
        fprintf(stderr, "Error: Cannot create directory %s\n", models_dir);
        return -1;
    }

    /* Download with curl */
    fprintf(stderr, "\n  Downloading BitNet b1.58 2B (~%d MB)...\n\n", MODEL_DOWNLOAD_SIZE_MB);
    char curl_cmd[2048];
    snprintf(curl_cmd, sizeof(curl_cmd), "curl -fL --progress-bar -o \"%s\" \"%s\"", model_path, MODEL_DOWNLOAD_URL);

    int ret = system(curl_cmd);
    if (ret != 0) {
        fprintf(stderr, "\n\033[31mDownload failed.\033[0m Try manually:\n"
                        "  curl -L -o %s %s\n",
                model_path, MODEL_DOWNLOAD_URL);
        /* Clean up partial file */
        char rm_cmd[1024];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -f \"%s\"", model_path);
        system(rm_cmd);
        return -1;
    }

    fprintf(stderr, "\n  \033[32m✓ Model ready: %s\033[0m\n\n", model_path);
    return 0;
}

/* ---- First-run welcome prompt ---- */
static const char * FIRST_RUN_WELCOME_PROMPT =
    "You just got installed on a new device. Introduce yourself in 3-4 sentences. "
    "State your name (NeuronOS), that you run 100% locally with zero cloud dependency, "
    "and list your key powers: persistent memory (SQLite), tool use (filesystem, shell, web), "
    "agent reasoning (ReAct), MCP protocol, and 1.58-bit ternary efficiency. "
    "End by inviting the user to chat or give you a task. Be confident and concise.";

static void run_first_run_welcome(neuronos_model_t * model) {
    const char * home = getenv("HOME");
#ifdef _WIN32
    if (!home)
        home = getenv("USERPROFILE");
#endif
    if (!home)
        return;

    /* Check if first run (marker file) */
    char marker_path[512];
    snprintf(marker_path, sizeof(marker_path), "%s/.neuronos/.first_run_done", home);
    FILE * marker = fopen(marker_path, "r");
    if (marker) {
        fclose(marker);
        return; /* Already ran welcome */
    }

    fprintf(stderr, "\n\033[36m── Welcome to NeuronOS ──\033[0m\n\n");

    /* Run generation */
    neuronos_chat_msg_t msgs[2] = {
        {.role = "system",
         .content = "You are NeuronOS, a powerful AI agent running locally. "
                    "Be enthusiastic but professional. Respond in 3-4 sentences."},
        {.role = "user", .content = FIRST_RUN_WELCOME_PROMPT},
    };

    char * formatted = NULL;
    neuronos_status_t fst = neuronos_chat_format(model, NULL, msgs, 2, true, &formatted);
    const char * effective = (fst == NEURONOS_OK && formatted) ? formatted : FIRST_RUN_WELCOME_PROMPT;

    neuronos_gen_params_t gparams = {
        .prompt = effective,
        .max_tokens = 256,
        .temperature = 0.7f,
        .top_p = 0.95f,
        .top_k = 40,
        .grammar = NULL,
        .on_token = stream_token,
        .user_data = NULL,
        .seed = 0,
    };

    neuronos_gen_result_t result = neuronos_generate(model, gparams);
    printf("\n\n");
    neuronos_free(formatted);
    neuronos_gen_result_free(&result);

    /* Write marker so we don't repeat */
    marker = fopen(marker_path, "w");
    if (marker) {
        fprintf(marker, "done\n");
        fclose(marker);
    }
}
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

    /* Wrap prompt in chat template for better response quality */
    neuronos_chat_msg_t msgs[2] = {
        { .role = "system",
          .content = "You are NeuronOS, a fast and helpful AI assistant running locally. "
                     "Be concise, accurate, and direct." },
        { .role = "user", .content = prompt },
    };

    char * formatted = NULL;
    neuronos_status_t fst = neuronos_chat_format(model, NULL, msgs, 2, true, &formatted);

    /* Use formatted prompt if available, otherwise fall back to raw prompt */
    const char * effective_prompt = (fst == NEURONOS_OK && formatted) ? formatted : prompt;

    if (verbose && formatted) {
        fprintf(stderr, "[chat template applied, %zu bytes]\n", strlen(formatted));
    }

    char * grammar = load_grammar_file(grammar_file);

    neuronos_gen_params_t gparams = {
        .prompt = effective_prompt,
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
    neuronos_free(formatted);
    free(grammar);
    neuronos_gen_result_free(&result);
    return rc;
}

/* ---- Run agent command ---- */
static int cmd_agent(neuronos_model_t * model, const char * prompt, int max_tokens, int max_steps, float temperature,
                     bool verbose, neuronos_memory_t * mem, const char * mcp_config_path) {
    if (!prompt) {
        fprintf(stderr, "Error: No task provided\n");
        return 1;
    }

    neuronos_tool_registry_t * tools = neuronos_tool_registry_create();
    neuronos_tool_register_defaults(tools, NEURONOS_CAP_FILESYSTEM | NEURONOS_CAP_NETWORK | NEURONOS_CAP_SHELL);

    /* Register memory tools if memory is available */
    if (mem) {
        neuronos_tool_register_memory(tools, mem);
    }

    /* MCP Client: connect to external MCP servers and register their tools */
    neuronos_mcp_client_t * mcp_client = NULL;
    if (mcp_config_path) {
        mcp_client = neuronos_mcp_client_create();
        if (mcp_client) {
            int loaded = neuronos_mcp_client_load_config(mcp_client, mcp_config_path);
            if (loaded > 0) {
                neuronos_mcp_client_connect(mcp_client);
                int mcp_tools = neuronos_mcp_client_register_tools(mcp_client, tools);
                fprintf(stderr, "MCP: %d external tools registered\n", mcp_tools);
            } else {
                fprintf(stderr, "MCP: no servers loaded from %s\n", mcp_config_path);
                neuronos_mcp_client_free(mcp_client);
                mcp_client = NULL;
            }
        }
    } else {
        /* Try default config path */
        char default_path[512];
        const char * home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif
        if (home) {
            snprintf(default_path, sizeof(default_path), "%s/.neuronos/mcp.json", home);
            FILE * f = fopen(default_path, "r");
            if (f) {
                fclose(f);
                mcp_client = neuronos_mcp_client_create();
                if (mcp_client) {
                    int loaded = neuronos_mcp_client_load_config(mcp_client, default_path);
                    if (loaded > 0) {
                        neuronos_mcp_client_connect(mcp_client);
                        int mcp_tools = neuronos_mcp_client_register_tools(mcp_client, tools);
                        fprintf(stderr, "MCP: %d external tools registered (auto)\n", mcp_tools);
                    } else {
                        neuronos_mcp_client_free(mcp_client);
                        mcp_client = NULL;
                    }
                }
            }
        }
    }

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

    /* Attach persistent memory if available */
    if (mem) {
        neuronos_agent_set_memory(agent, mem);
    }

    fprintf(stderr, "NeuronOS Agent v%s\n", neuronos_version());
    fprintf(stderr, "Task: %s\n", prompt);
    fprintf(stderr, "Tools: %d registered%s\n", neuronos_tool_count(tools),
            mem ? " (memory enabled)" : "");
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
    if (mcp_client)
        neuronos_mcp_client_free(mcp_client);
    return rc;
}

/* ---- REPL: Interactive Read-Eval-Print Loop ---- */
/* ---- Interactive agent step callback: show reasoning + tool use ---- */
static void interactive_step_cb(int step, const char * thought, const char * action,
                                const char * observation, void * user_data) {
    (void)user_data;
    (void)step;

    /* Show thinking (for tool calls and final answers) */
    if (thought && action && strcmp(action, "reply") != 0) {
        fprintf(stderr, "\033[33m  [thinking] %s\033[0m\n", thought);
    }

    /* Show tool call + observation */
    if (action && strcmp(action, "reply") != 0 && strcmp(action, "final_answer") != 0
        && strcmp(action, "error") != 0) {
        if (observation) {
            /* This is the observation callback (second call for same step) */
            int obs_len = (int)strlen(observation);
            if (obs_len > 300) {
                fprintf(stderr, "\033[36m  [tool: %s]\033[0m %.300s...\n", action, observation);
            } else {
                fprintf(stderr, "\033[36m  [tool: %s]\033[0m %s\n", action, observation);
            }
        }
    }
}

static int cmd_repl_model(neuronos_model_t * model, int max_tokens, int max_steps, float temperature,
                          const char * grammar_file, bool verbose, const char * mcp_config_path) {
    (void)grammar_file; /* grammar is now built into the agent */

    print_banner();

    neuronos_model_info_t minfo = neuronos_model_info(model);
    fprintf(stderr, "Model: %s (%lldM params)\n", minfo.description,
            (long long)(minfo.n_params / 1000000));

    /* Open persistent memory */
    neuronos_memory_t * mem = neuronos_memory_open(NULL); /* default: ~/.neuronos/mem.db */
    if (mem) {
        int fact_count = 0;
        neuronos_memory_archival_stats(mem, &fact_count);
        fprintf(stderr, "Memory: SQLite (persistent, %d facts stored)\n", fact_count);
    } else {
        fprintf(stderr, "Memory: unavailable (continuing without persistence)\n");
    }

    /* Tool registry */
    neuronos_tool_registry_t * tools = neuronos_tool_registry_create();
    neuronos_tool_register_defaults(tools, NEURONOS_CAP_FILESYSTEM | NEURONOS_CAP_NETWORK | NEURONOS_CAP_SHELL);
    if (mem) {
        neuronos_tool_register_memory(tools, mem);
    }

    /* MCP Client: connect to external MCP servers */
    neuronos_mcp_client_t * mcp_client = NULL;
    {
        const char * cfg = mcp_config_path;
        char default_path[512] = {0};
        if (!cfg) {
            const char * home = getenv("HOME");
#ifdef _WIN32
            if (!home) home = getenv("USERPROFILE");
#endif
            if (home) {
                snprintf(default_path, sizeof(default_path), "%s/.neuronos/mcp.json", home);
                FILE * f = fopen(default_path, "r");
                if (f) {
                    fclose(f);
                    cfg = default_path;
                }
            }
        }
        if (cfg) {
            mcp_client = neuronos_mcp_client_create();
            if (mcp_client) {
                int loaded = neuronos_mcp_client_load_config(mcp_client, cfg);
                if (loaded > 0) {
                    neuronos_mcp_client_connect(mcp_client);
                    int mcp_tools = neuronos_mcp_client_register_tools(mcp_client, tools);
                    fprintf(stderr, "MCP: %d external tools from %d server(s)\n", mcp_tools, loaded);
                } else {
                    neuronos_mcp_client_free(mcp_client);
                    mcp_client = NULL;
                }
            }
        }
    }

    /* Create the interactive agent */
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
        if (mcp_client) neuronos_mcp_client_free(mcp_client);
        if (mem) neuronos_memory_close(mem);
        return 1;
    }

    /* Attach memory */
    if (mem) {
        neuronos_agent_set_memory(agent, mem);
    }

    fprintf(stderr, "Tools: %d registered%s\n", neuronos_tool_count(tools),
            mem ? " | Memory: active" : "");
    fprintf(stderr, "Just talk naturally. I can use tools when needed.\n\n");

    char line[4096];

    while (1) {
        /* Print prompt */
        if (isatty(fileno(stdin))) {
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
            fprintf(stderr,
                "\033[1mNeuronOS Interactive Agent\033[0m\n"
                "\n"
                "Just type naturally — I'll use tools when needed.\n"
                "\n"
                "  /clear             Clear conversation history\n"
                "  /tools             List available tools\n"
                "  /status            Show system & model info\n"
                "  /memory            Show memory stats\n"
                "  /remember <text>   Store a fact in long-term memory\n"
                "  /recall <query>    Search long-term memory\n"
                "  /core <key> <val>  Update core memory block\n"
                "  /temp <float>      Set temperature (0.0-2.0)\n"
                "  /tokens <int>      Set max tokens per step\n"
                "  /verbose           Toggle verbose mode\n"
                "  /quit              Exit\n"
            );
            continue;
        }

        if (strcmp(line, "/clear") == 0) {
            neuronos_agent_clear_history(agent);
            fprintf(stderr, "Conversation cleared.\n");
            continue;
        }

        if (strcmp(line, "/status") == 0) {
            neuronos_model_info_t info = neuronos_model_info(model);
            neuronos_hal_print_info();
            fprintf(stderr, "Model: %s\n", info.description);
            fprintf(stderr, "Params: %lldM | Vocab: %d | Embd: %d\n", (long long)(info.n_params / 1000000),
                    info.n_vocab, info.n_embd);
            fprintf(stderr, "Tools: %d registered\n", neuronos_tool_count(tools));
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

        if (strcmp(line, "/verbose") == 0) {
            verbose = !verbose;
            fprintf(stderr, "Verbose mode: %s\n", verbose ? "on" : "off");
            continue;
        }

        /* ---- Memory commands ---- */
        if (strcmp(line, "/memory") == 0) {
            if (!mem) {
                fprintf(stderr, "Memory not available.\n");
            } else {
                int fact_count = 0;
                neuronos_memory_archival_stats(mem, &fact_count);
                fprintf(stderr, "Archival memory: %d facts\n", fact_count);

                /* Show core memory blocks */
                const char * core_keys[] = {"persona", "human", "goals", NULL};
                for (int k = 0; core_keys[k]; k++) {
                    char * core_val = neuronos_memory_core_get(mem, core_keys[k]);
                    if (core_val) {
                        fprintf(stderr, "  [%s] %s\n", core_keys[k], core_val);
                        free(core_val);
                    }
                }
            }
            continue;
        }

        if (strncmp(line, "/remember ", 10) == 0) {
            if (!mem) {
                fprintf(stderr, "Memory not available.\n");
            } else {
                const char * text = line + 10;
                while (*text == ' ') text++;
                if (*text == '\0') {
                    fprintf(stderr, "Usage: /remember <fact to store>\n");
                } else {
                    int64_t row_id = neuronos_memory_archival_store(
                        mem, text, text, "user", 0.8f);
                    if (row_id >= 0)
                        fprintf(stderr, "Stored in archival memory (id=%lld).\n", (long long)row_id);
                    else
                        fprintf(stderr, "Failed to store memory.\n");
                }
            }
            continue;
        }

        if (strncmp(line, "/recall ", 7) == 0) {
            if (!mem) {
                fprintf(stderr, "Memory not available.\n");
            } else {
                const char * query = line + 7;
                while (*query == ' ') query++;
                if (*query == '\0') {
                    fprintf(stderr, "Usage: /recall <search query>\n");
                } else {
                    neuronos_archival_entry_t * results = NULL;
                    int n_results = 0;
                    int st = neuronos_memory_archival_search(
                        mem, query, 5, &results, &n_results);
                    if (st == 0 && n_results > 0) {
                        fprintf(stderr, "Found %d result(s):\n", n_results);
                        for (int i = 0; i < n_results; i++) {
                            fprintf(stderr, "  [%d] %s: %s (importance=%.2f)\n",
                                    i + 1, results[i].key, results[i].value, results[i].importance);
                        }
                        neuronos_memory_archival_free(results, n_results);
                    } else if (st == 0) {
                        fprintf(stderr, "No results found for: %s\n", query);
                    } else {
                        fprintf(stderr, "Search failed (status=%d).\n", st);
                    }
                }
            }
            continue;
        }

        if (strncmp(line, "/core ", 6) == 0) {
            if (!mem) {
                fprintf(stderr, "Memory not available.\n");
            } else {
                /* Parse: /core <key> <value> */
                const char * rest = line + 6;
                while (*rest == ' ') rest++;
                const char * space = strchr(rest, ' ');
                if (!space) {
                    fprintf(stderr, "Usage: /core <key> <value>\n");
                } else {
                    char key[64];
                    size_t klen = (size_t)(space - rest);
                    if (klen >= sizeof(key)) klen = sizeof(key) - 1;
                    memcpy(key, rest, klen);
                    key[klen] = '\0';
                    const char * val = space + 1;
                    while (*val == ' ') val++;
                    int rc_mem = neuronos_memory_core_set(mem, key, val);
                    if (rc_mem == 0)
                        fprintf(stderr, "Core memory [%s] updated.\n", key);
                    else
                        fprintf(stderr, "Failed to update core memory (status=%d).\n", rc_mem);
                }
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

        /* /agent command: legacy one-shot agent for a single task */
        if (strncmp(line, "/agent ", 7) == 0) {
            const char * task = line + 7;
            while (*task == ' ')
                task++;
            cmd_agent(model, task, max_tokens, max_steps, temperature, verbose, mem, NULL);
            continue;
        }

        /* ---- Default: interactive agent (unified mode) ---- */
        {
            neuronos_agent_result_t result = neuronos_agent_chat(
                agent, line, interactive_step_cb, NULL);

            if (result.status == NEURONOS_OK && result.text) {
                printf("%s\n", result.text);
                if (verbose) {
                    fprintf(stderr, "[%d step(s), %.1f ms]\n",
                            result.steps_taken, result.total_ms);
                }
            } else {
                fprintf(stderr, "[agent error: status=%d, steps=%d]\n",
                        result.status, result.steps_taken);
            }

            neuronos_agent_result_free(&result);
        }
    }

    neuronos_agent_free(agent);
    neuronos_tool_registry_free(tools);
    if (mcp_client) neuronos_mcp_client_free(mcp_client);
    if (mem) neuronos_memory_close(mem);
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
    const char * mcp_config = NULL; /* --mcp <config.json> for MCP client */

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
        } else if (strcmp(argv[i], "--mcp") == 0 && i + 1 < argc) {
            mcp_config = argv[++i];
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
                strcmp(argv[i], "--models") == 0 || strcmp(argv[i], "--host") == 0 || strcmp(argv[i], "--port") == 0 ||
                strcmp(argv[i], "--mcp") == 0) {
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
        printf("[CLI] Initializing HAL explicitly...\n");
        neuronos_hal_status_t st = neuronos_hal_init();
        printf("[CLI] HAL Init status: %d (OK=%d)\n", (int)st, (int)NEURONOS_HAL_OK);

        neuronos_hw_info_t hw = neuronos_detect_hardware();
        neuronos_hw_print_info(&hw);
        printf("\n");
        neuronos_hal_print_info();
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

        neuronos_model_params_t mparams = {.model_path = model_path, .context_size = 0, .use_mmap = true};
        neuronos_model_t * model = neuronos_model_load(engine, mparams);
        if (!model) {
            fprintf(stderr, "Error: Failed to load model\n");
            neuronos_shutdown(engine);
            return 1;
        }

        int rc = 1;
        if (strcmp(sub_cmd, "generate") == 0 || strcmp(sub_cmd, "run") == 0)
            rc = cmd_generate(model, prompt, max_tokens, temperature, grammar_file, verbose);
        else if (strcmp(sub_cmd, "agent") == 0)
            rc = cmd_agent(model, prompt, max_tokens, max_steps, temperature, verbose, NULL, mcp_config);
        else if (strcmp(sub_cmd, "serve") == 0) {
            neuronos_server_params_t sparams = {.host = host, .port = port, .cors = true};
            neuronos_status_t status = neuronos_server_start(model, NULL, sparams);
            rc = (status == NEURONOS_OK) ? 0 : 1;
        } else if (strcmp(sub_cmd, "mcp") == 0) {
            neuronos_tool_registry_t * mcp_tools = neuronos_tool_registry_create();
            neuronos_tool_register_defaults(mcp_tools,
                                            NEURONOS_CAP_FILESYSTEM | NEURONOS_CAP_NETWORK | NEURONOS_CAP_SHELL);
            neuronos_status_t status = neuronos_mcp_serve_stdio(mcp_tools);
            neuronos_tool_registry_free(mcp_tools);
            rc = (status == NEURONOS_OK) ? 0 : 1;
        } else if (strcmp(sub_cmd, "repl") == 0 || strcmp(sub_cmd, "chat") == 0) {
            rc = cmd_repl_model(model, max_tokens, max_steps, temperature, grammar_file, verbose, mcp_config);
        } else
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
        /* No model found — offer to download one automatically */
        if (auto_download_model(verbose) == 0) {
            /* Retry auto-launch after download */
            ctx = neuronos_auto_launch(extra_models ? extra_dirs : NULL, verbose);
        }

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
    }

    /* First run: show welcome message */
    if (!command && isatty(fileno(stdin))) {
        run_first_run_welcome(ctx.model);
    }

    int rc = 0;

    /* ── RUN: one-shot text generation ── */
    if (command && strcmp(command, "run") == 0) {
        rc = cmd_generate(ctx.model, positional2, max_tokens, temperature, grammar_file, verbose);
    }
    /* ── AGENT: one-shot agent ── */
    else if (command && strcmp(command, "agent") == 0) {
        rc = cmd_agent(ctx.model, positional2, max_tokens, max_steps, temperature, verbose, NULL, mcp_config);
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
    /* ── MCP: Model Context Protocol server (STDIO) ── */
    else if (command && strcmp(command, "mcp") == 0) {
        neuronos_tool_registry_t * mcp_tools = neuronos_tool_registry_create();
        neuronos_tool_register_defaults(mcp_tools, NEURONOS_CAP_FILESYSTEM | NEURONOS_CAP_NETWORK | NEURONOS_CAP_SHELL);
        neuronos_status_t status = neuronos_mcp_serve_stdio(mcp_tools);
        neuronos_tool_registry_free(mcp_tools);
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
                rc = cmd_agent(ctx.model, auto_prompt, max_tokens, max_steps, temperature, verbose, NULL, mcp_config);
            else
                fprintf(stderr, "Unknown auto sub-command: %s\n", positional2);
        } else {
            fprintf(stderr, "Usage: %s auto <generate|agent> \"prompt\"\n", argv[0]);
            rc = 1;
        }
    }
    /* ── DEFAULT: Interactive REPL (zero args or unknown command) ── */
    else if (!command) {
        rc = cmd_repl_model(ctx.model, max_tokens, max_steps, temperature, grammar_file, verbose, mcp_config);
    } else {
        fprintf(stderr, "Unknown command: %s\n\n", command);
        print_usage(argv[0]);
        rc = 1;
    }

    neuronos_auto_release(&ctx);
    return rc;
}
