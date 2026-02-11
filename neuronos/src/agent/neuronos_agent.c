/* ============================================================
 * NeuronOS — ReAct Agent Loop
 *
 * Phase 2C: Think → Act → Observe loop.
 * The model generates structured JSON (via GBNF grammar),
 * we parse it, execute the tool, feed back the observation,
 * and repeat until "answer" or max_steps.
 * ============================================================ */
#include "neuronos/neuronos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ---- Built-in GBNF grammar for tool_call/final_answer (one-shot mode) ---- */
static const char TOOL_CALL_GRAMMAR[] =
    "root ::= ws \"{\" ws step ws \"}\" ws\n"
    "step ::= tool-call | final-answer\n"
    "tool-call ::= \"\\\"thought\\\"\" ws \":\" ws string ws \",\" ws "
    "\"\\\"action\\\"\" ws \":\" ws string ws \",\" ws "
    "\"\\\"args\\\"\" ws \":\" ws object\n"
    "final-answer ::= \"\\\"thought\\\"\" ws \":\" ws string ws \",\" ws "
    "\"\\\"answer\\\"\" ws \":\" ws string\n"
    "object ::= \"{\" ws \"}\" | \"{\" ws members ws \"}\"\n"
    "members ::= pair ( ws \",\" ws pair )*\n"
    "pair ::= string ws \":\" ws value\n"
    "value ::= string | number | object | array | \"true\" | \"false\" | \"null\"\n"
    "array ::= \"[\" ws \"]\" | \"[\" ws values ws \"]\"\n"
    "values ::= value ( ws \",\" ws value )*\n"
    "string ::= \"\\\"\" characters \"\\\"\"\n"
    "characters ::= character*\n"
    "character ::= [^\"\\\\] | \"\\\\\" escape\n"
    "escape ::= [\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]\n"
    "number ::= integer fraction? exponent?\n"
    "integer ::= \"-\"? ( \"0\" | [1-9] [0-9]* )\n"
    "fraction ::= \".\" [0-9]+\n"
    "exponent ::= [eE] [+-]? [0-9]+\n"
    "ws ::= [ \\t\\n\\r]*\n";

/* ---- Interactive GBNF grammar: reply OR tool_call OR final_answer ---- */
static const char INTERACTIVE_GRAMMAR[] =
    "root ::= ws \"{\" ws content ws \"}\" ws\n"
    "content ::= reply-content | tool-content | answer-content\n"
    "reply-content ::= \"\\\"reply\\\"\" ws \":\" ws string\n"
    "tool-content ::= \"\\\"thought\\\"\" ws \":\" ws string ws \",\" ws "
    "\"\\\"action\\\"\" ws \":\" ws string ws \",\" ws "
    "\"\\\"args\\\"\" ws \":\" ws object\n"
    "answer-content ::= \"\\\"thought\\\"\" ws \":\" ws string ws \",\" ws "
    "\"\\\"answer\\\"\" ws \":\" ws string\n"
    "object ::= \"{\" ws \"}\" | \"{\" ws members ws \"}\"\n"
    "members ::= pair ( ws \",\" ws pair )*\n"
    "pair ::= string ws \":\" ws value\n"
    "value ::= string | number | object | array | \"true\" | \"false\" | \"null\"\n"
    "array ::= \"[\" ws \"]\" | \"[\" ws values ws \"]\"\n"
    "values ::= value ( ws \",\" ws value )*\n"
    "string ::= \"\\\"\" characters \"\\\"\"\n"
    "characters ::= character*\n"
    "character ::= [^\"\\\\] | \"\\\\\" escape\n"
    "escape ::= [\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]\n"
    "number ::= integer fraction? exponent?\n"
    "integer ::= \"-\"? ( \"0\" | [1-9] [0-9]* )\n"
    "fraction ::= \".\" [0-9]+\n"
    "exponent ::= [eE] [+-]? [0-9]+\n"
    "ws ::= [ \\t\\n\\r]*\n";

/* ---- Default system prompt template (one-shot mode, backward compat) ---- */
static const char DEFAULT_SYSTEM_PROMPT_TEMPLATE[] =
    "You are a helpful AI assistant with access to tools.\n"
    "You MUST respond with a JSON object in one of two formats:\n\n"
    "1. To use a tool:\n"
    "{\"thought\": \"your reasoning\", \"action\": \"tool_name\", \"args\": {\"arg1\": \"value1\"}}\n\n"
    "2. To give a final answer:\n"
    "{\"thought\": \"your reasoning\", \"answer\": \"your final answer\"}\n\n"
    "%s\n" /* tool descriptions injected here */
    "Rules:\n"
    "- Always think step by step.\n"
    "- Use tools when you need information or to perform actions.\n"
    "- Give a final answer when you have enough information.\n"
    "- Respond ONLY with valid JSON, no other text.\n";

/* ---- Model-size-aware system prompt templates (one-shot mode) ---- */

/* Small models (<=3B params): ultra-concise, explicit examples */
static const char SYSTEM_PROMPT_SMALL[] =
    "You are an AI assistant with tools. Respond with JSON ONLY.\n\n"
    "FORMAT 1 - Use a tool:\n"
    "{\"thought\": \"I need to check...\", \"action\": \"tool_name\", \"args\": {\"key\": \"val\"}}\n\n"
    "FORMAT 2 - Final answer:\n"
    "{\"thought\": \"I know the answer\", \"answer\": \"my answer\"}\n\n"
    "%s\n"
    "RULES: Think step by step. Use tools when needed. Answer when ready. JSON only.\n";

