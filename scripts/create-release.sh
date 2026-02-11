#!/usr/bin/env bash
##
## NeuronOS — Create Release
##
## Bumps version in neuronos.h, commits, tags, and pushes.
## This triggers the release.yml workflow which builds for 3 platforms
## and publishes a GitHub Release with all binaries.
##
## Usage:
##   ./scripts/create-release.sh <version>
##   ./scripts/create-release.sh patch        # auto-bump patch (0.8.0 → 0.8.1)
##   ./scripts/create-release.sh minor        # auto-bump minor (0.8.0 → 0.9.0)
##   ./scripts/create-release.sh major        # auto-bump major (0.8.0 → 1.0.0)
##   ./scripts/create-release.sh 0.9.0        # explicit version
##
## Options:
##   --dry-run    Show what would happen, but don't do it
##   --force      Skip dirty tree check
##   --no-push    Commit + tag, but don't push (manual push later)
##
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HEADER_FILE="$REPO_ROOT/neuronos/include/neuronos/neuronos.h"

# ── Colors ──
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BOLD='\033[1m'; NC='\033[0m'
die()  { echo -e "${RED}ERROR:${NC} $*" >&2; exit 1; }
ok()   { echo -e "${GREEN}ok${NC} $*"; }
info() { echo -e "${BOLD}>>${NC} $*"; }

# ── Parse args ──
VERSION_ARG=""
DRY_RUN=false
FORCE=false
NO_PUSH=false

for arg in "$@"; do
    case "$arg" in
        --dry-run)  DRY_RUN=true ;;
        --force)    FORCE=true ;;
        --no-push)  NO_PUSH=true ;;
        -*)         die "Unknown flag: $arg" ;;
        *)          VERSION_ARG="$arg" ;;
    esac
done

if [ -z "$VERSION_ARG" ]; then
    echo "Usage: $0 <version|patch|minor|major> [--dry-run] [--force] [--no-push]"
    echo ""
    echo "Examples:"
    echo "  $0 patch          # 0.8.0 → 0.8.1"
    echo "  $0 minor          # 0.8.0 → 0.9.0"
    echo "  $0 major          # 0.8.0 → 1.0.0"
    echo "  $0 0.9.0          # explicit"
    exit 1
fi

# ── Read current version from header ──
if [ ! -f "$HEADER_FILE" ]; then
    die "Header not found: $HEADER_FILE"
fi

CUR_MAJOR=$(grep -oP '#define NEURONOS_VERSION_MAJOR\s+\K[0-9]+' "$HEADER_FILE")
CUR_MINOR=$(grep -oP '#define NEURONOS_VERSION_MINOR\s+\K[0-9]+' "$HEADER_FILE")
CUR_PATCH=$(grep -oP '#define NEURONOS_VERSION_PATCH\s+\K[0-9]+' "$HEADER_FILE")
CURRENT="${CUR_MAJOR}.${CUR_MINOR}.${CUR_PATCH}"
info "Current version: ${BOLD}v${CURRENT}${NC}"

# ── Compute new version ──
case "$VERSION_ARG" in
    patch) NEW_MAJOR=$CUR_MAJOR; NEW_MINOR=$CUR_MINOR; NEW_PATCH=$((CUR_PATCH + 1)) ;;
    minor) NEW_MAJOR=$CUR_MAJOR; NEW_MINOR=$((CUR_MINOR + 1)); NEW_PATCH=0 ;;
    major) NEW_MAJOR=$((CUR_MAJOR + 1)); NEW_MINOR=0; NEW_PATCH=0 ;;
    *)
        # Explicit version: validate format
        if ! echo "$VERSION_ARG" | grep -qP '^\d+\.\d+\.\d+$'; then
            die "Invalid version: $VERSION_ARG (expected: X.Y.Z, patch, minor, or major)"
        fi
        NEW_MAJOR=$(echo "$VERSION_ARG" | cut -d. -f1)
        NEW_MINOR=$(echo "$VERSION_ARG" | cut -d. -f2)
        NEW_PATCH=$(echo "$VERSION_ARG" | cut -d. -f3)
        ;;
esac

NEW_VERSION="${NEW_MAJOR}.${NEW_MINOR}.${NEW_PATCH}"
TAG="v${NEW_VERSION}"

if [ "$NEW_VERSION" = "$CURRENT" ]; then
    die "New version ($NEW_VERSION) is same as current ($CURRENT)"
fi

echo ""
echo -e "${BOLD}════════════════════════════════════════════${NC}"
echo -e "${BOLD}  NeuronOS Release${NC}"
echo -e "${BOLD}  ${CURRENT} → ${NEW_VERSION} (tag: ${TAG})${NC}"
echo -e "${BOLD}════════════════════════════════════════════${NC}"
echo ""

