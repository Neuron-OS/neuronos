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

# ═════════════════════════════════════════════════════════════
# Layer 1: HAL — Hardware Abstraction Layer
# ═════════════════════════════════════════════════════════════
set(HAL_SOURCES
    src/hal/hal_registry.c
    src/hal/hal_scalar.c
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|x86")
    list(APPEND HAL_SOURCES src/hal/hal_x86_avx2.c)
endif()

add_library(neuronos_hal STATIC ${HAL_SOURCES})
target_include_directories(neuronos_hal PUBLIC ${NEURONOS_INCLUDE_DIR})

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(neuronos_hal PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|x86")
        set_source_files_properties(src/hal/hal_x86_avx2.c
            PROPERTIES COMPILE_FLAGS "-mavx2 -mssse3")
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
target_link_directories(neuronos_engine PUBLIC
    ${LLAMA_BUILD_DIR}/src
    ${LLAMA_BUILD_DIR}/ggml/src
)
target_link_libraries(neuronos_engine PUBLIC llama ggml m)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(neuronos_engine PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

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
target_link_libraries(neuronos_agent PUBLIC neuronos_engine)

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
