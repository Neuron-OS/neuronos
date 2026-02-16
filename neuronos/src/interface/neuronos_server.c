/* ============================================================
 * NeuronOS — Minimal HTTP Server (OpenAI + Anthropic compatible + Agent UI)
 *
 * Endpoints:
 *   POST /v1/chat/completions  — Chat API (OpenAI-compatible, SSE)
 *   POST /v1/completions       — Text completion
 *   POST /v1/messages          — Anthropic Messages API (SSE) — Claude Code backend
 *   GET  /v1/models            — List models
 *   GET  /health               — Health check
 *   POST /api/chat             — Agent chat (SSE streaming, tool use)
 *   GET  /                     — Chat UI (agent mode) or status page
 *
 * No external dependencies — pure POSIX sockets.
 * Designed for: desktop apps, browser clients, mobile apps, Claude Code, OpenCode.
 * ============================================================ */
#include "neuronos/neuronos.h"
#include "neuronos/neuronos_json.h"
#include "neuronos_chat_ui.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
    #define INVALID_SOCK INVALID_SOCKET
    #define close_socket closesocket
    /* MSVC/Clang-cl don't define ssize_t — use the Windows SDK equivalent */
    typedef SSIZE_T ssize_t;
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
typedef int socket_t;
    #define INVALID_SOCK (-1)
    #define close_socket close
#endif

/* ---- Global state ---- */
static volatile int g_running = 1;
static neuronos_model_t * g_model = NULL;
static neuronos_tool_registry_t * g_tools = NULL;
static neuronos_agent_t * g_agent = NULL; /* Non-NULL = agent mode with chat UI */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* JSON escape: use nj_escape() / nj_escape_n() from neuronos_json.h */

/* ---- HTTP helpers ---- */

#define MAX_REQUEST 65536
#define MAX_RESPONSE 262144

typedef struct {
    char method[16];
    char path[256];
    char body[MAX_REQUEST];
    int body_len;
    int content_length;
    bool accept_gzip;
} http_request_t;

static int parse_request(const char * raw, int raw_len, http_request_t * req) {
    memset(req, 0, sizeof(*req));

    /* Parse request line */
    const char * line_end = strstr(raw, "\r\n");
    if (!line_end)
        return -1;

    sscanf(raw, "%15s %255s", req->method, req->path);

    /* Detect Accept-Encoding: gzip */
    const char * ae = strstr(raw, "Accept-Encoding:");
    if (!ae) ae = strstr(raw, "accept-encoding:");
    if (ae) {
        const char * ae_end = strstr(ae, "\r\n");
        if (ae_end) {
            /* Search within this header line */
            const char * gz = strstr(ae, "gzip");
            if (gz && gz < ae_end) req->accept_gzip = true;
        }
    }

    /* Find Content-Length */
    const char * cl = strstr(raw, "Content-Length:");
    if (!cl)
        cl = strstr(raw, "content-length:");
    if (cl) {
        req->content_length = atoi(cl + 15);
    }

    /* Find body (after \r\n\r\n) */
    const char * body_start = strstr(raw, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        int body_available = raw_len - (int)(body_start - raw);
        if (body_available > 0) {
            int copy_len = body_available;
            if (copy_len >= MAX_REQUEST)
                copy_len = MAX_REQUEST - 1;
            memcpy(req->body, body_start, (size_t)copy_len);
            req->body[copy_len] = '\0';
            req->body_len = copy_len;
        }
    }

    return 0;
}

static void send_response(socket_t sock, int status_code, const char * status_text, const char * content_type,
                          const char * body, int body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %d\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status_code, status_text, content_type, body_len);
    send(sock, header, hlen, 0);
    if (body && body_len > 0) {
        send(sock, body, body_len, 0);
    }
}

static void send_json(socket_t sock, int status, const char * json) {
    send_response(sock, status, status == 200 ? "OK" : "Error", "application/json", json, (int)strlen(json));
}

/* Send a gzip-compressed response (browser decompresses automatically) */
static void send_gzip_response(socket_t sock, const char * content_type,
                               const unsigned char * data, int data_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Encoding: gzip\r\n"
                        "Content-Length: %d\r\n"
                        "Cache-Control: no-cache\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        content_type, data_len);
    send(sock, header, hlen, 0);
    send(sock, (const char *)data, data_len, 0);
}

/* JSON parsing: use nj_copy_str/nj_find_int/nj_find_float from neuronos_json.h */