/* Large models (>=7B params): detailed instructions */
static const char SYSTEM_PROMPT_LARGE[] =
    "You are NeuronOS, an intelligent AI assistant running locally on the user's device.\n"
    "You have access to tools and persistent memory. Respond with exactly one JSON object.\n\n"
    "## To use a tool:\n"
    "{\"thought\": \"step-by-step reasoning about what to do\", \"action\": \"tool_name\", "
    "\"args\": {\"param\": \"value\"}}\n\n"
    "## To provide your final answer:\n"
    "{\"thought\": \"reasoning about why you have enough information\", "
    "\"answer\": \"your comprehensive answer\"}\n\n"
    "## Available Tools\n"
    "%s\n"
    "## Guidelines\n"
    "- Reason carefully before each action.\n"
    "- Use tools to gather information -- do not guess.\n"
    "- If a tool errors, try a different approach.\n"
    "- Give a final answer when you have sufficient information.\n"
    "- Be thorough but concise in your answers.\n"
    "- Respond with valid JSON ONLY, no other text.\n";

/* ---- Interactive mode system prompts (conversational + tools) ---- */

/* Small models (<=3B params): interactive */
static const char INTERACTIVE_PROMPT_SMALL[] =
    "You are NeuronOS, a helpful AI assistant. Respond with JSON ONLY.\n\n"
    "FORMAT 1 - Direct reply (for greetings, conversation, questions you can answer):\n"
    "{\"reply\": \"your response\"}\n\n"
    "FORMAT 2 - Use a tool (when you need to do something or get information):\n"
    "{\"thought\": \"why I need this tool\", \"action\": \"tool_name\", \"args\": {\"key\": \"val\"}}\n\n"
    "FORMAT 3 - Answer after tools (when you have results from tools):\n"
    "{\"thought\": \"what I learned\", \"answer\": \"my answer based on tool results\"}\n\n"
    "%s\n"
    "RULES:\n"
    "- Reply directly if you can answer from your knowledge.\n"
    "- Use tools when you need files, system info, time, calculations, etc.\n"
    "- After tools, give a final answer with your findings.\n"
    "- JSON only. No other text.\n";

/* Large models (>=7B params): interactive */
static const char INTERACTIVE_PROMPT_LARGE[] =
    "You are NeuronOS, an intelligent AI assistant running locally on the user's device.\n"
    "You have access to tools and persistent memory. Respond with exactly one JSON object.\n\n"
    "## Response Formats\n\n"
    "### Direct Reply (conversation, greetings, questions you can answer from knowledge):\n"
    "{\"reply\": \"your natural response\"}\n\n"
    "### Tool Use (when you need to take action or gather information):\n"
    "{\"thought\": \"step-by-step reasoning\", \"action\": \"tool_name\", "
    "\"args\": {\"param\": \"value\"}}\n\n"
    "### Final Answer (after using tools, when you have enough information):\n"
    "{\"thought\": \"reasoning about results\", \"answer\": \"your comprehensive answer\"}\n\n"
    "## Available Tools\n"
    "%s\n"
    "## Guidelines\n"
    "- Reply directly for conversation, greetings, and questions you can answer.\n"
    "- Use tools when you need to interact with files, system, time, calculations.\n"
    "- After tool results, provide a final answer summarizing your findings.\n"
    "- Do not guess about files or system state -- use tools.\n"
    "- Be helpful, concise, and accurate.\n"
    "- Respond with valid JSON ONLY, no other text.\n";

/* ---- Token estimation ---- */
static int estimate_tokens(const char * text) {
    /* Rough estimate: ~3.5 chars per token for mixed English/JSON text */
    return text ? (int)(strlen(text) * 10 / 35) : 0;
}

/* ---- Step history compaction ---- */

/*
 * Build a compact summary of agent steps [from..to).
 * Extracts key action/observation pairs in abbreviated form.
 * Returns newly allocated string. Caller must free.
 */
static char * compact_step_summary(const char ** step_actions, const char ** step_observations,
                                   int from, int to) {
    size_t cap = 256;
    char * summary = malloc(cap);
    if (!summary) return NULL;

    size_t len = 0;
    len += (size_t)snprintf(summary + len, cap - len, "[Earlier steps: ");

    for (int i = from; i < to; i++) {
        const char * act = step_actions[i] ? step_actions[i] : "unknown";
        const char * obs = step_observations[i] ? step_observations[i] : "";
        int obs_len = (int)strlen(obs);

        /* Grow buffer if needed */
        size_t need = strlen(act) + 80;
        while (len + need > cap) { cap *= 2; summary = realloc(summary, cap); }

        /* Truncate long observations in the summary */
        if (obs_len > 80) {
            len += (size_t)snprintf(summary + len, cap - len,
                "Used %s -> %.80s... ", act, obs);
        } else {
            len += (size_t)snprintf(summary + len, cap - len,
                "Used %s -> %s. ", act, obs);
        }
    }

    while (len + 2 > cap) { cap *= 2; summary = realloc(summary, cap); }
    len += (size_t)snprintf(summary + len, cap - len, "]");
    return summary;
}

/* ---- Internal agent struct ---- */
struct neuronos_agent {
    neuronos_model_t * model;
    neuronos_tool_registry_t * tools;
    neuronos_agent_params_t params;
    char * system_prompt;
    char * interactive_prompt;      /* prompt for interactive mode (with reply) */
    neuronos_memory_t * memory;     /* optional persistent memory (not owned) */
    int64_t session_id;             /* current recall memory session */

    /* Conversation history for multi-turn interactive mode */
    char ** conv_roles;             /* role strings (owned copies) */
    char ** conv_contents;          /* content strings (owned copies) */
    size_t conv_len;                /* number of messages stored */
    size_t conv_cap;                /* allocated capacity */
};

/* ---- Helpers ---- */
static double get_time_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

/*
 * Minimal JSON string extractor.
 * Finds "key": "value" and returns a newly allocated copy of value.
 * Returns NULL if not found.
 */
