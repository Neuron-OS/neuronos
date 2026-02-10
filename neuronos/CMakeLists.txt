##
## NeuronOS — CMakeLists.txt  (v0.7.0)
##
## Builds the full NeuronOS Universal Agent Engine:
##   - HAL (Hardware Abstraction Layer)
##   - Engine (inference wrapper + HW detect + model selector + auto-tune)
##   - Agent (tool registry + ReAct loop)
##   - Interface (HTTP server + MCP server)
##

cmake_minimum_required(VERSION 3.14)
project(neuronos C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ───── Paths ─────
set(NEURONOS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# llama.cpp from the parent BitNet build
set(LLAMA_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/llama.cpp)
set(LLAMA_BUILD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../build/3rdparty/llama.cpp)

# Detect if we're building standalone or as a subdirectory of the outer build.
# When integrated, llama/ggml targets are already available from add_subdirectory().
if(TARGET llama)
    set(NEURONOS_STANDALONE OFF)
else()
    set(NEURONOS_STANDALONE ON)
endif()

# ═════════════════════════════════════════════════════════════
# Layer 1: HAL — Hardware Abstraction Layer
# ═════════════════════════════════════════════════════════════
set(HAL_SOURCES
    src/hal/hal_registry.c
    src/hal/hal_scalar.c
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|x86")
    list(APPEND HAL_SOURCES src/hal/hal_x86_avx2.c)
    list(APPEND HAL_SOURCES src/hal/hal_x86_avxvnni.c)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    list(APPEND HAL_SOURCES src/hal/hal_arm_neon.c)
endif()

add_library(neuronos_hal STATIC ${HAL_SOURCES})
target_include_directories(neuronos_hal
    PUBLIC ${NEURONOS_INCLUDE_DIR}
    PRIVATE ${LLAMA_SRC_DIR}/include
    PRIVATE ${LLAMA_SRC_DIR}/ggml/include
    PRIVATE ${LLAMA_SRC_DIR}/src
)

# Link against llama.cpp libs (needed for SIMD backends that call BitNet kernels)
if(NEURONOS_STANDALONE)
    target_link_directories(neuronos_hal PUBLIC
        ${LLAMA_BUILD_DIR}/src
        ${LLAMA_BUILD_DIR}/ggml/src
    )
endif()
target_link_libraries(neuronos_hal PUBLIC llama ggml)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(neuronos_hal PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|x86")
        set_source_files_properties(${HAL_SOURCES}
            PROPERTIES COMPILE_FLAGS "-mavx2 -mssse3 -mavxvnni")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set_source_files_properties(src/hal/hal_arm_neon.c
            PROPERTIES COMPILE_FLAGS "-march=armv8-a+simd")
    endif()
endif()

# ═════════════════════════════════════════════════════════════
# Layer 2: Engine — Inference wrapper (llama.cpp)
# ═════════════════════════════════════════════════════════════
set(ENGINE_SOURCES
    src/engine/neuronos_engine.c
    src/engine/neuronos_model_selector.c
)

add_library(neuronos_engine STATIC ${ENGINE_SOURCES})
target_include_directories(neuronos_engine
    PUBLIC  ${NEURONOS_INCLUDE_DIR}
    PRIVATE ${LLAMA_SRC_DIR}/include     # llama.h
    PRIVATE ${LLAMA_SRC_DIR}/ggml/include # ggml.h
    PRIVATE ${LLAMA_SRC_DIR}/common       # common.h (if needed)
)

# Link against llama.cpp shared libs from the parent build
if(NEURONOS_STANDALONE)
    target_link_directories(neuronos_engine PUBLIC
        ${LLAMA_BUILD_DIR}/src
        ${LLAMA_BUILD_DIR}/ggml/src
    )
endif()
target_link_libraries(neuronos_engine PUBLIC llama ggml m)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(neuronos_engine PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ═════════════════════════════════════════════════════════════
# Layer 2.5: Memory — SQLite-backed persistent memory
# ═════════════════════════════════════════════════════════════
set(SQLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/sqlite)

set(MEMORY_SOURCES
    ${SQLITE_DIR}/sqlite3.c
    src/memory/neuronos_memory.c
)

add_library(neuronos_memory STATIC ${MEMORY_SOURCES})
target_include_directories(neuronos_memory
    PUBLIC  ${NEURONOS_INCLUDE_DIR}
    PRIVATE ${SQLITE_DIR}
)

# SQLite compile flags: enable FTS5 full-text search, compile as core
target_compile_definitions(neuronos_memory PRIVATE
    SQLITE_CORE
    SQLITE_ENABLE_FTS5
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_SHARED_CACHE
)

# Suppress warnings in SQLite amalgamation (3rd party code)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(${SQLITE_DIR}/sqlite3.c
        PROPERTIES COMPILE_FLAGS "-w")
    set_source_files_properties(src/memory/neuronos_memory.c
        PROPERTIES COMPILE_FLAGS "-Wall -Wextra -Wpedantic -Wno-unused-parameter")
endif()

# Link against pthreads and dl (required by SQLite on Linux)
find_package(Threads REQUIRED)
target_link_libraries(neuronos_memory PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

# ═════════════════════════════════════════════════════════════
# Layer 3: Agent — Tool registry + ReAct loop
# ═════════════════════════════════════════════════════════════
set(AGENT_SOURCES
    src/agent/neuronos_tool_registry.c
    src/agent/neuronos_agent.c
)

add_library(neuronos_agent STATIC ${AGENT_SOURCES})
target_include_directories(neuronos_agent
    PUBLIC ${NEURONOS_INCLUDE_DIR}
)
target_link_libraries(neuronos_agent PUBLIC neuronos_engine neuronos_memory)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(neuronos_agent PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ═════════════════════════════════════════════════════════════
# Layer 4: Interface — HTTP server + MCP server
# ═════════════════════════════════════════════════════════════
set(INTERFACE_SOURCES
    src/interface/neuronos_server.c
    src/mcp/neuronos_mcp_server.c
    src/mcp/neuronos_mcp_client.c
)

add_library(neuronos_interface STATIC ${INTERFACE_SOURCES})
target_include_directories(neuronos_interface
    PUBLIC ${NEURONOS_INCLUDE_DIR}
)
target_link_libraries(neuronos_interface PUBLIC neuronos_engine)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(neuronos_interface PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ═════════════════════════════════════════════════════════════
# Tests
# ═════════════════════════════════════════════════════════════
option(NEURONOS_BUILD_TESTS "Build NeuronOS tests" ON)

if(NEURONOS_BUILD_TESTS)
    # HAL test (existing)
    add_executable(test_hal tests/test_hal.c)
    target_link_libraries(test_hal PRIVATE neuronos_hal m)
    target_include_directories(test_hal PRIVATE ${NEURONOS_INCLUDE_DIR})

    # Engine + Agent test
    add_executable(test_engine tests/test_engine.c)
    target_include_directories(test_engine PRIVATE
        ${NEURONOS_INCLUDE_DIR}
        ${LLAMA_SRC_DIR}/include
        ${LLAMA_SRC_DIR}/ggml/include
    )
    target_link_libraries(test_engine PRIVATE neuronos_agent neuronos_interface neuronos_engine neuronos_hal m)

    # Memory test (no model needed — pure SQLite)
    add_executable(test_memory tests/test_memory.c)
    target_include_directories(test_memory PRIVATE ${NEURONOS_INCLUDE_DIR})
    target_link_libraries(test_memory PRIVATE neuronos_memory m)
endif()

# ═════════════════════════════════════════════════════════════
# neuronos-cli — Universal AI agent CLI
# ═════════════════════════════════════════════════════════════
add_executable(neuronos-cli src/cli/neuronos_cli.c)
target_include_directories(neuronos-cli PRIVATE
    ${NEURONOS_INCLUDE_DIR}
    ${LLAMA_SRC_DIR}/include
    ${LLAMA_SRC_DIR}/ggml/include
)
target_link_libraries(neuronos-cli PRIVATE neuronos_agent neuronos_interface neuronos_engine neuronos_hal m)
