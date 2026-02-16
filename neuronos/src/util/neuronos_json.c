/* ============================================================
 * NeuronOS — Unified JSON parser
 *
 * Single correct implementation replacing the 4 duplicated
 * "minimal JSON helpers" that used raw strstr() and could
 * match keys inside string values.
 *
 * Design:
 *   - nj_find_key() scans JSON correctly, skipping strings,
 *     and only matches keys at the current nesting level.
 *   - All public functions build on nj_find_key().
 *   - String skip handles \", \\, and all escape sequences.
 *   - Object/array extraction counts brace/bracket depth.
 *
 * Copyright (c) 2025 NeuronOS Project
 * SPDX-License-Identifier: MIT
 * ============================================================ */

#include "neuronos/neuronos_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────
 * Internal: skip helpers
 * ────────────────────────────────────────────────────────────── */

const char * nj_skip_ws(const char * p) {
    if (!p)
        return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/**
 * Skip a JSON string (starting at the opening quote).
 * Returns pointer PAST the closing quote, or NULL on error.
 */
static const char * skip_string(const char * p) {
    if (!p || *p != '"')
        return NULL;
    p++; /* skip opening quote */
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++; /* skip escape char */
            if (!*p)
                return NULL;
        }
        p++;
    }
    if (*p == '"')
        return p + 1; /* past closing quote */
    return NULL;       /* unterminated string */
}

/**
 * Skip a JSON number (integer or float, possibly negative).
 */
static const char * skip_number(const char * p) {
    if (!p)
        return NULL;
    if (*p == '-')
        p++;
    while (*p >= '0' && *p <= '9')
        p++;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9')
            p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-')
            p++;
        while (*p >= '0' && *p <= '9')
            p++;
    }
    return p;
}

const char * nj_skip_value(const char * p) {
    if (!p)
        return NULL;
    p = nj_skip_ws(p);
    if (!*p)
        return NULL;

    switch (*p) {
        case '"':
            return skip_string(p);

        case '{': {
            int depth = 1;
            p++;
            while (*p && depth > 0) {
                if (*p == '"') {
                    p = skip_string(p);
                    if (!p)
                        return NULL;
                    continue;
                }
                if (*p == '{')
                    depth++;
                else if (*p == '}')
                    depth--;
                if (depth > 0)
                    p++;
            }
            return (*p == '}') ? p + 1 : NULL;
        }

        case '[': {
            int depth = 1;
            p++;
            while (*p && depth > 0) {
                if (*p == '"') {
                    p = skip_string(p);
                    if (!p)
                        return NULL;
                    continue;
                }
                if (*p == '[')
                    depth++;
                else if (*p == ']')
                    depth--;
                if (depth > 0)
                    p++;
            }
            return (*p == ']') ? p + 1 : NULL;
        }

        case 't': /* true */
            if (strncmp(p, "true", 4) == 0)
                return p + 4;
            return NULL;

        case 'f': /* false */
            if (strncmp(p, "false", 5) == 0)
                return p + 5;
            return NULL;

        case 'n': /* null */
            if (strncmp(p, "null", 4) == 0)
                return p + 4;
            return NULL;

        default:
            if (*p == '-' || (*p >= '0' && *p <= '9'))
                return skip_number(p);
            return NULL;
    }
}

/* ──────────────────────────────────────────────────────────────
 * Internal: find key in JSON
 *
 * Scans the JSON and finds a key matching `key` at ANY nesting
 * level. Returns pointer to the value (after the colon and
 * whitespace).
 *
 * Correctly skips over string values, so "key" inside a string
 * value will NOT be matched as a key.
 * ────────────────────────────────────────────────────────────── */

static const char * nj_find_key(const char * json, const char * key) {
    if (!json || !key)
        return NULL;

    size_t key_len = strlen(key);
    const char * p = json;

    while (*p) {
        /* Skip whitespace */
        p = nj_skip_ws(p);
        if (!*p)
            break;

        if (*p == '"') {
            /* We found a string — check if it's a key */
            const char * str_start = p + 1;
            const char * str_end = skip_string(p);
            if (!str_end)
                break;

            /* Calculate string content length */
            size_t str_len = (size_t)(str_end - str_start - 1); /* -1 for closing quote */

            /* Check if this string is followed by a colon (= it's a key) */
            const char * after = nj_skip_ws(str_end);
            if (after && *after == ':') {
                /* It's a key! Check if it matches */
                if (str_len == key_len && memcmp(str_start, key, key_len) == 0) {
                    /* Match — return pointer to the value */
                    const char * val = nj_skip_ws(after + 1);
                    return val;
                }
                /* Not our key — skip the value */
                p = nj_skip_ws(after + 1);
                p = nj_skip_value(p);
                if (!p)
                    break;
            } else {
                /* It's a string value (not a key) — already skipped */
                p = str_end;
            }
        } else if (*p == '{' || *p == '[' || *p == ',' || *p == ':') {
            p++;
        } else if (*p == '}' || *p == ']') {
            p++;
        } else {
            /* Skip any other value (number, bool, null) */
            p = nj_skip_value(p);
            if (!p)
                break;
        }
    }

    return NULL;
}

/* ──────────────────────────────────────────────────────────────
 * Public API
 * ────────────────────────────────────────────────────────────── */

const char * nj_find_str(const char * json, const char * key, int * out_len) {
    const char * val = nj_find_key(json, key);
    if (!val || *val != '"')
        return NULL;

    const char * start = val + 1; /* past opening quote */
    const char * p = start;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            p += 2; /* skip escape sequence */
            continue;
        }
        p++;
    }

    if (out_len)
        *out_len = (int)(p - start);
    return start;
}