static char * json_extract_string(const char * json, const char * key) {
    /* Build search pattern: "key" */
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char * pos = strstr(json, pattern);
    if (!pos)
        return NULL;

    /* Skip past key and colon */
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == ':')
        pos++;

    /* Expect opening quote */
    if (*pos != '"')
        return NULL;
    pos++;

    /* Find closing quote, handling escapes */
    const char * start = pos;
    while (*pos && !(*pos == '"' && *(pos - 1) != '\\')) {
        pos++;
    }

    size_t len = (size_t)(pos - start);
    char * val = malloc(len + 1);
    if (!val)
        return NULL;
    memcpy(val, start, len);
    val[len] = '\0';

    return val;
}

/*
 * Extract "args" as a raw JSON object: everything between { and matching }
 */
static char * json_extract_object(const char * json, const char * key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char * pos = strstr(json, pattern);
    if (!pos)
        return NULL;

    pos += strlen(pattern);
    while (*pos && *pos != '{')
        pos++;
    if (*pos != '{')
        return NULL;

    /* Find matching closing brace */
    const char * start = pos;
    int depth = 0;
    bool in_string = false;

    while (*pos) {
        if (*pos == '"' && (pos == start || *(pos - 1) != '\\')) {
            in_string = !in_string;
        }
        if (!in_string) {
            if (*pos == '{')
                depth++;
            else if (*pos == '}') {
                depth--;
                if (depth == 0) {
                    pos++; /* include closing brace */
                    size_t len = (size_t)(pos - start);
                    char * obj = malloc(len + 1);
                    memcpy(obj, start, len);
                    obj[len] = '\0';
                    return obj;
                }
            }
        }
        pos++;
    }

    return NULL;
}

/*
 * Build the conversation prompt for the current step using chat template.
 *
 * Messages:
 *   [0] system  = agent system prompt (with tool descriptions)
 *   [1] user    = user input
 *   [2] assistant = step 0 output (JSON)
 *   [3] user    = "Observation from <tool>: <result>"
 *   [4] assistant = step 1 output ...
 *   ... (repeats for each step)
 *
 * Falls back to plain text formatting if chat template is unavailable.
 */
static char * build_prompt(const neuronos_agent_t * agent, const char * user_input, const char ** step_outputs,
                           const char ** step_actions, const char ** step_observations,
                           int first_step, int n_steps, const char * context_summary) {
    /* Count messages: system + user + (optional summary) + 2 per active step */
    size_t n_msgs = 2; /* system + user */
    if (context_summary)
        n_msgs++; /* compacted context summary */
    for (int i = first_step; i < n_steps; i++) {
        if (step_outputs[i])
            n_msgs++;
        if (step_observations[i])
            n_msgs++;
    }

    neuronos_chat_msg_t * msgs = calloc(n_msgs, sizeof(neuronos_chat_msg_t));
    int active_count = n_steps - first_step;
    char ** obs_bufs = calloc((size_t)(active_count > 0 ? active_count : 1), sizeof(char *));
    if (!msgs || !obs_bufs) {
        free(msgs);
        free(obs_bufs);
        return NULL;
    }

    size_t idx = 0;
    msgs[idx].role = "system";
    msgs[idx].content = agent->system_prompt;
    idx++;

    msgs[idx].role = "user";
    msgs[idx].content = user_input;
    idx++;

    /* Inject compacted context summary if present */
    if (context_summary) {
        msgs[idx].role = "user";
        msgs[idx].content = context_summary;
        idx++;
    }

    for (int i = first_step; i < n_steps; i++) {
        int obs_idx = i - first_step;
        if (step_outputs[i]) {
            msgs[idx].role = "assistant";
            msgs[idx].content = step_outputs[i];
            idx++;
        }
        if (step_observations[i]) {
            /* Build observation string: "Observation from <tool>: <result>" */
            const char * tool = step_actions[i] ? step_actions[i] : "tool";
            size_t obs_len = strlen("Observation from : ") + strlen(tool) + strlen(step_observations[i]) + 1;
            obs_bufs[obs_idx] = malloc(obs_len);
            snprintf(obs_bufs[obs_idx], obs_len, "Observation from %s: %s", tool, step_observations[i]);
            msgs[idx].role = "user";
            msgs[idx].content = obs_bufs[obs_idx];
            idx++;
        }
    }

    /* Try chat template formatting */
    char * formatted = NULL;
    neuronos_status_t st = neuronos_chat_format(agent->model, NULL, msgs, idx, true, &formatted);

    /* Free temporary observation buffers */
    for (int i = 0; i < active_count; i++) {
        free(obs_bufs[i]);
    }
    free(obs_bufs);
    free(msgs);

    if (st == NEURONOS_OK && formatted) {
        return formatted; /* Caller must use neuronos_free() */
    }

    /* Fallback: plain text (legacy format) */
    size_t total = strlen(agent->system_prompt) + strlen(user_input) + 256;
    if (context_summary)
        total += strlen(context_summary) + 32;
    for (int i = first_step; i < n_steps; i++) {
        if (step_outputs[i])
            total += strlen(step_outputs[i]) + 32;
        if (step_observations[i])
            total += strlen(step_observations[i]) + 64;
    }

    char * prompt = malloc(total);
    if (!prompt)
        return NULL;

    size_t len = 0;
    len += (size_t)sprintf(prompt + len, "%s\n", agent->system_prompt);
    len += (size_t)sprintf(prompt + len, "User: %s\n\n", user_input);

    if (context_summary) {
        len += (size_t)sprintf(prompt + len, "%s\n\n", context_summary);
    }

    for (int i = first_step; i < n_steps; i++) {
        if (step_outputs[i]) {
            len += (size_t)sprintf(prompt + len, "Assistant: %s\n", step_outputs[i]);
        }
        if (step_observations[i]) {
            len += (size_t)sprintf(prompt + len, "Observation from %s: %s\n\n",
                                   step_actions[i] ? step_actions[i] : "tool", step_observations[i]);
        }
    }

    len += (size_t)sprintf(prompt + len, "Assistant: ");
    return prompt;
}

/* ============================================================
 * AGENT LIFECYCLE
 * ============================================================ */

