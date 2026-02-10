#!/usr/bin/env bash
##
## NeuronOS — One-Line Installer
##
## Usage:
##   curl -sSL https://raw.githubusercontent.com/Neuron-OS/neuronos/main/scripts/install.sh | bash
##
## Options (env vars):
##   NEURONOS_VERSION=0.8.0     Pin a specific version (default: latest)
##   NEURONOS_INSTALL_DIR=PATH  Install location (default: ~/.neuronos)
##   NEURONOS_NO_MODEL=1        Skip model download
##
set -euo pipefail

# ── Configuration ──
GITHUB_REPO="Neuron-OS/neuronos"
DEFAULT_INSTALL_DIR="$HOME/.neuronos"
MODEL_REPO="microsoft/BitNet-b1.58-2B-4T-gguf"
MODEL_FILE="ggml-model-i2_s.gguf"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m' # No Color

info()  { echo -e "${BLUE}[info]${NC}  $*"; }
ok()    { echo -e "${GREEN}[ok]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC}  $*"; }
error() { echo -e "${RED}[error]${NC} $*" >&2; }
die()   { error "$*"; exit 1; }

# ── Detect platform ──
detect_platform() {
    local os arch

    os="$(uname -s)"
    arch="$(uname -m)"

    case "$os" in
        Linux)  os="linux" ;;
        Darwin) os="macos" ;;
        *)      die "Unsupported OS: $os. NeuronOS supports Linux and macOS." ;;
    esac

    case "$arch" in
        x86_64|amd64)  arch="x86_64" ;;
        aarch64|arm64) arch="arm64" ;;
        *)             die "Unsupported architecture: $arch. NeuronOS supports x86_64 and arm64." ;;
    esac

    PLATFORM="${os}-${arch}"
    info "Detected platform: ${BOLD}${PLATFORM}${NC}"
}

# ── Check dependencies ──
check_deps() {
    for cmd in curl tar; do
        if ! command -v "$cmd" &>/dev/null; then
            die "Required command not found: $cmd. Please install it first."
        fi
    done
}

# ── Get latest version from GitHub ──
get_latest_version() {
    if [ -n "${NEURONOS_VERSION:-}" ]; then
        VERSION="$NEURONOS_VERSION"
        info "Using pinned version: v${VERSION}"
        return
    fi

    info "Fetching latest release..."
    local api_url="https://api.github.com/repos/${GITHUB_REPO}/releases/latest"
    local response

    response=$(curl -sSL -w "%{http_code}" "$api_url" 2>/dev/null) || true
    local http_code="${response: -3}"
    local body="${response:0:${#response}-3}"

    if [ "$http_code" = "200" ]; then
        VERSION=$(echo "$body" | grep -oP '"tag_name":\s*"v?\K[^"]+' | head -1)
        if [ -n "$VERSION" ]; then
            info "Latest version: v${VERSION}"
            return
        fi
    fi

    # Fallback: list tags
    warn "No GitHub release found. Trying tags..."
    local tags_url="https://api.github.com/repos/${GITHUB_REPO}/tags"
    response=$(curl -sSL "$tags_url" 2>/dev/null) || true
    VERSION=$(echo "$response" | grep -oP '"name":\s*"v?\K[^"]+' | head -1)

    if [ -z "$VERSION" ]; then
        die "Could not determine latest version. Set NEURONOS_VERSION=x.y.z manually."
    fi
    info "Latest tag: v${VERSION}"
}