/* Extract content from messages array (last user message) */
static int extract_last_user_content(const char * json, char * out, size_t out_len) {
    /* Simple heuristic: find last "content": "..." with "role": "user" */
    const char * last_content = NULL;
    const char * p = json;
    while ((p = strstr(p, "\"content\"")) != NULL) {
        last_content = p;
        p++;
    }
    if (!last_content)
        return -1;

    return nj_copy_str(last_content - 1, "content", out, out_len) >= 0 ? 0 : -1;
}

/*
 * Parse OpenAI-format messages array into neuronos_chat_msg_t array.
 * The messages array looks like: "messages": [{"role":"user","content":"hello"}, ...]
 *
 * Returns malloc'd arrays. Caller must free msgs, and each role/content string,
 * then the msgs array itself. Returns count via *out_count.
 * Returns NULL on failure.
 */
typedef struct {
    char * role;
    char * content;
} parsed_msg_t;

static parsed_msg_t * parse_messages_array(const char * json, int * out_count) {
    *out_count = 0;

    /* Find "messages" array */
    const char * arr = strstr(json, "\"messages\"");
    if (!arr)
        return NULL;

    /* Find opening bracket */
    const char * bracket = strchr(arr + 10, '[');
    if (!bracket)
        return NULL;
    bracket++;

    /* Count messages (rough: count occurrences of "role" within the array) */
    int cap = 32;
    parsed_msg_t * msgs = calloc((size_t)cap, sizeof(parsed_msg_t));
    if (!msgs)
        return NULL;

    int count = 0;
    const char * p = bracket;

    while (*p) {
        /* Find next message object opening brace */
        const char * obj_start = strchr(p, '{');
        if (!obj_start)
            break;

        /* Check if we've gone past the closing bracket of the messages array */
        /* Scan from p to obj_start: if we hit ']' first, we're done */
        for (const char * scan = p; scan < obj_start; scan++) {
            if (*scan == ']')
                goto done;
        }

        /* Find matching closing brace for this message object */
        int depth = 1;
        const char * obj_end = obj_start + 1;
        bool in_str = false;
        while (*obj_end && depth > 0) {
            if (*obj_end == '"' && *(obj_end - 1) != '\\')
                in_str = !in_str;
            if (!in_str) {
                if (*obj_end == '{')
                    depth++;
                else if (*obj_end == '}')
                    depth--;
            }
            if (depth > 0)
                obj_end++;
        }
        if (depth != 0)
            break;

        /* We have the object from obj_start to obj_end (inclusive) */
        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        char * obj = malloc(obj_len + 1);
        if (!obj)
            break;
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        /* Extract role and content */
        char role_buf[64] = {0};
        char * content_str = NULL;
        if (nj_copy_str(obj, "role", role_buf, sizeof(role_buf)) >= 0 &&
            (content_str = nj_alloc_str(obj, "content")) != NULL) {
            if (count >= cap) {
                cap *= 2;
                msgs = realloc(msgs, (size_t)cap * sizeof(parsed_msg_t));
                if (!msgs) {
                    free(content_str);
                    free(obj);
                    *out_count = 0;
                    return NULL;
                }
            }
            msgs[count].role = strdup(role_buf);
            msgs[count].content = content_str; /* already allocated */
            if (!msgs[count].role || !msgs[count].content) {
                free(msgs[count].role);
                free(msgs[count].content);
                free(obj);
                /* Free previously allocated messages */
                for (int j = 0; j < count; j++) {
                    free(msgs[j].role);
                    free(msgs[j].content);
                }
                free(msgs);
                *out_count = 0;
                return NULL;
            }
            count++;
        }

        free(obj);
        p = obj_end + 1;
    }

done:
    *out_count = count;
    if (count == 0) {
        free(msgs);
        return NULL;
    }
    return msgs;
}

static void free_parsed_msgs(parsed_msg_t * msgs, int count) {
    if (!msgs)
        return;
    for (int i = 0; i < count; i++) {
        free(msgs[i].role);
        free(msgs[i].content);
    }
    free(msgs);
}

/* ---- Endpoint Handlers ---- */

static void handle_health(socket_t sock) {
    send_json(sock, 200, "{\"status\":\"ok\",\"engine\":\"neuronos\",\"version\":\"" NEURONOS_VERSION_STRING "\"}");
}

static void handle_models(socket_t sock) {
    const char * response = "{\"object\":\"list\",\"data\":[{"
                            "\"id\":\"neuronos-local\","
                            "\"object\":\"model\","
                            "\"owned_by\":\"local\","
                            "\"permission\":[]}]}";
    send_json(sock, 200, response);
}

