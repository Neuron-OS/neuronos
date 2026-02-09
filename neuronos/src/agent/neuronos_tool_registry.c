/* ============================================================
 * NeuronOS — Tool Registry
 * Register, discover, and execute tools for the agent.
 *
 * Phase 2C: tool registration and dispatch.
 * ============================================================ */
#include "neuronos/neuronos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>

/* ---- Constants ---- */
#define NEURONOS_MAX_TOOLS 64

/* ---- Internal struct ---- */
struct neuronos_tool_reg {
    neuronos_tool_desc_t tools[NEURONOS_MAX_TOOLS];
    int count;
};

/* ============================================================
 * REGISTRY LIFECYCLE
 * ============================================================ */

neuronos_tool_registry_t * neuronos_tool_registry_create(void) {
    neuronos_tool_registry_t * reg = calloc(1, sizeof(neuronos_tool_registry_t));
    return reg;
}

void neuronos_tool_registry_free(neuronos_tool_registry_t * reg) {
    free(reg);
}

/* ============================================================
 * REGISTER
 * ============================================================ */

int neuronos_tool_register(neuronos_tool_registry_t * reg, const neuronos_tool_desc_t * desc) {
    if (!reg || !desc || !desc->name || !desc->execute)
        return -1;
    if (reg->count >= NEURONOS_MAX_TOOLS)
        return -1;

    /* Check for duplicate name */
    for (int i = 0; i < reg->count; i++) {
        if (strcmp(reg->tools[i].name, desc->name) == 0) {
            return -1; /* duplicate */
        }
    }

    reg->tools[reg->count] = *desc;
    reg->count++;
    return 0;
}

/* ============================================================
 * EXECUTE
 * ============================================================ */

neuronos_tool_result_t neuronos_tool_execute(neuronos_tool_registry_t * reg, const char * tool_name,
                                             const char * args_json) {
    neuronos_tool_result_t result = {0};

    if (!reg || !tool_name) {
        result.success = false;
        result.error = strdup("Invalid arguments");
        return result;
    }

    /* Find tool */
    for (int i = 0; i < reg->count; i++) {
        if (strcmp(reg->tools[i].name, tool_name) == 0) {
            return reg->tools[i].execute(args_json ? args_json : "{}", reg->tools[i].user_data);
        }
    }

    result.success = false;
    result.error = strdup("Tool not found");
    return result;
}

void neuronos_tool_result_free(neuronos_tool_result_t * result) {
    if (!result)
        return;
    free(result->output);
    free(result->error);
    result->output = NULL;
    result->error = NULL;
}

/* ============================================================
 * INSPECTION
 * ============================================================ */

int neuronos_tool_count(const neuronos_tool_registry_t * reg) {
    return reg ? reg->count : 0;
}

const char * neuronos_tool_name(const neuronos_tool_registry_t * reg, int index) {
    if (!reg || index < 0 || index >= reg->count)
        return NULL;
    return reg->tools[index].name;
}

const char * neuronos_tool_description(const neuronos_tool_registry_t * reg, int index) {
    if (!reg || index < 0 || index >= reg->count)
        return NULL;
    return reg->tools[index].description;
}

const char * neuronos_tool_schema(const neuronos_tool_registry_t * reg, int index) {
    if (!reg || index < 0 || index >= reg->count)
        return NULL;
    return reg->tools[index].args_schema_json;
}

/* ============================================================
 * GBNF GRAMMAR GENERATION
 * ============================================================ */

/*
 * Generate GBNF rule for tool names:
 *   tool-name ::= "\"shell\"" | "\"read_file\"" | ...
 */
char * neuronos_tool_grammar_names(const neuronos_tool_registry_t * reg) {
    if (!reg || reg->count == 0)
        return strdup("tool-name ::= \"\\\"noop\\\"\"");

    /* Estimate buffer size: each tool adds ~30 chars */
    size_t cap = 256 + (size_t)reg->count * 40;
    char * buf = malloc(cap);
    if (!buf)
        return NULL;

    size_t len = 0;
    len += (size_t)snprintf(buf + len, cap - len, "tool-name ::= ");

    for (int i = 0; i < reg->count; i++) {
        if (i > 0) {
            len += (size_t)snprintf(buf + len, cap - len, " | ");
        }
        len += (size_t)snprintf(buf + len, cap - len, "\"\\\"");
        len += (size_t)snprintf(buf + len, cap - len, "%s", reg->tools[i].name);
        len += (size_t)snprintf(buf + len, cap - len, "\\\"\"");
    }

    return buf;
}

