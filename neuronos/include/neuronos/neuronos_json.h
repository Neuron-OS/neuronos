/**
 * @file neuronos_json.h
 * @brief NeuronOS — Minimal JSON parser (single-pass, correct string escaping)
 *
 * Replaces the 4 duplicated "minimal JSON helper" implementations that used
 * raw strstr() which could match keys inside string values.
 *
 * This parser is deliberately simple: it only handles well-formed JSON from
 * trusted sources (MCP servers, HTTP clients, config files). It does NOT
 * validate full JSON spec compliance — it extracts values by key correctly.
 *
 * Features:
 *   - Single-pass scanning with proper string skip (handles escaped quotes)
 *   - Distinguishes keys from values (only matches top-level or nested keys)
 *   - Extracts: strings, ints, objects, arrays
 *   - JSON string escaping for output
 *   - Zero dependencies (pure C11, no malloc for read-only ops)
 *
 * Copyright (c) 2025 NeuronOS Project
 * SPDX-License-Identifier: MIT
 */

#ifndef NEURONOS_JSON_H
#define NEURONOS_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────
 * READ — Extract values by key (no allocation for string/int)
 * ────────────────────────────────────────────────────────────── */

/**
 * Find a string value by key in JSON.
 *
 * Returns pointer to the first character INSIDE the quotes, and sets
 * *out_len to the string length (excluding quotes). Handles escaped
 * characters within the string.
 *
 * Returns NULL if key not found or value is not a string.
 *
 * @param json  NUL-terminated JSON text
 * @param key   Key name (without quotes)
 * @param out_len  Output: length of the string value (can be NULL)
 * @return Pointer into json at the start of the string value, or NULL
 */
const char * nj_find_str(const char * json, const char * key, int * out_len);

/**
 * Find an integer value by key in JSON.
 *
 * Returns the integer value, or `fallback` if not found.
 *
 * @param json     NUL-terminated JSON text
 * @param key      Key name (without quotes)
 * @param fallback Value to return if key not found
 * @return The integer value or fallback
 */
int nj_find_int(const char * json, const char * key, int fallback);

/**
 * Extract a JSON object value as a new NUL-terminated string.
 * Caller must free() the returned pointer.
 *
 * Returns NULL if key not found or value is not an object.
 *
 * @param json  NUL-terminated JSON text
 * @param key   Key name (without quotes)
 * @return Newly allocated string containing the object, or NULL
 */
char * nj_extract_object(const char * json, const char * key);

/**
 * Extract a JSON array value as a new NUL-terminated string.
 * Caller must free() the returned pointer.
 *
 * Returns NULL if key not found or value is not an array.
 *
 * @param json  NUL-terminated JSON text
 * @param key   Key name (without quotes)
 * @return Newly allocated string containing the array, or NULL
 */
char * nj_extract_array(const char * json, const char * key);

/**
 * Copy a string value by key into a caller-provided buffer.
 * Useful when you don't want to allocate.
 *
 * @param json    NUL-terminated JSON text
 * @param key     Key name
 * @param buf     Output buffer
 * @param bufsize Size of output buffer
 * @return Number of chars written (excluding NUL), or -1 if not found
 */
int nj_copy_str(const char * json, const char * key, char * buf, size_t bufsize);

/**
 * Find a boolean value by key in JSON.
 *
 * Returns true/false if found, or `fallback` if not found.
 */
int nj_find_bool(const char * json, const char * key, int fallback);

/**
 * Find a floating-point value by key in JSON.
 *
 * Returns the float value, or `fallback` if not found.
 */
float nj_find_float(const char * json, const char * key, float fallback);

/**
 * Extract a string value by key as a newly allocated copy.
 * Caller must free() the returned pointer.
 *
 * @return Newly allocated string or NULL if not found
 */
char * nj_alloc_str(const char * json, const char * key);

/* ──────────────────────────────────────────────────────────────
 * WRITE — Escape strings for JSON output
 * ────────────────────────────────────────────────────────────── */

/**
 * Escape a C string for safe embedding in JSON.
 * Handles: \\ \" \n \r \t and control characters (as \\uXXXX).
 *
 * Caller must free() the returned pointer.
 *
 * @param s  Input string (NULL returns strdup("null"))
 * @return Newly allocated escaped string, or NULL on OOM
 */
char * nj_escape(const char * s);

/**
 * Escape up to `max_len` characters of a string for JSON.
 * Useful when you want to truncate before escaping.
 *
 * Caller must free() the returned pointer.
 */
char * nj_escape_n(const char * s, size_t max_len);

/**
 * Unescape a JSON string (\\n → newline, \\t → tab, etc.).
 * Caller must free() the returned pointer.
 */
char * nj_unescape(const char * s);

/* ──────────────────────────────────────────────────────────────
 * SCAN — Low-level helpers for iterating JSON structures
 * ────────────────────────────────────────────────────────────── */

/**
 * Skip whitespace characters.
 * @return Pointer to next non-whitespace char
 */
const char * nj_skip_ws(const char * p);

/**
 * Skip a complete JSON value (string, number, object, array, bool, null).
 * @return Pointer past the end of the value, or NULL on malformed input
 */
const char * nj_skip_value(const char * p);

#ifdef __cplusplus
}
#endif

#endif /* NEURONOS_JSON_H */
