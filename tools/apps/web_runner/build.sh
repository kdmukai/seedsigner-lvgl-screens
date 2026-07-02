#!/usr/bin/env bash
# One-shot WASM build for the SeedSigner web_runner.
#
# Builds inside a ccache-enabled Emscripten image (the official emscripten/emsdk
# extended via tools/apps/web_runner/Dockerfile) — no host Emscripten install
# required. The repo is bind-mounted; output lands in
# tools/apps/web_runner/build-wasm/index.html (a single self-contained file).
#
# ccache (EM_COMPILER_WRAPPER) and the Emscripten port cache are persisted in
# tools/apps/web_runner/.ccache and .emcache so repeat builds are fast.
#
# Usage:
#   bash tools/apps/web_runner/build.sh                 # 240x240 default
#   DISPLAY_WIDTH=480 DISPLAY_HEIGHT=320 bash tools/apps/web_runner/build.sh
#   EMSDK_TAG=3.1.74 bash tools/apps/web_runner/build.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Repo root is three levels up: tools/apps/web_runner -> tools/apps -> tools -> repo.
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

WIDTH="${DISPLAY_WIDTH:-240}"
HEIGHT="${DISPLAY_HEIGHT:-240}"
EMSDK_TAG="${EMSDK_TAG:-3.1.74}"   # pinned to match CI (.github/workflows/pages.yml)
IMAGE="seedsigner-emsdk:${EMSDK_TAG}"

CCACHE_HOST="${SCRIPT_DIR}/.ccache"
EMCACHE_HOST="${SCRIPT_DIR}/.emcache"
mkdir -p "$CCACHE_HOST" "$EMCACHE_HOST"

# Build the ccache-enabled toolchain image (Docker layer-caches this; only the
# first run actually pulls emsdk + installs ccache).
# Guard: syntax-check the inline app script in shell.html before building, so a
# broken edit can't ship a shell where Module/ssOnReady never get defined.
if command -v node >/dev/null 2>&1; then
  echo "==> Syntax-checking shell.html script"
  python3 - "$SCRIPT_DIR/shell.html" >/tmp/web_runner_shell_app.js <<'PY'
import re, sys
html = open(sys.argv[1]).read()
m = re.search(r'<script type="text/javascript">(.*?)</script>\s*\{\{\{ SCRIPT \}\}\}', html, re.S)
sys.stdout.write(m.group(1) if m else 'throw new Error("app script block not found");')
PY
  node --check /tmp/web_runner_shell_app.js || { echo "ERROR: shell.html script has a syntax error (see above)"; exit 1; }
fi

echo "==> Ensuring toolchain image ${IMAGE}"
docker build -t "$IMAGE" --build-arg "EMSDK_TAG=${EMSDK_TAG}" "$SCRIPT_DIR"

echo "==> Building web_runner (WASM) at ${WIDTH}x${HEIGHT}"

# Run as the host user so output isn't root-owned; persist ccache + Emscripten
# port cache across runs; EM_COMPILER_WRAPPER routes clang through ccache.
docker run --rm \
  -v "$REPO_ROOT":/src -w /src \
  -v "$CCACHE_HOST":/ccache \
  -v "$EMCACHE_HOST":/emcache \
  -u "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -e EM_CACHE=/emcache \
  -e EM_COMPILER_WRAPPER=ccache \
  -e CCACHE_DIR=/ccache \
  "$IMAGE" \
  bash -lc "emcmake cmake -S tools/apps/web_runner -B tools/apps/web_runner/build-wasm \
              -DCMAKE_BUILD_TYPE=Release \
              -DDISPLAY_WIDTH=${WIDTH} -DDISPLAY_HEIGHT=${HEIGHT} \
            && cmake --build tools/apps/web_runner/build-wasm -j\"\$(nproc)\" \
            && ccache --show-stats | grep -iE 'hits|misses' || true"

# Stage the runtime assets (scenario catalogs + font packs + locale index) next
# to the bundle. The packs must already exist under lang-packs/ (regenerate with
# tools/i18n/build_fontpacks.py); pass --regen-packs below to rebuild them here.
echo "==> Staging runtime assets (scenarios + font packs + locale index)"
REGEN_PACKS="${REGEN_PACKS:-0}"
STAGE_ARGS=(--dest "${SCRIPT_DIR}/build-wasm")
if [ "$REGEN_PACKS" = "1" ]; then
  STAGE_ARGS+=(--regen-packs --gen-bin "${REPO_ROOT}/tools/apps/screenshot_generator/build/screenshot_gen")
fi
python3 "${SCRIPT_DIR}/stage_assets.py" "${STAGE_ARGS[@]}"

echo ""
echo "==> Done: ${REPO_ROOT}/tools/apps/web_runner/build-wasm/  (index.html + .js + .wasm + assets/)"
echo "    Multi-file bundle — serve it over HTTP (file:// won't fetch assets):"
echo "      bash tools/apps/web_runner/serve.sh"
