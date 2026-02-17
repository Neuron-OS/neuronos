#!/bin/sh
# ============================================================
# NeuronOS — Universal Installer & Launcher
#
# Usage:
#   curl -fsSL https://neuronos.dev/install.sh | sh
#   ./install.sh [options]
#
# Options:
#   --build       Force build from source (requires cmake/compiler)
#   --wasm        Build for Web/WASM (requires emscripten)
#   --clean       Clean build directory before building
#   --help        Show this help
#
# Environment:
#   NEURONOS_INSTALL   Install dir (default: ~/.local/bin)
#   NEURONOS_MODEL     Force specific model ID
#   NEURONOS_VULKAN    Force Vulkan ON/OFF (default: auto-detect)
# ============================================================
set -eu

# ---- Config ----
GITHUB_REPO="Neuron-OS/neuronos"
DATA_DIR="${HOME}/.neuronos"
MODELS_DIR="${DATA_DIR}/models"
BUILD_DIR="build"

# ---- Model Catalog (mirrors neuronos_model_registry.c) ----
CATALOG="
bitnet-2b|BitNet b1.58 2B (Microsoft)|https://huggingface.co/microsoft/bitnet-b1.58-2B-4T-gguf/resolve/main/ggml-model-i2_s.gguf|780|1500|2
falcon3-1b|Falcon3 1B Instruct (TII)|https://huggingface.co/tiiuae/Falcon3-1B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf|420|800|1
falcon3-3b|Falcon3 3B Instruct (TII)|https://huggingface.co/tiiuae/Falcon3-3B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf|1100|2000|3
falcon3-7b|Falcon3 7B Instruct (TII)|https://huggingface.co/tiiuae/Falcon3-7B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf|2600|4000|7
falcon3-10b|Falcon3 10B Instruct (TII)|https://huggingface.co/tiiuae/Falcon3-10B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf|3800|6000|10
"

# ---- Output Helpers ----
if [ -t 1 ]; then
    C='\033[36m' G='\033[32m' R='\033[31m' Y='\033[33m' D='\033[2m' N='\033[0m'
else
    C='' G='' R='' Y='' D='' N=''
fi

info() { printf "  ${C}%s${N}\n" "$1"; }
ok()   { printf "  ${G}%s${N}\n" "$1"; }
err()  { printf "  ${R}%s${N}\n" "$1" >&2; }
warn() { printf "  ${Y}%s${N}\n" "$1"; }
dim()  { printf "  ${D}%s${N}\n" "$1"; }

# ---- Args ----
DO_BUILD=false
DO_WASM=false
DO_CLEAN=false

for arg in "$@"; do
    case $arg in
        --build) DO_BUILD=true ;;
        --wasm)  DO_WASM=true ;;
        --clean) DO_CLEAN=true ;;
        --help)
            echo "Usage: ./install.sh [--build] [--wasm] [--clean]"
            exit 0
            ;;
    esac
done

# ---- Detect OS & Arch ----
detect_system() {
    OS="$(uname -s)"
    ARCH="$(uname -m)"

    # Android (Termux) detection
    if [ -n "${ANDROID_ROOT:-}" ] || [ -f "/system/build.prop" ]; then
        OS="android"
    fi

    # Distro detection (Linux)
    DISTRO="unknown"
    if [ "$OS" = "Linux" ] && [ -f "/etc/os-release" ]; then
        . /etc/os-release
        DISTRO="${ID:-unknown}"
    fi

    case "$OS" in
        Linux)   OS_TYPE="linux" ;;
        Darwin)  OS_TYPE="macos" ;;
        android) OS_TYPE="android" ;;
        MINGW*|MSYS*|CYGWIN*) OS_TYPE="windows" ;;
        *)       OS_TYPE="unknown" ;;
    esac

    case "$ARCH" in
        x86_64|amd64)  ARCH="x86_64" ;;
        aarch64|arm64) ARCH="arm64" ;;
        *)             ARCH="unknown" ;;
    esac
}

