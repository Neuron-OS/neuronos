/* ============================================================
 * NeuronOS MCP Client — Connect to external MCP servers
 *
 * First MCP client in pure C. Zero dependencies.
 *
 * Transforms NeuronOS from "10 hardcoded tools" to
 * "access to 10,000+ tools from the MCP ecosystem".
 *
 * Supports:
 *   - STDIO transport (fork+exec, pipe JSON-RPC 2.0)
 *   - Auto-discovery of tools via tools/list
 *   - Bridge function to register MCP tools in tool_registry
 *   - Config loading from ~/.neuronos/mcp.json
 *
 * Copyright (c) 2025 NeuronOS Project
 * SPDX-License-Identifier: MIT
 * ============================================================ */

#include "neuronos/neuronos.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #error "MCP Client STDIO transport not yet supported on Windows"
#else
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

/* ============================================================
 * CONSTANTS
 * ============================================================ */

#define MCP_CLIENT_VERSION       "0.1.0"
#define MCP_PROTOCOL_VERSION     "2025-11-25"
#define MCP_MAX_LINE             131072 /* 128 KB max JSON-RPC line  */
#define MCP_MAX_SERVERS          16     /* Max simultaneous servers  */
#define MCP_MAX_TOOLS            256    /* Max tools across servers  */
#define MCP_MAX_TOOL_NAME        128
#define MCP_MAX_TOOL_DESC        1024
#define MCP_MAX_TOOL_SCHEMA      8192
#define MCP_READ_TIMEOUT_MS      30000  /* 30s timeout for responses */

/* ============================================================
 * INTERNAL JSON HELPERS (minimal, no-dependency parser)
 *
 * These are deliberately simple: we only need to parse
 * well-formed JSON-RPC responses from trusted MCP servers.
 * ============================================================ */

/* Find a string value in JSON by key. Returns pointer to first char
 * of value (inside quotes). Sets *len to string length. */
static const char * json_find_str(const char * json, const char * key, int * len) {
    if (!json || !key)
        return NULL;

    char pattern[256];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern))
        return NULL;

    const char * p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        p += plen;
        /* Skip whitespace and colon */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p == '"') {
            p++; /* skip opening quote */
            const char * start = p;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1))
                    p++; /* skip escaped char */
                p++;
            }
            if (len)
                *len = (int)(p - start);
            return start;
        }
        /* For non-string values, skip */
    }
    return NULL;
}

/* Find integer value in JSON by key */
static int json_find_int(const char * json, const char * key) {
    if (!json || !key)
        return -1;

    char pattern[256];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern))
        return -1;

    const char * p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        p += plen;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p == '-' || (*p >= '0' && *p <= '9'))
            return atoi(p);
    }
    return -1;
}

/* Extract a JSON object value as a new string. Caller must free. */
static char * json_extract_object(const char * json, const char * key) {
    if (!json || !key)
        return NULL;

    char pattern[256];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern))
        return NULL;

    const char * p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        p += plen;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p == '{') {
            int depth = 1;
            const char * start = p;
            p++;
            while (*p && depth > 0) {
                if (*p == '{')
                    depth++;
                else if (*p == '}')
                    depth--;
                else if (*p == '"') {
                    p++;
                    while (*p && *p != '"') {
                        if (*p == '\\' && *(p + 1))
                            p++;
                        p++;
                    }
                }
                if (depth > 0)
                    p++;
            }
            if (depth == 0) {
                size_t len = (size_t)(p - start + 1);
                char * obj = malloc(len + 1);
                if (obj) {
                    memcpy(obj, start, len);
                    obj[len] = '\0';
                }
                return obj;
            }
        }
    }
    return NULL;
}

/* Extract a JSON array value as a new string. Caller must free. */
static char * json_extract_array(const char * json, const char * key) {
    if (!json || !key)
        return NULL;

    char pattern[256];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern))
        return NULL;

    const char * p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        p += plen;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p == '[') {
            int depth = 1;
            const char * start = p;
            p++;
            while (*p && depth > 0) {
                if (*p == '[')
                    depth++;
                else if (*p == ']')
                    depth--;
                else if (*p == '"') {
                    p++;
                    while (*p && *p != '"') {
                        if (*p == '\\' && *(p + 1))
                            p++;
                        p++;
                    }
                }
                if (depth > 0)
                    p++;
            }
            if (depth == 0) {
                size_t len = (size_t)(p - start + 1);
                char * arr = malloc(len + 1);
                if (arr) {
                    memcpy(arr, start, len);
                    arr[len] = '\0';
                }
                return arr;
            }
        }
    }
    return NULL;
}

