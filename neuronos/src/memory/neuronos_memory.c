/* ============================================================
 * NeuronOS — Persistent Memory (MemGPT-inspired)
 *
 * SQLite-backed 3-tier memory system:
 *  1. Core Memory  — agent personality/instructions (in prompt)
 *  2. Recall Memory — full conversation history
 *  3. Archival Memory — long-term facts (FTS5 searchable)
 *
 * Dependencies: SQLite 3 (amalgamation), FTS5 extension
 * No runtime dependencies beyond libc.
 *
 * Copyright (c) 2025 NeuronOS Project
 * SPDX-License-Identifier: MIT
 * ============================================================ */
#include "neuronos/neuronos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>   /* _mkdir */
#include <windows.h>
#define neuronos_mkdir(path) _mkdir(path)
#else
#include <unistd.h>
#define neuronos_mkdir(path) mkdir(path, 0755)
#endif

/* SQLite amalgamation (compiled with -DSQLITE_CORE -DSQLITE_ENABLE_FTS5) */
#include "sqlite3.h"

/* ---- Internal struct ---- */
struct neuronos_memory {
    sqlite3 * db;
    int64_t current_session_id;
};

/* ---- Forward declarations ---- */
static int  memory_create_schema(sqlite3 * db);
static char * memory_resolve_path(const char * db_path);

/* ============================================================
 * OPEN / CLOSE
 * ============================================================ */