# ── Download and install ──
install_neuronos() {
    local install_dir="${NEURONOS_INSTALL_DIR:-$DEFAULT_INSTALL_DIR}"
    local tarball_name="neuronos-v${VERSION}-${PLATFORM}.tar.gz"
    local download_url="https://github.com/${GITHUB_REPO}/releases/download/v${VERSION}/${tarball_name}"
    local tmp_dir

    tmp_dir=$(mktemp -d)
    trap "rm -rf '$tmp_dir'" EXIT

    info "Downloading ${BOLD}${tarball_name}${NC}..."
    if ! curl -sSL --fail -o "$tmp_dir/$tarball_name" "$download_url"; then
        die "Download failed: $download_url
Check that the release exists: https://github.com/${GITHUB_REPO}/releases"
    fi
    ok "Downloaded $(du -h "$tmp_dir/$tarball_name" | cut -f1)"

    info "Installing to ${BOLD}${install_dir}${NC}..."
    mkdir -p "$install_dir"

    # Extract (strip the top-level directory from tarball)
    tar xzf "$tmp_dir/$tarball_name" -C "$tmp_dir"
    local extracted_dir="$tmp_dir/neuronos-v${VERSION}-${PLATFORM}"

    if [ ! -d "$extracted_dir" ]; then
        # Try without version prefix
        extracted_dir=$(find "$tmp_dir" -maxdepth 1 -type d -name "neuronos-*" | head -1)
    fi

    if [ ! -d "$extracted_dir" ]; then
        die "Extraction failed: could not find neuronos directory in tarball"
    fi

    # Copy files
    cp -r "$extracted_dir"/* "$install_dir/"
    chmod +x "$install_dir/bin/neuronos" "$install_dir/bin/neuronos-cli"

    ok "Installed to ${install_dir}"

    # ── Create config directory ──
    mkdir -p "$HOME/.neuronos"
    if [ ! -f "$HOME/.neuronos/mcp.json" ]; then
        cat > "$HOME/.neuronos/mcp.json" << 'MCP_EOF'
{
    "mcpServers": {}
}
MCP_EOF
        ok "Created default config: ~/.neuronos/mcp.json"
    fi
}

# ── Download model (optional) ──
download_model() {
    if [ "${NEURONOS_NO_MODEL:-}" = "1" ]; then
        info "Skipping model download (NEURONOS_NO_MODEL=1)"
        return
    fi

    local install_dir="${NEURONOS_INSTALL_DIR:-$DEFAULT_INSTALL_DIR}"
    local models_dir="$install_dir/models"
    local model_path="$models_dir/$MODEL_FILE"

    if [ -f "$model_path" ]; then
        ok "Model already exists: $model_path"
        return
    fi

    echo ""
    info "Downloading BitNet b1.58 2B model (~400MB)..."
    info "This is the recommended model for edge devices."
    echo ""

    mkdir -p "$models_dir"
    local model_url="https://huggingface.co/${MODEL_REPO}/resolve/main/${MODEL_FILE}"

    if curl -L --fail --progress-bar -o "$model_path" "$model_url"; then
        local size
        size=$(du -h "$model_path" | cut -f1)
        ok "Model downloaded: ${model_path} (${size})"
    else
        warn "Model download failed. You can download it later:"
        warn "  curl -L -o $model_path $model_url"
        rm -f "$model_path"
    fi
}

# ── Setup PATH ──
setup_path() {
    local install_dir="${NEURONOS_INSTALL_DIR:-$DEFAULT_INSTALL_DIR}"
    local bin_dir="$install_dir/bin"
    local shell_rc=""

    # Check if already in PATH
    if echo "$PATH" | tr ':' '\n' | grep -q "^${bin_dir}$"; then
        ok "Already in PATH"
        return
    fi

    # Detect shell config file
    local shell_name
    shell_name=$(basename "${SHELL:-/bin/bash}")
    case "$shell_name" in
        zsh)  shell_rc="$HOME/.zshrc" ;;
        bash)
            if [ -f "$HOME/.bashrc" ]; then
                shell_rc="$HOME/.bashrc"
            elif [ -f "$HOME/.bash_profile" ]; then
                shell_rc="$HOME/.bash_profile"
            else
                shell_rc="$HOME/.bashrc"
            fi
            ;;
        fish) shell_rc="$HOME/.config/fish/config.fish" ;;
        *)    shell_rc="$HOME/.profile" ;;
    esac

    local path_line="export PATH=\"${bin_dir}:\$PATH\""
    if [ "$shell_name" = "fish" ]; then
        path_line="set -gx PATH ${bin_dir} \$PATH"
    fi

    # Check if path entry already in rc file
    if [ -f "$shell_rc" ] && grep -q "neuronos" "$shell_rc" 2>/dev/null; then
        ok "PATH already configured in $shell_rc"
        return
    fi

    echo "" >> "$shell_rc"
    echo "# NeuronOS" >> "$shell_rc"
    echo "$path_line" >> "$shell_rc"

    ok "Added to PATH in $shell_rc"
    warn "Run: source $shell_rc  (or open a new terminal)"
}

# ── Main ──
main() {
    echo ""
    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "${BOLD}  NeuronOS Installer${NC}"
    echo -e "${BOLD}  The Android of AI — Edge Agent Engine${NC}"
    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo ""

    check_deps
    detect_platform
    get_latest_version
    install_neuronos
    download_model
    setup_path

    local install_dir="${NEURONOS_INSTALL_DIR:-$DEFAULT_INSTALL_DIR}"

    echo ""
    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo -e "${GREEN}  ✓ NeuronOS v${VERSION} installed!${NC}"
    echo -e "${BOLD}════════════════════════════════════════════${NC}"
    echo ""
    echo "  Quick start:"
    echo ""
    if [ -f "$install_dir/models/$MODEL_FILE" ]; then
        echo "    neuronos $install_dir/models/$MODEL_FILE run \"Hello\""
        echo "    neuronos $install_dir/models/$MODEL_FILE agent \"List files\""
    else
        echo "    # Download a model first:"
        echo "    curl -L -o model.gguf \\"
        echo "      https://huggingface.co/${MODEL_REPO}/resolve/main/${MODEL_FILE}"
        echo ""
        echo "    neuronos model.gguf run \"Hello\""
    fi
    echo ""
    echo "  More info: https://github.com/${GITHUB_REPO}"
    echo ""
}

main "$@"