/* Escape a string for JSON output. Caller must free. */
static char * json_escape(const char * s) {
    if (!s)
        return strdup("null");
    size_t cap = strlen(s) * 2 + 3;
    char * out = malloc(cap);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; s[i] && j < cap - 2; i++) {
        switch (s[i]) {
            case '"':  out[j++] = '\\'; out[j++] = '"';  break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
            case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
            case '\t': out[j++] = '\\'; out[j++] = 't';  break;
            default:
                if ((unsigned char)s[i] < 0x20) {
                    j += (size_t)snprintf(out + j, cap - j, "\\u%04x", (unsigned char)s[i]);
                } else {
                    out[j++] = s[i];
                }
                break;
        }
    }
    out[j] = '\0';
    return out;
}

/* ============================================================
 * INTERNAL STRUCTURES
 * ============================================================ */

/* Discovered tool from an MCP server */
typedef struct {
    char name[MCP_MAX_TOOL_NAME];
    char description[MCP_MAX_TOOL_DESC];
    char schema[MCP_MAX_TOOL_SCHEMA]; /* JSON Schema for input */
    int server_index;                 /* which server owns this tool */
} mcp_tool_entry_t;

/* A connected MCP server process */
typedef struct {
    char name[128];                     /* human-readable name               */
    neuronos_mcp_transport_t transport; /* STDIO or HTTP                     */
    pid_t pid;                          /* child process PID (STDIO)         */
    int fd_write;                       /* pipe: parent writes → child stdin */
    int fd_read;                        /* pipe: child stdout → parent reads */
    int next_id;                        /* JSON-RPC request ID counter       */
    bool connected;                     /* successfully initialized?         */
    char * read_buf;                    /* line buffer for reading           */
    /* Config storage (owned copies) */
    char * command;
    char ** args;
    int n_args;
    char ** env;
    int n_env;
} mcp_server_conn_t;

/* The MCP client */
struct neuronos_mcp_client {
    mcp_server_conn_t servers[MCP_MAX_SERVERS];
    int n_servers;
    mcp_tool_entry_t tools[MCP_MAX_TOOLS];
    int n_tools;
};

/* ============================================================
 * STDIO TRANSPORT: fork + exec + pipe
 * ============================================================ */

/* Send a JSON-RPC request to a server. Returns the request ID. */
static int mcp_client_send(mcp_server_conn_t * srv, const char * json) {
    if (!srv || srv->fd_write < 0 || !json)
        return -1;

    size_t len = strlen(json);
    const char newline = '\n';

    ssize_t w = write(srv->fd_write, json, len);
    if (w < 0) {
        fprintf(stderr, "[mcp-client] Write error to '%s': %s\n", srv->name, strerror(errno));
        return -1;
    }
    write(srv->fd_write, &newline, 1);
    return 0;
}

/* Read a line from the server (blocking, with timeout via poll/select).
 * Returns the line (in srv->read_buf) or NULL on error/timeout. */
static char * mcp_client_readline(mcp_server_conn_t * srv) {
    if (!srv || srv->fd_read < 0)
        return NULL;

    if (!srv->read_buf) {
        srv->read_buf = malloc(MCP_MAX_LINE);
        if (!srv->read_buf)
            return NULL;
    }

    /* Use select() for timeout */
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(srv->fd_read, &fds);
    tv.tv_sec = MCP_READ_TIMEOUT_MS / 1000;
    tv.tv_usec = (MCP_READ_TIMEOUT_MS % 1000) * 1000;

    int ret = select(srv->fd_read + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) {
        if (ret == 0)
            fprintf(stderr, "[mcp-client] Timeout reading from '%s'\n", srv->name);
        else
            fprintf(stderr, "[mcp-client] Select error on '%s': %s\n", srv->name, strerror(errno));
        return NULL;
    }

    /* Read byte-by-byte until newline (JSON-RPC newline-delimited) */
    size_t pos = 0;
    while (pos < MCP_MAX_LINE - 1) {
        ssize_t r = read(srv->fd_read, srv->read_buf + pos, 1);
        if (r <= 0)
            break;
        if (srv->read_buf[pos] == '\n') {
            srv->read_buf[pos] = '\0';
            return srv->read_buf;
        }
        pos++;
    }

    if (pos > 0) {
        srv->read_buf[pos] = '\0';
        return srv->read_buf;
    }

    return NULL;
}

/* Send a JSON-RPC request and wait for the response.
 * Returns the response line (in srv->read_buf). Caller must NOT free. */
