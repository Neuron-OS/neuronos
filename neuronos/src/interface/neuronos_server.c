/* ============================================================
 * NeuronOS — Minimal HTTP Server (OpenAI-compatible)
 *
 * Endpoints:
 *   POST /v1/chat/completions  — Chat API
 *   POST /v1/completions       — Text completion
 *   GET  /v1/models            — List models
 *   GET  /health               — Health check
 *   GET  /                     — Status page
 *
 * No external dependencies — pure POSIX sockets.
 * Designed for: desktop apps, browser clients, mobile apps.
 * ============================================================ */
#include "neuronos/neuronos.h"

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

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ---- HTTP helpers ---- */

#define MAX_REQUEST 65536
#define MAX_RESPONSE 262144

typedef struct {
    char method[16];
    char path[256];
    char body[MAX_REQUEST];
    int body_len;
    int content_length;
} http_request_t;

static int parse_request(const char * raw, int raw_len, http_request_t * req) {
    memset(req, 0, sizeof(*req));

    /* Parse request line */
    const char * line_end = strstr(raw, "\r\n");
    if (!line_end)
        return -1;

    sscanf(raw, "%15s %255s", req->method, req->path);

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

/* ---- Extract simple JSON string value ---- */
static int json_get_string(const char * json, const char * key, char * out, size_t out_len) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char * found = strstr(json, pattern);
    if (!found)
        return -1;

    /* Find the value string */
    const char * colon = strchr(found + strlen(pattern), ':');
    if (!colon)
        return -1;

    const char * quote1 = strchr(colon, '"');
    if (!quote1)
        return -1;
    quote1++;

    const char * quote2 = strchr(quote1, '"');
    if (!quote2)
        return -1;

    size_t len = (size_t)(quote2 - quote1);
    if (len >= out_len)
        len = out_len - 1;
    memcpy(out, quote1, len);
    out[len] = '\0';
    return 0;
}

static int json_get_int(const char * json, const char * key, int default_val) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char * found = strstr(json, pattern);
    if (!found)
        return default_val;

    const char * colon = strchr(found + strlen(pattern), ':');
    if (!colon)
        return default_val;

    return atoi(colon + 1);
}

static float json_get_float(const char * json, const char * key, float default_val) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char * found = strstr(json, pattern);
    if (!found)
        return default_val;

    const char * colon = strchr(found + strlen(pattern), ':');
    if (!colon)
        return default_val;

    return (float)atof(colon + 1);
}

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

    return json_get_string(last_content - 1, "content", out, out_len);
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
    json_get_string(body, "prompt", prompt, sizeof(prompt));
    if (prompt[0] == '\0') {
        send_json(sock, 400, "{\"error\":{\"message\":\"Missing prompt\"}}");
        return;
    }

    int max_tokens = json_get_int(body, "max_tokens", 256);
    float temperature = json_get_float(body, "temperature", 0.7f);

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
        /* Build OpenAI-compatible response */
        char response[MAX_RESPONSE];
        int rlen = snprintf(response, sizeof(response),
                            "{\"id\":\"cmpl-neuronos\","
                            "\"object\":\"text_completion\","
                            "\"created\":%d,"
                            "\"model\":\"neuronos-local\","
                            "\"choices\":[{"
                            "\"text\":\"%.*s\","
                            "\"index\":0,"
                            "\"finish_reason\":\"stop\""
                            "}],"
                            "\"usage\":{"
                            "\"completion_tokens\":%d,"
                            "\"total_tokens\":%d"
                            "}}",
                            0, /* timestamp placeholder */
                            (int)(sizeof(response) - 512), result.text, result.n_tokens, result.n_tokens);
        (void)rlen;
        send_json(sock, 200, response);
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
    char escaped[2048] = {0};
    size_t j = 0;
    for (size_t i = 0; token_text[i] && j < sizeof(escaped) - 8; i++) {
        switch (token_text[i]) {
            case '"':
                escaped[j++] = '\\';
                escaped[j++] = '"';
                break;
            case '\\':
                escaped[j++] = '\\';
                escaped[j++] = '\\';
                break;
            case '\n':
                escaped[j++] = '\\';
                escaped[j++] = 'n';
                break;
            case '\r':
                escaped[j++] = '\\';
                escaped[j++] = 'r';
                break;
            case '\t':
                escaped[j++] = '\\';
                escaped[j++] = 't';
                break;
            default:
                escaped[j++] = token_text[i];
                break;
        }
    }
    escaped[j] = '\0';

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