/*
 * Generate tool descriptions for the system prompt:
 * Available tools:
 * - shell: Execute a shell command. Args: {"command": "<string>"}
 * - read_file: Read a file. Args: {"path": "<string>"}
 * ...
 */
char * neuronos_tool_prompt_description(const neuronos_tool_registry_t * reg) {
    if (!reg || reg->count == 0) {
        return strdup("No tools available.\n");
    }

    size_t cap = 512 + (size_t)reg->count * 256;
    char * buf = malloc(cap);
    if (!buf)
        return NULL;

    size_t len = 0;
    len += (size_t)snprintf(buf + len, cap - len, "Available tools:\n");

    for (int i = 0; i < reg->count; i++) {
        len += (size_t)snprintf(buf + len, cap - len, "- %s: %s", reg->tools[i].name,
                                reg->tools[i].description ? reg->tools[i].description : "No description");

        if (reg->tools[i].args_schema_json) {
            len += (size_t)snprintf(buf + len, cap - len, " Args schema: %s", reg->tools[i].args_schema_json);
        }
        len += (size_t)snprintf(buf + len, cap - len, "\n");
    }

    return buf;
}

/* ============================================================
 * BUILT-IN TOOLS
 * ============================================================ */

/* ---- Input sanitization helpers ---- */

/**
 * Check if a string contains any shell metacharacters that could
 * allow command injection when embedded in single-quoted shell args.
 * Returns true if the string is SAFE (no dangerous chars).
 */
static bool is_safe_for_shell_embed(const char * str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch (str[i]) {
            case '\'': /* breaks out of single quotes */
            case '`':  /* command substitution */
            case '$':  /* variable expansion */
            case '|':  /* pipe */
            case ';':  /* command separator */
            case '&':  /* background / AND */
            case '\n': /* command separator */
            case '\r':
            case '\0':
                return false;
            default:
                break;
        }
    }
    return true;
}

/**
 * Validate a math expression for the calculate tool.
 * Only allow: digits, decimal points, operators, parens, whitespace,
 * and known bc function names.
 * Returns true if the expression is SAFE.
 */
static bool is_safe_math_expression(const char * expr, size_t len) {
    /* Allowed single characters */
    static const char allowed_chars[] = "0123456789.+-*/^%() \t";

    for (size_t i = 0; i < len; i++) {
        char c = expr[i];

        /* Check simple allowed chars */
        if (strchr(allowed_chars, c))
            continue;

        /* Allow known bc function names: a-z letters for identifiers like sqrt, scale, etc. */
        if (c >= 'a' && c <= 'z')
            continue;
        if (c >= 'A' && c <= 'Z')
            continue;
        if (c == '_')
            continue;

        /* Anything else (including ', ", ;, |, &, $, `, \, etc.) is rejected */
        return false;
    }
    return true;
}

/**
 * Validate a filesystem path: reject shell metacharacters and path traversal.
 * Returns true if the path is SAFE.
 */
static bool is_safe_path(const char * path, size_t len) {
    if (!is_safe_for_shell_embed(path, len))
        return false;

    /* Reject null bytes (already covered above) and excessive .. traversal */
    /* We allow .. in general paths but the shell-embed check handles injection */
    return true;
}

/* --- shell tool ---
 * NOTE: This tool intentionally executes arbitrary shell commands.
 * Access is gated by NEURONOS_CAP_SHELL capability flag.
 * Future: add configurable allowlist/denylist and sandbox mode.
 */