static const char * mcp_client_request(mcp_server_conn_t * srv, const char * method,
                                       const char * params_json) {
    if (!srv || !method)
        return NULL;

    int id = srv->next_id++;
    char buf[MCP_MAX_LINE];

    if (params_json && params_json[0]) {
        snprintf(buf, sizeof(buf),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
                 id, method, params_json);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":{}}",
                 id, method);
    }

    if (mcp_client_send(srv, buf) < 0)
        return NULL;

    /* Read response(s) — skip notifications, look for matching id */
    for (int attempts = 0; attempts < 20; attempts++) {
        char * line = mcp_client_readline(srv);
        if (!line)
            return NULL;

        /* Skip empty lines */
        if (line[0] == '\0')
            continue;

        /* Check if this is a response (has "id") matching ours */
        int resp_id = json_find_int(line, "id");
        if (resp_id == id) {
            return line; /* Matched! */
        }

        /* This is either a notification or a response with different id.
         * Log and continue reading. */
        int method_len = 0;
        const char * m = json_find_str(line, "method", &method_len);
        if (m) {
            /* It's a notification — ignore (e.g., progress, log) */
            continue;
        }
    }

    fprintf(stderr, "[mcp-client] No matching response from '%s' for id %d\n", srv->name, id);
    return NULL;
}

/* Send a JSON-RPC notification (no response expected) */
static void mcp_client_notify(mcp_server_conn_t * srv, const char * method) {
    if (!srv || !method)
        return;

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"method\":\"%s\"}", method);
    mcp_client_send(srv, buf);
}

/* Spawn MCP server process with STDIO transport */
static int mcp_server_spawn(mcp_server_conn_t * srv) {
    if (!srv || !srv->command)
        return -1;

    /* Create pipes: parent_to_child and child_to_parent */
    int pipe_in[2];  /* parent writes → pipe_in[1], child reads ← pipe_in[0] */
    int pipe_out[2]; /* child writes → pipe_out[1], parent reads ← pipe_out[0] */

    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) {
        fprintf(stderr, "[mcp-client] pipe() failed: %s\n", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[mcp-client] fork() failed: %s\n", strerror(errno));
        close(pipe_in[0]); close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);
        return -1;
    }

    if (pid == 0) {
        /* ---- Child process ---- */

        /* Redirect stdin to pipe_in[0] */
        dup2(pipe_in[0], STDIN_FILENO);
        close(pipe_in[0]);
        close(pipe_in[1]);

        /* Redirect stdout to pipe_out[1] */
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_out[0]);
        close(pipe_out[1]);

        /* Set environment variables */
        for (int i = 0; i < srv->n_env; i++) {
            if (srv->env[i])
                putenv(srv->env[i]); /* putenv doesn't copy — but we're in child */
        }

        /* Build argv: command + args + NULL */
        int argc = 1 + srv->n_args;
        char ** argv = calloc((size_t)(argc + 1), sizeof(char *));
        if (!argv)
            _exit(127);

        argv[0] = srv->command;
        for (int i = 0; i < srv->n_args; i++)
            argv[1 + i] = srv->args[i];
        argv[argc] = NULL;

        execvp(srv->command, argv);

        /* If we get here, exec failed */
        fprintf(stderr, "[mcp-client] execvp '%s' failed: %s\n", srv->command, strerror(errno));
        _exit(127);
    }

    /* ---- Parent process ---- */
    close(pipe_in[0]);  /* parent doesn't read from child's stdin pipe */
    close(pipe_out[1]); /* parent doesn't write to child's stdout pipe */

    srv->pid = pid;
    srv->fd_write = pipe_in[1];
    srv->fd_read = pipe_out[0];

    /* Set non-blocking flags could be useful but we use select() */

    fprintf(stderr, "[mcp-client] Spawned '%s' (PID %d): %s", srv->name, pid, srv->command);
    for (int i = 0; i < srv->n_args; i++)
        fprintf(stderr, " %s", srv->args[i]);
    fprintf(stderr, "\n");

    return 0;
}

/* Initialize handshake with an MCP server */
static int mcp_server_initialize(mcp_server_conn_t * srv) {
    if (!srv)
        return -1;

    char params[1024];
    snprintf(params, sizeof(params),
             "{\"protocolVersion\":\"%s\","
             "\"capabilities\":{},"
             "\"clientInfo\":{"
             "\"name\":\"NeuronOS\","
             "\"version\":\"%s\""
             "}}",
             MCP_PROTOCOL_VERSION, MCP_CLIENT_VERSION);

    const char * resp = mcp_client_request(srv, "initialize", params);
    if (!resp) {
        fprintf(stderr, "[mcp-client] Initialize failed for '%s'\n", srv->name);
        return -1;
    }

    /* Check for error */
    if (strstr(resp, "\"error\"")) {
        int elen = 0;
        const char * emsg = json_find_str(resp, "message", &elen);
        fprintf(stderr, "[mcp-client] Server '%s' error: %.*s\n",
                srv->name, elen, emsg ? emsg : "unknown");
        return -1;
    }

    /* Check protocol version in result */
    int vlen = 0;
    const char * ver = json_find_str(resp, "protocolVersion", &vlen);
    if (ver) {
        fprintf(stderr, "[mcp-client] '%s' protocol: %.*s\n", srv->name, vlen, ver);
    }

    /* Send initialized notification */
    mcp_client_notify(srv, "notifications/initialized");

    srv->connected = true;
    fprintf(stderr, "[mcp-client] '%s' connected successfully\n", srv->name);
    return 0;
}

