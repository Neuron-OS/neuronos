#!/bin/sh
# ============================================================
# NeuronOS — One-Command Installer (HW-Aware)
#
# Remote:  curl -fsSL https://neuronos.dev/install.sh | sh
# Local:   ./install.sh
#
# Detects hardware (RAM, CPU) and downloads the best model
# from a catalog of 1.58-bit ternary GGUF models.
#
# Environment overrides:
#   NEURONOS_INSTALL   Install dir (default: ~/.local/bin)
#   NEURONOS_MODEL     Force specific model ID (e.g. "falcon3-7b")
# ============================================================
set -eu

# ---- Config ----
GITHUB_REPO="Neuron-OS/neuronos"
DATA_DIR="${HOME}/.neuronos"
MODELS_DIR="${DATA_DIR}/models"

# ---- Model Catalog (mirrors neuronos_model_registry.c) ----
# Format: id|display_name|url|size_mb|min_ram_mb|params_b
CATALOG="
bitnet-2b|BitNet b1.58 2B (Microsoft)|https://huggingface.co/microsoft/bitnet-b1.58-2B-4T-gguf/resolve/main/ggml-model-i2_s.gguf|780|1500|2
falcon3-1b|Falcon3 1B Instruct (TII)|https://huggingface.co/tiiuae/Falcon3-1B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf|420|800|1
falcon3-3b|Falcon3 3B Instruct (TII)|https://huggingface.co/tiiuae/Falcon3-3B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf|1100|2000|3
falcon3-7b|Falcon3 7B Instruct (TII)|https://huggingface.co/tiiuae/Falcon3-7B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf|2600|4000|7
falcon3-10b|Falcon3 10B Instruct (TII)|https://huggingface.co/tiiuae/Falcon3-10B-Instruct-1.58bit-GGUF/resolve/main/ggml-model-i2_s.gguf|3800|6000|10
"

# ---- Colors (only if terminal) ----
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

# ---- Cleanup ----
TEMP_DIR=""
cleanup() { [ -n "$TEMP_DIR" ] && rm -rf "$TEMP_DIR"; }
trap cleanup EXIT

# ---- Detect platform ----
detect_platform() {
    OS="$(uname -s)"
    ARCH="$(uname -m)"
    case "$OS" in
        Linux)  OS="linux" ;;
        Darwin) OS="darwin" ;;
        MINGW*|MSYS*|CYGWIN*) OS="windows" ;;
        *) err "Unsupported OS: $OS"; exit 1 ;;
    esac
    case "$ARCH" in
        x86_64|amd64)  ARCH="amd64" ;;
        aarch64|arm64) ARCH="arm64" ;;
        *) err "Unsupported arch: $ARCH"; exit 1 ;;
    esac
}

# ---- Detect RAM in MB ----
detect_ram() {
    RAM_MB=0
    case "$(uname -s)" in
        Linux)
            RAM_KB=$(grep '^MemTotal:' /proc/meminfo 2>/dev/null | awk '{print $2}')
            [ -n "$RAM_KB" ] && RAM_MB=$((RAM_KB / 1024))
            ;;
        Darwin)
            RAM_BYTES=$(sysctl -n hw.memsize 2>/dev/null)
            [ -n "$RAM_BYTES" ] && RAM_MB=$((RAM_BYTES / 1024 / 1024))
            ;;
        *)
            RAM_MB=4096  # fallback
            ;;
    esac
    [ "$RAM_MB" -eq 0 ] && RAM_MB=4096
}

# ---- Select best model for the hardware ----
select_model() {
    # If user forced a model ID
    if [ -n "${NEURONOS_MODEL:-}" ]; then
        SELECTED_LINE=$(echo "$CATALOG" | grep "^${NEURONOS_MODEL}|" | head -1)
        if [ -z "$SELECTED_LINE" ]; then
            err "Unknown model: ${NEURONOS_MODEL}"
            err "Available: bitnet-2b, falcon3-1b, falcon3-3b, falcon3-7b, falcon3-10b"
            exit 1
        fi
    else
        # Find the biggest model that fits in RAM (best quality)
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

        # Fallback: smallest model if nothing picked
        if [ -z "$SELECTED_LINE" ]; then
            SELECTED_LINE=$(echo "$CATALOG" | grep -v '^$' | sort -t'|' -k5 -n | head -1)
        fi
    fi

    MODEL_ID=$(echo "$SELECTED_LINE" | cut -d'|' -f1)
    MODEL_NAME=$(echo "$SELECTED_LINE" | cut -d'|' -f2)
    MODEL_URL=$(echo "$SELECTED_LINE" | cut -d'|' -f3)
    MODEL_SIZE=$(echo "$SELECTED_LINE" | cut -d'|' -f4)
    MODEL_MIN_RAM=$(echo "$SELECTED_LINE" | cut -d'|' -f5)
    MODEL_PARAMS=$(echo "$SELECTED_LINE" | cut -d'|' -f6)
}

# ---- Install dir ----
find_install_dir() {
    if [ -n "${NEURONOS_INSTALL:-}" ]; then
        INSTALL_DIR="$NEURONOS_INSTALL"
    elif [ -w /usr/local/bin ] || [ "$(id -u)" = "0" ]; then
        INSTALL_DIR="/usr/local/bin"
    else
        INSTALL_DIR="${HOME}/.local/bin"
    fi
}

