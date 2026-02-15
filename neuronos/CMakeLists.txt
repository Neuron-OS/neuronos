##
## NeuronOS — CMakeLists.txt  (v0.8.1)
##
## Builds the full NeuronOS Universal Agent Engine:
##   - HAL (Hardware Abstraction Layer)
##   - Engine (inference wrapper + HW detect + model selector + auto-tune)
##   - Memory (SQLite+FTS5 persistent memory)
##   - Agent (tool registry + ReAct loop)
##   - Interface (HTTP server + MCP server)
##
## Supports: Linux (x86_64, ARM64), macOS (ARM64), Windows (x64), Android (ARM64)
##

cmake_minimum_required(VERSION 3.14)
project(neuronos C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ───── Cross-platform helpers ─────
# libm is only needed on Unix (not Windows, even with Clang)
if(UNIX)
    set(NEURONOS_LIBM m)
else()
    set(NEURONOS_LIBM "")
endif()

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
    src/hal/hal_vulkan.c  # Always included (has stubs when SDK not found)
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

# Compiler-specific warning and SIMD flags
if(MSVC)
    target_compile_options(neuronos_hal PRIVATE /W3)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|x86|x64")
        target_compile_options(neuronos_hal PRIVATE /arch:AVX2)
    endif()
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
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
    src/engine/neuronos_model_registry.c
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
target_link_libraries(neuronos_engine PUBLIC llama ggml ${NEURONOS_LIBM})

if(MSVC)
    target_compile_options(neuronos_engine PRIVATE /W3)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
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
if(MSVC)
    set_source_files_properties(${SQLITE_DIR}/sqlite3.c
        PROPERTIES COMPILE_FLAGS "/w")
    target_compile_options(neuronos_memory PRIVATE /W3)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(${SQLITE_DIR}/sqlite3.c
        PROPERTIES COMPILE_FLAGS "-w")
    set_source_files_properties(src/memory/neuronos_memory.c
        PROPERTIES COMPILE_FLAGS "-Wall -Wextra -Wpedantic -Wno-unused-parameter")
endif()

# Link against pthreads and dl (required by SQLite on Linux/macOS)
find_package(Threads REQUIRED)
target_link_libraries(neuronos_memory PUBLIC Threads::Threads ${CMAKE_DL_LIBS} ${NEURONOS_LIBM})

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

if(MSVC)
    target_compile_options(neuronos_agent PRIVATE /W3)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(neuronos_agent PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ═════════════════════════════════════════════════════════════
# GPU Backend Support (CUDA + Vulkan)
# ═════════════════════════════════════════════════════════════

# CUDA Support (NVIDIA GPUs)
option(NEURONOS_CUDA "Enable CUDA GPU support" ON)

if(NEURONOS_CUDA)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        message(STATUS "NeuronOS: CUDA Toolkit ${CUDAToolkit_VERSION} found at ${CUDAToolkit_LIBRARY_ROOT}")
        message(STATUS "NeuronOS: CUDA backend ENABLED")

        # Enable CUDA in llama.cpp (ggml backend)
        set(GGML_CUDA ON CACHE BOOL "Enable CUDA backend in ggml" FORCE)

        # Define compile-time flag
        add_compile_definitions(NEURONOS_HAS_CUDA=1)

        # Link against CUDA runtime
        target_link_libraries(neuronos_hal PUBLIC CUDA::cudart)
        target_link_libraries(neuronos_engine PUBLIC CUDA::cudart)
    else()
        message(STATUS "NeuronOS: CUDA Toolkit not found. CUDA support disabled.")
        message(STATUS "  Install: https://developer.nvidia.com/cuda-downloads")
        set(NEURONOS_CUDA OFF)
    endif()
endif()

# Vulkan Support (Universal: AMD/Intel/NVIDIA)
option(NEURONOS_VULKAN "Enable Vulkan GPU support" ON)

if(NEURONOS_VULKAN)
    find_package(Vulkan QUIET)

    if(Vulkan_FOUND)
        message(STATUS "NeuronOS: Vulkan SDK found at ${Vulkan_LIBRARY}")
        message(STATUS "NeuronOS: Vulkan backend ENABLED")

        # Enable Vulkan in llama.cpp (ggml backend)
        set(GGML_VULKAN ON CACHE BOOL "Enable Vulkan backend in ggml" FORCE)

        # Define compile-time flag for conditional compilation
        add_compile_definitions(NEURONOS_HAS_VULKAN=1)

        # Link HAL against Vulkan
        target_link_libraries(neuronos_hal PUBLIC Vulkan::Vulkan)
    else()
        message(STATUS "NeuronOS: Vulkan SDK not found. Vulkan support disabled.")
        message(STATUS "  Install: https://vulkan.lunarg.com/sdk/home")
        set(NEURONOS_VULKAN OFF)
    endif()
endif()

# ═════════════════════════════════════════════════════════════
# Layer 4: Interface — HTTP server + MCP server
# ═════════════════════════════════════════════════════════════

# ── WebUI embedding: regenerate C header from HTML if Python3 available ──
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/webui/index.html")
    set(WEBUI_INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/webui/index.html")
    set(WEBUI_OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/src/interface/neuronos_chat_ui.h")
    set(WEBUI_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_webui.py")
    add_custom_command(
        OUTPUT  ${WEBUI_OUTPUT}
        COMMAND ${Python3_EXECUTABLE} ${WEBUI_SCRIPT} --gzip ${WEBUI_INPUT} ${WEBUI_OUTPUT}
        DEPENDS ${WEBUI_INPUT} ${WEBUI_SCRIPT}
        COMMENT "Embedding WebUI: index.html → neuronos_chat_ui.h (gzipped)"
    )
    add_custom_target(neuronos_webui DEPENDS ${WEBUI_OUTPUT})
    message(STATUS "NeuronOS: WebUI auto-embed enabled (Python3 found)")
else()
    message(STATUS "NeuronOS: Using pre-generated neuronos_chat_ui.h (Python3 not found or webui/index.html missing)")
endif()

set(INTERFACE_SOURCES
    src/interface/neuronos_server.c
    src/mcp/neuronos_mcp_server.c
    src/mcp/neuronos_mcp_client.c
)

add_library(neuronos_interface STATIC ${INTERFACE_SOURCES})
target_include_directories(neuronos_interface
    PUBLIC ${NEURONOS_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/interface
            ${CMAKE_CURRENT_SOURCE_DIR}/src/mcp
)
target_link_libraries(neuronos_interface PUBLIC neuronos_engine)
if(TARGET neuronos_webui)
    add_dependencies(neuronos_interface neuronos_webui)
endif()

if(MSVC)
    target_compile_options(neuronos_interface PRIVATE /W3)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
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
    target_link_libraries(test_hal PRIVATE neuronos_hal ${NEURONOS_LIBM})
    target_include_directories(test_hal PRIVATE ${NEURONOS_INCLUDE_DIR})

    # Engine + Agent test
    add_executable(test_engine tests/test_engine.c)
    target_include_directories(test_engine PRIVATE
        ${NEURONOS_INCLUDE_DIR}
        ${LLAMA_SRC_DIR}/include
        ${LLAMA_SRC_DIR}/ggml/include
    )
    target_link_libraries(test_engine PRIVATE neuronos_agent neuronos_interface neuronos_engine neuronos_hal ${NEURONOS_LIBM})

    # Memory test (no model needed — pure SQLite)
    add_executable(test_memory tests/test_memory.c)
    target_include_directories(test_memory PRIVATE ${NEURONOS_INCLUDE_DIR})
    target_link_libraries(test_memory PRIVATE neuronos_memory ${NEURONOS_LIBM})
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
target_link_libraries(neuronos-cli PRIVATE neuronos_agent neuronos_interface neuronos_engine neuronos_hal ${NEURONOS_LIBM})

# Create `neuronos` symlink → neuronos-cli (convenience alias)
if(NOT WIN32)
    add_custom_command(TARGET neuronos-cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE_NAME:neuronos-cli>"
            "$<TARGET_FILE_DIR:neuronos-cli>/neuronos"
        COMMENT "Creating neuronos → neuronos-cli symlink"
    )
endif()

# On Windows, link Winsock and WinHTTP for networking
if(WIN32)
    target_link_libraries(neuronos-cli PRIVATE ws2_32)
endif()
