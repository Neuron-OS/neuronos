# NeuronOS WASM Architecture

> Run a complete AI agent 100% locally in the browser. Zero cloud. Zero API keys. Zero server.

## Overview

NeuronOS WASM compiles the entire agent stack — inference engine (BitNet/llama.cpp), MemGPT memory
(SQLite+FTS5), ReAct agent loop, and tool dispatch — to WebAssembly. This is **not** just inference
in the browser; it's the first **complete AI agent runtime** running client-side.

```
┌─────────────────────────────────────────────────────────┐
│  Browser Main Thread                                     │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  playground.html (UI)                                │ │
│  │  ├── Model download + OPFS caching                  │ │
│  │  ├── Chat interface (streaming)                     │ │
│  │  ├── Agent step visualization                       │ │
│  │  └── Settings (temp, tokens, ctx)                   │ │
│  └────────────────┬────────────────────────────────────┘ │
│                    │ postMessage                          │
│  ┌─────────────────▼───────────────────────────────────┐ │
│  │  neuronos-web.js (API layer)                         │ │
│  │  ├── NeuronOS class (Promise-based)                 │ │
│  │  ├── OPFS model cache (navigator.storage)           │ │
│  │  ├── Split model download (parallel chunks)         │ │
│  │  └── Auto thread detection                          │ │
│  └────────────────┬────────────────────────────────────┘ │
│                    │ Worker postMessage                   │
├────────────────────┼─────────────────────────────────────┤
│  Web Worker        │                                     │
│  ┌─────────────────▼───────────────────────────────────┐ │
│  │  neuronos-inference-worker.js                        │ │
│  │  ├── Loads WASM module (Emscripten)                 │ │
│  │  ├── cwrap bindings to C functions                  │ │
│  │  ├── Model buffer management (malloc/copy/free)     │ │
│  │  └── Command dispatch (init/load/chat/generate)     │ │
│  └────────────────┬────────────────────────────────────┘ │
│                    │ Emscripten ABI                       │
│  ┌─────────────────▼───────────────────────────────────┐ │
│  │  neuronos-worker.wasm                                │ │
│  │  ┌─────────────────────────────────────────────────┐ │ │
│  │  │  neuronos_wasm_glue.c (exported C API)          │ │ │
│  │  │  ├── neuronos_wasm_init()                       │ │ │
│  │  │  ├── neuronos_wasm_load_model_from_buffer()     │ │ │
│  │  │  ├── neuronos_wasm_generate()                   │ │ │
│  │  │  ├── neuronos_wasm_agent_chat()                 │ │ │
│  │  │  └── neuronos_wasm_memory_*()                   │ │ │
│  │  ├───────────────────────────────────────────────────│ │
│  │  │  NeuronOS Core (C11)                            │ │ │
│  │  │  ├── neuronos_engine.c (inference)              │ │ │
│  │  │  ├── neuronos_agent.c (ReAct loop)              │ │ │
│  │  │  ├── neuronos_tool_registry.c (10 tools)        │ │ │
│  │  │  └── neuronos_memory.c (SQLite+FTS5)            │ │ │
│  │  ├───────────────────────────────────────────────────│ │
│  │  │  llama.cpp (ggml + ternary kernels)             │ │ │
│  │  │  └── WASM SIMD 128-bit backend                  │ │ │
│  │  ├───────────────────────────────────────────────────│ │
│  │  │  SQLite 3.47.2 amalgamation + FTS5              │ │ │
│  │  │  └── Emscripten FS → OPFS persistence           │ │ │
│  │  └─────────────────────────────────────────────────┘ │ │
│  └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

## File Structure

```
neuronos/neuronos/wasm/
├── CMakeLists.txt                # WASM build system (Emscripten)
├── neuronos_wasm_glue.c          # C API bridge (exported to JS)
├── neuronos-web.js               # High-level JS API (NeuronOS class)
├── neuronos-inference-worker.js  # Web Worker (loads + runs WASM)
├── playground.html               # Interactive web UI
├── build_wasm.sh                 # Build script
├── WASM_ARCHITECTURE.md          # This document
└── dist/                         # Build output (generated)
    ├── neuronos-worker.js        # Multi-thread Emscripten glue
    ├── neuronos-worker.wasm      # Multi-thread WASM binary
    ├── neuronos-worker.worker.js # pthread worker (auto-generated)
    ├── neuronos-worker-st.js     # Single-thread Emscripten glue
    ├── neuronos-worker-st.wasm   # Single-thread WASM binary
    ├── neuronos-web.js           # JS API (copied)
    ├── neuronos-inference-worker.js
    ├── index.html                # Playground (copied)
    └── serve.sh                  # Dev server with COOP/COEP headers
