/* ============================================================
 * NeuronOS — MCP Server (Model Context Protocol)
 *
 * Implements the MCP specification (2025-11-25) over STDIO transport.
 * This makes NeuronOS tools available to any MCP client:
 *   - Claude Desktop
 *   - VS Code (GitHub Copilot)
 *   - Cursor, Windsurf, etc.
 *
 * Protocol: JSON-RPC 2.0 over stdin/stdout (newline-delimited)
 * Features: tools/list, tools/call, ping
 *
 * First MCP server written in pure C.
 *
 * Copyright (c) 2025 NeuronOS Project
 * SPDX-License-Identifier: MIT
 * ============================================================ */
#include "neuronos/neuronos.h"
#include "neuronos/neuronos_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Constants ---- */
#define MCP_MAX_LINE 131072 /* 128KB max JSON-RPC message */
#define MCP_PROTOCOL_VERSION "2025-11-25"
#define MCP_SERVER_NAME "neuronos"
#define MCP_SERVER_VERSION NEURONOS_VERSION_STRING

/* JSON helpers provided by neuronos/neuronos_json.h (nj_*) */

/* Write a JSON-RPC response to stdout (newline-delimited) */
static void mcp_send(const char * json) {
    fprintf(stdout, "%s\n", json);
    fflush(stdout);
}

/* Send a JSON-RPC error response */
static void mcp_send_error(int id, int code, const char * message) {
    char buf[4096];
    if (id >= 0) {
        snprintf(buf, sizeof(buf),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,"
                 "\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                 id, code, message);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"jsonrpc\":\"2.0\",\"id\":null,"
                 "\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                 code, message);
    }
    mcp_send(buf);
}

/* ============================================================
 * MCP HANDLERS
 * ============================================================ */

/* Handle "initialize" — respond with server capabilities */
static void handle_initialize(int id) {
    char buf[4096];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,"
             "\"result\":{"
             "\"protocolVersion\":\"%s\","
             "\"capabilities\":{"
             "\"tools\":{\"listChanged\":false},"
             "\"logging\":{}"
             "},"
             "\"serverInfo\":{"
             "\"name\":\"%s\","
             "\"version\":\"%s\","
             "\"description\":\"NeuronOS — The fastest AI agent engine. "
             "Universal, offline, runs on any device.\""
             "}"
             "}}",
             id, MCP_PROTOCOL_VERSION, MCP_SERVER_NAME, MCP_SERVER_VERSION);
    mcp_send(buf);
    fprintf(stderr, "[mcp] Initialized (protocol %s)\n", MCP_PROTOCOL_VERSION);
}

/* Handle "ping" — respond with empty result */
static void handle_ping(int id) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{}}", id);
    mcp_send(buf);
}

/* Handle "tools/list" — enumerate all registered tools as MCP tool objects */
static void handle_tools_list(int id, neuronos_tool_registry_t * tools) {
    if (!tools) {
        mcp_send_error(id, -32603, "No tool registry available");
        return;
    }

    int n = neuronos_tool_count(tools);

    /* Build JSON array of tools */
    size_t cap = 32768;
    char * buf = malloc(cap);
    if (!buf) {
        mcp_send_error(id, -32603, "Out of memory");
        return;
    }

    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, cap - pos,
                            "{\"jsonrpc\":\"2.0\",\"id\":%d,"
                            "\"result\":{\"tools\":[",
                            id);

    for (int i = 0; i < n; i++) {
        /* Access tool descriptor details */
        const char * name = neuronos_tool_name(tools, i);
        const char * desc = neuronos_tool_description(tools, i);
        const char * schema = neuronos_tool_schema(tools, i);

        if (i > 0)
            buf[pos++] = ',';

        char * esc_desc = nj_escape(desc ? desc : "");

        /* Tool object: name, description, inputSchema */
        pos += (size_t)snprintf(buf + pos, cap - pos,
                                "{\"name\":\"%s\","
                                "\"description\":\"%s\","
                                "\"inputSchema\":%s}",
                                name ? name : "unknown", esc_desc ? esc_desc : "",
                                (schema && schema[0] == '{') ? schema
                                                             : "{\"type\":\"object\",\"additionalProperties\":false}");

        free(esc_desc);

        /* Grow buffer if needed */
        if (pos > cap - 4096) {
            cap *= 2;
            char * newbuf = realloc(buf, cap);
            if (!newbuf) {
                free(buf);
                mcp_send_error(id, -32603, "Out of memory");
                return;
            }
            buf = newbuf;
        }
    }

    pos += (size_t)snprintf(buf + pos, cap - pos, "]}}");
    mcp_send(buf);
    free(buf);

    fprintf(stderr, "[mcp] tools/list → %d tools\n", n);
}