neuronos_memory_t * neuronos_memory_open(const char * db_path) {
    neuronos_memory_t * mem = calloc(1, sizeof(neuronos_memory_t));
    if (!mem) return NULL;

    char * resolved = memory_resolve_path(db_path);
    if (!resolved) {
        free(mem);
        return NULL;
    }

    int rc = sqlite3_open(resolved, &mem->db);
    free(resolved);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "[neuronos-memory] Failed to open DB: %s\n",
                sqlite3_errmsg(mem->db));
        sqlite3_close(mem->db);
        free(mem);
        return NULL;
    }

    /* Performance pragmas for edge devices */
    sqlite3_exec(mem->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(mem->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(mem->db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    /* Create schema if needed */
    if (memory_create_schema(mem->db) != 0) {
        fprintf(stderr, "[neuronos-memory] Failed to create schema\n");
        sqlite3_close(mem->db);
        free(mem);
        return NULL;
    }

    /* Auto-create session 1 if none exists */
    mem->current_session_id = 1;

    return mem;
}

void neuronos_memory_close(neuronos_memory_t * mem) {
    if (!mem) return;
    if (mem->db) {
        sqlite3_close(mem->db);
    }
    free(mem);
}

/* ============================================================
 * SCHEMA
 * ============================================================ */

static int memory_create_schema(sqlite3 * db) {
    const char * sql =
        /* Core memory blocks (persona, human, instructions) */
        "CREATE TABLE IF NOT EXISTS core_blocks ("
        "  label TEXT PRIMARY KEY,"
        "  content TEXT NOT NULL DEFAULT '',"
        "  max_chars INTEGER NOT NULL DEFAULT 2000,"
        "  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
        ");\n"

        /* Recall memory (conversation log) */
        "CREATE TABLE IF NOT EXISTS recall_memory ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL DEFAULT 1,"
        "  role TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  token_count INTEGER NOT NULL DEFAULT 0,"
        "  timestamp INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "  summary_of INTEGER DEFAULT 0"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_recall_session "
        "  ON recall_memory(session_id, timestamp);\n"

        /* Recall memory FTS5 index */
        "CREATE VIRTUAL TABLE IF NOT EXISTS recall_fts USING fts5("
        "  content, content=recall_memory, content_rowid=id"
        ");\n"

        /* FTS triggers for recall */
        "CREATE TRIGGER IF NOT EXISTS recall_ai AFTER INSERT ON recall_memory BEGIN "
        "  INSERT INTO recall_fts(rowid, content) VALUES (new.id, new.content); "
        "END;\n"
        "CREATE TRIGGER IF NOT EXISTS recall_ad AFTER DELETE ON recall_memory BEGIN "
        "  INSERT INTO recall_fts(recall_fts, rowid, content) VALUES('delete', old.id, old.content); "
        "END;\n"
        "CREATE TRIGGER IF NOT EXISTS recall_au AFTER UPDATE ON recall_memory BEGIN "
        "  INSERT INTO recall_fts(recall_fts, rowid, content) VALUES('delete', old.id, old.content); "
        "  INSERT INTO recall_fts(rowid, content) VALUES (new.id, new.content); "
        "END;\n"

        /* Archival memory (long-term facts) */
        "CREATE TABLE IF NOT EXISTS archival_memory ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  key TEXT NOT NULL,"
        "  value TEXT NOT NULL,"
        "  category TEXT DEFAULT 'general',"
        "  importance REAL NOT NULL DEFAULT 0.5,"
        "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "  updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "  access_count INTEGER NOT NULL DEFAULT 0"
        ");\n"
        "CREATE INDEX IF NOT EXISTS idx_archival_key "
        "  ON archival_memory(key);\n"

        /* Archival FTS5 index */
        "CREATE VIRTUAL TABLE IF NOT EXISTS archival_fts USING fts5("
        "  key, value, content=archival_memory, content_rowid=id"
        ");\n"

        /* FTS triggers for archival */
        "CREATE TRIGGER IF NOT EXISTS archival_ai AFTER INSERT ON archival_memory BEGIN "
        "  INSERT INTO archival_fts(rowid, key, value) VALUES (new.id, new.key, new.value); "
        "END;\n"
        "CREATE TRIGGER IF NOT EXISTS archival_ad AFTER DELETE ON archival_memory BEGIN "
        "  INSERT INTO archival_fts(archival_fts, rowid, key, value) "
        "    VALUES('delete', old.id, old.key, old.value); "
        "END;\n"
        "CREATE TRIGGER IF NOT EXISTS archival_au AFTER UPDATE ON archival_memory BEGIN "
        "  INSERT INTO archival_fts(archival_fts, rowid, key, value) "
        "    VALUES('delete', old.id, old.key, old.value); "
        "  INSERT INTO archival_fts(rowid, key, value) VALUES (new.id, new.key, new.value); "
        "END;\n"

        /* Sessions */
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "  title TEXT DEFAULT ''"
        ");\n"

        /* Insert default session if empty */
        "INSERT OR IGNORE INTO sessions(id, title) VALUES(1, 'default');\n"

        /* Insert default core blocks */
        "INSERT OR IGNORE INTO core_blocks(label, content) VALUES "
        "  ('persona', 'You are a helpful AI assistant running on NeuronOS, a local AI agent engine.');\n"
        "INSERT OR IGNORE INTO core_blocks(label, content) VALUES "
        "  ('human', '');\n"
        "INSERT OR IGNORE INTO core_blocks(label, content) VALUES "
        "  ('instructions', 'Respond concisely and accurately. Use tools when needed.');\n";

    char * err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[neuronos-memory] Schema error: %s\n", err_msg ? err_msg : "unknown");
        sqlite3_free(err_msg);
        return -1;
    }
    return 0;
}

/* ============================================================
 * PATH RESOLUTION
 * ============================================================ */

static char * memory_resolve_path(const char * db_path) {
    /* ":memory:" pass-through for in-memory DB */
    if (db_path && strcmp(db_path, ":memory:") == 0) {
        return strdup(":memory:");
    }

    /* Explicit path */
    if (db_path && db_path[0] != '\0') {
        return strdup(db_path);
    }

    /* Default: ~/.neuronos/mem.db */
    const char * home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) home = "/tmp";

    size_t len = strlen(home) + 32;
    char * path = malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s/.neuronos", home);

    /* Create directory if needed */
    neuronos_mkdir(path);

    snprintf(path, len, "%s/.neuronos/mem.db", home);
    return path;
}

/* ============================================================
 * CORE MEMORY
 * ============================================================ */