neuronos_agent_t * neuronos_agent_create(neuronos_model_t * model, neuronos_tool_registry_t * tools,
                                         neuronos_agent_params_t params) {
    if (!model)
        return NULL;

    neuronos_agent_t * agent = calloc(1, sizeof(neuronos_agent_t));
    if (!agent)
        return NULL;

    agent->model = model;
    agent->tools = tools;

    /* Apply defaults */
    agent->params.max_steps = params.max_steps > 0 ? params.max_steps : 10;
    agent->params.max_tokens_per_step = params.max_tokens_per_step > 0 ? params.max_tokens_per_step : 512;
    agent->params.temperature = params.temperature > 0.0f ? params.temperature : 0.3f;
    /* Context budget: use 80% of model context, minimum 1536 */
    int ctx_cap = neuronos_model_context_size(model);
    int auto_budget = ctx_cap > 0 ? (int)(ctx_cap * 0.80f) : 1536;
    if (auto_budget < 1536) auto_budget = 1536;
    agent->params.context_budget = params.context_budget > 0 ? params.context_budget : auto_budget;
    agent->params.verbose = params.verbose;
    agent->memory = NULL;
    agent->session_id = 1;

    /* Init conversation history */
    agent->conv_cap = 32;
    agent->conv_roles = calloc(agent->conv_cap, sizeof(char *));
    agent->conv_contents = calloc(agent->conv_cap, sizeof(char *));
    agent->conv_len = 0;

    /* Select system prompt based on model size */
    neuronos_model_info_t minfo = neuronos_model_info(model);
    const char * oneshot_template;
    const char * interactive_template;
    if (minfo.n_params > 0 && minfo.n_params <= 4000000000LL) {
        oneshot_template = SYSTEM_PROMPT_SMALL;
        interactive_template = INTERACTIVE_PROMPT_SMALL;
    } else if (minfo.n_params > 4000000000LL) {
        oneshot_template = SYSTEM_PROMPT_LARGE;
        interactive_template = INTERACTIVE_PROMPT_LARGE;
    } else {
        oneshot_template = DEFAULT_SYSTEM_PROMPT_TEMPLATE;
        interactive_template = INTERACTIVE_PROMPT_SMALL;
    }

    /* Build system prompts with tool descriptions */
    char * tool_desc;
    if (tools) {
        tool_desc = neuronos_tool_prompt_description(tools);
    } else {
        tool_desc = strdup("No tools available.\n");
    }

    /* One-shot prompt (for agent_run) */
    size_t prompt_size = strlen(oneshot_template) + strlen(tool_desc) + 64;
    agent->system_prompt = malloc(prompt_size);
    snprintf(agent->system_prompt, prompt_size, oneshot_template, tool_desc);

    /* Interactive prompt (for agent_chat) */
    size_t iprompt_size = strlen(interactive_template) + strlen(tool_desc) + 64;
    agent->interactive_prompt = malloc(iprompt_size);
    snprintf(agent->interactive_prompt, iprompt_size, interactive_template, tool_desc);

    free(tool_desc);

    if (params.verbose) {
        const char * size_label = minfo.n_params <= 4000000000LL ? "small" : "large";
        fprintf(stderr, "[neuronos] Agent created: %s prompt template (model %lldM params, ctx_budget=%d)\n",
                size_label, (long long)(minfo.n_params / 1000000), agent->params.context_budget);
    }

    return agent;
}

void neuronos_agent_free(neuronos_agent_t * agent) {
    if (!agent)
        return;
    free(agent->system_prompt);
    free(agent->interactive_prompt);
    /* Free conversation history */
    for (size_t i = 0; i < agent->conv_len; i++) {
        free(agent->conv_roles[i]);
        free(agent->conv_contents[i]);
    }
    free(agent->conv_roles);
    free(agent->conv_contents);
    free(agent);
}

void neuronos_agent_set_system_prompt(neuronos_agent_t * agent, const char * system_prompt) {
    if (!agent || !system_prompt)
        return;
    free(agent->system_prompt);
    agent->system_prompt = strdup(system_prompt);
}

void neuronos_agent_set_memory(neuronos_agent_t * agent, neuronos_memory_t * mem) {
    if (!agent) return;
    agent->memory = mem;
    if (mem) {
        agent->session_id = neuronos_memory_session_create(mem);
    }
}

/*
 * Build enriched system prompt with core memory blocks and A/R stats.
 * Called when memory is attached and before each agent run.
 */
static char * build_memory_enriched_prompt(const neuronos_agent_t * agent, const char * base_prompt) {
    if (!agent->memory) return strdup(base_prompt);

    char * core_dump = neuronos_memory_core_dump(agent->memory);
    int recall_msgs = 0, recall_tokens = 0;
    int archival_facts = 0;

    neuronos_memory_recall_stats(agent->memory, agent->session_id, &recall_msgs, &recall_tokens);
    neuronos_memory_archival_stats(agent->memory, &archival_facts);

    /* Build enriched prompt */
    size_t len = strlen(base_prompt) + (core_dump ? strlen(core_dump) : 0) + 512;
    char * enriched = malloc(len);
    snprintf(enriched, len,
        "%s\n"
        "### Core Memory ###\n"
        "%s"
        "\n"
        "### Memory Stats ###\n"
        "Recall memory: %d messages (%d tokens) in this session.\n"
        "Archival memory: %d facts stored.\n"
        "You can use memory_store to save important facts, memory_search to find them, "
        "and memory_core_update to update your core memory blocks.\n",
        base_prompt,
        core_dump ? core_dump : "(empty)\n",
        recall_msgs, recall_tokens, archival_facts);

    free(core_dump);
    return enriched;
}

/* ============================================================
 * AGENT RUN — The ReAct Loop
 * ============================================================ */