# ---- Dependency Management ----
install_deps() {
    info "Checking dependencies for $OS_TYPE..."

    case "$OS_TYPE" in
        linux)
            if [ "$DISTRO" = "ubuntu" ] || [ "$DISTRO" = "debian" ] || [ "$DISTRO" = "pop" ] || [ "$DISTRO" = "kali" ]; then
                if ! command -v cmake >/dev/null || ! command -v vulkaninfo >/dev/null; then
                    info "Installing build tools & Vulkan SDK (requires sudo)..."
                    sudo apt-get update && sudo apt-get install -y build-essential cmake vulkan-tools libvulkan-dev curl git
                fi
            elif [ "$DISTRO" = "fedora" ]; then
                 if ! command -v cmake >/dev/null; then
                    info "Installing deps (requires sudo)..."
                    sudo dnf install -y cmake gcc-c++ vulkan-tools vulkan-loader-devel curl git
                 fi
            elif [ "$DISTRO" = "arch" ] || [ "$DISTRO" = "manjaro" ]; then
                 if ! command -v cmake >/dev/null; then
                    info "Installing deps (requires sudo)..."
                    sudo pacman -S --needed cmake base-devel vulkan-devel vulkan-tools git curl
                 fi
            else
                warn "Unknown Linux distro '$DISTRO'. Please install CMake and Vulkan SDK manually."
            fi
            ;;
        macos)
            if ! command -v brew >/dev/null; then
                err "Homebrew not found. Please install it first: https://brew.sh"
                exit 1
            fi
            if ! command -v cmake >/dev/null; then
                info "Installing CMake via Homebrew..."
                brew install cmake
            fi
            # Vulkan on macOS (MoltenVK) - strictly optional for users, but good for devs
            # brew install vulkan-loader
            ;;
        android)
            # Termux
            if ! command -v cmake >/dev/null; then
                info "Installing build tools via pkg..."
                pkg update && pkg install -y build-essential cmake vulkan-loader-android git curl
            fi
            ;;
        windows)
            # Git Bash / MSYS2 / WSL
            if ! command -v cmake >/dev/null; then
                warn "CMake not found."
                warn "Please install CMake and Vulkan SDK from https://cmake.org and https://vulkan.lunarg.com"
                warn "Or use 'winget install Kitware.CMake KhronosGroup.VulkanSDK'"
            fi
            ;;
    esac
}

# ---- Build from Source ----
build_source() {
    info "Building from source..."

    # Check if we are in the repo
    if [ ! -f "CMakeLists.txt" ]; then
        # Try to clone if not here
        warn "Not in a NeuronOS repository. Cloning..."
        git clone https://github.com/$GITHUB_REPO.git neuronos-build
        cd neuronos-build
    fi

    setup_build_flags="-DNEURONOS_BUILD_TESTS=OFF"

    # Auto-enable Vulkan if available
    if [ "${NEURONOS_VULKAN:-auto}" != "OFF" ]; then
        if command -v vulkaninfo >/dev/null || [ "$OS_TYPE" = "macos" ] || [ "$OS_TYPE" = "android" ]; then
             setup_build_flags="$setup_build_flags -DNEURONOS_VULKAN=ON"
             ok "Vulkan support enabled."
        fi
    fi

    if [ "$DO_CLEAN" = true ]; then
        rm -rf "$BUILD_DIR"
    fi

    # WASM Build
    if [ "$DO_WASM" = true ]; then
        if [ ! -f "neuronos/wasm/build_wasm.sh" ]; then
            err "WASM build script not found."
            exit 1
        fi
        info "Building for Web/WASM..."
        # Check emcc
        if ! command -v emcc >/dev/null; then
            err "Emscripten (emcc) not found. Please install Emscripten SDK."
            exit 1
        fi

        bash neuronos/wasm/build_wasm.sh --mt-only
        ok "WASM build complete. Artifacts in neuronos/wasm/dist/"
        # We don't 'install' WASM to bin, it's a web artifact.
        exit 0
    fi

    # Native Build
    cmake -B "$BUILD_DIR" -S . -DCMAKE_BUILD_TYPE=Release $setup_build_flags
    cmake --build "$BUILD_DIR" --config Release --parallel

    # Install binary to system
    BIN_SRC="$BUILD_DIR/bin/neuronos"
    if [ -f "$BIN_SRC" ]; then
        install -d "$INSTALL_DIR"
        install "$BIN_SRC" "$INSTALL_DIR/neuronos"
        ok "Installed locally built binary to $INSTALL_DIR/neuronos"
    else
        err "Build failed: Binary not found at $BIN_SRC"
        exit 1
    fi
}

# ---- Model Logic (Shared) ----
    detect_ram() {
    RAM_MB=0
    case "$OS_TYPE" in
        linux)
            RAM_KB=$(grep '^MemTotal:' /proc/meminfo 2>/dev/null | awk '{print $2}')
            [ -n "$RAM_KB" ] && RAM_MB=$((RAM_KB / 1024))
            ;;
        macos)
            RAM_BYTES=$(sysctl -n hw.memsize 2>/dev/null)
            [ -n "$RAM_BYTES" ] && RAM_MB=$((RAM_BYTES / 1024 / 1024))
            ;;
        *) RAM_MB=4096 ;;
    esac
    if [ "$RAM_MB" -eq 0 ]; then
        RAM_MB=4096
    fi
}