int neuronos_memory_core_set(neuronos_memory_t * mem, const char * label, const char * content) {
    if (!mem || !mem->db || !label || !content) return -1;

    const char * sql =
        "INSERT INTO core_blocks(label, content, updated_at) VALUES(?1, ?2, strftime('%s','now')) "
        "ON CONFLICT(label) DO UPDATE SET content=?2, updated_at=strftime('%s','now');";

    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

char * neuronos_memory_core_get(neuronos_memory_t * mem, const char * label) {
    if (!mem || !mem->db || !label) return NULL;

    const char * sql = "SELECT content FROM core_blocks WHERE label = ?1;";
    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;

    sqlite3_bind_text(stmt, 1, label, -1, SQLITE_TRANSIENT);

    char * result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char * text = (const char *)sqlite3_column_text(stmt, 0);
        if (text) result = strdup(text);
    }
    sqlite3_finalize(stmt);
    return result;
}

int neuronos_memory_core_append(neuronos_memory_t * mem, const char * label, const char * text) {
    if (!mem || !mem->db || !label || !text) return -1;

    char * existing = neuronos_memory_core_get(mem, label);
    if (!existing) {
        /* Block doesn't exist — create it */
        return neuronos_memory_core_set(mem, label, text);
    }

    size_t new_len = strlen(existing) + strlen(text) + 2; /* +newline+null */
    char * combined = malloc(new_len);
    if (!combined) {
        free(existing);
        return -1;
    }
    snprintf(combined, new_len, "%s\n%s", existing, text);
    free(existing);

    int rc = neuronos_memory_core_set(mem, label, combined);
    free(combined);
    return rc;
}

char * neuronos_memory_core_dump(neuronos_memory_t * mem) {
    if (!mem || !mem->db) return NULL;

    const char * sql = "SELECT label, content FROM core_blocks ORDER BY label;";
    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;

    /* Build formatted string */
    size_t cap = 4096;
    size_t len = 0;
    char * buf = malloc(cap);
    if (!buf) { sqlite3_finalize(stmt); return NULL; }
    buf[0] = '\0';

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char * label   = (const char *)sqlite3_column_text(stmt, 0);
        const char * content = (const char *)sqlite3_column_text(stmt, 1);
        if (!label || !content) continue;

        size_t need = strlen(label) + strlen(content) + 16;
        while (len + need > cap) {
            cap *= 2;
            void * tmp = realloc(buf, cap);
            if (!tmp) { free(buf); sqlite3_finalize(stmt); return NULL; }
            buf = tmp;
        }
        len += (size_t)snprintf(buf + len, cap - len, "<%s>:\n%s\n---\n", label, content);
    }
    sqlite3_finalize(stmt);
    return buf;
}

/* ============================================================
 * RECALL MEMORY
 * ============================================================ */

int64_t neuronos_memory_recall_add(neuronos_memory_t * mem, int64_t session_id,
                                   const char * role, const char * content, int token_count) {
    if (!mem || !mem->db || !role || !content) return -1;

    const char * sql =
        "INSERT INTO recall_memory(session_id, role, content, token_count) "
        "VALUES(?1, ?2, ?3, ?4);";

    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, token_count);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        return sqlite3_last_insert_rowid(mem->db);
    }
    return -1;
}

int neuronos_memory_recall_recent(neuronos_memory_t * mem, int64_t session_id,
                                  int limit, neuronos_recall_entry_t ** out_entries, int * out_count) {
    if (!mem || !mem->db || !out_entries || !out_count) return -1;
    *out_entries = NULL;
    *out_count = 0;

    const char * sql =
        "SELECT id, role, content, timestamp, token_count, session_id, summary_of "
        "FROM recall_memory WHERE session_id=?1 "
        "ORDER BY timestamp DESC LIMIT ?2;";

    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int(stmt, 2, limit > 0 ? limit : 100);

    /* First pass: count rows */
    int count = 0;
    int cap = 32;
    neuronos_recall_entry_t * entries = calloc((size_t)cap, sizeof(neuronos_recall_entry_t));
    if (!entries) { sqlite3_finalize(stmt); *out_entries = NULL; *out_count = 0; return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            void * tmp = realloc(entries, (size_t)cap * sizeof(neuronos_recall_entry_t));
            if (!tmp) { neuronos_memory_recall_free(entries, count); sqlite3_finalize(stmt); *out_entries = NULL; *out_count = 0; return -1; }
            entries = tmp;
        }
        neuronos_recall_entry_t * e = &entries[count];
        e->id         = sqlite3_column_int64(stmt, 0);
        e->role       = strdup((const char *)sqlite3_column_text(stmt, 1));
        e->content    = strdup((const char *)sqlite3_column_text(stmt, 2));
        e->timestamp  = sqlite3_column_int64(stmt, 3);
        e->token_count = sqlite3_column_int(stmt, 4);
        e->session_id = sqlite3_column_int64(stmt, 5);
        e->summary_of = sqlite3_column_int64(stmt, 6);
        count++;
    }
    sqlite3_finalize(stmt);

    *out_entries = entries;
    *out_count = count;
    return 0;
}