neuronos_agent_result_t neuronos_agent_run(neuronos_agent_t * agent, const char * user_input,
                                           neuronos_agent_step_cb on_step, void * user_data) {
    neuronos_agent_result_t result = {0};
    double t_start = get_time_ms();

    if (!agent || !user_input) {
        result.status = NEURONOS_ERROR_INVALID_PARAM;
        return result;
    }

    /* If memory is attached, enrich system prompt with core blocks + stats */
    char * original_prompt = NULL;
    if (agent->memory) {
        original_prompt = agent->system_prompt; /* save original */
        agent->system_prompt = build_memory_enriched_prompt(agent, original_prompt);

        /* Log user input to recall memory (rough token estimate: chars/4) */
        neuronos_memory_recall_add(agent->memory, agent->session_id,
                                   "user", user_input, (int)(strlen(user_input) / 4));
    }

    int max_steps = agent->params.max_steps;

    /* Allocate history arrays */
    const char ** step_outputs = calloc((size_t)max_steps, sizeof(char *));
    const char ** step_actions = calloc((size_t)max_steps, sizeof(char *));
    const char ** step_observations = calloc((size_t)max_steps, sizeof(char *));

    if (!step_outputs || !step_actions || !step_observations) {
        free(step_outputs);
        free(step_actions);
        free(step_observations);
        result.status = NEURONOS_ERROR_GENERATE;
        return result;
    }

    /* Context compaction state */
    int first_active_step = 0;     /* first step to include in prompt */
    char * context_summary = NULL; /* summary of compacted earlier steps */
    int ctx_capacity = neuronos_model_context_size(agent->model);
    int gen_budget = agent->params.max_tokens_per_step;
    int steps_taken = 0;

    for (int step = 0; step < max_steps; step++) {
        if (agent->params.verbose) {
            fprintf(stderr, "\n[neuronos] ── Step %d/%d ──\n", step + 1, max_steps);
        }

        /* ---- Context compaction check ---- */
        if (ctx_capacity > 0 && step >= 3) {
            /* Estimate total prompt tokens */
            int est_tokens = estimate_tokens(agent->system_prompt) + estimate_tokens(user_input);
            if (context_summary) est_tokens += estimate_tokens(context_summary);
            for (int i = first_active_step; i < step; i++) {
                est_tokens += estimate_tokens(step_outputs[i]);
                est_tokens += estimate_tokens(step_observations[i]);
                est_tokens += 20; /* overhead per step (role tags, etc.) */
            }

            float usage_ratio = (float)(est_tokens + gen_budget) / (float)ctx_capacity;

            if (usage_ratio > 0.80f) {
                /* Need to compact: keep last 2 steps, summarize the rest */
                int keep_last = 2;
                int compact_end = step - keep_last;
                if (compact_end > first_active_step) {
                    if (agent->params.verbose) {
                        fprintf(stderr, "[neuronos] Context compaction: %.0f%% used (%d/%d tokens), "
                                "compacting steps %d-%d\n",
                                usage_ratio * 100.0f, est_tokens, ctx_capacity,
                                first_active_step + 1, compact_end);
                    }

                    /* Build new summary, merge with existing if present */
                    char * new_summary = compact_step_summary(
                        step_actions, step_observations, first_active_step, compact_end);

                    if (context_summary && new_summary) {
                        /* Merge old + new summary */
                        size_t merged_len = strlen(context_summary) + strlen(new_summary) + 4;
                        char * merged = malloc(merged_len);
                        snprintf(merged, merged_len, "%s %s", context_summary, new_summary);
                        free(context_summary);
                        free(new_summary);
                        context_summary = merged;
                    } else if (new_summary) {
                        context_summary = new_summary;
                    }

                    /* Store compacted steps to recall memory if available */
                    if (agent->memory) {
                        for (int i = first_active_step; i < compact_end; i++) {
                            if (step_outputs[i]) {
                                neuronos_memory_recall_add(agent->memory, agent->session_id,
                                    "assistant", step_outputs[i],
                                    estimate_tokens(step_outputs[i]));
                            }
                        }
                    }

                    first_active_step = compact_end;
                }
            }
        }

        /* Build the prompt with conversation history */
        char * prompt = build_prompt(agent, user_input, step_outputs, step_actions,
                                     step_observations, first_active_step, step,
                                     context_summary);

        if (!prompt) {
            result.status = NEURONOS_ERROR_GENERATE;
            break;
        }

        if (agent->params.verbose) {
            fprintf(stderr, "[neuronos] Prompt: %zu chars (~%d tokens), ctx_cap=%d\n",
                    strlen(prompt), estimate_tokens(prompt), ctx_capacity);
        }

        /* Generate with grammar constraint */
        neuronos_gen_params_t gen_params = {
            .prompt = prompt,
            .max_tokens = agent->params.max_tokens_per_step,
            .temperature = agent->params.temperature,
            .top_p = 0.95f,
            .top_k = 40,
            .grammar = TOOL_CALL_GRAMMAR,
            .grammar_root = "root",
            .on_token = NULL,
            .user_data = NULL,
            .seed = 0,
        };

        neuronos_gen_result_t gen = neuronos_generate(agent->model, gen_params);
        free(prompt);

        if (gen.status != NEURONOS_OK || !gen.text) {
            neuronos_gen_result_free(&gen);
            result.status = NEURONOS_ERROR_GENERATE;
            break;
        }

        if (agent->params.verbose) {
            fprintf(stderr, "[neuronos] Model output: %s\n", gen.text);
        }

        /* Store raw output */
        step_outputs[step] = strdup(gen.text);
        steps_taken++;

        /* Parse the JSON response */
        char * thought = json_extract_string(gen.text, "thought");
        char * answer = json_extract_string(gen.text, "answer");
        char * action = json_extract_string(gen.text, "action");
        char * args = json_extract_object(gen.text, "args");

        neuronos_gen_result_free(&gen);

        /* ---- Final answer path ---- */
        if (answer) {
            if (on_step) {
                on_step(step, thought, "final_answer", answer, user_data);
            }
            if (agent->params.verbose) {
                fprintf(stderr, "[neuronos] Final answer: %s\n", answer);
            }

            result.text = answer;
            result.steps_taken = steps_taken;
            result.total_ms = get_time_ms() - t_start;
            result.status = NEURONOS_OK;

            free(thought);
            free(action);
            free(args);
            goto cleanup;
        }

        /* ---- Tool call path ---- */
        if (action && agent->tools) {
            step_actions[step] = strdup(action);

            if (agent->params.verbose) {
                fprintf(stderr, "[neuronos] Tool: %s(%s)\n", action, args ? args : "{}");
            }

            neuronos_tool_result_t tool_result = neuronos_tool_execute(agent->tools, action, args ? args : "{}");

            const char * obs = tool_result.success ? tool_result.output
                                                   : (tool_result.error ? tool_result.error : "Tool execution failed");

            step_observations[step] = strdup(obs);

            if (on_step) {
                on_step(step, thought, action, obs, user_data);
            }

            if (agent->params.verbose) {
                fprintf(stderr, "[neuronos] Observation: %.200s%s\n", obs, strlen(obs) > 200 ? "..." : "");
            }

            neuronos_tool_result_free(&tool_result);
        } else {
            /* No action and no answer — model confused, try to continue */
            step_observations[step] = strdup("Error: You must provide either \"action\" with \"args\" to use a tool, "
                                             "or \"answer\" to give a final answer. Please try again.");
            step_actions[step] = strdup("error");
        }

        free(thought);
        free(action);
        free(args);
    }

    /* If we get here, max steps reached without final answer */
    if (result.status != NEURONOS_OK) {
        if (steps_taken >= max_steps) {
            result.status = NEURONOS_ERROR_MAX_STEPS;
        }
    }
    result.steps_taken = steps_taken;
    result.total_ms = get_time_ms() - t_start;

cleanup:
    /* Log final answer to recall memory */
    if (agent->memory && result.text) {
        neuronos_memory_recall_add(agent->memory, agent->session_id,
                                   "assistant", result.text, (int)(strlen(result.text) / 4));
    }

    /* Restore original system prompt (enriched one was temporary) */
    if (original_prompt) {
        free(agent->system_prompt);
        agent->system_prompt = original_prompt;
    }

    /* Free history */
    for (int i = 0; i < max_steps; i++) {
        free((void *)step_outputs[i]);
        free((void *)step_actions[i]);
        free((void *)step_observations[i]);
    }
    free(step_outputs);
    free(step_actions);
    free(step_observations);
    free(context_summary);

    return result;
}