select_model() {
    if [ -n "${NEURONOS_MODEL:-}" ]; then
        SELECTED_LINE=$(echo "$CATALOG" | grep "^${NEURONOS_MODEL}|" | head -1)
        if [ -z "$SELECTED_LINE" ]; then
            err "Unknown model: ${NEURONOS_MODEL}"
            exit 1
        fi
    else
        SELECTED_LINE=""
        BEST_PARAMS=0
        echo "$CATALOG" | while IFS='|' read -r id name url size min_ram params; do
            [ -z "$id" ] && continue
            if [ "$min_ram" -le "$RAM_MB" ] && [ "$params" -gt "$BEST_PARAMS" ]; then
                BEST_PARAMS="$params"
                echo "$id|$name|$url|$size|$min_ram|$params"
            fi
        done | tail -1 > /tmp/_neuronos_model_pick 2>/dev/null || true
        SELECTED_LINE=$(cat /tmp/_neuronos_model_pick 2>/dev/null || echo "")
        rm -f /tmp/_neuronos_model_pick

        if [ -z "$SELECTED_LINE" ]; then
            SELECTED_LINE=$(echo "$CATALOG" | grep -v '^$' | sort -t'|' -k5 -n | head -1)
        fi
    fi

    MODEL_ID=$(echo "$SELECTED_LINE" | cut -d'|' -f1)
    MODEL_NAME=$(echo "$SELECTED_LINE" | cut -d'|' -f2)
    MODEL_URL=$(echo "$SELECTED_LINE" | cut -d'|' -f3)
    MODEL_SIZE=$(echo "$SELECTED_LINE" | cut -d'|' -f4)
    MODEL_PARAMS=$(echo "$SELECTED_LINE" | cut -d'|' -f6)
}

install_model() {
    MODEL_DIR="${MODELS_DIR}/${MODEL_ID}"
    MODEL_FILE="ggml-model-i2_s.gguf"
    mkdir -p "$MODEL_DIR"

    if [ -f "${MODEL_DIR}/${MODEL_FILE}" ]; then
        dim "Model ${MODEL_NAME} already installed."
        return
    fi

    info "Downloading ${MODEL_NAME} (~${MODEL_SIZE} MB)..."
    curl -fSL --progress-bar -C - -o "${MODEL_DIR}/${MODEL_FILE}" "$MODEL_URL" || \
        { err "Download failed."; exit 1; }
}

# ---- Setup PATH ----
setup_path() {
    case ":${PATH}:" in
        *":${INSTALL_DIR}:"*) return ;;
    esac
    RC=""
    case "$(basename "${SHELL:-sh}")" in
        zsh)  RC="${HOME}/.zshrc" ;;
        bash) RC="${HOME}/.bashrc" ;;
        *)    RC="${HOME}/.profile" ;;
    esac
    [ -n "$RC" ] && ! grep -q "${INSTALL_DIR}" "$RC" 2>/dev/null && \
        printf '\nexport PATH="%s:$PATH"\n' "$INSTALL_DIR" >> "$RC" && \
        dim "Added ${INSTALL_DIR} to $(basename "$RC")"
}

# ==================== MAIN ====================

detect_system
detect_ram

# Define Install Dir
if [ -n "${NEURONOS_INSTALL:-}" ]; then
    INSTALL_DIR="$NEURONOS_INSTALL"
elif [ -w /usr/local/bin ] || [ "$(id -u)" = "0" ]; then
    INSTALL_DIR="/usr/local/bin"
else
    INSTALL_DIR="${HOME}/.local/bin"
fi

info "NeuronOS Setup ($OS_TYPE/$ARCH)"

# 1. Check if we should build
if [ "$DO_BUILD" = true ] || [ "$DO_WASM" = true ] || [ -f "CMakeLists.txt" ]; then
    install_deps
    build_source
    # If we built WASM, we exit inside build_source.
    # If we built native, we proceed to model install.