/* Discover tools from a connected MCP server */
static int mcp_discover_tools(mcp_server_conn_t * srv, mcp_tool_entry_t * tools,
                              int max_tools, int server_index) {
    if (!srv || !srv->connected || !tools)
        return 0;

    const char * resp = mcp_client_request(srv, "tools/list", "{}");
    if (!resp) {
        fprintf(stderr, "[mcp-client] tools/list failed for '%s'\n", srv->name);
        return 0;
    }

    /* Extract tools array from result */
    char * result = json_extract_object(resp, "result");
    if (!result) {
        fprintf(stderr, "[mcp-client] No result in tools/list response from '%s'\n", srv->name);
        return 0;
    }

    char * tools_arr = json_extract_array(result, "tools");
    free(result);
    if (!tools_arr) {
        fprintf(stderr, "[mcp-client] No tools array in response from '%s'\n", srv->name);
        return 0;
    }

    /* Parse individual tools from array.
     * Simple approach: scan for {"name": patterns within the array */
    int count = 0;
    const char * p = tools_arr + 1; /* skip '[' */

    while (*p && count < max_tools) {
        /* Find next object start */
        while (*p && *p != '{')
            p++;
        if (!*p)
            break;

        /* Find matching close brace */
        const char * obj_start = p;
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '{')
                depth++;
            else if (*p == '}')
                depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && *(p + 1))
                        p++;
                    p++;
                }
            }
            if (depth > 0)
                p++;
        }

        if (depth != 0)
            break;

        /* Extract tool object as string */
        size_t obj_len = (size_t)(p - obj_start + 1);
        char * tool_json = malloc(obj_len + 1);
        if (!tool_json)
            break;
        memcpy(tool_json, obj_start, obj_len);
        tool_json[obj_len] = '\0';

        /* Parse name */
        int name_len = 0;
        const char * name = json_find_str(tool_json, "name", &name_len);
        if (name && name_len > 0) {
            mcp_tool_entry_t * t = &tools[count];
            int cpy = name_len < MCP_MAX_TOOL_NAME - 1 ? name_len : MCP_MAX_TOOL_NAME - 1;
            memcpy(t->name, name, (size_t)cpy);
            t->name[cpy] = '\0';

            /* Parse description */
            int desc_len = 0;
            const char * desc = json_find_str(tool_json, "description", &desc_len);
            if (desc && desc_len > 0) {
                int dcpy = desc_len < MCP_MAX_TOOL_DESC - 1 ? desc_len : MCP_MAX_TOOL_DESC - 1;
                memcpy(t->description, desc, (size_t)dcpy);
                t->description[dcpy] = '\0';
            } else {
                snprintf(t->description, MCP_MAX_TOOL_DESC, "MCP tool from %s", srv->name);
            }

            /* Parse inputSchema */
            char * schema = json_extract_object(tool_json, "inputSchema");
            if (schema) {
                size_t slen = strlen(schema);
                if (slen < MCP_MAX_TOOL_SCHEMA) {
                    memcpy(t->schema, schema, slen + 1);
                } else {
                    strncpy(t->schema, "{\"type\":\"object\"}", MCP_MAX_TOOL_SCHEMA);
                }
                free(schema);
            } else {
                strncpy(t->schema, "{\"type\":\"object\"}", MCP_MAX_TOOL_SCHEMA);
            }

            t->server_index = server_index;
            count++;
        }

        free(tool_json);
        if (*p)
            p++; /* move past '}' */
    }

    free(tools_arr);
    fprintf(stderr, "[mcp-client] Discovered %d tools from '%s'\n", count, srv->name);
    return count;
}