static void handle_completions(socket_t sock, const char * body) {
    if (!g_model) {
        send_json(sock, 503, "{\"error\":{\"message\":\"No model loaded\"}}");
        return;
    }

    char prompt[8192] = {0};
    nj_copy_str(body, "prompt", prompt, sizeof(prompt));
    if (prompt[0] == '\0') {
        send_json(sock, 400, "{\"error\":{\"message\":\"Missing prompt\"}}");
        return;
    }

    int max_tokens = nj_find_int(body, "max_tokens", 256);
    float temperature = nj_find_float(body, "temperature", 0.7f);

    neuronos_gen_params_t gparams = {
        .prompt = prompt,
        .max_tokens = max_tokens,
        .temperature = temperature,
        .top_p = 0.95f,
        .top_k = 40,
        .grammar = NULL,
        .on_token = NULL,
        .user_data = NULL,
        .seed = 0,
    };

    neuronos_gen_result_t result = neuronos_generate(g_model, gparams);

    if (result.status == NEURONOS_OK && result.text) {
        /* JSON-escape the generated text */
        char * escaped = nj_escape(result.text);
        if (!escaped) {
            send_json(sock, 500, "{\"error\":{\"message\":\"Memory allocation failed\"}}");
            neuronos_gen_result_free(&result);
            return;
        }

        /* Build OpenAI-compatible response */
        size_t resp_cap = strlen(escaped) + 512;
        char * response = malloc(resp_cap);
        if (!response) {
            free(escaped);
            send_json(sock, 500, "{\"error\":{\"message\":\"Memory allocation failed\"}}");
            neuronos_gen_result_free(&result);
            return;
        }

        snprintf(response, resp_cap,
                 "{\"id\":\"cmpl-neuronos\","
                 "\"object\":\"text_completion\","
                 "\"created\":%d,"
                 "\"model\":\"neuronos-local\","
                 "\"choices\":[{"
                 "\"text\":\"%s\","
                 "\"index\":0,"
                 "\"finish_reason\":\"stop\""
                 "}],"
                 "\"usage\":{"
                 "\"completion_tokens\":%d,"
                 "\"total_tokens\":%d"
                 "}}",
                 0, /* timestamp placeholder */
                 escaped, result.n_tokens, result.n_tokens);

        send_json(sock, 200, response);
        free(response);
        free(escaped);
    } else {
        send_json(sock, 500, "{\"error\":{\"message\":\"Generation failed\"}}");
    }

    neuronos_gen_result_free(&result);
}

/* ---- SSE Streaming support ---- */

typedef struct {
    socket_t sock;
    int n_tokens;
} sse_stream_ctx_t;

/* SSE streaming callback: sends each token as an SSE event */
static bool sse_token_callback(const char * token_text, void * user_data) {
    sse_stream_ctx_t * ctx = (sse_stream_ctx_t *)user_data;
    if (!ctx || !token_text)
        return false;

    /* JSON-escape the token text */
    char * escaped = nj_escape(token_text);
    if (!escaped)
        return false;

    /* OpenAI streaming format: data: {"choices":[{"delta":{"content":"..."}}]} */
    char chunk[4096];
    int len = snprintf(chunk, sizeof(chunk),
                       "data: {\"id\":\"chatcmpl-neuronos\","
                       "\"object\":\"chat.completion.chunk\","
                       "\"model\":\"neuronos-local\","
                       "\"choices\":[{"
                       "\"index\":0,"
                       "\"delta\":{\"content\":\"%s\"},"
                       "\"finish_reason\":null"
                       "}]}\n\n",
                       escaped);
    free(escaped);

    ssize_t sent = send(ctx->sock, chunk, (size_t)len, 0);
    ctx->n_tokens++;

    return (sent > 0);
}

/* Send SSE headers to start streaming */
static void send_sse_headers(socket_t sock) {
    const char * headers = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/event-stream\r\n"
                           "Cache-Control: no-cache\r\n"
                           "Connection: keep-alive\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                           "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
                           "\r\n";
    send(sock, headers, strlen(headers), 0);
}

/* JSON bool: use nj_find_bool() from neuronos_json.h */

