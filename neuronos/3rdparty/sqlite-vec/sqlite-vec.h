/*
 * sqlite-vec v0.1.6 - Header file
 * https://github.com/asg017/sqlite-vec
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef SQLITE_VEC_H
#define SQLITE_VEC_H

#include "sqlite3.h"

#define SQLITE_VEC_VERSION "v0.1.6"
#define SQLITE_VEC_VERSION_MAJOR 0
#define SQLITE_VEC_VERSION_MINOR 1
#define SQLITE_VEC_VERSION_PATCH 6
#define SQLITE_VEC_DATE "2024-11-20"
#define SQLITE_VEC_SOURCE "639fca5739fe056fdc98f3d539c4cd79328d7dc7"

#ifdef SQLITE_VEC_STATIC
#define SQLITE_VEC_API
#else
#ifdef _WIN32
#define SQLITE_VEC_API __declspec(dllexport)
#else
#define SQLITE_VEC_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

SQLITE_VEC_API int sqlite3_vec_init(sqlite3 *db, char **pzErrMsg,
                                    const sqlite3_api_routines *pApi);

SQLITE_VEC_API int sqlite3_vec_numpy_init(sqlite3 *db, char **pzErrMsg,
                                          const sqlite3_api_routines *pApi);

SQLITE_VEC_API int sqlite3_vec_static_blobs_init(sqlite3 *db, char **pzErrMsg,
                                                 const sqlite3_api_routines *pApi);

#ifdef __cplusplus
}
#endif

#endif /* SQLITE_VEC_H */