/* Stop an MCP server process */
static void mcp_server_stop(mcp_server_conn_t * srv) {
    if (!srv)
        return;

    if (srv->fd_write >= 0) {
        close(srv->fd_write);
        srv->fd_write = -1;
    }
    if (srv->fd_read >= 0) {
        close(srv->fd_read);
        srv->fd_read = -1;
    }

    if (srv->pid > 0) {
        kill(srv->pid, SIGTERM);
        int status;
        waitpid(srv->pid, &status, WNOHANG);
        srv->pid = -1;
    }

    free(srv->read_buf);
    srv->read_buf = NULL;
    free(srv->command);
    srv->command = NULL;

    if (srv->args) {
        for (int i = 0; i < srv->n_args; i++)
            free(srv->args[i]);
        free(srv->args);
        srv->args = NULL;
    }

    if (srv->env) {
        for (int i = 0; i < srv->n_env; i++)
            free(srv->env[i]);
        free(srv->env);
        srv->env = NULL;
    }

    srv->connected = false;
}

/* ============================================================
 * TOOL BRIDGE: MCP tool → neuronos_tool_registry
 *
 * Each discovered MCP tool gets a wrapper function that:
 *   1. Takes args_json from the tool registry
 *   2. Sends tools/call JSON-RPC to the correct server
 *   3. Returns the result as a tool_result_t
 * ============================================================ */

/* User data for the bridge function */
typedef struct {
    neuronos_mcp_client_t * client;
    int tool_index; /* index into client->tools[] */
} mcp_bridge_data_t;

/* Bridge function: tool_registry → MCP server */
static neuronos_tool_result_t mcp_tool_bridge(const char * args_json, void * user_data) {
    neuronos_tool_result_t result = {0};
    mcp_bridge_data_t * bd = (mcp_bridge_data_t *)user_data;

    if (!bd || !bd->client) {
        result.success = false;
        result.error = strdup("MCP bridge: invalid state");
        return result;
    }

    const mcp_tool_entry_t * tool = &bd->client->tools[bd->tool_index];
    char * output = neuronos_mcp_client_call_tool(bd->client, tool->name, args_json ? args_json : "{}");

    if (output) {
        result.success = true;
        result.output = output; /* caller frees via neuronos_free */
    } else {
        result.success = false;
        result.error = strdup("MCP tool call failed");
    }

    return result;
}

/* ============================================================
 * CONFIG FILE PARSER
 *
 * Parses ~/.neuronos/mcp.json in Claude Desktop format:
 * {
 *   "mcpServers": {
 *     "name": {
 *       "command": "npx",
 *       "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"],
 *       "env": { "KEY": "value" }
 *     }
 *   }
 * }
 * ============================================================ */

/* Parse a JSON array of strings. Returns count. Allocates *out. */
static int parse_string_array(const char * arr_json, char *** out) {
    if (!arr_json || arr_json[0] != '[' || !out)
        return 0;

    *out = NULL;
    int count = 0;
    int capacity = 8;
    char ** items = calloc((size_t)capacity, sizeof(char *));
    if (!items)
        return 0;

    const char * p = arr_json + 1; /* skip '[' */
    while (*p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ','))
            p++;
        if (*p == ']')
            break;
        if (*p == '"') {
            p++; /* skip opening quote */
            const char * start = p;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1))
                    p++;
                p++;
            }
            size_t slen = (size_t)(p - start);
            if (*p == '"')
                p++;

            if (count >= capacity) {
                capacity *= 2;
                char ** tmp = realloc(items, (size_t)capacity * sizeof(char *));
                if (!tmp)
                    break;
                items = tmp;
            }

            items[count] = malloc(slen + 1);
            if (items[count]) {
                memcpy(items[count], start, slen);
                items[count][slen] = '\0';
                count++;
            }
        } else {
            p++;
        }
    }

    *out = items;
    return count;
}

/* Parse env object { "KEY": "val", ... } → array of "KEY=val" strings */
static int parse_env_object(const char * obj_json, char *** out) {
    if (!obj_json || obj_json[0] != '{' || !out)
        return 0;

    *out = NULL;
    int count = 0;
    int capacity = 8;
    char ** items = calloc((size_t)capacity, sizeof(char *));
    if (!items)
        return 0;

    const char * p = obj_json + 1; /* skip '{' */

    while (*p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ','))
            p++;
        if (*p == '}')
            break;

        /* Parse key */
        if (*p != '"')
            break;
        p++; /* skip '"' */
        const char * key_start = p;
        while (*p && *p != '"')
            p++;
        size_t key_len = (size_t)(p - key_start);
        if (*p == '"')
            p++;

        /* Skip : */
        while (*p && (*p == ' ' || *p == ':'))
            p++;

        /* Parse value */
        if (*p != '"')
            break;
        p++;
        const char * val_start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p + 1))
                p++;
            p++;
        }
        size_t val_len = (size_t)(p - val_start);
        if (*p == '"')
            p++;

        if (count >= capacity) {
            capacity *= 2;
            char ** tmp = realloc(items, (size_t)capacity * sizeof(char *));
            if (!tmp)
                break;
            items = tmp;
        }

        /* Format: KEY=value */
        items[count] = malloc(key_len + 1 + val_len + 1);
        if (items[count]) {
            memcpy(items[count], key_start, key_len);
            items[count][key_len] = '=';
            memcpy(items[count] + key_len + 1, val_start, val_len);
            items[count][key_len + 1 + val_len] = '\0';
            count++;
        }
    }

    *out = items;
    return count;
}