static neuronos_tool_result_t tool_shell(const char * args_json, void * user_data) {
    (void)user_data;
    neuronos_tool_result_t result = {0};

    /* Minimal JSON parsing: extract "command" field */
    const char * cmd_start = strstr(args_json, "\"command\"");
    if (!cmd_start) {
        result.success = false;
        result.error = strdup("Missing 'command' argument");
        return result;
    }

    /* Find the value string */
    cmd_start = strchr(cmd_start + 9, '"');
    if (!cmd_start) {
        result.success = false;
        result.error = strdup("Invalid 'command' format");
        return result;
    }
    cmd_start++; /* skip opening quote */

    /* Find closing quote (handle escapes simply) */
    const char * cmd_end = cmd_start;
    while (*cmd_end && *cmd_end != '"') {
        if (*cmd_end == '\\')
            cmd_end++; /* skip escaped char */
        if (*cmd_end)
            cmd_end++;
    }

    size_t cmd_len = (size_t)(cmd_end - cmd_start);
    char * command = malloc(cmd_len + 1);
    memcpy(command, cmd_start, cmd_len);
    command[cmd_len] = '\0';

    /* Execute with popen */
    FILE * fp = popen(command, "r");
    free(command);

    if (!fp) {
        result.success = false;
        result.error = strdup("Failed to execute command");
        return result;
    }

    /* Read output */
    size_t out_cap = 4096;
    size_t out_len = 0;
    char * out_buf = malloc(out_cap);

    char line[512];
    while (fgets(line, (int)sizeof(line), fp)) {
        size_t line_len = strlen(line);
        while (out_len + line_len + 1 > out_cap) {
            out_cap *= 2;
            out_buf = realloc(out_buf, out_cap);
        }
        memcpy(out_buf + out_len, line, line_len);
        out_len += line_len;
    }
    out_buf[out_len] = '\0';

    int status = pclose(fp);
    result.success = (status == 0);
    result.output = out_buf;

    if (!result.success) {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "Command exited with status %d", status);
        result.error = strdup(err_msg);
    }

    return result;
}

/* --- read_file tool --- */
static neuronos_tool_result_t tool_read_file(const char * args_json, void * user_data) {
    (void)user_data;
    neuronos_tool_result_t result = {0};

    const char * path_start = strstr(args_json, "\"path\"");
    if (!path_start) {
        result.success = false;
        result.error = strdup("Missing 'path' argument");
        return result;
    }
    path_start = strchr(path_start + 6, '"');
    if (!path_start) {
        result.success = false;
        result.error = strdup("Invalid 'path'");
        return result;
    }
    path_start++;
    const char * path_end = strchr(path_start, '"');
    if (!path_end) {
        result.success = false;
        result.error = strdup("Invalid 'path'");
        return result;
    }

    size_t path_len = (size_t)(path_end - path_start);
    char * path = malloc(path_len + 1);
    memcpy(path, path_start, path_len);
    path[path_len] = '\0';

    FILE * fp = fopen(path, "r");
    free(path);

    if (!fp) {
        result.success = false;
        result.error = strdup("File not found or cannot read");
        return result;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    /* Limit to 32KB for context budget */
    if (fsize > 32 * 1024)
        fsize = 32 * 1024;

    char * content = malloc((size_t)fsize + 1);
    size_t nread = fread(content, 1, (size_t)fsize, fp);
    content[nread] = '\0';
    fclose(fp);

    result.success = true;
    result.output = content;
    return result;
}

/* --- write_file tool --- */
static neuronos_tool_result_t tool_write_file(const char * args_json, void * user_data) {
    (void)user_data;
    neuronos_tool_result_t result = {0};

    /* Extract path */
    const char * path_start = strstr(args_json, "\"path\"");
    if (!path_start) {
        result.success = false;
        result.error = strdup("Missing 'path'");
        return result;
    }
    path_start = strchr(path_start + 6, '"');
    if (!path_start) {
        result.success = false;
        result.error = strdup("Invalid 'path'");
        return result;
    }
    path_start++;
    const char * path_end = strchr(path_start, '"');
    if (!path_end) {
        result.success = false;
        result.error = strdup("Invalid 'path'");
        return result;
    }

    size_t path_len = (size_t)(path_end - path_start);
    char * path = malloc(path_len + 1);
    memcpy(path, path_start, path_len);
    path[path_len] = '\0';

    /* Extract content */
    const char * cnt_start = strstr(args_json, "\"content\"");
    if (!cnt_start) {
        free(path);
        result.success = false;
        result.error = strdup("Missing 'content'");
        return result;
    }
    cnt_start = strchr(cnt_start + 9, '"');
    if (!cnt_start) {
        free(path);
        result.success = false;
        result.error = strdup("Invalid 'content'");
        return result;
    }
    cnt_start++;
    const char * cnt_end = cnt_start;
    while (*cnt_end && *cnt_end != '"') {
        if (*cnt_end == '\\')
            cnt_end++;
        if (*cnt_end)
            cnt_end++;
    }

    size_t cnt_len = (size_t)(cnt_end - cnt_start);
    char * content = malloc(cnt_len + 1);
    memcpy(content, cnt_start, cnt_len);
    content[cnt_len] = '\0';

    FILE * fp = fopen(path, "w");
    free(path);

    if (!fp) {
        free(content);
        result.success = false;
        result.error = strdup("Cannot write file");
        return result;
    }

    fwrite(content, 1, cnt_len, fp);
    fclose(fp);
    free(content);

    result.success = true;
    result.output = strdup("File written successfully");
    return result;
}

/* --- calculate tool --- */
static neuronos_tool_result_t tool_calculate(const char * args_json, void * user_data) {
    (void)user_data;
    neuronos_tool_result_t result = {0};

    /* Simple: extract "expression" and eval via shell bc */
    const char * expr_start = strstr(args_json, "\"expression\"");
    if (!expr_start) {
        result.success = false;
        result.error = strdup("Missing 'expression' argument");
        return result;
    }
    expr_start = strchr(expr_start + 12, '"');
    if (!expr_start) {
        result.success = false;
        result.error = strdup("Invalid 'expression'");
        return result;
    }
    expr_start++;
    const char * expr_end = strchr(expr_start, '"');
    if (!expr_end) {
        result.success = false;
        result.error = strdup("Invalid 'expression'");
        return result;
    }

    size_t expr_len = (size_t)(expr_end - expr_start);

    /* Validate expression: reject shell metacharacters */
    if (!is_safe_math_expression(expr_start, expr_len)) {
        result.success = false;
        result.error = strdup("Invalid expression: contains disallowed characters");
        return result;
    }

    /* Build command: echo 'EXPR' | bc -l */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "echo '%.*s' | bc -l 2>&1", (int)expr_len, expr_start);

    FILE * fp = popen(cmd, "r");
    if (!fp) {
        result.success = false;
        result.error = strdup("bc not available");
        return result;
    }

    char out[256];
    if (fgets(out, (int)sizeof(out), fp)) {
        /* Trim trailing newline */
        size_t len = strlen(out);
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
            out[--len] = '\0';
        }
        result.output = strdup(out);
    } else {
        result.output = strdup("0");
    }

    pclose(fp);
    result.success = true;
    return result;
}