static void handle_chat_completions(socket_t sock, const char * body) {
    if (!g_model) {
        send_json(sock, 503, "{\"error\":{\"message\":\"No model loaded\"}}");
        return;
    }

    /* Parse messages array and format with chat template */
    int msg_count = 0;
    parsed_msg_t * parsed = parse_messages_array(body, &msg_count);

    char * formatted_prompt = NULL;

    if (parsed && msg_count > 0) {
        /* Build neuronos_chat_msg_t array from parsed messages */
        neuronos_chat_msg_t * chat_msgs = calloc((size_t)msg_count, sizeof(neuronos_chat_msg_t));
        if (!chat_msgs) {
            free_parsed_msgs(parsed, msg_count);
            send_json(sock, 500, "{\"error\":{\"message\":\"Memory allocation failed\"}}");
            return;
        }
        if (chat_msgs) {
            for (int i = 0; i < msg_count; i++) {
                chat_msgs[i].role = parsed[i].role;
                chat_msgs[i].content = parsed[i].content;
            }

            neuronos_chat_format(g_model, NULL, chat_msgs, (size_t)msg_count, true, &formatted_prompt);
            free(chat_msgs);
        }
    }

    /* Fallback: extract last user content if template formatting failed */
    char content_fallback[8192] = {0};
    if (!formatted_prompt) {
        if (extract_last_user_content(body, content_fallback, sizeof(content_fallback)) != 0) {
            free_parsed_msgs(parsed, msg_count);
            send_json(sock, 400, "{\"error\":{\"message\":\"Missing messages content\"}}");
            return;
        }
    }

    const char * effective_prompt = formatted_prompt ? formatted_prompt : content_fallback;

    int max_tokens = nj_find_int(body, "max_tokens", 256);
    float temperature = nj_find_float(body, "temperature", 0.7f);
    bool stream = nj_find_bool(body, "stream", false);

    if (stream) {
        /* ── SSE Streaming mode ── */
        send_sse_headers(sock);

        /* Send initial role delta */
        const char * role_chunk = "data: {\"id\":\"chatcmpl-neuronos\","
                                  "\"object\":\"chat.completion.chunk\","
                                  "\"model\":\"neuronos-local\","
                                  "\"choices\":[{"
                                  "\"index\":0,"
                                  "\"delta\":{\"role\":\"assistant\",\"content\":\"\"},"
                                  "\"finish_reason\":null"
                                  "}]}\n\n";
        send(sock, role_chunk, strlen(role_chunk), 0);

        sse_stream_ctx_t ctx = {.sock = sock, .n_tokens = 0};

        neuronos_gen_params_t gparams = {
            .prompt = effective_prompt,
            .max_tokens = max_tokens,
            .temperature = temperature,
            .top_p = 0.95f,
            .top_k = 40,
            .grammar = NULL,
            .on_token = sse_token_callback,
            .user_data = &ctx,
            .seed = 0,
        };

        neuronos_gen_result_t result = neuronos_generate(g_model, gparams);

        /* Send finish chunk */
        const char * done_chunk = "data: {\"id\":\"chatcmpl-neuronos\","
                                  "\"object\":\"chat.completion.chunk\","
                                  "\"model\":\"neuronos-local\","
                                  "\"choices\":[{"
                                  "\"index\":0,"
                                  "\"delta\":{},"
                                  "\"finish_reason\":\"stop\""
                                  "}]}\n\n"
                                  "data: [DONE]\n\n";
        send(sock, done_chunk, strlen(done_chunk), 0);

        neuronos_gen_result_free(&result);
    } else {
        /* ── Non-streaming mode ── */
        neuronos_gen_params_t gparams = {
            .prompt = effective_prompt,
            .max_tokens = max_tokens,
            .temperature = temperature,
            .top_p = 0.95f,
            .top_k = 40,
            .grammar = NULL,
            .on_token = NULL,
            .user_data = NULL,
            .seed = 0,
        };

        neuronos_gen_result_t result = neuronos_generate(g_model, gparams);

        if (result.status == NEURONOS_OK && result.text) {
            /* JSON-escape the generated text */
            char * escaped = nj_escape(result.text);
            if (!escaped) {
                send_json(sock, 500, "{\"error\":{\"message\":\"Memory allocation failed\"}}");
                neuronos_gen_result_free(&result);
                neuronos_free(formatted_prompt);
                free_parsed_msgs(parsed, msg_count);
                return;
            }

            size_t resp_cap = strlen(escaped) + 512;
            char * response = malloc(resp_cap);
            if (!response) {
                free(escaped);
                send_json(sock, 500, "{\"error\":{\"message\":\"Memory allocation failed\"}}");
                neuronos_gen_result_free(&result);
                neuronos_free(formatted_prompt);
                free_parsed_msgs(parsed, msg_count);
                return;
            }

            snprintf(response, resp_cap,
                     "{\"id\":\"chatcmpl-neuronos\","
                     "\"object\":\"chat.completion\","
                     "\"created\":%d,"
                     "\"model\":\"neuronos-local\","
                     "\"choices\":[{"
                     "\"index\":0,"
                     "\"message\":{\"role\":\"assistant\",\"content\":\"%s\"},"
                     "\"finish_reason\":\"stop\""
                     "}],"
                     "\"usage\":{"
                     "\"prompt_tokens\":0,"
                     "\"completion_tokens\":%d,"
                     "\"total_tokens\":%d"
                     "}}",
                     0, escaped, result.n_tokens, result.n_tokens);

            send_json(sock, 200, response);
            free(response);
            free(escaped);
        } else {
            send_json(sock, 500, "{\"error\":{\"message\":\"Generation failed\"}}");
        }

        neuronos_gen_result_free(&result);
    }

    neuronos_free(formatted_prompt);
    free_parsed_msgs(parsed, msg_count);
}