/* ============================================================
 * PUBLIC API IMPLEMENTATION
 * ============================================================ */

neuronos_mcp_client_t * neuronos_mcp_client_create(void) {
    neuronos_mcp_client_t * client = calloc(1, sizeof(neuronos_mcp_client_t));
    if (!client)
        return NULL;

    /* Initialize server slots */
    for (int i = 0; i < MCP_MAX_SERVERS; i++) {
        client->servers[i].pid = -1;
        client->servers[i].fd_write = -1;
        client->servers[i].fd_read = -1;
        client->servers[i].next_id = 1;
    }

    return client;
}

int neuronos_mcp_client_add_server(neuronos_mcp_client_t * client,
                                   const neuronos_mcp_server_config_t * config) {
    if (!client || !config || !config->name)
        return -1;
    if (client->n_servers >= MCP_MAX_SERVERS) {
        fprintf(stderr, "[mcp-client] Max servers (%d) reached\n", MCP_MAX_SERVERS);
        return -1;
    }
    if (config->transport == NEURONOS_MCP_TRANSPORT_STDIO && !config->command) {
        fprintf(stderr, "[mcp-client] STDIO transport requires 'command'\n");
        return -1;
    }

    mcp_server_conn_t * srv = &client->servers[client->n_servers];
    memset(srv, 0, sizeof(*srv));
    srv->pid = -1;
    srv->fd_write = -1;
    srv->fd_read = -1;
    srv->next_id = 1;

    strncpy(srv->name, config->name, sizeof(srv->name) - 1);
    srv->transport = config->transport;

    /* Copy command */
    if (config->command)
        srv->command = strdup(config->command);

    /* Copy args */
    if (config->args && config->n_args > 0) {
        srv->args = calloc((size_t)config->n_args, sizeof(char *));
        srv->n_args = config->n_args;
        for (int i = 0; i < config->n_args; i++) {
            srv->args[i] = config->args[i] ? strdup(config->args[i]) : NULL;
        }
    }

    /* Copy env */
    if (config->env && config->n_env > 0) {
        srv->env = calloc((size_t)config->n_env, sizeof(char *));
        srv->n_env = config->n_env;
        for (int i = 0; i < config->n_env; i++) {
            srv->env[i] = config->env[i] ? strdup(config->env[i]) : NULL;
        }
    }

    client->n_servers++;
    fprintf(stderr, "[mcp-client] Added server '%s' (%s)\n",
            config->name,
            config->transport == NEURONOS_MCP_TRANSPORT_STDIO ? "STDIO" : "HTTP");
    return 0;
}

int neuronos_mcp_client_connect(neuronos_mcp_client_t * client) {
    if (!client)
        return -1;

    int connected = 0;
    int total_tools = 0;

    fprintf(stderr, "[mcp-client] Connecting to %d MCP server(s)...\n", client->n_servers);

    for (int i = 0; i < client->n_servers; i++) {
        mcp_server_conn_t * srv = &client->servers[i];

        if (srv->transport != NEURONOS_MCP_TRANSPORT_STDIO) {
            fprintf(stderr, "[mcp-client] Skipping '%s' (HTTP transport not yet implemented)\n",
                    srv->name);
            continue;
        }

        /* Spawn the server process */
        if (mcp_server_spawn(srv) < 0) {
            fprintf(stderr, "[mcp-client] Failed to spawn '%s'\n", srv->name);
            continue;
        }

        /* Small delay to let the server start */
        usleep(200000); /* 200ms */

        /* Initialize handshake */
        if (mcp_server_initialize(srv) < 0) {
            fprintf(stderr, "[mcp-client] Initialization failed for '%s'\n", srv->name);
            mcp_server_stop(srv);
            continue;
        }

        connected++;

        /* Discover tools */
        int remaining = MCP_MAX_TOOLS - client->n_tools;
        if (remaining > 0) {
            int found = mcp_discover_tools(srv, &client->tools[client->n_tools],
                                           remaining, i);
            client->n_tools += found;
            total_tools += found;
        }
    }

    fprintf(stderr, "[mcp-client] Connected: %d/%d servers, %d tools discovered\n",
            connected, client->n_servers, total_tools);

    return connected > 0 ? 0 : -1;
}