void neuronos_agent_result_free(neuronos_agent_result_t * result) {
    if (!result)
        return;
    free(result->text);
    result->text = NULL;
}

/* ============================================================
 * CONTEXT API: Token counting and usage tracking
 * ============================================================ */

int neuronos_context_token_count(const neuronos_agent_t * agent) {
    if (!agent || !agent->model) return 0;
    /* Returns estimated tokens used by system prompt + core memory.
     * During agent_run, actual token count is tracked per-step internally. */
    return estimate_tokens(agent->system_prompt);
}

int neuronos_context_capacity(const neuronos_agent_t * agent) {
    if (!agent || !agent->model) return 0;
    return neuronos_model_context_size(agent->model);
}

float neuronos_context_usage_ratio(const neuronos_agent_t * agent) {
    int cap = neuronos_context_capacity(agent);
    if (cap <= 0) return 0.0f;
    return (float)neuronos_context_token_count(agent) / (float)cap;
}

int neuronos_context_compact(neuronos_agent_t * agent) {
    /* Context compaction is performed automatically during agent_run()
     * when the prompt exceeds 80% of context capacity.
     * This function is a no-op outside of an active agent run.
     * Returns 0 (no tokens freed outside of a run). */
    (void)agent;
    return 0;
}

/* ============================================================
 * CONVERSATION HISTORY HELPERS (for interactive mode)
 * ============================================================ */

static void conv_history_push(neuronos_agent_t * agent, const char * role, const char * content) {
    if (!agent || !role || !content) return;

    /* Grow if needed */
    if (agent->conv_len >= agent->conv_cap) {
        size_t new_cap = agent->conv_cap * 2;
        char ** new_roles = realloc(agent->conv_roles, new_cap * sizeof(char *));
        char ** new_contents = realloc(agent->conv_contents, new_cap * sizeof(char *));
        if (!new_roles || !new_contents) return;
        agent->conv_roles = new_roles;
        agent->conv_contents = new_contents;
        agent->conv_cap = new_cap;
    }

    agent->conv_roles[agent->conv_len] = strdup(role);
    agent->conv_contents[agent->conv_len] = strdup(content);
    agent->conv_len++;
}

void neuronos_agent_clear_history(neuronos_agent_t * agent) {
    if (!agent) return;
    for (size_t i = 0; i < agent->conv_len; i++) {
        free(agent->conv_roles[i]);
        free(agent->conv_contents[i]);
    }
    agent->conv_len = 0;
}

/*
 * Unescape a JSON string value (handle \\n, \\t, \\", \\\\, etc.)
 * Returns newly allocated string. Caller must free.
 */