/* ---- Register defaults ---- */

/* --- list_dir tool --- */
static neuronos_tool_result_t tool_list_dir(const char * args_json, void * user_data) {
    (void)user_data;
    neuronos_tool_result_t result = {0};

    /* Extract "path" from JSON */
    const char * path_start = strstr(args_json, "\"path\"");
    if (!path_start) {
        result.success = false;
        result.error = strdup("Missing 'path' argument");
        return result;
    }
    path_start = strchr(path_start + 6, '"');
    if (!path_start) {
        result.success = false;
        result.error = strdup("Invalid 'path'");
        return result;
    }
    path_start++;
    const char * path_end = strchr(path_start, '"');
    if (!path_end) {
        result.success = false;
        result.error = strdup("Invalid 'path'");
        return result;
    }

    size_t plen = (size_t)(path_end - path_start);
    char * path = malloc(plen + 1);
    memcpy(path, path_start, plen);
    path[plen] = '\0';

    DIR * dir = opendir(path);
    free(path);
    if (!dir) {
        result.success = false;
        result.error = strdup("Cannot open directory");
        return result;
    }

    /* Build JSON array of entries */
    char buf[8192];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "[");

    struct dirent * entry;
    int first = 1;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (!first)
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",");
        const char * type = (entry->d_type == DT_DIR) ? "dir" : "file";
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "{\"name\":\"%s\",\"type\":\"%s\"}", entry->d_name, type);
        first = 0;
        if (pos >= (int)sizeof(buf) - 100)
            break;
    }
    closedir(dir);
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]");

    result.success = true;
    result.output = strdup(buf);
    return result;
}