int neuronos_mcp_client_tool_count(const neuronos_mcp_client_t * client) {
    return client ? client->n_tools : 0;
}

int neuronos_mcp_client_register_tools(neuronos_mcp_client_t * client,
                                       neuronos_tool_registry_t * registry) {
    if (!client || !registry)
        return -1;

    int registered = 0;

    for (int i = 0; i < client->n_tools; i++) {
        const mcp_tool_entry_t * tool = &client->tools[i];

        /* Allocate bridge data (leaked intentionally — lives as long as registry) */
        mcp_bridge_data_t * bd = malloc(sizeof(mcp_bridge_data_t));
        if (!bd)
            continue;
        bd->client = client;
        bd->tool_index = i;

        neuronos_tool_desc_t desc = {
            .name = tool->name,
            .description = tool->description,
            .args_schema_json = tool->schema[0] ? tool->schema : "{\"type\":\"object\"}",
            .execute = mcp_tool_bridge,
            .user_data = bd,
            .required_caps = NEURONOS_CAP_NETWORK, /* MCP tools use IPC = network-like */
        };

        if (neuronos_tool_register(registry, &desc) == 0) {
            registered++;
        } else {
            free(bd);
        }
    }

    fprintf(stderr, "[mcp-client] Registered %d/%d MCP tools in tool registry\n",
            registered, client->n_tools);
    return registered;
}

char * neuronos_mcp_client_call_tool(neuronos_mcp_client_t * client,
                                     const char * tool_name,
                                     const char * args_json) {
    if (!client || !tool_name)
        return NULL;

    /* Find the tool and its server */
    int tool_idx = -1;
    for (int i = 0; i < client->n_tools; i++) {
        if (strcmp(client->tools[i].name, tool_name) == 0) {
            tool_idx = i;
            break;
        }
    }

    if (tool_idx < 0) {
        fprintf(stderr, "[mcp-client] Tool '%s' not found\n", tool_name);
        return NULL;
    }

    int srv_idx = client->tools[tool_idx].server_index;
    if (srv_idx < 0 || srv_idx >= client->n_servers) {
        fprintf(stderr, "[mcp-client] Invalid server index for tool '%s'\n", tool_name);
        return NULL;
    }

    mcp_server_conn_t * srv = &client->servers[srv_idx];
    if (!srv->connected) {
        fprintf(stderr, "[mcp-client] Server '%s' not connected\n", srv->name);
        return NULL;
    }

    /* Build tools/call params */
    char * esc_name = json_escape(tool_name);
    char params[MCP_MAX_LINE];
    snprintf(params, sizeof(params),
             "{\"name\":\"%s\",\"arguments\":%s}",
             esc_name ? esc_name : tool_name,
             (args_json && args_json[0] == '{') ? args_json : "{}");
    free(esc_name);

    fprintf(stderr, "[mcp-client] Calling tool '%s' on '%s'\n", tool_name, srv->name);

    const char * resp = mcp_client_request(srv, "tools/call", params);
    if (!resp) {
        fprintf(stderr, "[mcp-client] No response for tools/call '%s'\n", tool_name);
        return NULL;
    }

    /* Check for error */
    if (strstr(resp, "\"error\"") && !strstr(resp, "\"isError\"")) {
        int elen = 0;
        const char * emsg = json_find_str(resp, "message", &elen);
        if (emsg) {
            char * err = malloc((size_t)(elen + 32));
            if (err) {
                snprintf(err, (size_t)(elen + 32), "MCP error: %.*s", elen, emsg);
                return err;
            }
        }
        return strdup("MCP tool call returned an error");
    }

    /* Extract text content from result.content[0].text */
    char * result_obj = json_extract_object(resp, "result");
    if (!result_obj) {
        /* Try direct text extraction */
        int tlen = 0;
        const char * text = json_find_str(resp, "text", &tlen);
        if (text && tlen > 0) {
            char * out = malloc((size_t)(tlen + 1));
            if (out) {
                memcpy(out, text, (size_t)tlen);
                out[tlen] = '\0';
                return out;
            }
        }
        return strdup("(empty result)");
    }

    /* Look for text in content array */
    int tlen = 0;
    const char * text = json_find_str(result_obj, "text", &tlen);
    if (text && tlen > 0) {
        char * out = malloc((size_t)(tlen + 1));
        if (out) {
            memcpy(out, text, (size_t)tlen);
            out[tlen] = '\0';
        }
        free(result_obj);
        return out;
    }

    /* Fallback: return the whole result object */
    return result_obj;
}

