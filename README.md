<p align="center">
  <h1 align="center">NeuronOS</h1>
  <p align="center"><strong>Sovereign AI agent runtime for every device.</strong></p>
  <p align="center">
    <a href="#quick-start">Quick Start</a> •
    <a href="#features">Features</a> •
    <a href="#architecture">Architecture</a> •
    <a href="#supported-hardware">Hardware</a> •
    <a href="#building-from-source">Build</a> •
    <a href="#documentation">Docs</a>
  </p>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-0.9.0-blue" alt="Version">
  <img src="https://img.shields.io/badge/language-C11-orange" alt="C11">
  <img src="https://img.shields.io/badge/tests-27%2F27-brightgreen" alt="Tests">
  <img src="https://img.shields.io/badge/license-MIT-green" alt="MIT">
  <img src="https://img.shields.io/badge/dependencies-zero-purple" alt="Zero deps">
</p>

---

NeuronOS is a self-contained AI agent engine written in pure C11. It runs complete autonomous agents — with reasoning, memory, tool use, and inter-agent communication — on any device, from a Raspberry Pi to a cloud server, with **zero runtime dependencies** and **zero cloud requirements**.

Built on [BitNet b1.58](https://github.com/microsoft/BitNet) ternary models, NeuronOS delivers useful AI agents on hardware as modest as 1.5 GB of RAM, entirely offline.

```
$ curl -fsSL https://neuronos.dev/install | bash
$ neuronos
> What files are in my project?
[tool: list_dir] Scanning ./...
Found 12 files. Here's what I see:
  src/main.c        — Entry point
  src/utils.c       — Helper functions
  Makefile           — Build configuration
  ...
> Remember that the deadline for this project is March 15
[tool: memory_store] Saved to archival memory.
Noted. I'll remember the March 15 deadline.
```

## Quick Start

**One-command install** (Linux, macOS, Windows WSL):

```bash
curl -fsSL https://neuronos.dev/install | bash
```

The installer detects your hardware (CPU features, RAM, GPU), downloads the optimal ternary model, and sets up everything automatically.

**Or build from source:**

```bash
git clone https://github.com/neuronos-project/neuronos
cd neuronos
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/bin/neuronos
```

## Features

### Agent Engine
- **ReAct reasoning loop** — Think → Act → Observe cycles with transparent reasoning
- **12 built-in tools** — Shell, file read/write, directory listing, file search, PDF reading, HTTP requests, calculator, time, and 3 memory tools
- **10,000+ external tools** via MCP client integration
- **3-format GBNF grammar** — Constrained generation for reliable tool calling
- **Multi-turn conversations** with persistent context

### Memory (MemGPT 3-Tier)
- **Core Memory** — Key-value blocks injected into every prompt (persona, instructions)
- **Recall Memory** — Full chat history per session, FTS5 full-text searchable
- **Archival Memory** — Permanent facts with unique keys, searchable, access-tracked
- **Automatic context compaction** at ~85% capacity with summarization

### Protocols
- **MCP Server** — Expose NeuronOS tools to any MCP-compatible client (JSON-RPC 2.0, STDIO)
- **MCP Client** — Connect to external MCP servers, auto-discover and use their tools (~1,370 lines of pure C)
- **OpenAI-compatible HTTP API** — `/v1/chat/completions`, `/v1/models`, SSE streaming
- **A2A Protocol** — Agent-to-agent communication *(coming next — first C implementation worldwide)*

### Inference
- **BitNet b1.58 ternary models** — 2B params in 1.71 GiB, runs on 1.5 GB RAM
- **21 tokens/sec generation** on a laptop CPU (i7-12650H, 4 threads)
- **95 tokens/sec prompt processing** on the same hardware
- **Multi-model support** — BitNet 2B, Falcon3-7B/10B (1.58-bit), Qwen2.5-3B/14B (Q4_K_M)
- **Automatic model selection** based on detected hardware capabilities

### Hardware Abstraction
- **5 ISA backends** with automatic runtime detection:
  - `hal_scalar` — Pure C fallback (works everywhere)
  - `hal_x86_avx2` — Intel/AMD Haswell+ (2013+)
  - `hal_x86_avxvnni` — Intel Alder Lake+ (2021+)
  - `hal_arm_neon` — Apple Silicon, Raspberry Pi 4/5
  - CUDA build available for NVIDIA GPUs (Q4_K_M models)

## Architecture

```
┌─────────────────────────────────────────────────┐
│  Layer 7: Applications                          │
│    CLI (8 modes) • HTTP Server • MCP Server     │
├─────────────────────────────────────────────────┤
│  Layer 6: Agent                                 │
│    ReAct Loop • Tool Dispatch • Step Callbacks  │
├─────────────────────────────────────────────────┤
│  Layer 5: Tools                                 │
│    Registry (12 built-in) • MCP Bridge • Sandbox│
├─────────────────────────────────────────────────┤
│  Layer 4: Grammar                               │
│    GBNF Constrained Generation (3 formats)      │
├─────────────────────────────────────────────────┤
│  Layer 3: Inference                             │
│    llama.cpp wrapper (BitNet I2_S kernels)      │
├─────────────────────────────────────────────────┤
│  Layer 2.5: Memory                              │
│    SQLite 3.47.2 + FTS5 (MemGPT 3-tier)        │
├─────────────────────────────────────────────────┤
│  Layer 2: HAL                                   │
│    Runtime ISA dispatch (scalar/AVX2/VNNI/NEON) │
├─────────────────────────────────────────────────┤
│  Layer 1: Hardware                              │
│    x86-64 • ARM64 • RISC-V • WASM (planned)    │
└─────────────────────────────────────────────────┘
```

**~9,400 lines of C11** across 19 source files. No C++ in the public API.

## Supported Hardware

| Hardware | Backend | Min RAM | Status |
|----------|---------|---------|--------|
| Intel Haswell+ (2013+) | AVX2 | 1.5 GB | ✅ |
| Intel Alder Lake+ (2021+) | AVX-VNNI | 1.5 GB | ✅ |
| Apple M1/M2/M3/M4 | ARM NEON | 1.5 GB | ✅ |
| Raspberry Pi 4/5 | ARM NEON | 1.5 GB | ✅ |
| Any CPU | Scalar (C pure) | 1.5 GB | ✅ |
| NVIDIA GPU | CUDA | 4 GB VRAM | ✅ (Q4_K_M) |
| Any GPU | Vulkan | — | Planned |
| Browser | WASM | — | Planned |

## Usage

### Interactive Agent (default)

```bash
neuronos                          # Auto-detect model, launch agent
neuronos run "Summarize this"     # Single prompt
neuronos agent                    # Explicit agent mode
```

### With MCP Tools

```bash
neuronos --mcp                    # Load tools from ~/.neuronos/mcp.json
```

### HTTP Server (OpenAI-compatible)

```bash
neuronos serve --port 8080        # Start API server
curl http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"neuronos","messages":[{"role":"user","content":"Hello"}]}'
```

### MCP Server

```bash
neuronos mcp                      # JSON-RPC 2.0 over STDIO
```

### Hardware Info

```bash
neuronos hwinfo                   # Show detected hardware + backends
neuronos scan                     # Scan for available models
```

## Building from Source

### Requirements
- C11 compiler (Clang 14+ or GCC 12+ recommended)
- CMake 3.20+
- ~2 GB disk space for build

### Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)
```

### Test

```bash
./build/bin/test_hal && ./build/bin/test_engine && ./build/bin/test_memory
# Expected: 27/27 PASS
```

### Build Options

| Option | Description | Default |
|--------|-------------|---------|
| `CMAKE_BUILD_TYPE` | Release / Debug | Release |
| `BITNET_X86_TL2` | x86 TL2 kernel (experimental) | OFF |
| `CMAKE_EXPORT_COMPILE_COMMANDS` | Generate compile_commands.json | OFF |

## Project Structure

```
neuronos/
├── include/neuronos/
│   ├── neuronos.h              # Public API (694 lines, v0.9.0)
│   └── neuronos_hal.h          # HAL API (331 lines)
├── src/
│   ├── hal/                    # Hardware abstraction backends
│   │   ├── hal_registry.c      # Backend registry + CPUID detection
│   │   ├── hal_scalar.c        # Pure C fallback
│   │   ├── hal_x86_avx2.c     # AVX2 backend
│   │   ├── hal_x86_avxvnni.c  # AVX-VNNI backend
│   │   └── hal_arm_neon.c     # ARM NEON backend
│   ├── engine/
│   │   ├── neuronos_engine.c   # Inference engine (llama.cpp wrapper)
│   │   └── neuronos_model_selector.c  # HW detection + model scoring
│   ├── memory/
│   │   └── neuronos_memory.c   # MemGPT 3-tier memory (SQLite+FTS5)
│   ├── agent/
│   │   ├── neuronos_agent.c    # ReAct agent loop + memory integration
│   │   └── neuronos_tool_registry.c   # Tool registry + 12 built-in tools
│   ├── cli/
│   │   └── neuronos_cli.c     # CLI with 8 modes
│   ├── interface/
│   │   └── neuronos_server.c  # HTTP server (OpenAI API + SSE)
│   └── mcp/
│       ├── neuronos_mcp_server.c  # MCP server (JSON-RPC STDIO)
│       └── neuronos_mcp_client.c  # MCP client (~1370 lines)
├── 3rdparty/
│   ├── sqlite/                 # SQLite 3.47.2 amalgamation
│   └── sqlite-vec/            # sqlite-vec v0.1.6 (prepared)
├── tests/
│   ├── test_hal.c             # 4 HAL tests
│   ├── test_engine.c          # 11 engine + agent tests
│   └── test_memory.c          # 12 memory tests
└── grammars/
    ├── tool_call.gbnf         # Tool calling grammar
    └── json.gbnf              # JSON output grammar
```

## Documentation

| Document | Description |
|----------|-------------|
| [ROADMAP.md](../../ROADMAP.md) | Strategic roadmap and execution plan |
| [TRACKING.md](../../TRACKING.md) | Iteration-by-iteration progress log |
| [AGENTS.md](../../AGENTS.md) | Instructions for AI coding agents |
| [ARSENAL.md](../../ARSENAL.md) | Technology arsenal and market research |

## What NeuronOS Is Not

- **Not an inference speed benchmark.** llama.cpp will always be faster. We optimize for agent utility.
- **Not a cloud service.** Everything runs locally. Your data never leaves your device.
- **Not a Python framework.** Pure C11, zero runtime dependencies. Compiles to a single binary.
- **Not a replacement for GPT-5.** Ternary models have limits. We bring intelligence where frontier models can't reach: offline, embedded, private, free.

## Contributing

We welcome contributions. Please read [AGENTS.md](../../AGENTS.md) for coding standards and architecture guidelines before submitting PRs.

All tests must pass before any commit:
```bash
./build/bin/test_hal && ./build/bin/test_engine && ./build/bin/test_memory
```

## License

MIT License. See [LICENSE](../LICENSE) for details.

SQLite is public domain. sqlite-vec is MIT/Apache-2.0.