```

## Build System

### Prerequisites

1. **Emscripten SDK** (emsdk) >= 3.1.50
2. **CMake** >= 3.18

```bash
# Install Emscripten (one-time)
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source emsdk_env.sh

# Build NeuronOS WASM
cd neuronos/neuronos/wasm
./build_wasm.sh
```

### Build Variants

| Variant | Flag | Output | Use Case |
|---------|------|--------|----------|
| Multi-thread | `-DNEURONOS_WASM_THREADS=ON` | `neuronos-worker.{js,wasm}` | Modern browsers with SharedArrayBuffer |
| Single-thread | `-DNEURONOS_WASM_THREADS=OFF` | `neuronos-worker-st.{js,wasm}` | Fallback for all browsers |

The JS API auto-detects thread support and loads the appropriate WASM:
```javascript
const multiThread = typeof SharedArrayBuffer !== 'undefined'
                 && (crossOriginIsolated || isSecureContext);
```

### Exported C Functions

The WASM binary exports these functions via Emscripten `EXPORTED_FUNCTIONS`:

| Function | Purpose |
|----------|---------|
| `neuronos_wasm_init(threads, ctx_size)` | Initialize engine with thread count and context |
| `neuronos_wasm_load_model_from_buffer(ptr, size)` | Load GGUF from memory buffer |
| `neuronos_wasm_generate(prompt, n_predict, temp)` | Raw text generation |
| `neuronos_wasm_agent_chat(message)` | Agent chat with ReAct loop |
| `neuronos_wasm_model_info()` | Get model metadata (JSON) |
| `neuronos_wasm_memory_init()` | Initialize SQLite memory |
| `neuronos_wasm_memory_store(tier, key, value)` | Store to memory tier |
| `neuronos_wasm_memory_search(query, limit)` | FTS5 search across memories |
| `neuronos_wasm_free()` | Clean shutdown |
| `neuronos_wasm_free_string(ptr)` | Free string allocated by WASM |

## Browser Requirements

### Required
- **WebAssembly** — supported in all modern browsers (Chrome 57+, Firefox 52+, Safari 11+)
- **WASM SIMD** — Chrome 91+, Firefox 89+, Safari 16.4+

### Recommended (for multi-threading)
- **SharedArrayBuffer** — requires COOP/COEP HTTP headers:
  ```
  Cross-Origin-Embedder-Policy: require-corp
  Cross-Origin-Opener-Policy: same-origin
  ```
- **Web Workers** — all modern browsers

### Optional (for model caching)
- **OPFS (Origin Private File System)** — Chrome 86+, Firefox 111+, Safari 15.2+
- With OPFS, downloaded models are cached locally; without it, models re-download each session

### Compatibility Matrix

| Feature | Chrome | Firefox | Safari | Edge |
|---------|--------|---------|--------|------|
| WASM | 57+ | 52+ | 11+ | 16+ |
| WASM SIMD | 91+ | 89+ | 16.4+ | 91+ |
| SharedArrayBuffer | 68+ | 79+ | 15.2+ | 79+ |
| OPFS | 86+ | 111+ | 15.2+ | 86+ |
| Multi-thread | ✅ | ✅ | ✅ | ✅ |
| Single-thread | ✅ | ✅ | ✅ | ✅ |

## Model Loading Pipeline

```
1. User selects model (HuggingFace URL or local file)
        │
2. Check OPFS cache
        │ hit → read from cache → step 5
        │ miss ↓
3. Download model
        │ ├── Single file: fetch with progress
        │ └── Split model (>2GB): parallel chunk download
        │
4. Cache to OPFS (if available)
        │
5. Transfer ArrayBuffer to Worker via postMessage
        │
6. Worker: malloc(size) on WASM heap
        │
7. Worker: copy ArrayBuffer → WASM memory
        │
8. Worker: call neuronos_wasm_load_model_from_buffer()
        │ └── Writes to Emscripten VFS as /model.gguf
        │ └── Calls neuronos_init() + neuronos_load_model()
        │