/* Detect "stream": true in JSON body */
static bool json_get_bool(const char * json, const char * key, bool default_val) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char * found = strstr(json, pattern);
    if (!found)
        return default_val;
    const char * colon = strchr(found + strlen(pattern), ':');
    if (!colon)
        return default_val;
    /* skip whitespace */
    colon++;
    while (*colon == ' ' || *colon == '\t')
        colon++;
    if (strncmp(colon, "true", 4) == 0)
        return true;
    if (strncmp(colon, "false", 5) == 0)
        return false;
    return default_val;
}

static void handle_chat_completions(socket_t sock, const char * body) {
    if (!g_model) {
        send_json(sock, 503, "{\"error\":{\"message\":\"No model loaded\"}}");
        return;
    }

    char content[8192] = {0};
    if (extract_last_user_content(body, content, sizeof(content)) != 0) {
        send_json(sock, 400, "{\"error\":{\"message\":\"Missing messages content\"}}");
        return;
    }

    int max_tokens = json_get_int(body, "max_tokens", 256);
    float temperature = json_get_float(body, "temperature", 0.7f);
    bool stream = json_get_bool(body, "stream", false);

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
            .prompt = content,
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
        /* ── Non-streaming mode (original) ── */
        neuronos_gen_params_t gparams = {
            .prompt = content,
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
            char response[MAX_RESPONSE];
            snprintf(response, sizeof(response),
                     "{\"id\":\"chatcmpl-neuronos\","
                     "\"object\":\"chat.completion\","
                     "\"created\":%d,"
                     "\"model\":\"neuronos-local\","
                     "\"choices\":[{"
                     "\"index\":0,"
                     "\"message\":{\"role\":\"assistant\",\"content\":\"%.*s\"},"
                     "\"finish_reason\":\"stop\""
                     "}],"
                     "\"usage\":{"
                     "\"prompt_tokens\":0,"
                     "\"completion_tokens\":%d,"
                     "\"total_tokens\":%d"
                     "}}",
                     0, (int)(sizeof(response) - 512), result.text, result.n_tokens, result.n_tokens);
            send_json(sock, 200, response);
        } else {
            send_json(sock, 500, "{\"error\":{\"message\":\"Generation failed\"}}");
        }

        neuronos_gen_result_free(&result);
    }
}

static void handle_root(socket_t sock) {
    const char * html = "<!DOCTYPE html><html><head><title>NeuronOS</title>"
                        "<style>body{font-family:monospace;background:#0a0a0a;color:#0f0;padding:40px}"
                        "h1{color:#0ff}pre{color:#aaa}</style></head><body>"
                        "<h1>NeuronOS v" NEURONOS_VERSION_STRING "</h1>"
                        "<p>The fastest AI agent engine. Universal. Offline. Any device.</p>"
                        "<pre>Endpoints:\n"
                        "  POST /v1/chat/completions  - Chat API (OpenAI compatible, SSE streaming)\n"
                        "  POST /v1/completions       - Text completion\n"
                        "  GET  /v1/models            - List models\n"
                        "  GET  /health               - Health check\n"
                        "\nStreaming: Set \"stream\": true for SSE token streaming\n"
                        "</pre></body></html>";
    send_response(sock, 200, "OK", "text/html", html, (int)strlen(html));
}

/* ---- Main Server Loop ---- */

neuronos_status_t neuronos_server_start(neuronos_model_t * model, neuronos_tool_registry_t * tools,
                                        neuronos_server_params_t params) {
    g_model = model;
    g_tools = tools;

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
            "║  OpenAI-compatible API ready             ║\n"
            "║  Press Ctrl+C to stop                    ║\n"
            "╚══════════════════════════════════════════╝\n\n",
            NEURONOS_VERSION_STRING, params.host, params.port);

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
                } else if (strcmp(req.path, "/") == 0) {
                    handle_root(client_fd);
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