if $DRY_RUN; then
    echo -e "${YELLOW}[DRY RUN]${NC} No changes will be made."
    echo ""
fi

# ── Prerequisites ──
cd "$REPO_ROOT"

if ! $FORCE && [ -n "$(git status --porcelain)" ]; then
    die "Working tree dirty. Commit first or use --force"
fi

# ── Run tests ──
info "Running tests..."
if [ -f "$REPO_ROOT/build/bin/test_hal" ]; then
    "$REPO_ROOT/build/bin/test_hal" > /dev/null 2>&1 || die "test_hal FAILED"
    ok "test_hal"
fi
if [ -f "$REPO_ROOT/build/bin/test_engine" ]; then
    "$REPO_ROOT/build/bin/test_engine" > /dev/null 2>&1 || die "test_engine FAILED"
    ok "test_engine"
fi
if [ -f "$REPO_ROOT/build/bin/test_memory" ]; then
    "$REPO_ROOT/build/bin/test_memory" > /dev/null 2>&1 || die "test_memory FAILED"
    ok "test_memory"
fi

# ── Update version in header ──
info "Updating version in neuronos.h..."

if $DRY_RUN; then
    echo "  Would set: NEURONOS_VERSION_MAJOR ${NEW_MAJOR}"
    echo "  Would set: NEURONOS_VERSION_MINOR ${NEW_MINOR}"
    echo "  Would set: NEURONOS_VERSION_PATCH ${NEW_PATCH}"
    echo "  Would set: NEURONOS_VERSION_STRING \"${NEW_VERSION}\""
else
    sed -i "s/#define NEURONOS_VERSION_MAJOR .*/#define NEURONOS_VERSION_MAJOR ${NEW_MAJOR}/" "$HEADER_FILE"
    sed -i "s/#define NEURONOS_VERSION_MINOR .*/#define NEURONOS_VERSION_MINOR ${NEW_MINOR}/" "$HEADER_FILE"
    sed -i "s/#define NEURONOS_VERSION_PATCH .*/#define NEURONOS_VERSION_PATCH ${NEW_PATCH}/" "$HEADER_FILE"
    sed -i "s/#define NEURONOS_VERSION_STRING .*/#define NEURONOS_VERSION_STRING \"${NEW_VERSION}\"/" "$HEADER_FILE"
    ok "neuronos.h updated"

    # Verify
    VERIFY=$(grep '#define NEURONOS_VERSION_STRING' "$HEADER_FILE")
    echo "  $VERIFY"
fi

# ── Commit ──
info "Committing..."
if $DRY_RUN; then
    echo "  Would commit: 'release: v${NEW_VERSION}'"
else
    git add "$HEADER_FILE"
    git commit -m "release: v${NEW_VERSION}

Version bump: ${CURRENT} → ${NEW_VERSION}
This commit triggers the release workflow."
    ok "Committed"
fi

# ── Tag ──
info "Creating tag ${TAG}..."
if $DRY_RUN; then
    echo "  Would create annotated tag: ${TAG}"
else
    if git rev-parse "$TAG" >/dev/null 2>&1; then
        die "Tag $TAG already exists. Delete it first: git tag -d $TAG && git push origin :refs/tags/$TAG"
    fi
    git tag -a "$TAG" -m "NeuronOS ${TAG}"
    ok "Tag ${TAG} created"
fi

# ── Push ──
if $NO_PUSH; then
    echo ""
    info "Skipping push (--no-push). Push manually:"
    echo "  git push origin main && git push origin ${TAG}"
    echo ""
elif $DRY_RUN; then
    echo "  Would push: main + ${TAG}"
else
    info "Pushing to origin..."
    git push origin main
    git push origin "$TAG"
    ok "Pushed main + ${TAG}"
fi

# ── Done ──
echo ""
echo -e "${BOLD}════════════════════════════════════════════${NC}"
if $DRY_RUN; then
    echo -e "${YELLOW}  [DRY RUN] No changes made${NC}"
else
    echo -e "${GREEN}  Release ${TAG} initiated${NC}"
fi
echo -e "${BOLD}════════════════════════════════════════════${NC}"
echo ""

if ! $DRY_RUN && ! $NO_PUSH; then
    echo "  The release workflow is now building binaries for:"
    echo "    - Linux x86_64"
    echo "    - Linux ARM64"
    echo "    - macOS ARM64"
    echo ""
    echo "  Monitor: https://github.com/Neuron-OS/neuronos/actions"
    echo "  Release: https://github.com/Neuron-OS/neuronos/releases/tag/${TAG}"
    echo ""
fi