int neuronos_memory_recall_search(neuronos_memory_t * mem, const char * query,
                                  int max_results, neuronos_recall_entry_t ** out_entries, int * out_count) {
    if (!mem || !mem->db || !query || !out_entries || !out_count) return -1;
    *out_entries = NULL;
    *out_count = 0;

    const char * sql =
        "SELECT r.id, r.role, r.content, r.timestamp, r.token_count, r.session_id, r.summary_of "
        "FROM recall_fts f "
        "JOIN recall_memory r ON f.rowid = r.id "
        "WHERE recall_fts MATCH ?1 "
        "ORDER BY rank LIMIT ?2;";

    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, max_results > 0 ? max_results : 10);

    int count = 0;
    int cap = 16;
    neuronos_recall_entry_t * entries = calloc((size_t)cap, sizeof(neuronos_recall_entry_t));
    if (!entries) { sqlite3_finalize(stmt); *out_entries = NULL; *out_count = 0; return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            void * tmp = realloc(entries, (size_t)cap * sizeof(neuronos_recall_entry_t));
            if (!tmp) { neuronos_memory_recall_free(entries, count); sqlite3_finalize(stmt); *out_entries = NULL; *out_count = 0; return -1; }
            entries = tmp;
        }
        neuronos_recall_entry_t * e = &entries[count];
        e->id         = sqlite3_column_int64(stmt, 0);
        e->role       = strdup((const char *)sqlite3_column_text(stmt, 1));
        e->content    = strdup((const char *)sqlite3_column_text(stmt, 2));
        e->timestamp  = sqlite3_column_int64(stmt, 3);
        e->token_count = sqlite3_column_int(stmt, 4);
        e->session_id = sqlite3_column_int64(stmt, 5);
        e->summary_of = sqlite3_column_int64(stmt, 6);
        count++;
    }
    sqlite3_finalize(stmt);

    *out_entries = entries;
    *out_count = count;
    return 0;
}

void neuronos_memory_recall_free(neuronos_recall_entry_t * entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free((void *)entries[i].role);
        free((void *)entries[i].content);
    }
    free(entries);
}

int neuronos_memory_recall_stats(neuronos_memory_t * mem, int64_t session_id,
                                 int * out_msg_count, int * out_token_count) {
    if (!mem || !mem->db) return -1;

    const char * sql =
        "SELECT COUNT(*), COALESCE(SUM(token_count), 0) "
        "FROM recall_memory WHERE session_id=?1;";

    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, session_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (out_msg_count)   *out_msg_count   = sqlite3_column_int(stmt, 0);
        if (out_token_count) *out_token_count  = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return 0;
}

int neuronos_memory_recall_gc(neuronos_memory_t * mem, int64_t session_id,
                              int max_messages, int max_age_seconds) {
    if (!mem || !mem->db) return -1;
    int deleted = 0;

    /* 1. Delete messages older than max_age_seconds (if > 0) */
    if (max_age_seconds > 0) {
        const char * age_sql =
            "DELETE FROM recall_memory "
            "WHERE session_id=?1 AND timestamp < (strftime('%s','now') - ?2);";
        sqlite3_stmt * stmt = NULL;
        int rc = sqlite3_prepare_v2(mem->db, age_sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, session_id);
            sqlite3_bind_int(stmt, 2, max_age_seconds);
            sqlite3_step(stmt);
            deleted += sqlite3_changes(mem->db);
            sqlite3_finalize(stmt);
        }
    }

    /* 2. Keep only the newest max_messages per session (if > 0) */
    if (max_messages > 0) {
        const char * trim_sql =
            "DELETE FROM recall_memory "
            "WHERE session_id=?1 AND id NOT IN ("
            "  SELECT id FROM recall_memory WHERE session_id=?1 "
            "  ORDER BY timestamp DESC LIMIT ?2"
            ");";
        sqlite3_stmt * stmt = NULL;
        int rc = sqlite3_prepare_v2(mem->db, trim_sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, session_id);
            sqlite3_bind_int(stmt, 2, max_messages);
            sqlite3_step(stmt);
            deleted += sqlite3_changes(mem->db);
            sqlite3_finalize(stmt);
        }
    }

    return deleted;
}