/* --- search_files tool --- */
static neuronos_tool_result_t tool_search_files(const char * args_json, void * user_data) {
    (void)user_data;
    neuronos_tool_result_t result = {0};

    /* Extract "pattern" and optional "directory" */
    const char * pat_start = strstr(args_json, "\"pattern\"");
    if (!pat_start) {
        result.success = false;
        result.error = strdup("Missing 'pattern' argument");
        return result;
    }
    pat_start = strchr(pat_start + 9, '"');
    if (!pat_start) {
        result.success = false;
        result.error = strdup("Invalid 'pattern'");
        return result;
    }
    pat_start++;
    const char * pat_end = strchr(pat_start, '"');
    if (!pat_end) {
        result.success = false;
        result.error = strdup("Invalid 'pattern'");
        return result;
    }

    size_t plen = (size_t)(pat_end - pat_start);

    /* Optional directory, default to "." */
    const char * dir = ".";
    char dir_buf[512] = ".";
    const char * dir_start = strstr(args_json, "\"directory\"");
    if (dir_start) {
        dir_start = strchr(dir_start + 11, '"');
        if (dir_start) {
            dir_start++;
            const char * dir_end = strchr(dir_start, '"');
            if (dir_end) {
                size_t dlen = (size_t)(dir_end - dir_start);
                if (dlen < sizeof(dir_buf)) {
                    memcpy(dir_buf, dir_start, dlen);
                    dir_buf[dlen] = '\0';
                }
            }
        }
    }
    dir = dir_buf;

    /* Validate pattern and directory: reject shell metacharacters */
    if (!is_safe_for_shell_embed(pat_start, plen)) {
        result.success = false;
        result.error = strdup("Invalid pattern: contains disallowed characters");
        return result;
    }
    if (!is_safe_path(dir, strlen(dir))) {
        result.success = false;
        result.error = strdup("Invalid directory: contains disallowed characters");
        return result;
    }

    /* Use find command for file search */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "find \"%s\" -maxdepth 4 -name '%.*s' -type f 2>/dev/null | head -20", dir, (int)plen,
             pat_start);

    FILE * fp = popen(cmd, "r");
    if (!fp) {
        result.success = false;
        result.error = strdup("find command failed");
        return result;
    }

    char buf[4096];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "[");
    char line[512];
    int first = 1;
    while (fgets(line, (int)sizeof(line), fp) && pos < (int)sizeof(buf) - 100) {
        size_t llen = strlen(line);
        while (llen > 0 && (line[llen - 1] == '\n' || line[llen - 1] == '\r'))
            line[--llen] = '\0';
        if (llen == 0)
            continue;
        if (!first)
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\"%s\"", line);
        first = 0;
    }
    pclose(fp);
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]");

    result.success = true;
    result.output = strdup(buf);
    return result;
}

/* --- http_get tool --- */
static neuronos_tool_result_t tool_http_get(const char * args_json, void * user_data) {
    (void)user_data;
    neuronos_tool_result_t result = {0};

    /* Extract "url" */
    const char * url_start = strstr(args_json, "\"url\"");
    if (!url_start) {
        result.success = false;
        result.error = strdup("Missing 'url' argument");
        return result;
    }
    url_start = strchr(url_start + 5, '"');
    if (!url_start) {
        result.success = false;
        result.error = strdup("Invalid 'url'");
        return result;
    }
    url_start++;
    const char * url_end = strchr(url_start, '"');
    if (!url_end) {
        result.success = false;
        result.error = strdup("Invalid 'url'");
        return result;
    }

    size_t ulen = (size_t)(url_end - url_start);

    /* Validate URL starts with http:// or https:// */
    if (ulen < 8 || (strncmp(url_start, "http://", 7) != 0 && strncmp(url_start, "https://", 8) != 0)) {
        result.success = false;
        result.error = strdup("URL must start with http:// or https://");
        return result;
    }

    /* Reject shell metacharacters in URL to prevent command injection */
    if (!is_safe_for_shell_embed(url_start, ulen)) {
        result.success = false;
        result.error = strdup("URL contains disallowed characters");
        return result;
    }

    /* Use curl for HTTP request (timeout 10s, max 32KB) */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "curl -sL --max-time 10 --max-filesize 32768 "
             "-H 'User-Agent: NeuronOS/%s' '%.*s' 2>/dev/null | head -c 32768",
             NEURONOS_VERSION_STRING, (int)ulen, url_start);

    FILE * fp = popen(cmd, "r");
    if (!fp) {
        result.success = false;
        result.error = strdup("curl not available");
        return result;
    }

    /* Read response (max 32KB) */
    char * buf = malloc(32769);
    if (!buf) {
        pclose(fp);
        result.success = false;
        result.error = strdup("Memory allocation failed");
        return result;
    }

    size_t total = 0;
    size_t n;
    while ((n = fread(buf + total, 1, 32768 - total, fp)) > 0) {
        total += n;
        if (total >= 32768)
            break;
    }
    buf[total] = '\0';
    pclose(fp);

    if (total == 0) {
        free(buf);
        result.success = false;
        result.error = strdup("Empty response or connection failed");
        return result;
    }

    result.success = true;
    result.output = buf;
    return result;
}