9. Model ready — enable chat
```

### Split Model Support

GGUF models >2GB exceed the browser's ArrayBuffer limit. The system detects split models
by the naming pattern `*-00001-of-NNNNN.gguf` and:

1. Parses total chunk count from filename
2. Downloads all chunks in parallel (4 concurrent)
3. Concatenates into a single ArrayBuffer
4. Proceeds with normal loading pipeline

## Memory (SQLite in WASM)

SQLite 3.47.2 compiles to WASM via the amalgamation (single `sqlite3.c` file). FTS5 is enabled
for full-text search across memory tiers.

### Persistence via OPFS

The Emscripten filesystem mounts `/neuronos_memory.db` which maps to browser storage. Currently
uses the Emscripten in-memory filesystem; for persistence across sessions, the OPFS-backed
filesystem (`WASMFS` with OPFS backend) can be used with Emscripten 3.1.46+.

### Memory Tiers (MemGPT-style)

| Tier | Description | WASM Support |
|------|-------------|--------------|
| Core Memory | Key-value blocks in agent prompt | ✅ Full |
| Recall Memory | Chat history per session, FTS5 searchable | ✅ Full |
| Archival Memory | Persistent facts, FTS5 searchable | ✅ Full |

## Performance Expectations

Based on wllama benchmarks and llama.cpp WASM tests:

| Metric | Expected (BitNet 2B) | Notes |
|--------|----------------------|-------|
| Model size | 1.71 GB | I2_S quantization |
| WASM binary | ~5-8 MB | Compressed ~2-3 MB (gzip) |
| Load time | 5-15s | Depends on OPFS vs network |
| Prompt eval | 5-15 t/s | WASM SIMD, depends on CPU |
| Generation | 3-8 t/s | Single-thread; ~2x with MT |
| Memory (RAM) | ~2.5-3 GB | Model + context + WASM heap |

**Note:** Browser WASM performance is typically 30-50% of native. The ternary BitNet
quantization helps because I2_S kernels are less compute-intensive than FP16/Q4.

## Security Model

- **100% client-side** — no data leaves the browser
- **No network after load** — once model is cached, works offline
- **OPFS sandboxed** — model cache isolated per origin
- **WASM sandbox** — all C code runs in WASM memory sandbox
- **No eval/inline-script** — all JS is modular ES modules

## Deployment

### Static Hosting (Production)

Serve the `dist/` directory from any static file host. The server MUST set:

```
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Opener-Policy: same-origin
```

Examples:
- **Nginx**: `add_header Cross-Origin-Embedder-Policy "require-corp";`
- **Cloudflare Pages**: Add `_headers` file
- **Vercel**: Add `vercel.json` with headers config

### Development

```bash
cd neuronos/neuronos/wasm/dist
./serve.sh 8080
# Opens http://localhost:8080 with correct COOP/COEP headers
```

### CDN Integration

For HuggingFace model hosting:
```
https://huggingface.co/microsoft/BitNet-b1.58-2B-4T-gguf/resolve/main/ggml-model-i2_s.gguf
```

CORS must be enabled on the model host. HuggingFace supports CORS by default.

## Roadmap (WASM-specific)

### Phase 1: Foundation (Current)
- [x] CMake build system for Emscripten
- [x] C glue layer (neuronos_wasm_glue.c)
- [x] JS API wrapper (neuronos-web.js)
- [x] Web Worker architecture
- [x] Interactive playground UI
- [x] Build script
- [ ] End-to-end compilation test

### Phase 2: Optimization
- [ ] WASMFS + OPFS backend for SQLite persistence
- [ ] Service Worker for offline support (PWA)
- [ ] Model quantization selection in UI (I2_S, Q4_0)
- [ ] KV cache state save/restore across sessions
- [ ] Web GPU backend (via llama.cpp WebGPU)

### Phase 3: Distribution
- [ ] NPM package (`@neuronos/wasm`)
- [ ] Embeddable widget (`<neuronos-chat>` web component)
- [ ] HuggingFace Spaces demo
- [ ] CDN-hosted WASM binaries

## Differences vs Native NeuronOS

| Feature | Native | WASM |
|---------|--------|------|
| HAL backends | AVX2, AVX-VNNI, NEON, Scalar | WASM SIMD 128-bit only |
| Threads | OS threads (pthreads) | Web Workers + SharedArrayBuffer |
| Model loading | mmap (zero-copy) | ArrayBuffer copy to VFS |
| SQLite | Direct file I/O | Emscripten FS (memory or OPFS) |
| MCP server | STDIO JSON-RPC | Not applicable (browser context) |
| HTTP server | Built-in | Not applicable |
| File system tools | Full access | Sandboxed (OPFS only) |
| Performance | 21 t/s gen, 95 t/s prompt | ~3-8 t/s gen (estimated) |

## Reference Projects

- **wllama** (ngxson/wllama) — WASM binding for llama.cpp inference (no agent)
- **llama.cpp WASM** — ggml-org/llama.cpp#17237 active WASM build discussion
- **SQLite WASM** — Official SQLite WASM build with OPFS support
- **user/neuronos** — NeuronOS native runtime (this project)
