#!/usr/bin/env bash
##
## NeuronOS — Release Packaging Script
##
## Usage: ./scripts/package-release.sh [version] [build_dir]
##   version:   e.g. "0.8.0" (default: reads from neuronos.h)
##   build_dir: path to cmake build dir (default: "build")
##
## Output: neuronos-v${VERSION}-${OS}-${ARCH}.tar.gz
##
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Detect version from header if not provided ──
if [ -n "${1:-}" ]; then
    VERSION="$1"
else
    VERSION=$(grep -oP '#define NEURONOS_VERSION\s+"\K[^"]+' \
        "$REPO_ROOT/neuronos/include/neuronos/neuronos.h" 2>/dev/null || echo "0.8.0")
fi

BUILD_DIR="${2:-$REPO_ROOT/build}"

# ── Detect platform ──
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

# Normalize
case "$OS" in
    darwin) OS="macos" ;;
    linux)  OS="linux" ;;
    *)      echo "ERROR: Unsupported OS: $OS"; exit 1 ;;
esac

case "$ARCH" in
    x86_64|amd64)   ARCH="x86_64" ;;
    aarch64|arm64)   ARCH="arm64" ;;
    *)               echo "ERROR: Unsupported arch: $ARCH"; exit 1 ;;
esac

RELEASE_NAME="neuronos-v${VERSION}-${OS}-${ARCH}"
STAGING_DIR="/tmp/${RELEASE_NAME}"
OUTPUT_FILE="${REPO_ROOT}/${RELEASE_NAME}.tar.gz"

echo "════════════════════════════════════════════"
echo "  NeuronOS Release Packager"
echo "════════════════════════════════════════════"
echo "  Version:  v${VERSION}"
echo "  Platform: ${OS}-${ARCH}"
echo "  Build:    ${BUILD_DIR}"
echo "  Output:   ${OUTPUT_FILE}"
echo "════════════════════════════════════════════"
echo ""

# ── Verify build artifacts exist ──
NEURONOS_CLI="$BUILD_DIR/bin/neuronos-cli"
LIBLLAMA="$BUILD_DIR/3rdparty/llama.cpp/src/libllama.so"
LIBGGML="$BUILD_DIR/3rdparty/llama.cpp/ggml/src/libggml.so"

if [ "$OS" = "macos" ]; then
    LIBLLAMA="$BUILD_DIR/3rdparty/llama.cpp/src/libllama.dylib"
    LIBGGML="$BUILD_DIR/3rdparty/llama.cpp/ggml/src/libggml.dylib"
fi

for f in "$NEURONOS_CLI" "$LIBLLAMA" "$LIBGGML"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: Required file not found: $f"
        echo "       Did you build first? Run: cmake --build $BUILD_DIR -j\$(nproc)"
        exit 1
    fi
done

# ── Create staging directory ──
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"/{bin,lib,share/neuronos/grammars}

# ── Copy binaries ──
echo "[1/5] Copying neuronos-cli..."
cp "$NEURONOS_CLI" "$STAGING_DIR/bin/"
strip "$STAGING_DIR/bin/neuronos-cli" 2>/dev/null || true

echo "[2/5] Copying shared libraries..."
cp "$LIBLLAMA" "$STAGING_DIR/lib/"
cp "$LIBGGML" "$STAGING_DIR/lib/"
strip "$STAGING_DIR/lib/"*.so 2>/dev/null || true
strip "$STAGING_DIR/lib/"*.dylib 2>/dev/null || true

# ── Copy grammars ──
echo "[3/5] Copying grammars..."
if [ -d "$REPO_ROOT/grammars" ]; then
    cp "$REPO_ROOT/grammars/"*.gbnf "$STAGING_DIR/share/neuronos/grammars/" 2>/dev/null || true
fi

# ── Create wrapper script ──
echo "[4/5] Creating wrapper script..."
cat > "$STAGING_DIR/bin/neuronos" << 'WRAPPER_EOF'
#!/usr/bin/env bash
## NeuronOS launcher — sets library path and runs neuronos-cli
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NEURONOS_HOME="$(cd "$SCRIPT_DIR/.." && pwd)"

export LD_LIBRARY_PATH="${NEURONOS_HOME}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DYLD_LIBRARY_PATH="${NEURONOS_HOME}/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export NEURONOS_GRAMMAR_DIR="${NEURONOS_HOME}/share/neuronos/grammars"

exec "$SCRIPT_DIR/neuronos-cli" "$@"
WRAPPER_EOF
chmod +x "$STAGING_DIR/bin/neuronos"

# ── Create README in package ──
cat > "$STAGING_DIR/README.md" << README_EOF
# NeuronOS v${VERSION}

Universal AI agent engine for edge devices.

## Quick Start

    # Interactive chat with BitNet 2B model
    ./bin/neuronos <model.gguf> run "Hello"

    # Agent mode with tools
    ./bin/neuronos <model.gguf> agent "List files in /tmp"

    # Start HTTP server (OpenAI-compatible)
    ./bin/neuronos <model.gguf> serve --port 8080

    # MCP server mode
    ./bin/neuronos <model.gguf> mcp

## Download a model

    # BitNet b1.58 2B (recommended, ~400MB)
    curl -L -o model.gguf \\
      https://huggingface.co/microsoft/BitNet-b1.58-2B-4T-gguf/resolve/main/ggml-model-i2_s.gguf

## Install system-wide

    sudo cp bin/neuronos bin/neuronos-cli /usr/local/bin/
    sudo cp lib/* /usr/local/lib/
    sudo ldconfig

## More info

    https://github.com/Neuron-OS/neuronos
README_EOF

# ── Create tarball ──
echo "[5/5] Creating tarball..."
cd /tmp
tar czf "$OUTPUT_FILE" "$RELEASE_NAME"
rm -rf "$STAGING_DIR"

SIZE=$(du -h "$OUTPUT_FILE" | cut -f1)
echo ""
echo "════════════════════════════════════════════"
echo "  ✓ Release packaged: ${OUTPUT_FILE}"
echo "  Size: ${SIZE}"
echo "════════════════════════════════════════════"
