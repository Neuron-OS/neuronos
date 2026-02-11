#!/bin/bash
# ══════════════════════════════════════════════════════════════════════
# NeuronOS WASM Build Script
# Compiles the entire NeuronOS stack (llama.cpp + SQLite + NeuronOS)
# to WebAssembly for browser execution.
#
# Usage:
#   ./build_wasm.sh              # Multi-thread + single-thread builds
#   ./build_wasm.sh --st-only    # Single-thread build only
#   ./build_wasm.sh --mt-only    # Multi-thread build only
#   ./build_wasm.sh --clean      # Clean and rebuild
#
# Prerequisites:
#   - Emscripten SDK (emsdk) installed and activated
#   - CMake >= 3.18
#
# Output:
#   dist/neuronos-worker.js       (multi-thread)
#   dist/neuronos-worker.wasm
#   dist/neuronos-worker.worker.js
#   dist/neuronos-worker-st.js    (single-thread)
#   dist/neuronos-worker-st.wasm
#   dist/neuronos-web.js          (JS API)
#   dist/neuronos-inference-worker.js
#   dist/playground.html
# ══════════════════════════════════════════════════════════════════════
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WASM_DIR="$SCRIPT_DIR"
NEURONOS_DIR="$(dirname "$WASM_DIR")"
BITNET_DIR="$(dirname "$NEURONOS_DIR")"
DIST_DIR="$WASM_DIR/dist"
BUILD_MT="$WASM_DIR/build-wasm-mt"
BUILD_ST="$WASM_DIR/build-wasm-st"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()    { echo -e "${CYAN}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[OK]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
error()   { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# ── Parse arguments ──
BUILD_MULTITHREAD=true
BUILD_SINGLETHREAD=true
CLEAN=false

for arg in "$@"; do
  case "$arg" in
    --st-only) BUILD_MULTITHREAD=false ;;
    --mt-only) BUILD_SINGLETHREAD=false ;;
    --clean)   CLEAN=true ;;
    --help|-h)
      echo "Usage: $0 [--st-only|--mt-only|--clean|--help]"
      exit 0
      ;;
    *) error "Unknown argument: $arg" ;;
  esac
done

# ── Check Emscripten ──
info "Checking Emscripten SDK..."
if ! command -v emcmake &>/dev/null; then
  error "Emscripten not found. Install it:
    git clone https://github.com/emscripten-core/emsdk.git
    cd emsdk
    ./emsdk install latest
    ./emsdk activate latest
    source emsdk_env.sh"
fi

EMCC_VERSION=$(emcc --version 2>&1 | head -1)
success "Found: $EMCC_VERSION"

# ── Check CMake ──
if ! command -v cmake &>/dev/null; then
  error "CMake not found. Install cmake >= 3.18"
fi

CMAKE_VERSION=$(cmake --version | head -1)
success "Found: $CMAKE_VERSION"

# ── Clean if requested ──
if $CLEAN; then
  info "Cleaning previous builds..."
  rm -rf "$BUILD_MT" "$BUILD_ST" "$DIST_DIR"
  success "Clean done"
fi

# ── Create dist directory ──
mkdir -p "$DIST_DIR"

# ── Multi-thread build ──
if $BUILD_MULTITHREAD; then
  echo ""
  info "════════════════════════════════════════════"
  info "Building NeuronOS WASM (MULTI-THREAD)"
  info "════════════════════════════════════════════"

  mkdir -p "$BUILD_MT"
  pushd "$BUILD_MT" > /dev/null

  info "Configuring (multi-thread)..."
  emcmake cmake "$WASM_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEURONOS_WASM_THREADS=ON

  info "Building (multi-thread)..."
  emmake make -j$(nproc) 2>&1 | tail -5

  popd > /dev/null

  # Copy outputs — CMake may put them in dist/ or build dir
  if [[ -f "$DIST_DIR/neuronos-worker.js" ]]; then
    success "Multi-thread build: $(du -h "$DIST_DIR/neuronos-worker.wasm" | cut -f1)"
  elif [[ -f "$BUILD_MT/neuronos-worker.js" ]]; then
    cp "$BUILD_MT/neuronos-worker.js"   "$DIST_DIR/"
    cp "$BUILD_MT/neuronos-worker.wasm" "$DIST_DIR/"
    [[ -f "$BUILD_MT/neuronos-worker.worker.js" ]] && cp "$BUILD_MT/neuronos-worker.worker.js" "$DIST_DIR/"
    success "Multi-thread build: $(du -h "$DIST_DIR/neuronos-worker.wasm" | cut -f1)"
  else
    warn "Multi-thread build output not found"
  fi