/* ---- Anthropic Messages API (Claude Code backend) ---- */

/**
 * Parse Anthropic-format "system" field.
 * Anthropic puts system prompt as a top-level field, not in messages.
 * Supports: "system": "text" (string form).
 * Returns malloc'd string or NULL.
 */
static char * parse_anthropic_system(const char * json) {
    char buf[8192] = {0};
    if (nj_copy_str(json, "system", buf, sizeof(buf)) >= 0 && buf[0] != '\0') {
        return strdup(buf);
    }
    return NULL;
}

/**
 * SSE callback for Anthropic streaming format.
 * Sends content_block_delta events with text_delta.
 */
typedef struct {
    socket_t sock;
    int n_tokens;
} anthropic_stream_ctx_t;

static bool anthropic_sse_token_callback(const char * token_text, void * user_data) {
    anthropic_stream_ctx_t * ctx = (anthropic_stream_ctx_t *)user_data;
    if (!ctx || !token_text)
        return false;

    char * escaped = nj_escape(token_text);
    if (!escaped)
        return false;

    /* Anthropic streaming format:
     * event: content_block_delta
     * data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"..."}}
     */
    char chunk[4096];
    int len = snprintf(chunk, sizeof(chunk),
                       "event: content_block_delta\n"
                       "data: {\"type\":\"content_block_delta\",\"index\":0,"
                       "\"delta\":{\"type\":\"text_delta\",\"text\":\"%s\"}}\n\n",
                       escaped);
    free(escaped);

    ssize_t sent = send(ctx->sock, chunk, (size_t)len, 0);
    ctx->n_tokens++;

    return (sent > 0);
}

/**
 * POST /v1/messages — Anthropic Messages API
 *
 * Request:  { model, max_tokens, system?, messages, stream?, temperature? }
 * Response: { id, type:"message", role, content:[{type:"text",text}], stop_reason, usage }
 * Streaming: message_start → content_block_start → content_block_delta* →
 *            content_block_stop → message_delta → message_stop
 */