int neuronos_mcp_client_load_config(neuronos_mcp_client_t * client,
                                    const char * config_path) {
    if (!client || !config_path)
        return -1;

    FILE * f = fopen(config_path, "r");
    if (!f) {
        fprintf(stderr, "[mcp-client] Cannot open config: %s\n", config_path);
        return -1;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 1048576) { /* max 1MB config */
        fclose(f);
        return -1;
    }

    char * json = malloc((size_t)fsize + 1);
    if (!json) {
        fclose(f);
        return -1;
    }

    size_t nread = fread(json, 1, (size_t)fsize, f);
    fclose(f);
    json[nread] = '\0';

    fprintf(stderr, "[mcp-client] Loading config from %s (%zu bytes)\n", config_path, nread);

    /* Extract mcpServers object */
    char * servers_obj = json_extract_object(json, "mcpServers");
    if (!servers_obj) {
        fprintf(stderr, "[mcp-client] No 'mcpServers' key in config\n");
        free(json);
        return -1;
    }

    /* Iterate over server entries.
     * Simple approach: find each "name": { ... } pattern */
    int loaded = 0;
    const char * p = servers_obj + 1; /* skip '{' */

    while (*p) {
        /* Skip whitespace and commas */
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ','))
            p++;
        if (*p == '}')
            break;

        /* Parse server name (key) */
        if (*p != '"')
            break;
        p++;
        const char * name_start = p;
        while (*p && *p != '"')
            p++;
        size_t name_len = (size_t)(p - name_start);
        if (*p == '"')
            p++;

        /* Skip colon */
        while (*p && (*p == ' ' || *p == ':'))
            p++;

        /* Parse server object */
        if (*p != '{')
            break;
        const char * obj_start = p;
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '{')
                depth++;
            else if (*p == '}')
                depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && *(p + 1))
                        p++;
                    p++;
                }
            }
            if (depth > 0)
                p++;
        }

        if (depth != 0)
            break;

        size_t obj_len = (size_t)(p - obj_start + 1);
        char * srv_json = malloc(obj_len + 1);
        if (!srv_json)
            break;
        memcpy(srv_json, obj_start, obj_len);
        srv_json[obj_len] = '\0';

        /* Extract command */
        int cmd_len = 0;
        const char * cmd = json_find_str(srv_json, "command", &cmd_len);

        if (cmd && cmd_len > 0) {
            char server_name[128] = {0};
            size_t ncpy = name_len < sizeof(server_name) - 1 ? name_len : sizeof(server_name) - 1;
            memcpy(server_name, name_start, ncpy);

            char command[512] = {0};
            int ccpy = cmd_len < (int)sizeof(command) - 1 ? cmd_len : (int)sizeof(command) - 1;
            memcpy(command, cmd, (size_t)ccpy);

            /* Parse args array */
            char ** args = NULL;
            int n_args = 0;
            char * args_arr = json_extract_array(srv_json, "args");
            if (args_arr) {
                n_args = parse_string_array(args_arr, &args);
                free(args_arr);
            }

            /* Parse env object */
            char ** env = NULL;
            int n_env = 0;
            char * env_obj = json_extract_object(srv_json, "env");
            if (env_obj) {
                n_env = parse_env_object(env_obj, &env);
                free(env_obj);
            }

            neuronos_mcp_server_config_t config = {
                .name = server_name,
                .transport = NEURONOS_MCP_TRANSPORT_STDIO,
                .command = command,
                .args = (const char **)args,
                .n_args = n_args,
                .env = (const char **)env,
                .n_env = n_env,
            };

            if (neuronos_mcp_client_add_server(client, &config) == 0) {
                loaded++;
            }

            /* Free temporary arrays (add_server made copies) */
            if (args) {
                for (int i = 0; i < n_args; i++)
                    free(args[i]);
                free(args);
            }
            if (env) {
                for (int i = 0; i < n_env; i++)
                    free(env[i]);
                free(env);
            }
        }

        free(srv_json);
        if (*p)
            p++;
    }

    free(servers_obj);
    free(json);

    fprintf(stderr, "[mcp-client] Loaded %d server(s) from config\n", loaded);
    return loaded;
}

void neuronos_mcp_client_free(neuronos_mcp_client_t * client) {
    if (!client)
        return;

    fprintf(stderr, "[mcp-client] Shutting down %d server(s)...\n", client->n_servers);

    for (int i = 0; i < client->n_servers; i++) {
        mcp_server_stop(&client->servers[i]);
    }

    free(client);
}