/* ============================================================
 * ARCHIVAL MEMORY
 * ============================================================ */

int64_t neuronos_memory_archival_store(neuronos_memory_t * mem, const char * key,
                                       const char * value, const char * category, float importance) {
    if (!mem || !mem->db || !key || !value) return -1;

    /* Check for existing key — update if exists */
    const char * check_sql = "SELECT id FROM archival_memory WHERE key=?1 LIMIT 1;";
    sqlite3_stmt * check_stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, check_sql, -1, &check_stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(check_stmt, 1, key, -1, SQLITE_TRANSIENT);

    int64_t existing_id = -1;
    if (sqlite3_step(check_stmt) == SQLITE_ROW) {
        existing_id = sqlite3_column_int64(check_stmt, 0);
    }
    sqlite3_finalize(check_stmt);

    if (existing_id >= 0) {
        /* Update existing */
        const char * update_sql =
            "UPDATE archival_memory SET value=?1, category=?2, importance=?3, "
            "updated_at=strftime('%s','now') WHERE id=?4;";
        sqlite3_stmt * stmt = NULL;
        rc = sqlite3_prepare_v2(mem->db, update_sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) return -1;
        sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, category ? category : "general", -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, (double)importance);
        sqlite3_bind_int64(stmt, 4, existing_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE) ? existing_id : -1;
    }

    /* Insert new */
    const char * insert_sql =
        "INSERT INTO archival_memory(key, value, category, importance) "
        "VALUES(?1, ?2, ?3, ?4);";
    sqlite3_stmt * stmt = NULL;
    rc = sqlite3_prepare_v2(mem->db, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, category ? category : "general", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, (double)importance);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        return sqlite3_last_insert_rowid(mem->db);
    }
    return -1;
}

char * neuronos_memory_archival_recall(neuronos_memory_t * mem, const char * key) {
    if (!mem || !mem->db || !key) return NULL;

    const char * sql =
        "UPDATE archival_memory SET access_count = access_count + 1 WHERE key=?1;"
        ;
    sqlite3_stmt * upd = NULL;
    if (sqlite3_prepare_v2(mem->db, sql, -1, &upd, NULL) == SQLITE_OK) {
        sqlite3_bind_text(upd, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_step(upd);
        sqlite3_finalize(upd);
    }

    const char * sel_sql = "SELECT value FROM archival_memory WHERE key=?1 LIMIT 1;";
    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sel_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    char * result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char * val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) result = strdup(val);
    }
    sqlite3_finalize(stmt);
    return result;
}