/* Handle "tools/call" — execute a tool and return result */
static void handle_tools_call(int id, const char * params, neuronos_tool_registry_t * tools) {
    if (!tools || !params) {
        mcp_send_error(id, -32602, "Missing params or tool registry");
        return;
    }

    /* Extract tool name */
    int name_len = 0;
    const char * name_ptr = nj_find_str(params, "name", &name_len);
    if (!name_ptr || name_len <= 0) {
        mcp_send_error(id, -32602, "Missing 'name' in params");
        return;
    }

    char tool_name[256] = {0};
    if (name_len >= (int)sizeof(tool_name))
        name_len = (int)sizeof(tool_name) - 1;
    memcpy(tool_name, name_ptr, (size_t)name_len);

    /* Extract arguments object */
    char * args = nj_extract_object(params, "arguments");
    const char * args_str = args ? args : "{}";

    fprintf(stderr, "[mcp] tools/call → %s(%s)\n", tool_name, args_str);

    /* Execute the tool */
    neuronos_tool_result_t result = neuronos_tool_execute(tools, tool_name, args_str);

    /* Build response */
    size_t cap = 65536;
    char * buf = malloc(cap);
    if (!buf) {
        free(args);
        neuronos_tool_result_free(&result);
        mcp_send_error(id, -32603, "Out of memory");
        return;
    }

    char * esc_output = nj_escape(result.success ? result.output : result.error);

    snprintf(buf, cap,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,"
             "\"result\":{"
             "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],"
             "\"isError\":%s"
             "}}",
             id, esc_output ? esc_output : "null", result.success ? "false" : "true");

    mcp_send(buf);

    free(esc_output);
    free(buf);
    free(args);
    neuronos_tool_result_free(&result);

    fprintf(stderr, "[mcp] tools/call → %s: %s\n", tool_name, result.success ? "OK" : "ERROR");
}

/* ============================================================
 * MCP MAIN LOOP
 * ============================================================ */

neuronos_status_t neuronos_mcp_serve_stdio(neuronos_tool_registry_t * tools) {
    if (!tools) {
        fprintf(stderr, "[mcp] Error: no tool registry\n");
        return NEURONOS_ERROR_INVALID_PARAM;
    }

    fprintf(stderr,
            "[mcp] NeuronOS MCP Server v%s starting (STDIO transport)\n"
            "[mcp] Protocol: %s\n"
            "[mcp] Tools: %d registered\n"
            "[mcp] Waiting for JSON-RPC messages on stdin...\n",
            MCP_SERVER_VERSION, MCP_PROTOCOL_VERSION, neuronos_tool_count(tools));

    char * line = malloc(MCP_MAX_LINE);
    if (!line)
        return NEURONOS_ERROR_MEMORY;

    bool initialized = false;

    while (fgets(line, MCP_MAX_LINE, stdin)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0)
            continue; /* skip empty lines */

        /* Parse JSON-RPC envelope: extract method and id */
        int msg_id = nj_find_int(line, "id", -1);

        int method_len = 0;
        const char * method_ptr = nj_find_str(line, "method", &method_len);

        if (!method_ptr) {
            /* Might be a response or notification — ignore for now */
            if (msg_id >= 0) {
                /* Unknown request without method */
                mcp_send_error(msg_id, -32600, "Invalid Request: missing method");
            }
            continue;
        }

        char method[256] = {0};
        if (method_len >= (int)sizeof(method))
            method_len = (int)sizeof(method) - 1;
        memcpy(method, method_ptr, (size_t)method_len);

        /* ---- Dispatch ---- */

        if (strcmp(method, "initialize") == 0) {
            handle_initialize(msg_id);
            initialized = true;

        } else if (strcmp(method, "notifications/initialized") == 0) {
            /* Notification — no response needed */
            fprintf(stderr, "[mcp] Client initialized, ready for operations\n");

        } else if (strcmp(method, "ping") == 0) {
            handle_ping(msg_id);

        } else if (strcmp(method, "tools/list") == 0) {
            if (!initialized) {
                mcp_send_error(msg_id, -32002, "Server not initialized");
                continue;
            }
            handle_tools_list(msg_id, tools);

        } else if (strcmp(method, "tools/call") == 0) {
            if (!initialized) {
                mcp_send_error(msg_id, -32002, "Server not initialized");
                continue;
            }
            char * params = nj_extract_object(line, "params");
            handle_tools_call(msg_id, params, tools);
            free(params);

        } else if (strcmp(method, "notifications/cancelled") == 0) {
            /* Cancellation notification — log and ignore */
            fprintf(stderr, "[mcp] Cancellation received\n");

        } else {
            /* Unknown method */
            if (msg_id >= 0)
                mcp_send_error(msg_id, -32601, "Method not found");
            else
                fprintf(stderr, "[mcp] Unknown notification: %s\n", method);
        }
    }

    free(line);
    fprintf(stderr, "[mcp] STDIO stream closed, shutting down\n");
    return NEURONOS_OK;
}