static void handle_anthropic_messages(socket_t sock, const char * body) {
    if (!g_model) {
        /* Anthropic error format */
        send_json(sock, 503,
                  "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
                  "\"message\":\"No model loaded\"}}");
        return;
    }

    /* Parse Anthropic-specific fields */
    char * system_prompt = parse_anthropic_system(body);
    int max_tokens = nj_find_int(body, "max_tokens", 1024);
    float temperature = nj_find_float(body, "temperature", 0.7f);
    bool stream = nj_find_bool(body, "stream", false);

    /* Parse messages array (same format as OpenAI: [{role, content}]) */
    int msg_count = 0;
    parsed_msg_t * parsed = parse_messages_array(body, &msg_count);

    if (!parsed || msg_count == 0) {
        free(system_prompt);
        send_json(sock, 400,
                  "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
                  "\"message\":\"Missing or empty messages array\"}}");
        return;
    }

    /*
     * Build chat messages for neuronos_chat_format().
     * Anthropic puts system prompt in a separate field, so we prepend it
     * as a system message if present.
     */
    int total_msgs = msg_count + (system_prompt ? 1 : 0);
    neuronos_chat_msg_t * chat_msgs = calloc((size_t)total_msgs, sizeof(neuronos_chat_msg_t));
    char * formatted_prompt = NULL;

    if (chat_msgs) {
        int offset = 0;
        if (system_prompt) {
            chat_msgs[0].role = "system";
            chat_msgs[0].content = system_prompt;
            offset = 1;
        }
        for (int i = 0; i < msg_count; i++) {
            chat_msgs[offset + i].role = parsed[i].role;
            chat_msgs[offset + i].content = parsed[i].content;
        }

        neuronos_chat_format(g_model, NULL, chat_msgs, (size_t)total_msgs, true, &formatted_prompt);
        free(chat_msgs);
    }

    /* Fallback: extract last user content */
    char content_fallback[8192] = {0};
    if (!formatted_prompt) {
        if (extract_last_user_content(body, content_fallback, sizeof(content_fallback)) != 0) {
            free(system_prompt);
            free_parsed_msgs(parsed, msg_count);
            send_json(sock, 400,
                      "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
                      "\"message\":\"Missing messages content\"}}");
            return;
        }
    }

    const char * effective_prompt = formatted_prompt ? formatted_prompt : content_fallback;

    if (stream) {
        /* ── Anthropic SSE Streaming ── */
        send_sse_headers(sock);

        /* Event 1: message_start */
        const char * msg_start =
            "event: message_start\n"
            "data: {\"type\":\"message_start\",\"message\":"
            "{\"id\":\"msg_neuronos_01\",\"type\":\"message\",\"role\":\"assistant\","
            "\"content\":[],\"model\":\"neuronos-local\",\"stop_reason\":null,"
            "\"stop_sequence\":null,"
            "\"usage\":{\"input_tokens\":0,\"output_tokens\":0}}}\n\n";
        send(sock, msg_start, strlen(msg_start), 0);

        /* Event 2: content_block_start */
        const char * block_start =
            "event: content_block_start\n"
            "data: {\"type\":\"content_block_start\",\"index\":0,"
            "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
        send(sock, block_start, strlen(block_start), 0);

        /* Event 3 (repeated): content_block_delta — via token callback */
        anthropic_stream_ctx_t ctx = {.sock = sock, .n_tokens = 0};

        neuronos_gen_params_t gparams = {
            .prompt = effective_prompt,
            .max_tokens = max_tokens,
            .temperature = temperature,
            .top_p = 0.95f,
            .top_k = 40,
            .grammar = NULL,
            .on_token = anthropic_sse_token_callback,
            .user_data = &ctx,
            .seed = 0,
        };

        neuronos_gen_result_t result = neuronos_generate(g_model, gparams);

        /* Event 4: content_block_stop */
        const char * block_stop =
            "event: content_block_stop\n"
            "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n";
        send(sock, block_stop, strlen(block_stop), 0);

        /* Event 5: message_delta (with final usage) */
        char msg_delta[512];
        snprintf(msg_delta, sizeof(msg_delta),
                 "event: message_delta\n"
                 "data: {\"type\":\"message_delta\","
                 "\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},"
                 "\"usage\":{\"output_tokens\":%d}}\n\n",
                 ctx.n_tokens);
        send(sock, msg_delta, strlen(msg_delta), 0);

        /* Event 6: message_stop */
        const char * msg_stop =
            "event: message_stop\n"
            "data: {\"type\":\"message_stop\"}\n\n";
        send(sock, msg_stop, strlen(msg_stop), 0);

        neuronos_gen_result_free(&result);
    } else {
        /* ── Non-streaming Anthropic response ── */
        neuronos_gen_params_t gparams = {
            .prompt = effective_prompt,
            .max_tokens = max_tokens,
            .temperature = temperature,
            .top_p = 0.95f,
            .top_k = 40,
            .grammar = NULL,
            .on_token = NULL,
            .user_data = NULL,
            .seed = 0,
        };

        neuronos_gen_result_t result = neuronos_generate(g_model, gparams);

        if (result.status == NEURONOS_OK && result.text) {
            char * escaped = nj_escape(result.text);
            if (!escaped) {
                send_json(sock, 500,
                          "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
                          "\"message\":\"Memory allocation failed\"}}");
                neuronos_gen_result_free(&result);
                neuronos_free(formatted_prompt);
                free(system_prompt);
                free_parsed_msgs(parsed, msg_count);
                return;
            }

            /* Anthropic Messages response format */
            size_t resp_cap = strlen(escaped) + 512;
            char * response = malloc(resp_cap);
            if (!response) {
                free(escaped);
                send_json(sock, 500,
                          "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
                          "\"message\":\"Memory allocation failed\"}}");
                neuronos_gen_result_free(&result);
                neuronos_free(formatted_prompt);
                free(system_prompt);
                free_parsed_msgs(parsed, msg_count);
                return;
            }

            snprintf(response, resp_cap,
                     "{\"id\":\"msg_neuronos_01\","
                     "\"type\":\"message\","
                     "\"role\":\"assistant\","
                     "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],"
                     "\"model\":\"neuronos-local\","
                     "\"stop_reason\":\"end_turn\","
                     "\"stop_sequence\":null,"
                     "\"usage\":{"
                     "\"input_tokens\":0,"
                     "\"output_tokens\":%d"
                     "}}",
                     escaped, result.n_tokens);

            send_json(sock, 200, response);
            free(response);
            free(escaped);
        } else {
            send_json(sock, 500,
                      "{\"type\":\"error\",\"error\":{\"type\":\"api_error\","
                      "\"message\":\"Generation failed\"}}");
        }

        neuronos_gen_result_free(&result);
    }

    neuronos_free(formatted_prompt);
    free(system_prompt);
    free_parsed_msgs(parsed, msg_count);
}