else
    # Binary Download Mode — fetch pre-built binary from GitHub Releases
    download_binary() {
        # Determine release tag
        RELEASE_TAG=$(curl -fsSL "https://api.github.com/repos/${GITHUB_REPO}/releases/latest" \
            | grep '"tag_name"' | head -1 | cut -d'"' -f4 2>/dev/null || echo "")

        if [ -z "$RELEASE_TAG" ]; then
            err "Could not fetch latest release from GitHub."
            err "Try: curl -fsSL https://raw.githubusercontent.com/${GITHUB_REPO}/main/install.sh | sh -s -- --build"
            exit 1
        fi

        VERSION="${RELEASE_TAG#v}"

        # Map to release asset name
        case "${OS_TYPE}-${ARCH}" in
            linux-x86_64)   ASSET="neuronos-linux-x86_64.tar.gz" ;;
            linux-arm64)    ASSET="neuronos-linux-arm64.tar.gz" ;;
            macos-arm64)    ASSET="neuronos-macos-arm64.tar.gz" ;;
            macos-x86_64)   ASSET="neuronos-macos-x86_64.tar.gz" ;;
            *)
                warn "No pre-built binary for ${OS_TYPE}-${ARCH}."
                warn "Building from source instead..."
                install_deps
                git clone --recursive "https://github.com/${GITHUB_REPO}.git" /tmp/neuronos-build
                cd /tmp/neuronos-build
                build_source
                return
                ;;
        esac

        DOWNLOAD_URL="https://github.com/${GITHUB_REPO}/releases/download/${RELEASE_TAG}/${ASSET}"
        TMPDIR=$(mktemp -d)

        info "Downloading NeuronOS ${RELEASE_TAG} for ${OS_TYPE}/${ARCH}..."
        curl -fSL --progress-bar -o "${TMPDIR}/${ASSET}" "$DOWNLOAD_URL" || {
            err "Download failed: ${DOWNLOAD_URL}"
            err "Try building from source: curl -fsSL https://raw.githubusercontent.com/${GITHUB_REPO}/main/install.sh | sh -s -- --build"
            rm -rf "$TMPDIR"
            exit 1
        }

        # Extract
        info "Installing to ${INSTALL_DIR}..."
        mkdir -p "$INSTALL_DIR"
        cd "$TMPDIR"
        tar xzf "$ASSET"

        # Find and install binary
        BIN=$(find . -name "neuronos-cli" -type f | head -1)
        if [ -z "$BIN" ]; then
            err "neuronos-cli not found in archive"
            rm -rf "$TMPDIR"
            exit 1
        fi

        install -m 755 "$BIN" "${INSTALL_DIR}/neuronos-cli"

        # Create neuronos wrapper
        cat > "${INSTALL_DIR}/neuronos" << 'WRAP'
#!/bin/sh
exec "$(dirname "$0")/neuronos-cli" "$@"
WRAP
        chmod +x "${INSTALL_DIR}/neuronos"

        # Copy grammars if present
        GRAMMAR_SRC=$(find . -type d -name "grammars" | head -1)
        if [ -n "$GRAMMAR_SRC" ]; then
            mkdir -p "${DATA_DIR}/grammars"
            cp "$GRAMMAR_SRC"/*.gbnf "${DATA_DIR}/grammars/" 2>/dev/null || true
        fi

        rm -rf "$TMPDIR"
        ok "Installed neuronos ${RELEASE_TAG} to ${INSTALL_DIR}"
    }

    download_binary
fi

# 2. Install Model
select_model
dim "Selected: ${MODEL_NAME}"
install_model

# 3. Path
setup_path

# 4. Welcome
echo ""
echo "  ╔══════════════════════════════════════════════════╗"
echo "  ║  ${G}NeuronOS installed successfully${N}                ║"
echo "  ║                                                  ║"
echo "  ║  Quick start:                                    ║"
echo "  ║    ${C}neuronos chat${N}          Interactive agent       ║"
echo "  ║    ${C}neuronos agent \"...\"${N}   One-shot task           ║"
echo "  ║    ${C}neuronos serve${N}         HTTP API server         ║"
echo "  ║    ${C}neuronos mcp${N}           MCP server for Claude   ║"
echo "  ║                                                  ║"
echo "  ║  Model: ${MODEL_NAME}"
echo "  ║  Path:  ${INSTALL_DIR}/neuronos"
echo "  ╚══════════════════════════════════════════════════╝"
echo ""
info "Run ${C}neuronos chat${N} to start."

# Hint to reload shell if PATH was modified
case ":${PATH}:" in
    *":${INSTALL_DIR}:"*) ;;
    *) warn "Restart your shell or run: export PATH=\"${INSTALL_DIR}:\$PATH\"" ;;
esac
