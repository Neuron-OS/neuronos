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

/* ---- Built-in GBNF grammar for tool_call/final_answer ---- */
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
    "characters ::= \"\" | characters character\n"
    "character ::= [^\"\\\\] | \"\\\\\" escape\n"
    "escape ::= [\"\\\\/bfnrt] | \"u\" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]\n"
    "number ::= integer fraction? exponent?\n"
    "integer ::= \"-\"? ( \"0\" | [1-9] [0-9]* )\n"
    "fraction ::= \".\" [0-9]+\n"
    "exponent ::= [eE] [+-]? [0-9]+\n"
    "ws ::= [ \\t\\n\\r]*\n";

/* ---- Default system prompt template ---- */
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

/* ---- Internal agent struct ---- */
struct neuronos_agent {
    neuronos_model_t * model;
    neuronos_tool_registry_t * tools;
    neuronos_agent_params_t params;
    char * system_prompt;
};

/* ---- Helpers ---- */
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
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
                           const char ** step_actions, const char ** step_observations, int n_steps) {
    /* Count messages: system + user + 2 per completed step (assistant + observation) */
    size_t n_msgs = 2; /* system + user */
    for (int i = 0; i < n_steps; i++) {
        if (step_outputs[i])
            n_msgs++;
        if (step_observations[i])
            n_msgs++;
    }

    neuronos_chat_msg_t * msgs = calloc(n_msgs, sizeof(neuronos_chat_msg_t));
    char ** obs_bufs = calloc((size_t)n_steps, sizeof(char *)); /* owned observation strings */
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

    for (int i = 0; i < n_steps; i++) {
        if (step_outputs[i]) {
            msgs[idx].role = "assistant";
            msgs[idx].content = step_outputs[i];
            idx++;
        }
        if (step_observations[i]) {
            /* Build observation string: "Observation from <tool>: <result>" */
            const char * tool = step_actions[i] ? step_actions[i] : "tool";
            size_t obs_len = strlen("Observation from : ") + strlen(tool) + strlen(step_observations[i]) + 1;
            obs_bufs[i] = malloc(obs_len);
            snprintf(obs_bufs[i], obs_len, "Observation from %s: %s", tool, step_observations[i]);
            msgs[idx].role = "user";
            msgs[idx].content = obs_bufs[i];
            idx++;
        }
    }

    /* Try chat template formatting */
    char * formatted = NULL;
    neuronos_status_t st = neuronos_chat_format(agent->model, NULL, msgs, idx, true, &formatted);

    /* Free temporary observation buffers */
    for (int i = 0; i < n_steps; i++) {
        free(obs_bufs[i]);
    }
    free(obs_bufs);
    free(msgs);

    if (st == NEURONOS_OK && formatted) {
        return formatted; /* Caller must use neuronos_free() */
    }

    /* Fallback: plain text (legacy format) */
    size_t total = strlen(agent->system_prompt) + strlen(user_input) + 256;
    for (int i = 0; i < n_steps; i++) {
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

    for (int i = 0; i < n_steps; i++) {
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
    agent->params.context_budget = params.context_budget > 0 ? params.context_budget : 1536;
    agent->params.verbose = params.verbose;

    /* Build default system prompt with tool descriptions */
    char * tool_desc;
    if (tools) {
        tool_desc = neuronos_tool_prompt_description(tools);
    } else {
        tool_desc = strdup("No tools available.\n");
    }

    size_t prompt_size = strlen(DEFAULT_SYSTEM_PROMPT_TEMPLATE) + strlen(tool_desc) + 64;
    agent->system_prompt = malloc(prompt_size);
    snprintf(agent->system_prompt, prompt_size, DEFAULT_SYSTEM_PROMPT_TEMPLATE, tool_desc);
    free(tool_desc);

    return agent;
}

void neuronos_agent_free(neuronos_agent_t * agent) {
    if (!agent)
        return;
    free(agent->system_prompt);
    free(agent);
}

void neuronos_agent_set_system_prompt(neuronos_agent_t * agent, const char * system_prompt) {
    if (!agent || !system_prompt)
        return;
    free(agent->system_prompt);
    agent->system_prompt = strdup(system_prompt);
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

    int steps_taken = 0;

    for (int step = 0; step < max_steps; step++) {
        if (agent->params.verbose) {
            fprintf(stderr, "\n[neuronos] ── Step %d/%d ──\n", step + 1, max_steps);
        }

        /* Build the prompt with conversation history */
        char * prompt = build_prompt(agent, user_input, step_outputs, step_actions, step_observations, step);

        if (!prompt) {
            result.status = NEURONOS_ERROR_GENERATE;
            break;
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
    /* Free history */
    for (int i = 0; i < max_steps; i++) {
        free((void *)step_outputs[i]);
        free((void *)step_actions[i]);
        free((void *)step_observations[i]);
    }
    free(step_outputs);
    free(step_actions);
    free(step_observations);

    return result;
}

void neuronos_agent_result_free(neuronos_agent_result_t * result) {
    if (!result)
        return;
    free(result->text);
    result->text = NULL;
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
        .context_size = 2048,
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
