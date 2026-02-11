<p align="center">
  <h1 align="center">NeuronOS</h1>
  <p align="center"><em>Universal AI Agent Engine for Edge Devices</em></p>
  <p align="center"><strong>"The Android of AI"</strong> — Local AI agents on any device, no cloud required.</p>
</p>

<p align="center">
  <a href="https://opensource.org/licenses/MIT"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/version-0.8.1-green" alt="Version 0.8.1">
  <img src="https://img.shields.io/badge/C11-pure-blue" alt="C11">
  <img src="https://img.shields.io/badge/platforms-5-orange" alt="5 Platforms">
  <img src="https://img.shields.io/badge/tests-31%2F31_passing-brightgreen" alt="Tests: 31/31">
</p>

---

NeuronOS is a complete AI agent runtime that runs **locally on your hardware** — no GPU required, no cloud, no Python. It wraps [BitNet b1.58](https://huggingface.co/microsoft/BitNet-b1.58-2B-4T) ternary models for ultra-efficient inference and adds a full agent stack on top: tools, memory, protocols, and interfaces.

## Key Features

| Feature | Description |
|---------|-------------|
| **ReAct Agent** | Autonomous reasoning + acting loop with step callbacks |
| **12 Built-in Tools** | File I/O, shell exec, HTTP, calculator, memory ops, and more |
| **Persistent Memory** | MemGPT-style 3-tier memory (Core/Recall/Archival) with SQLite+FTS5 |
| **MCP Server** | Model Context Protocol — works with Claude Desktop, VS Code, Cursor |
| **MCP Client** | Connect to external MCP servers as tool providers |
| **HTTP API** | OpenAI-compatible REST API for easy integration |
| **GBNF Grammar** | Constrained output generation (JSON, tool calls) |
| **HAL** | Hardware Abstraction Layer with runtime ISA dispatch |
| **Zero Dependencies** | Single static binary, ~4 MB, no Python/Node/Java needed |
| **5 Platforms** | Linux x86_64, Linux ARM64, macOS ARM64, Windows x64, Android ARM64 |

## Platform Support

| Platform | Architecture | ISA Backend | Status |
|----------|-------------|-------------|--------|
| Linux | x86_64 | AVX2 / AVX-VNNI | ✅ Tested |
| Linux | ARM64 | NEON | ✅ Tested |
| macOS | ARM64 (Apple Silicon) | NEON | ✅ Tested |
| Windows | x64 | AVX2 | ✅ CI Build |
| Android | ARM64 | NEON | ✅ CI Build |
| *Any* | *Any* | Scalar fallback | ✅ Always works |

## Quick Start

### 1. Download

Grab the latest release for your platform from [Releases](https://github.com/Neuron-OS/neuronos/releases).

```bash
# Linux / macOS
tar xzf neuronos-v*-linux-x86_64.tar.gz
cd neuronos-v*/
```

```powershell
# Windows
Expand-Archive neuronos-v*-windows-x64.zip -DestinationPath neuronos
cd neuronos
```

### 2. Download a Model

NeuronOS uses BitNet b1.58 ternary models in GGUF format (~1.7 GB for 2B params):

```bash
curl -L -o model.gguf \
  https://huggingface.co/microsoft/BitNet-b1.58-2B-4T-gguf/resolve/main/ggml-model-i2_s.gguf
```

### 3. Run

```bash
# Interactive chat
./bin/neuronos-cli model.gguf chat

# Single prompt
./bin/neuronos-cli model.gguf run "Explain quantum computing in 3 sentences"

# Agent mode (autonomous, with tools)
./bin/neuronos-cli model.gguf agent "List all .c files in the current directory"

# Start OpenAI-compatible HTTP server
./bin/neuronos-cli model.gguf serve --port 8080

# Start MCP server (for Claude Desktop / VS Code)
./bin/neuronos-cli model.gguf mcp

# Hardware info
./bin/neuronos-cli hwinfo
```

## Install Script (Linux / macOS)

One-line install via `gh` CLI (works with private repos):

```bash
bash <(gh api repos/Neuron-OS/neuronos/contents/scripts/install.sh --jq '.content' | base64 -d)
```

This downloads the latest release for your platform and optionally downloads the model.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  Layer 7: Applications                              │
│    CLI (6 modes) │ HTTP Server │ MCP Server          │
├─────────────────────────────────────────────────────┤
│  Layer 6: Agent                                     │
│    ReAct loop │ Tool dispatch │ Memory integration   │
├─────────────────────────────────────────────────────┤
│  Layer 5: Tools                                     │
│    12 built-ins │ Registry │ Capability sandboxing   │
├─────────────────────────────────────────────────────┤
│  Layer 4: Grammar                                   │
│    GBNF constrained generation (JSON, tool calls)   │
├─────────────────────────────────────────────────────┤
│  Layer 3: Inference                                 │
│    llama.cpp wrapper (BitNet I2_S ternary kernels)  │
├─────────────────────────────────────────────────────┤
│  Layer 2.5: Memory                                  │
│    SQLite+FTS5 │ Core/Recall/Archival (MemGPT-style)│
├─────────────────────────────────────────────────────┤
│  Layer 2: HAL (Hardware Abstraction Layer)          │
│    Runtime ISA dispatch: Scalar│AVX2│VNNI│NEON      │
├─────────────────────────────────────────────────────┤
│  Layer 1: Hardware                                  │
│    x86-64 │ ARM64 │ RISC-V │ WASM (planned)        │
└─────────────────────────────────────────────────────┘
```

## Build from Source

### Requirements

- **CMake** ≥ 3.14
- **C/C++ Compiler:** GCC 11+, Clang 14+, or MSVC 2022
- No Python, no conda, no pip needed

### Linux / macOS

```bash
git clone --recursive https://github.com/Neuron-OS/neuronos.git
cd neuronos

cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DBITNET_X86_TL2=OFF \
  -DBUILD_SHARED_LIBS=OFF

cmake --build build -j$(nproc)

# Run tests (31/31 should pass)
./build/bin/test_hal && ./build/bin/test_memory && ./build/bin/test_engine
```

### Windows (MSVC)

Open a **Developer PowerShell for VS 2022**, then:

```powershell
git clone --recursive https://github.com/Neuron-OS/neuronos.git
cd neuronos

cmake -B build -G "Ninja Multi-Config" `
  -DBITNET_X86_TL2=OFF `
  -DBUILD_SHARED_LIBS=OFF

cmake --build build --config Release -j $env:NUMBER_OF_PROCESSORS

# Run tests
.\build\bin\Release\test_hal.exe
.\build\bin\Release\test_memory.exe
.\build\bin\Release\test_engine.exe
```

### Android (cross-compile)

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 \
  -DBITNET_X86_TL2=OFF \
  -DBUILD_SHARED_LIBS=OFF \
  -DGGML_OPENMP=OFF

cmake --build build --config Release -j$(nproc)
```

## CLI Modes

| Mode | Command | Description |
|------|---------|-------------|
| **run** | `neuronos-cli model.gguf run "prompt"` | Single completion |
| **chat** | `neuronos-cli model.gguf chat` | Interactive REPL |
| **agent** | `neuronos-cli model.gguf agent "task"` | ReAct agent with tools |
| **serve** | `neuronos-cli model.gguf serve --port 8080` | HTTP server (OpenAI API) |
| **mcp** | `neuronos-cli model.gguf mcp` | MCP server (stdio) |
| **hwinfo** | `neuronos-cli hwinfo` | Show hardware capabilities |

## Performance

Measured on Intel i7-12650H (AVX-VNNI), BitNet b1.58 2B model, 4 threads:

| Metric | Value |
|--------|-------|
| Prompt processing | 95.02 tokens/sec |
| Text generation | 21.01 tokens/sec |
| Binary size | ~4 MB (static, stripped) |
| RAM usage | ~1.8 GB (2B model) |
| Context window | 2048 tokens (8192 planned) |

## Supported Models

NeuronOS runs any BitNet b1.58 ternary model in GGUF format:

| Model | Parameters | HuggingFace |
|-------|-----------|-------------|
| **BitNet b1.58 2B-4T** (recommended) | 2.4B | [microsoft/BitNet-b1.58-2B-4T-gguf](https://huggingface.co/microsoft/BitNet-b1.58-2B-4T-gguf) |
| bitnet_b1_58-large | 0.7B | [1bitLLM/bitnet_b1_58-large](https://huggingface.co/1bitLLM/bitnet_b1_58-large) |
| bitnet_b1_58-3B | 3.3B | [1bitLLM/bitnet_b1_58-3B](https://huggingface.co/1bitLLM/bitnet_b1_58-3B) |
| Llama3-8B-1.58 | 8.0B | [HF1BitLLM/Llama3-8B-1.58-100B-tokens](https://huggingface.co/HF1BitLLM/Llama3-8B-1.58-100B-tokens) |
| Falcon3 Family | 1B-10B | [tiiuae/Falcon3](https://huggingface.co/collections/tiiuae/falcon3-67605ae03578be86e4e87026) |

## MCP Integration

NeuronOS can act as an **MCP server** for AI-powered editors and assistants:

```jsonc
// Claude Desktop config (~/.config/claude/claude_desktop_config.json)
{
  "mcpServers": {
    "neuronos": {
      "command": "/usr/local/bin/neuronos-cli",
      "args": ["/path/to/model.gguf", "mcp"]
    }
  }
}
```

## Project Status

- **Version:** 0.8.1
- **Phase:** 3A — Agent Intelligence
- **Tests:** 31/31 passing (HAL 4, Engine 11, Memory 12, + Agent 4)
- **Next:** Context compaction, KV cache management, extended context (8192+ tokens)

## Based On

NeuronOS is built on top of [microsoft/BitNet](https://github.com/microsoft/BitNet) (a fork of [llama.cpp](https://github.com/ggerganov/llama.cpp)). We wrap — not rewrite — the inference engine, adding a complete agent runtime on top.

## License

MIT License — see [LICENSE](LICENSE) for details.