int nj_find_int(const char * json, const char * key, int fallback) {
    const char * val = nj_find_key(json, key);
    if (!val)
        return fallback;

    /* Accept numbers (possibly negative) */
    if (*val == '-' || (*val >= '0' && *val <= '9'))
        return atoi(val);

    return fallback;
}

/**
 * Extract a delimited structure (object or array) as a new string.
 */
static char * extract_delimited(const char * json, const char * key, char open, char close) {
    const char * val = nj_find_key(json, key);
    if (!val || *val != open)
        return NULL;

    const char * start = val;
    int depth = 1;
    val++;
    while (*val && depth > 0) {
        if (*val == '"') {
            val = skip_string(val);
            if (!val)
                return NULL;
            continue;
        }
        if (*val == open)
            depth++;
        else if (*val == close)
            depth--;
        if (depth > 0)
            val++;
    }

    if (depth != 0)
        return NULL;

    size_t len = (size_t)(val - start + 1);
    char * out = malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

char * nj_extract_object(const char * json, const char * key) {
    return extract_delimited(json, key, '{', '}');
}

char * nj_extract_array(const char * json, const char * key) {
    return extract_delimited(json, key, '[', ']');
}

int nj_copy_str(const char * json, const char * key, char * buf, size_t bufsize) {
    if (!buf || bufsize == 0)
        return -1;

    int len = 0;
    const char * val = nj_find_str(json, key, &len);
    if (!val)
        return -1;

    size_t cpy = (size_t)len < bufsize - 1 ? (size_t)len : bufsize - 1;
    memcpy(buf, val, cpy);
    buf[cpy] = '\0';
    return (int)cpy;
}

char * nj_escape(const char * s) {
    if (!s)
        return strdup("null");

    size_t slen = strlen(s);
    /* Worst case: every char becomes \uXXXX (6 chars) + NUL */
    size_t cap = slen * 6 + 1;
    char * out = malloc(cap);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; s[i] && j < cap - 6; i++) {
        switch (s[i]) {
            case '"':
                out[j++] = '\\';
                out[j++] = '"';
                break;
            case '\\':
                out[j++] = '\\';
                out[j++] = '\\';
                break;
            case '\n':
                out[j++] = '\\';
                out[j++] = 'n';
                break;
            case '\r':
                out[j++] = '\\';
                out[j++] = 'r';
                break;
            case '\t':
                out[j++] = '\\';
                out[j++] = 't';
                break;
            case '\b':
                out[j++] = '\\';
                out[j++] = 'b';
                break;
            case '\f':
                out[j++] = '\\';
                out[j++] = 'f';
                break;
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

int nj_find_bool(const char * json, const char * key, int fallback) {
    const char * val = nj_find_key(json, key);
    if (!val)
        return fallback;
    val = nj_skip_ws(val);
    if (strncmp(val, "true", 4) == 0)
        return 1;
    if (strncmp(val, "false", 5) == 0)
        return 0;
    return fallback;
}

float nj_find_float(const char * json, const char * key, float fallback) {
    const char * val = nj_find_key(json, key);
    if (!val)
        return fallback;
    if (*val == '-' || (*val >= '0' && *val <= '9'))
        return (float)atof(val);
    return fallback;
}

char * nj_alloc_str(const char * json, const char * key) {
    int len = 0;
    const char * val = nj_find_str(json, key, &len);
    if (!val)
        return NULL;
    char * out = malloc((size_t)len + 1);
    if (!out)
        return NULL;
    memcpy(out, val, (size_t)len);
    out[len] = '\0';
    return out;
}

char * nj_escape_n(const char * s, size_t max_len) {
    if (!s)
        return strdup("null");

    size_t slen = strlen(s);
    if (max_len < slen)
        slen = max_len;

    size_t cap = slen * 6 + 1;
    char * out = malloc(cap);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < slen && s[i] && j < cap - 6; i++) {
        switch (s[i]) {
            case '"':
                out[j++] = '\\';
                out[j++] = '"';
                break;
            case '\\':
                out[j++] = '\\';
                out[j++] = '\\';
                break;
            case '\n':
                out[j++] = '\\';
                out[j++] = 'n';
                break;
            case '\r':
                out[j++] = '\\';
                out[j++] = 'r';
                break;
            case '\t':
                out[j++] = '\\';
                out[j++] = 't';
                break;
            case '\b':
                out[j++] = '\\';
                out[j++] = 'b';
                break;
            case '\f':
                out[j++] = '\\';
                out[j++] = 'f';
                break;
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

char * nj_unescape(const char * s) {
    if (!s)
        return NULL;
    size_t len = strlen(s);
    char * out = malloc(len + 1);
    if (!out)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            switch (s[i + 1]) {
                case 'n':
                    out[j++] = '\n';
                    i++;
                    break;
                case 't':
                    out[j++] = '\t';
                    i++;
                    break;
                case 'r':
                    out[j++] = '\r';
                    i++;
                    break;
                case '"':
                    out[j++] = '"';
                    i++;
                    break;
                case '\\':
                    out[j++] = '\\';
                    i++;
                    break;
                case '/':
                    out[j++] = '/';
                    i++;
                    break;
                case 'b':
                    out[j++] = '\b';
                    i++;
                    break;
                case 'f':
                    out[j++] = '\f';
                    i++;
                    break;
                default:
                    out[j++] = s[i];
                    break;
            }
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}