static void handle_root(socket_t sock, bool accept_gzip) {
#if NEURONOS_CHAT_UI_IS_GZIPPED
    if (accept_gzip) {
        /* Serve gzip-compressed embedded Chat UI */
        send_gzip_response(sock, "text/html; charset=utf-8",
                           neuronos_chat_ui_html_gz, (int)neuronos_chat_ui_html_gz_len);
        return;
    }
    /* Fallback: browser doesn't support gzip — serve a redirect hint */
    const char * html = "<!DOCTYPE html><html><head><title>NeuronOS</title></head><body>"
                        "<h1>NeuronOS v" NEURONOS_VERSION_STRING "</h1>"
                        "<p>Your browser needs gzip support for the Chat UI.</p>"
                        "<p>Use a modern browser (Chrome, Firefox, Safari, Edge).</p>"
                        "</body></html>";
    send_response(sock, 200, "OK", "text/html; charset=utf-8", html, (int)strlen(html));
#else
    /* Non-gzipped embedded Chat UI */
    send_response(sock, 200, "OK", "text/html; charset=utf-8",
                  (const char *)neuronos_chat_ui_html, (int)neuronos_chat_ui_html_len);
#endif
}

/* ---- Agent SSE Chat Endpoint ---- */

/* Context for agent step callback during SSE streaming */
typedef struct {
    socket_t sock;
    bool ok;
} agent_sse_ctx_t;

/* Send an SSE event to the client */
static void sse_send_event(socket_t sock, const char * json_payload) {
    char buf[16384];
    int len = snprintf(buf, sizeof(buf), "data: %s\n\n", json_payload);
    if (len > 0 && len < (int)sizeof(buf)) {
        send(sock, buf, (size_t)len, 0);
    }
}

/* Agent step callback: sends thinking/tool/observation as SSE events */
static void agent_sse_step_cb(int step, const char * thought, const char * action,
                               const char * observation, void * user_data) {
    agent_sse_ctx_t * ctx = (agent_sse_ctx_t *)user_data;
    if (!ctx || !ctx->ok)
        return;

    (void)step;

    /* Send thinking event */
    if (thought && action && strcmp(action, "reply") != 0) {
        char * esc = nj_escape(thought);
        if (esc) {
            char ev[8192];
            snprintf(ev, sizeof(ev), "{\"type\":\"thinking\",\"text\":\"%s\"}", esc);
            sse_send_event(ctx->sock, ev);
            free(esc);
        }
    }

    /* Send tool call event */
    if (action && strcmp(action, "reply") != 0 && strcmp(action, "final_answer") != 0 &&
        strcmp(action, "error") != 0) {

        if (!observation) {
            /* First callback for this step: tool invocation */
            char * esc_act = nj_escape(action);
            if (esc_act) {
                char ev[4096];
                snprintf(ev, sizeof(ev), "{\"type\":\"tool\",\"name\":\"%s\"}", esc_act);
                sse_send_event(ctx->sock, ev);
                free(esc_act);
            }
        } else {
            /* Second callback: observation result */
            size_t obs_len = strlen(observation);
            if (obs_len > 500)
                obs_len = 500;
            char * esc_obs = nj_escape_n(observation, obs_len);
            if (esc_obs) {
                char ev[8192];
                snprintf(ev, sizeof(ev), "{\"type\":\"observation\",\"text\":\"%s\"}", esc_obs);
                sse_send_event(ctx->sock, ev);
                free(esc_obs);
            }
        }
    }
}