fi

# ── Single-thread build ──
if $BUILD_SINGLETHREAD; then
  echo ""
  info "════════════════════════════════════════════"
  info "Building NeuronOS WASM (SINGLE-THREAD)"
  info "════════════════════════════════════════════"

  mkdir -p "$BUILD_ST"
  pushd "$BUILD_ST" > /dev/null

  info "Configuring (single-thread)..."
  emcmake cmake "$WASM_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEURONOS_WASM_THREADS=OFF

  info "Building (single-thread)..."
  emmake make -j$(nproc) 2>&1 | tail -5

  popd > /dev/null

  # Copy outputs — CMake puts them directly in dist/ via RUNTIME_OUTPUT_DIRECTORY
  if [[ -f "$DIST_DIR/neuronos-worker-st.js" ]]; then
    success "Single-thread build: $(du -h "$DIST_DIR/neuronos-worker-st.wasm" | cut -f1)"
  elif [[ -f "$BUILD_ST/neuronos-worker-st.js" ]]; then
    cp "$BUILD_ST/neuronos-worker-st.js"   "$DIST_DIR/"
    cp "$BUILD_ST/neuronos-worker-st.wasm" "$DIST_DIR/"
    success "Single-thread build: $(du -h "$DIST_DIR/neuronos-worker-st.wasm" | cut -f1)"
  else
    warn "Single-thread build output not found"
  fi
fi

# ── Copy JS & HTML assets ──
echo ""
info "Copying JS and HTML assets..."
cp "$WASM_DIR/neuronos-web.js"               "$DIST_DIR/"
cp "$WASM_DIR/neuronos-inference-worker.js"   "$DIST_DIR/"
cp "$WASM_DIR/playground.html"                "$DIST_DIR/index.html"
success "Assets copied to dist/"

# ── Generate serve script ──
cat > "$DIST_DIR/serve.sh" << 'SERVE_EOF'
#!/bin/bash
# Quick development server with required COOP/COEP headers for SharedArrayBuffer
# Usage: ./serve.sh [port]
PORT=${1:-8080}
echo "Serving NeuronOS Playground at http://localhost:$PORT"
echo "  Cross-Origin-Embedder-Policy: require-corp"
echo "  Cross-Origin-Opener-Policy: same-origin"
echo ""
echo "Press Ctrl+C to stop"

# Check for available server options
if command -v npx &>/dev/null; then
  npx --yes http-server . -p $PORT \
    --cors \
    -c-1 \
    --header "Cross-Origin-Embedder-Policy: require-corp" \
    --header "Cross-Origin-Opener-Policy: same-origin"
elif python3 -c "import http.server" 2>/dev/null; then
  python3 -c "
import http.server, functools

class COOPHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Access-Control-Allow-Origin', '*')
        super().end_headers()

server = http.server.HTTPServer(('', $PORT), COOPHandler)
server.serve_forever()
"
else
  echo "Error: No server found. Install Node.js (npx) or Python 3."
  exit 1
fi
SERVE_EOF
chmod +x "$DIST_DIR/serve.sh"

# ── Summary ──
echo ""
echo "═══════════════════════════════════════════════════════"
info "Build complete!"
echo "═══════════════════════════════════════════════════════"
echo ""
echo "  Output directory: $DIST_DIR/"
echo ""

ls -lh "$DIST_DIR/" 2>/dev/null | grep -v '^total' | while read -r line; do
  echo "    $line"
done

echo ""
echo "  To test locally:"
echo "    cd $DIST_DIR && ./serve.sh"
echo "    Open http://localhost:8080 in your browser"
echo ""
echo "  Requirements for multi-thread support:"
echo "    - Browser with SharedArrayBuffer support"
echo "    - COOP/COEP headers (serve.sh handles this)"
echo ""
success "Done!"
