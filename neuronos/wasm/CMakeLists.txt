##
## NeuronOS WASM — CMakeLists.txt
##
## Cross-compiles NeuronOS + llama.cpp (BitNet fork) to WebAssembly.
## Produces: neuronos-worker.js + neuronos-worker.wasm (runs in Web Worker)
##
## This targets the BitNet fork of llama.cpp (pre-refactor, monolithic ggml.c).
## NOT the modern ggml-org/llama.cpp with ggml-cpu/ split.
##
## Usage:
##   cd neuronos/neuronos/wasm && ./build_wasm.sh
##

cmake_minimum_required(VERSION 3.14)
project(neuronos-wasm C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ───── Paths ─────
# wasm/ lives at neuronos/neuronos/wasm/ → NEURONOS_ROOT = neuronos/neuronos/
set(NEURONOS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(NEURONOS_INCLUDE ${NEURONOS_ROOT}/include)
set(NEURONOS_SRC ${NEURONOS_ROOT}/src)

# BitNet root = neuronos/ (parent of neuronos/neuronos/)
set(BITNET_ROOT ${NEURONOS_ROOT}/..)

# llama.cpp fork lives at neuronos/3rdparty/llama.cpp/
set(LLAMA_ROOT ${BITNET_ROOT}/3rdparty/llama.cpp)

# BitNet-specific files at neuronos/src/ and neuronos/include/
set(BITNET_SRC ${BITNET_ROOT}/src)
set(BITNET_INC ${BITNET_ROOT}/include)

# SQLite amalgamation
set(SQLITE_DIR ${NEURONOS_ROOT}/3rdparty/sqlite)

# ───── Option: multi-thread vs single-thread ─────
option(NEURONOS_WASM_THREADS "Build with pthread support (multi-thread)" ON)

# ───── Emscripten link flags ─────
set(WASM_LINK_FLAGS
    -sWASM=1
    -sALLOW_MEMORY_GROWTH=1
    -sMAXIMUM_MEMORY=4GB
    -sSTACK_SIZE=8388608
    -sINITIAL_MEMORY=536870912
    -sABORTING_MALLOC=0
    -sENVIRONMENT=worker
    -sEXPORT_ES6=0
    -sMODULARIZE=1
    -sEXPORT_NAME=NeuronOSModule
    "-sEXPORTED_FUNCTIONS=[\"_neuronos_wasm_init\",\"_neuronos_wasm_load_model_from_buffer\",\"_neuronos_wasm_generate\",\"_neuronos_wasm_agent_chat\",\"_neuronos_wasm_free\",\"_neuronos_wasm_model_info\",\"_neuronos_wasm_memory_init\",\"_neuronos_wasm_memory_store\",\"_neuronos_wasm_memory_search\",\"_neuronos_wasm_free_string\",\"_malloc\",\"_free\"]"
    "-sEXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\",\"UTF8ToString\",\"stringToUTF8\",\"lengthBytesUTF8\",\"getValue\",\"setValue\",\"HEAPU8\",\"wasmMemory\"]"
    -sNO_EXIT_RUNTIME=1
    -sFILESYSTEM=1
    -sFORCE_FILESYSTEM=1
    --no-entry
)

# SIMD (WASM SIMD 128-bit — all modern browsers since 2021)
set(WASM_SIMD_FLAGS -msimd128)

if(NEURONOS_WASM_THREADS)
    list(APPEND WASM_LINK_FLAGS
        -pthread
        -sPTHREAD_POOL_SIZE=4
        -sPTHREAD_POOL_SIZE_STRICT=0
    )
    add_compile_options(-pthread)
    set(WASM_OUTPUT_NAME "neuronos-worker")
else()
    set(WASM_OUTPUT_NAME "neuronos-worker-st")
endif()

# ───── Optimization ─────
add_compile_options(-O3 -DNDEBUG)

# ═════════════════════════════════════════════════════════════
# GGML — Core tensor library (monolithic, BitNet fork)
#
# This fork has ggml.c (not split into ggml-cpu/), plus BitNet
# ternary kernel files ggml-bitnet-mad.cpp and ggml-bitnet-lut.cpp.
# ═════════════════════════════════════════════════════════════
add_library(ggml-wasm STATIC
    # Core C sources
    ${LLAMA_ROOT}/ggml/src/ggml.c
    ${LLAMA_ROOT}/ggml/src/ggml-alloc.c
    ${LLAMA_ROOT}/ggml/src/ggml-quants.c

    # ggml-aarch64.c — despite the name, provides reference implementations of
    # quantize_mat_q8_0, gemv/gemm for q4_0 and i2_s that ggml.c expects.
    # Required on ALL platforms (including WASM).
    ${LLAMA_ROOT}/ggml/src/ggml-aarch64.c

    # Backend (C++)
    ${LLAMA_ROOT}/ggml/src/ggml-backend.cpp

    # llamafile SGEMM (optimized matrix multiply)
    ${LLAMA_ROOT}/ggml/src/llamafile/sgemm.cpp

    # BitNet ternary kernels — compile without TL1/TL2 for WASM
    # (uses pure C MAD fallback, not x86/ARM LUT kernels)
    ${BITNET_SRC}/ggml-bitnet-mad.cpp
    ${BITNET_SRC}/ggml-bitnet-lut.cpp
)

target_include_directories(ggml-wasm PUBLIC
    ${LLAMA_ROOT}/ggml/include
    ${BITNET_INC}
)
target_include_directories(ggml-wasm PRIVATE
    ${LLAMA_ROOT}/ggml/src
    ${LLAMA_ROOT}/ggml/src/llamafile
)

target_compile_options(ggml-wasm PRIVATE ${WASM_SIMD_FLAGS} -w)
target_compile_definitions(ggml-wasm PRIVATE
    _GNU_SOURCE
    # Do NOT define GGML_BITNET_X86_TL2 or GGML_BITNET_ARM_TL1
    # — WASM uses scalar/MAD fallback for ternary ops
)

if(NEURONOS_WASM_THREADS)
    target_compile_definitions(ggml-wasm PRIVATE GGML_USE_PTHREADS)
endif()

# ═════════════════════════════════════════════════════════════
# llama.cpp — LLM inference (C++11)
# ═════════════════════════════════════════════════════════════
add_library(llama-wasm STATIC
    ${LLAMA_ROOT}/src/llama.cpp
    ${LLAMA_ROOT}/src/llama-vocab.cpp
    ${LLAMA_ROOT}/src/llama-grammar.cpp
    ${LLAMA_ROOT}/src/llama-sampling.cpp
    ${LLAMA_ROOT}/src/unicode.cpp
    ${LLAMA_ROOT}/src/unicode-data.cpp
)

target_include_directories(llama-wasm PUBLIC
    ${LLAMA_ROOT}/include
    ${BITNET_INC}
)
target_include_directories(llama-wasm PRIVATE
    ${LLAMA_ROOT}/src
    ${LLAMA_ROOT}/ggml/include
    ${LLAMA_ROOT}/ggml/src
)
target_link_libraries(llama-wasm PUBLIC ggml-wasm)
target_compile_options(llama-wasm PRIVATE ${WASM_SIMD_FLAGS} -w)

# ═════════════════════════════════════════════════════════════
# SQLite 3.47.2 — Persistent memory (FTS5 enabled)
# ═════════════════════════════════════════════════════════════
add_library(sqlite-wasm STATIC ${SQLITE_DIR}/sqlite3.c)
target_include_directories(sqlite-wasm PUBLIC ${SQLITE_DIR})
target_compile_options(sqlite-wasm PRIVATE -w)
target_compile_definitions(sqlite-wasm PRIVATE
    SQLITE_CORE
    SQLITE_ENABLE_FTS5
    SQLITE_THREADSAFE=0
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_SHARED_CACHE
)

# ═════════════════════════════════════════════════════════════
# NeuronOS Core — Agent engine (C11)
# ═════════════════════════════════════════════════════════════
add_library(neuronos-core-wasm STATIC
    # HAL — scalar fallback only (WASM has no AVX2/NEON)
    ${NEURONOS_SRC}/hal/hal_registry.c
    ${NEURONOS_SRC}/hal/hal_scalar.c
    # Engine — wraps llama.cpp
    ${NEURONOS_SRC}/engine/neuronos_engine.c
    ${NEURONOS_SRC}/engine/neuronos_model_selector.c
    ${NEURONOS_SRC}/engine/neuronos_model_registry.c
    # Memory — SQLite + FTS5
    ${NEURONOS_SRC}/memory/neuronos_memory.c
    # Agent — ReAct loop + tools
    ${NEURONOS_SRC}/agent/neuronos_agent.c
    ${NEURONOS_SRC}/agent/neuronos_tool_registry.c
)

target_include_directories(neuronos-core-wasm PUBLIC
    ${NEURONOS_INCLUDE}
)
target_include_directories(neuronos-core-wasm PRIVATE
    ${LLAMA_ROOT}/include
    ${LLAMA_ROOT}/ggml/include
    ${BITNET_INC}
    ${SQLITE_DIR}
)
target_link_libraries(neuronos-core-wasm PUBLIC llama-wasm sqlite-wasm)
target_compile_options(neuronos-core-wasm PRIVATE
    ${WASM_SIMD_FLAGS}
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
)

# ═════════════════════════════════════════════════════════════
# WASM Glue — Exported C API for JavaScript
# ═════════════════════════════════════════════════════════════
add_executable(${WASM_OUTPUT_NAME}
    ${CMAKE_CURRENT_SOURCE_DIR}/neuronos_wasm_glue.c
)
target_include_directories(${WASM_OUTPUT_NAME} PRIVATE
    ${NEURONOS_INCLUDE}
    ${LLAMA_ROOT}/include
    ${LLAMA_ROOT}/ggml/include
    ${SQLITE_DIR}
)
target_link_libraries(${WASM_OUTPUT_NAME} PRIVATE neuronos-core-wasm)
target_compile_options(${WASM_OUTPUT_NAME} PRIVATE
    ${WASM_SIMD_FLAGS}
    -Wall -Wextra -Wno-unused-parameter
)
target_link_options(${WASM_OUTPUT_NAME} PRIVATE
    ${WASM_LINK_FLAGS}
    ${WASM_SIMD_FLAGS}
)

set_target_properties(${WASM_OUTPUT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/dist
    SUFFIX ".js"
)

# ───── Summary ─────
message(STATUS "")
message(STATUS "═══════════════════════════════════════════")
message(STATUS "  NeuronOS WASM Build Configuration")
message(STATUS "═══════════════════════════════════════════")
message(STATUS "  Threads:     ${NEURONOS_WASM_THREADS}")
message(STATUS "  SIMD:        128-bit (WASM SIMD)")
message(STATUS "  Output:      ${WASM_OUTPUT_NAME}.js/.wasm")
message(STATUS "  SQLite:      3.47.2 + FTS5")
message(STATUS "  BitNet:      MAD fallback (no TL1/TL2)")
message(STATUS "  Filesystem:  Emscripten FS")
message(STATUS "═══════════════════════════════════════════")
message(STATUS "")