/* POST /api/chat — Interactive agent with SSE streaming of steps */
static void handle_agent_chat(socket_t sock, const char * body) {
    if (!g_agent) {
        send_json(sock, 503, "{\"error\":{\"message\":\"Agent not available\"}}");
        return;
    }

    /* Extract message from body: {"message": "user text"} */
    char message[8192] = {0};
    if (nj_copy_str(body, "message", message, sizeof(message)) < 0) {
        send_json(sock, 400, "{\"error\":{\"message\":\"Missing 'message' field\"}}");
        return;
    }

    /* Send SSE headers */
    send_sse_headers(sock);

    /* Run agent with SSE step callback */
    agent_sse_ctx_t ctx = {.sock = sock, .ok = true};
    neuronos_agent_result_t result = neuronos_agent_chat(g_agent, message, agent_sse_step_cb, &ctx);

    /* Send final response event */
    if (result.status == NEURONOS_OK && result.text) {
        char * esc = nj_escape(result.text);
        if (esc) {
            /* Build response event with capacity for escaped text */
            size_t ev_cap = strlen(esc) + 256;
            char * ev = malloc(ev_cap);
            if (ev) {
                snprintf(ev, ev_cap, "{\"type\":\"response\",\"text\":\"%s\",\"steps\":%d}",
                         esc, result.steps_taken);
                sse_send_event(sock, ev);
                free(ev);
            }
            free(esc);
        }
    } else {
        sse_send_event(sock, "{\"type\":\"error\",\"text\":\"Agent failed to generate response\"}");
    }

    /* Send done marker */
    const char * done = "data: [DONE]\n\n";
    send(sock, done, strlen(done), 0);

    neuronos_agent_result_free(&result);
}

/* ---- Main Server Loop ---- */

neuronos_status_t neuronos_server_start(neuronos_model_t * model, neuronos_tool_registry_t * tools,
                                        neuronos_server_params_t params) {
    g_model = model;
    g_tools = tools;
    g_agent = params.agent; /* May be NULL (raw inference only) */

    if (!params.host)
        params.host = "127.0.0.1";
    if (params.port <= 0)
        params.port = 8080;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCK) {
        fprintf(stderr, "Error: Cannot create socket\n");
        return NEURONOS_ERROR_INIT;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)params.port);
    inet_pton(AF_INET, params.host, &addr.sin_addr);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Cannot bind to %s:%d (%s)\n", params.host, params.port, strerror(errno));
        close_socket(server_fd);
        return NEURONOS_ERROR_INIT;
    }

    if (listen(server_fd, 16) < 0) {
        fprintf(stderr, "Error: Cannot listen\n");
        close_socket(server_fd);
        return NEURONOS_ERROR_INIT;
    }

    fprintf(stderr,
            "\n╔══════════════════════════════════════════╗\n"
            "║  NeuronOS Server v%s                 ║\n"
            "║  http://%s:%-5d                   ║\n"
            "║  %s║\n"
            "║  Press Ctrl+C to stop                    ║\n"
            "╚══════════════════════════════════════════╝\n\n",
            NEURONOS_VERSION_STRING, params.host, params.port,
            g_agent ? "Agent chat UI ready                     "
                    : "OpenAI-compatible API ready             ");

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        socket_t client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd == INVALID_SOCK) {
            if (g_running)
                fprintf(stderr, "Warning: accept() failed\n");
            continue;
        }

        /* Read request */
        char raw[MAX_REQUEST] = {0};
        int total_read = 0;
        int n = (int)recv(client_fd, raw, sizeof(raw) - 1, 0);
        if (n > 0)
            total_read = n;

        /* If we have Content-Length, read the rest of body if needed */
        if (total_read > 0) {
            http_request_t req;
            if (parse_request(raw, total_read, &req) == 0) {
                /* Route request */
                if (strcmp(req.method, "OPTIONS") == 0) {
                    /* CORS preflight */
                    send_response(client_fd, 204, "No Content", "text/plain", "", 0);
                } else if (strcmp(req.path, "/health") == 0) {
                    handle_health(client_fd);
                } else if (strcmp(req.path, "/v1/models") == 0) {
                    handle_models(client_fd);
                } else if (strcmp(req.path, "/v1/completions") == 0 && strcmp(req.method, "POST") == 0) {
                    handle_completions(client_fd, req.body);
                } else if (strcmp(req.path, "/v1/chat/completions") == 0 && strcmp(req.method, "POST") == 0) {
                    handle_chat_completions(client_fd, req.body);
                } else if (strcmp(req.path, "/v1/messages") == 0 && strcmp(req.method, "POST") == 0) {
                    handle_anthropic_messages(client_fd, req.body);
                } else if (strcmp(req.path, "/api/chat") == 0 && strcmp(req.method, "POST") == 0) {
                    handle_agent_chat(client_fd, req.body);
                } else if (strcmp(req.path, "/") == 0) {
                    handle_root(client_fd, req.accept_gzip);
                } else {
                    send_json(client_fd, 404, "{\"error\":{\"message\":\"Not found\"}}");
                }
            }
        }

        close_socket(client_fd);
    }

    close_socket(server_fd);

#ifdef _WIN32
    WSACleanup();
#endif

    fprintf(stderr, "\nServer stopped.\n");
    return NEURONOS_OK;
}