# ---- Find local build (dev install from repo) ----
find_local_build() {
    SCRIPT_DIR="$(cd "$(dirname "$0")" 2>/dev/null && pwd)" || SCRIPT_DIR=""
    LOCAL_BIN=""
    [ -z "$SCRIPT_DIR" ] && return
    for d in \
        "$(dirname "$SCRIPT_DIR")/build-static/bin/neuronos-cli" \
        "$(dirname "$SCRIPT_DIR")/build/bin/neuronos-cli" \
        "${SCRIPT_DIR}/build-static/bin/neuronos-cli" \
        "${SCRIPT_DIR}/build/bin/neuronos-cli"; do
        [ -f "$d" ] && LOCAL_BIN="$d" && return
    done
}

# ---- Install binary ----
install_binary() {
    mkdir -p "$INSTALL_DIR"
    if [ -n "$LOCAL_BIN" ]; then
        cp "$LOCAL_BIN" "${INSTALL_DIR}/neuronos"
        dim "from local build"
    else
        TEMP_DIR="$(mktemp -d)"

        if [ "$OS" = "windows" ]; then
            URL="https://github.com/${GITHUB_REPO}/releases/latest/download/neuronos-${OS}-${ARCH}.zip"
            FILE="${TEMP_DIR}/neuronos.zip"
            info "Downloading neuronos-${OS}-${ARCH}.zip..."
            curl -fSL --progress-bar -o "$FILE" "$URL" || { err "Download failed."; exit 1; }

            # Unzip (try unzip, then python)
            if command -v unzip >/dev/null 2>&1; then
                unzip -q -o "$FILE" -d "$TEMP_DIR"
            else
                # Very basic python unzip
                python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1],'r').extractall(sys.argv[2])" "$FILE" "$TEMP_DIR"
            fi
        else
            URL="https://github.com/${GITHUB_REPO}/releases/latest/download/neuronos-${OS}-${ARCH}.tar.gz"
            info "Downloading neuronos-${OS}-${ARCH}..."
            curl -fSL --progress-bar -o "${TEMP_DIR}/neuronos.tar.gz" "$URL" \
                || { err "Download failed. Build from source or check $URL"; exit 1; }
            tar -xzf "${TEMP_DIR}/neuronos.tar.gz" -C "${TEMP_DIR}"
        fi

        # Find binary (handle varied internal structure)
        BINARY_FOUND=$(find "$TEMP_DIR" -type f -name "neuronos-cli*" -o -name "neuronos*" | grep -v "\.tar\.gz" | grep -v "\.zip" | head -1)

        if [ -z "$BINARY_FOUND" ]; then
            err "Binary not found in archive!"
            exit 1
        fi

        cp "$BINARY_FOUND" "${INSTALL_DIR}/neuronos"
    fi
    chmod +x "${INSTALL_DIR}/neuronos"
}

# ---- Install model (HW-aware) ----
install_model() {
    # Create model subdirectory
    MODEL_DIR="${MODELS_DIR}/${MODEL_ID}"
    MODEL_FILE="ggml-model-i2_s.gguf"
    mkdir -p "$MODEL_DIR"

    # Check if already downloaded
    if [ -f "${MODEL_DIR}/${MODEL_FILE}" ]; then
        dim "model already installed (${MODEL_NAME})"
        return
    fi

    # Check for local model file (dev mode)
    if [ -n "${SCRIPT_DIR:-}" ]; then
        for d in \
            "${SCRIPT_DIR}/models/BitNet-b1.58-2B-4T-gguf/${MODEL_FILE}" \
            "$(dirname "${SCRIPT_DIR:-}")/neuronos/models/BitNet-b1.58-2B-4T-gguf/${MODEL_FILE}"; do
            if [ -f "$d" ]; then
                ln -sf "$d" "${MODEL_DIR}/${MODEL_FILE}"
                dim "model linked from local"
                return
            fi
        done
    fi

    info "Downloading ${MODEL_NAME} (~${MODEL_SIZE} MB)..."
    dim "${MODEL_PARAMS}B params • 1.58-bit ternary • CPU-optimized"
    printf "\n"

    curl -fSL --progress-bar -C - -o "${MODEL_DIR}/${MODEL_FILE}" "$MODEL_URL" \
        || { err "Model download failed. Try: NEURONOS_MODEL=falcon3-1b ./install.sh"; exit 1; }
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
        dim "added ${INSTALL_DIR} to $(basename "$RC")"
}

# ════════════════════════════════════════════════════════
# MAIN
# ════════════════════════════════════════════════════════
printf "\n"
info "NeuronOS Installer"
printf "\n"

detect_platform
detect_ram
find_install_dir
find_local_build
select_model

dim "${OS}/${ARCH} • ${RAM_MB} MB RAM → ${MODEL_NAME} (${MODEL_PARAMS}B)"
dim "Install: ${INSTALL_DIR}/neuronos"
printf "\n"

info "[1/3] Binary..."
install_binary

info "[2/3] AI Model (${MODEL_NAME})..."
install_model

info "[3/3] PATH..."
setup_path

printf "\n"
if "${INSTALL_DIR}/neuronos" --help >/dev/null 2>&1; then
    ok "✓ Installed. Run:"
    printf "\n    ${C}neuronos${N}\n\n"
    dim "Manage models: neuronos model list | neuronos model recommend"
    printf "\n"
else
    ok "✓ Installed to ${INSTALL_DIR}/neuronos"
    printf "\n    Restart your shell, then run: ${C}neuronos${N}\n\n"
fi