int neuronos_tool_register_defaults(neuronos_tool_registry_t * reg, uint32_t allowed_caps) {
    if (!reg)
        return -1;
    int registered = 0;

    if (allowed_caps & NEURONOS_CAP_SHELL) {
        neuronos_tool_desc_t desc = {
            .name = "shell",
            .description = "Execute a shell command and return its output.",
            .args_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":"
                                "\"The shell command to execute\"}},\"required\":[\"command\"]}",
            .execute = tool_shell,
            .user_data = NULL,
            .required_caps = NEURONOS_CAP_SHELL,
        };
        if (neuronos_tool_register(reg, &desc) == 0)
            registered++;
    }

    if (allowed_caps & NEURONOS_CAP_FILESYSTEM) {
        neuronos_tool_desc_t desc_read = {
            .name = "read_file",
            .description = "Read the contents of a file (max 32KB).",
            .args_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":"
                                "\"File path to read\"}},\"required\":[\"path\"]}",
            .execute = tool_read_file,
            .user_data = NULL,
            .required_caps = NEURONOS_CAP_FILESYSTEM,
        };
        if (neuronos_tool_register(reg, &desc_read) == 0)
            registered++;

        neuronos_tool_desc_t desc_write = {
            .name = "write_file",
            .description = "Write content to a file.",
            .args_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{"
                                "\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
            .execute = tool_write_file,
            .user_data = NULL,
            .required_caps = NEURONOS_CAP_FILESYSTEM,
        };
        if (neuronos_tool_register(reg, &desc_write) == 0)
            registered++;

        neuronos_tool_desc_t desc_list_dir = {
            .name = "list_dir",
            .description = "List files and directories in a path.",
            .args_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":"
                                "\"Directory path to list\"}},\"required\":[\"path\"]}",
            .execute = tool_list_dir,
            .user_data = NULL,
            .required_caps = NEURONOS_CAP_FILESYSTEM,
        };
        if (neuronos_tool_register(reg, &desc_list_dir) == 0)
            registered++;

        neuronos_tool_desc_t desc_search = {
            .name = "search_files",
            .description = "Search for files by name pattern (glob). Returns matching paths.",
            .args_schema_json =
                "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"File name "
                "pattern, e.g. *.py, *.c, config*\"},\"directory\":{\"type\":\"string\",\"description\":\"Root "
                "directory to search (default: .)\"}},\"required\":[\"pattern\"]}",
            .execute = tool_search_files,
            .user_data = NULL,
            .required_caps = NEURONOS_CAP_FILESYSTEM,
        };
        if (neuronos_tool_register(reg, &desc_search) == 0)
            registered++;
    }

    if (allowed_caps & NEURONOS_CAP_NETWORK) {
        neuronos_tool_desc_t desc_http = {
            .name = "http_get",
            .description = "Fetch content from a URL via HTTP GET (max 32KB, 10s timeout).",
            .args_schema_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":"
                                "\"URL to fetch (http:// or https://)\"}},\"required\":[\"url\"]}",
            .execute = tool_http_get,
            .user_data = NULL,
            .required_caps = NEURONOS_CAP_NETWORK,
        };
        if (neuronos_tool_register(reg, &desc_http) == 0)
            registered++;
    }

    {
        neuronos_tool_desc_t desc_calc = {
            .name = "calculate",
            .description = "Evaluate a mathematical expression (uses bc).",
            .args_schema_json =
                "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\",\"description\":\"Math "
                "expression, e.g. 2+2, sqrt(144)\"}},\"required\":[\"expression\"]}",
            .execute = tool_calculate,
            .user_data = NULL,
            .required_caps = 0, /* no special capabilities needed */
        };
        if (neuronos_tool_register(reg, &desc_calc) == 0)
            registered++;
    }

    return registered;
}