int neuronos_memory_archival_search(neuronos_memory_t * mem, const char * query,
                                    int max_results, neuronos_archival_entry_t ** out_entries, int * out_count) {
    if (!mem || !mem->db || !query || !out_entries || !out_count) return -1;
    *out_entries = NULL;
    *out_count = 0;

    const char * sql =
        "SELECT a.id, a.key, a.value, a.category, a.importance, "
        "       a.created_at, a.updated_at, a.access_count "
        "FROM archival_fts f "
        "JOIN archival_memory a ON f.rowid = a.id "
        "WHERE archival_fts MATCH ?1 "
        "ORDER BY rank LIMIT ?2;";

    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, max_results > 0 ? max_results : 10);

    int count = 0;
    int cap = 16;
    neuronos_archival_entry_t * entries = calloc((size_t)cap, sizeof(neuronos_archival_entry_t));
    if (!entries) { sqlite3_finalize(stmt); *out_entries = NULL; *out_count = 0; return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            void * tmp = realloc(entries, (size_t)cap * sizeof(neuronos_archival_entry_t));
            if (!tmp) { neuronos_memory_archival_free(entries, count); sqlite3_finalize(stmt); *out_entries = NULL; *out_count = 0; return -1; }
            entries = tmp;
        }
        neuronos_archival_entry_t * e = &entries[count];
        e->id           = sqlite3_column_int64(stmt, 0);
        e->key          = strdup((const char *)sqlite3_column_text(stmt, 1));
        e->value        = strdup((const char *)sqlite3_column_text(stmt, 2));
        const char * cat = (const char *)sqlite3_column_text(stmt, 3);
        e->category     = cat ? strdup(cat) : strdup("general");
        e->importance   = (float)sqlite3_column_double(stmt, 4);
        e->created_at   = sqlite3_column_int64(stmt, 5);
        e->updated_at   = sqlite3_column_int64(stmt, 6);
        e->access_count = sqlite3_column_int(stmt, 7);
        count++;
    }
    sqlite3_finalize(stmt);

    *out_entries = entries;
    *out_count = count;
    return 0;
}

void neuronos_memory_archival_free(neuronos_archival_entry_t * entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free((void *)entries[i].key);
        free((void *)entries[i].value);
        free((void *)entries[i].category);
    }
    free(entries);
}

int neuronos_memory_archival_stats(neuronos_memory_t * mem, int * out_fact_count) {
    if (!mem || !mem->db) return -1;

    const char * sql = "SELECT COUNT(*) FROM archival_memory;";
    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (out_fact_count) *out_fact_count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return 0;
}

/* ============================================================
 * SESSION MANAGEMENT
 * ============================================================ */

int64_t neuronos_memory_session_create(neuronos_memory_t * mem) {
    if (!mem || !mem->db) return -1;

    const char * sql = "INSERT INTO sessions(title) VALUES('');";
    sqlite3_stmt * stmt = NULL;
    int rc = sqlite3_prepare_v2(mem->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        int64_t sid = sqlite3_last_insert_rowid(mem->db);
        mem->current_session_id = sid;
        return sid;
    }
    return -1;
}

/* ============================================================
 * LEGACY COMPATIBILITY (simple key-value facade)
 * ============================================================ */

int neuronos_memory_store(neuronos_memory_t * mem, const char * key, const char * value) {
    int64_t id = neuronos_memory_archival_store(mem, key, value, "general", 0.5f);
    return (id >= 0) ? 0 : -1;
}

char * neuronos_memory_recall(neuronos_memory_t * mem, const char * key) {
    return neuronos_memory_archival_recall(mem, key);
}

int neuronos_memory_search(neuronos_memory_t * mem, const char * query, char *** results,
                           int * n_results, int max_results) {
    if (!mem || !query || !results || !n_results) return -1;
    *results = NULL;
    *n_results = 0;

    neuronos_archival_entry_t * entries = NULL;
    int count = 0;
    int rc = neuronos_memory_archival_search(mem, query, max_results, &entries, &count);
    if (rc != 0) return -1;

    if (count == 0) {
        neuronos_memory_archival_free(entries, count);
        return 0;
    }

    /* Convert to string array: "key: value" */
    char ** strs = calloc((size_t)count, sizeof(char *));
    if (!strs) {
        neuronos_memory_archival_free(entries, count);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        size_t len = strlen(entries[i].key) + strlen(entries[i].value) + 4;
        strs[i] = malloc(len);
        if (!strs[i]) {
            for (int j = 0; j < i; j++) free(strs[j]);
            free(strs);
            neuronos_memory_archival_free(entries, count);
            return -1;
        }
        snprintf(strs[i], len, "%s: %s", entries[i].key, entries[i].value);
    }
    neuronos_memory_archival_free(entries, count);

    *results = strs;
    *n_results = count;
    return 0;
}

void neuronos_memory_free_results(char ** results, int n) {
    if (!results) return;
    for (int i = 0; i < n; i++) {
        free(results[i]);
    }
    free(results);
}
