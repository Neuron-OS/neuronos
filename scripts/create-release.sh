#!/usr/bin/env bash
##
## NeuronOS — Create GitHub Release (local)
##
## Usage: ./scripts/create-release.sh <version>
##   e.g.: ./scripts/create-release.sh 0.8.0
##
## Prerequisites:
##   - gh CLI installed (brew install gh / apt install gh)
##   - Authenticated: gh auth login
##   - All tests passing
##   - Clean git state (or use --force)
##
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

die() { echo -e "${RED}ERROR:${NC} $*" >&2; exit 1; }
ok()  { echo -e "${GREEN}✓${NC} $*"; }
info() { echo -e "${BOLD}→${NC} $*"; }

# ── Parse args ──
VERSION="${1:-}"
FORCE="${2:-}"

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version> [--force]"
    echo "  e.g.: $0 0.8.0"
    exit 1
fi

TAG="v${VERSION}"

echo ""
echo -e "${BOLD}════════════════════════════════════════════${NC}"
echo -e "${BOLD}  NeuronOS Release Creator${NC}"
echo -e "${BOLD}  Version: ${TAG}${NC}"
echo -e "${BOLD}════════════════════════════════════════════${NC}"
echo ""

# ── Check prerequisites ──
info "Checking prerequisites..."

command -v gh &>/dev/null || die "gh CLI not installed. Install: https://cli.github.com/"
command -v cmake &>/dev/null || die "cmake not installed"

# Check gh auth
gh auth status &>/dev/null || die "Not authenticated. Run: gh auth login"
ok "gh CLI authenticated"

cd "$REPO_ROOT"

# ── Check git state ──
if [ "$FORCE" != "--force" ]; then
    if [ -n "$(git status --porcelain)" ]; then
        die "Working tree is dirty. Commit changes first or use --force"
    fi
    ok "Git working tree clean"
fi

# ── Run tests ──
info "Running tests..."
export LD_LIBRARY_PATH="${REPO_ROOT}/build/3rdparty/llama.cpp/src:${REPO_ROOT}/build/3rdparty/llama.cpp/ggml/src"

if [ -f "$REPO_ROOT/build/bin/test_hal" ]; then
    "$REPO_ROOT/build/bin/test_hal" > /dev/null 2>&1 || die "test_hal FAILED"
    ok "test_hal passed"
else
    die "Tests not built. Run: cmake --build build -j\$(nproc)"
fi

if [ -f "$REPO_ROOT/build/bin/test_memory" ]; then
    "$REPO_ROOT/build/bin/test_memory" > /dev/null 2>&1 || die "test_memory FAILED"
    ok "test_memory passed"
fi

if [ -f "$REPO_ROOT/build/bin/test_engine" ]; then
    info "test_engine requires model — skipping (run manually)"
fi

# ── Package ──
info "Packaging release..."
bash "$SCRIPT_DIR/package-release.sh" "$VERSION"
ok "Package created"

TARBALL=$(find "$REPO_ROOT" -maxdepth 1 -name "neuronos-v${VERSION}-*.tar.gz" | head -1)
if [ -z "$TARBALL" ]; then
    die "Tarball not found after packaging"
fi

# ── Create git tag ──
if git rev-parse "$TAG" >/dev/null 2>&1; then
    info "Tag $TAG already exists"
else
    info "Creating tag ${TAG}..."
    git tag -a "$TAG" -m "Release ${TAG}"
    ok "Tag created: ${TAG}"
fi

# ── Push tag ──
info "Pushing tag to origin..."
git push origin "$TAG" 2>/dev/null || info "Tag push failed (may already exist on remote)"

# ── Create GitHub Release ──
info "Creating GitHub Release..."

RELEASE_NOTES="## NeuronOS ${TAG}

Universal AI agent engine for edge devices.

### Quick Install
\`\`\`bash
curl -sSL https://raw.githubusercontent.com/Neuron-OS/neuronos/main/scripts/install.sh | bash
\`\`\`

### Features
- ReAct agent engine with 12 built-in tools
- MCP Client + Server (JSON-RPC 2.0 STDIO)
- SQLite+FTS5 persistent memory (MemGPT 3-tier)
- BitNet 1.58-bit ternary model support
- OpenAI-compatible HTTP server
- Zero runtime dependencies (C11 pure)

### Downloads
Download the tarball for your platform and extract:
\`\`\`bash
tar xzf neuronos-${TAG}-linux-x86_64.tar.gz
./neuronos-${TAG}-linux-x86_64/bin/neuronos model.gguf run \"Hello\"
\`\`\`
"

gh release create "$TAG" \
    --title "NeuronOS ${TAG}" \
    --notes "$RELEASE_NOTES" \
    "$TARBALL" \
    || die "Failed to create release"

ok "Release created!"

echo ""
echo -e "${BOLD}════════════════════════════════════════════${NC}"
echo -e "${GREEN}  ✓ Release ${TAG} published!${NC}"
echo -e "${BOLD}════════════════════════════════════════════${NC}"
echo ""
echo "  View: https://github.com/Neuron-OS/neuronos/releases/tag/${TAG}"
echo "  Install: curl -sSL https://raw.githubusercontent.com/Neuron-OS/neuronos/main/scripts/install.sh | bash"
echo ""