static char * json_unescape(const char * s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char * out = malloc(len + 1);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            switch (s[i + 1]) {
            case 'n':  out[j++] = '\n'; i++; break;
            case 't':  out[j++] = '\t'; i++; break;
            case 'r':  out[j++] = '\r'; i++; break;
            case '"':  out[j++] = '"';  i++; break;
            case '\\': out[j++] = '\\'; i++; break;
            case '/':  out[j++] = '/';  i++; break;
            default:   out[j++] = s[i]; break;
            }
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

/*
 * Build the interactive prompt from conversation history + current turn steps.
 *
 * Messages:
 *   [0] system  = interactive system prompt (with tool descriptions + memory)
 *   [1..N] user/assistant = conversation history
 *   [N+1] user = current user input
 *   [N+2..] assistant/user = current turn steps (tool calls + observations)
 */
static char * build_interactive_prompt(const neuronos_agent_t * agent,
                                       const char * enriched_prompt,
                                       const char ** step_outputs,
                                       const char ** step_actions,
                                       const char ** step_observations,
                                       int n_steps) {
    /* Count total messages */
    size_t n_msgs = 1; /* system */
    n_msgs += agent->conv_len; /* conversation history (includes current user input) */
    for (int i = 0; i < n_steps; i++) {
        if (step_outputs[i]) n_msgs++;
        if (step_observations[i]) n_msgs++;
    }

    neuronos_chat_msg_t * msgs = calloc(n_msgs, sizeof(neuronos_chat_msg_t));
    char ** obs_bufs = calloc((size_t)(n_steps > 0 ? n_steps : 1), sizeof(char *));
    if (!msgs || !obs_bufs) {
        free(msgs);
        free(obs_bufs);
        return NULL;
    }

    size_t idx = 0;

    /* System prompt */
    msgs[idx].role = "system";
    msgs[idx].content = enriched_prompt;
    idx++;

    /* Conversation history */
    for (size_t i = 0; i < agent->conv_len; i++) {
        msgs[idx].role = agent->conv_roles[i];
        msgs[idx].content = agent->conv_contents[i];
        idx++;
    }

    /* Current turn steps (tool calls + observations) */
    for (int i = 0; i < n_steps; i++) {
        if (step_outputs[i]) {
            msgs[idx].role = "assistant";
            msgs[idx].content = step_outputs[i];
            idx++;
        }
        if (step_observations[i]) {
            const char * tool = step_actions[i] ? step_actions[i] : "tool";
            size_t obs_len = strlen("Observation from : ") + strlen(tool) + strlen(step_observations[i]) + 1;
            obs_bufs[i] = malloc(obs_len);
            snprintf(obs_bufs[i], obs_len, "Observation from %s: %s", tool, step_observations[i]);
            msgs[idx].role = "user";
            msgs[idx].content = obs_bufs[i];
            idx++;
        }
    }

    /* Format with chat template */
    char * formatted = NULL;
    neuronos_status_t st = neuronos_chat_format(agent->model, NULL, msgs, idx, true, &formatted);

    /* Free temp buffers */
    for (int i = 0; i < n_steps; i++) {
        free(obs_bufs[i]);
    }
    free(obs_bufs);
    free(msgs);

    if (st == NEURONOS_OK && formatted) {
        return formatted;
    }

    return NULL;
}

/* ============================================================
 * INTERACTIVE AGENT CHAT — Multi-turn Conversational Agent
 * ============================================================ */

neuronos_agent_result_t neuronos_agent_chat(neuronos_agent_t * agent, const char * user_input,
                                            neuronos_agent_step_cb on_step, void * user_data) {
    neuronos_agent_result_t result = {0};
    double t_start = get_time_ms();

    if (!agent || !user_input) {
        result.status = NEURONOS_ERROR_INVALID_PARAM;
        return result;
    }

    /* Add user message to conversation history */
    conv_history_push(agent, "user", user_input);

    /* Enrich system prompt with memory if attached */
    char * enriched_prompt = NULL;
    if (agent->memory) {
        enriched_prompt = build_memory_enriched_prompt(agent, agent->interactive_prompt);
        /* Log user input to recall memory */
        neuronos_memory_recall_add(agent->memory, agent->session_id,
                                   "user", user_input, (int)(strlen(user_input) / 4));
    } else {
        enriched_prompt = strdup(agent->interactive_prompt);
    }

    int max_steps = agent->params.max_steps;

    /* Step history (tool calls within this turn only) */
    const char ** step_outputs = calloc((size_t)max_steps, sizeof(char *));
    const char ** step_actions = calloc((size_t)max_steps, sizeof(char *));
    const char ** step_observations = calloc((size_t)max_steps, sizeof(char *));

    if (!step_outputs || !step_actions || !step_observations) {
        free(step_outputs);
        free(step_actions);
        free(step_observations);
        free(enriched_prompt);
        result.status = NEURONOS_ERROR_GENERATE;
        return result;
    }

    int steps_taken = 0;

    for (int step = 0; step < max_steps; step++) {
        if (agent->params.verbose) {
            fprintf(stderr, "\n[neuronos] ── Turn step %d/%d ──\n", step + 1, max_steps);
        }

        /* Build prompt from conversation history + current turn steps */
        char * prompt = build_interactive_prompt(agent, enriched_prompt,
                                                 step_outputs, step_actions,
                                                 step_observations, step);
        if (!prompt) {
            result.status = NEURONOS_ERROR_GENERATE;
            break;
        }

        if (agent->params.verbose) {
            fprintf(stderr, "[neuronos] Prompt: %zu chars (~%d tokens)\n",
                    strlen(prompt), estimate_tokens(prompt));
        }

        /* Generate with interactive grammar (reply + tool + answer) */
        neuronos_gen_params_t gen_params = {
            .prompt = prompt,
            .max_tokens = agent->params.max_tokens_per_step,
            .temperature = agent->params.temperature,
            .top_p = 0.95f,
            .top_k = 40,
            .grammar = INTERACTIVE_GRAMMAR,
            .grammar_root = "root",
            .on_token = NULL,
            .user_data = NULL,
            .seed = 0,
        };

        neuronos_gen_result_t gen = neuronos_generate(agent->model, gen_params);
        free(prompt);

        if (gen.status != NEURONOS_OK || !gen.text) {
            neuronos_gen_result_free(&gen);
            result.status = NEURONOS_ERROR_GENERATE;
            break;
        }

        if (agent->params.verbose) {
            fprintf(stderr, "[neuronos] Model output: %s\n", gen.text);
        }

        steps_taken++;

        /* Parse the JSON response — check for reply, action, or answer */
        char * reply = json_extract_string(gen.text, "reply");
        char * thought = json_extract_string(gen.text, "thought");
        char * answer = json_extract_string(gen.text, "answer");
        char * action = json_extract_string(gen.text, "action");
        char * args = json_extract_object(gen.text, "args");

        /* ---- Direct reply path (new: conversational response) ---- */
        if (reply) {
            char * unescaped = json_unescape(reply);
            if (on_step) {
                on_step(step, NULL, "reply", unescaped ? unescaped : reply, user_data);
            }

            /* Add assistant reply to conversation history */
            conv_history_push(agent, "assistant", unescaped ? unescaped : reply);

            result.text = unescaped ? unescaped : strdup(reply);
            if (unescaped) free(reply); else result.text = reply;
            result.steps_taken = steps_taken;
            result.total_ms = get_time_ms() - t_start;
            result.status = NEURONOS_OK;

            free(thought);
            free(answer);
            free(action);
            free(args);
            neuronos_gen_result_free(&gen);
            goto cleanup;
        }

        /* ---- Final answer path (after tool use) ---- */
        if (answer) {
            char * unescaped = json_unescape(answer);
            if (on_step) {
                on_step(step, thought, "final_answer",
                        unescaped ? unescaped : answer, user_data);
            }

            /* Add answer to conversation history */
            conv_history_push(agent, "assistant", unescaped ? unescaped : answer);

            result.text = unescaped ? unescaped : strdup(answer);
            if (unescaped) free(answer); else result.text = answer;
            result.steps_taken = steps_taken;
            result.total_ms = get_time_ms() - t_start;
            result.status = NEURONOS_OK;

            free(reply);
            free(thought);
            free(action);
            free(args);
            neuronos_gen_result_free(&gen);
            goto cleanup;
        }

        /* ---- Tool call path ---- */
        if (action && agent->tools) {
            step_outputs[step] = strdup(gen.text);
            step_actions[step] = strdup(action);

            if (on_step) {
                on_step(step, thought, action, NULL, user_data);
            }

            if (agent->params.verbose) {
                fprintf(stderr, "[neuronos] Tool: %s(%s)\n", action, args ? args : "{}");
            }

            neuronos_tool_result_t tool_result = neuronos_tool_execute(
                agent->tools, action, args ? args : "{}");

            const char * obs = tool_result.success
                ? tool_result.output
                : (tool_result.error ? tool_result.error : "Tool execution failed");

            step_observations[step] = strdup(obs);

            if (on_step) {
                on_step(step, NULL, action, obs, user_data);
            }

            if (agent->params.verbose) {
                fprintf(stderr, "[neuronos] Observation: %.200s%s\n",
                        obs, strlen(obs) > 200 ? "..." : "");
            }

            neuronos_tool_result_free(&tool_result);
        } else {
            /* No reply, no answer, no action — model confused */
            step_outputs[step] = strdup(gen.text);
            step_observations[step] = strdup(
                "Error: respond with {\"reply\": \"...\"} to chat, "
                "or {\"thought\": \"...\", \"action\": \"...\", \"args\": {...}} to use a tool.");
            step_actions[step] = strdup("error");
        }

        free(reply);
        free(thought);
        free(answer);
        free(action);
        free(args);
        neuronos_gen_result_free(&gen);
    }

    /* Max steps reached without final response */
    if (result.status != NEURONOS_OK) {
        if (steps_taken >= max_steps) {
            result.status = NEURONOS_ERROR_MAX_STEPS;
        }
        /* Provide fallback text */
        result.text = strdup("I wasn't able to complete that task within the step limit. "
                             "Please try rephrasing your request.");
        conv_history_push(agent, "assistant", result.text);
    }
    result.steps_taken = steps_taken;
    result.total_ms = get_time_ms() - t_start;

cleanup:
    /* Log final response to recall memory */
    if (agent->memory && result.text) {
        neuronos_memory_recall_add(agent->memory, agent->session_id,
                                   "assistant", result.text, (int)(strlen(result.text) / 4));
    }

    /* Free turn-local step history */
    for (int i = 0; i < max_steps; i++) {
        free((void *)step_outputs[i]);
        free((void *)step_actions[i]);
        free((void *)step_observations[i]);
    }
    free(step_outputs);
    free(step_actions);
    free(step_observations);
    free(enriched_prompt);

    return result;
}

/* ============================================================
 * QUICK AGENT — One-shot convenience
 * ============================================================ */

char * neuronos_quick_agent(const char * model_path, const char * prompt, int max_steps) {
    if (!model_path || !prompt)
        return NULL;

    /* Init engine */
    neuronos_engine_params_t eparams = {
        .n_threads = 0, /* auto-detect */
        .n_gpu_layers = 0,
        .verbose = false,
    };
    neuronos_engine_t * engine = neuronos_init(eparams);
    if (!engine)
        return NULL;

    /* Load model */
    neuronos_model_params_t mparams = {
        .model_path = model_path,
        .context_size = 0, /* auto-detect optimal context */
        .use_mmap = true,
    };
    neuronos_model_t * model = neuronos_model_load(engine, mparams);
    if (!model) {
        neuronos_shutdown(engine);
        return NULL;
    }

    /* Create tools with default set */
    neuronos_tool_registry_t * tools = neuronos_tool_registry_create();
    neuronos_tool_register_defaults(tools, NEURONOS_CAP_FILESYSTEM);

    /* Create and run agent */
    neuronos_agent_params_t aparams = {
        .max_steps = max_steps > 0 ? max_steps : 5,
        .max_tokens_per_step = 512,
        .temperature = 0.3f,
        .verbose = false,
    };
    neuronos_agent_t * agent = neuronos_agent_create(model, tools, aparams);
    neuronos_agent_result_t result = neuronos_agent_run(agent, prompt, NULL, NULL);

    char * answer = result.text ? strdup(result.text) : NULL;

    /* Cleanup */
    neuronos_agent_result_free(&result);
    neuronos_agent_free(agent);
    neuronos_tool_registry_free(tools);
    neuronos_model_free(model);
    neuronos_shutdown(engine);

    return answer;
}
