#!/usr/bin/env bash
# Shared CI script for desktop tool builds, screenshot generation, and comparison.
# Called by platform-specific CI configs (GitHub, GitLab, Forgejo/Codeberg).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# Use sudo if available (GitHub Actions, Forgejo/Codeberg runners);
# fall back to direct invocation (GitLab Docker containers run as root).
SUDO=""
if command -v sudo >/dev/null 2>&1; then
  SUDO="sudo"
fi

# Portable CPU count (nproc is Linux-only; macOS uses sysctl).
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)

COMMAND="${1:-help}"
shift || true

case "$COMMAND" in

  # ---------------------------------------------------------------------------
  # Screenshot generator
  # ---------------------------------------------------------------------------

  install-screenshot-deps)
    $SUDO apt-get update
    $SUDO apt-get install -y cmake build-essential libpng-dev imagemagick python3
    ;;

  build-screenshots)
    cmake -S tools/apps/screenshot_generator -B tools/apps/screenshot_generator/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DDISPLAY_WIDTH=480 -DDISPLAY_HEIGHT=320
    cmake --build tools/apps/screenshot_generator/build -j"$NPROC"
    ;;

  generate-screenshots)
    tools/apps/screenshot_generator/build/screenshot_gen ${1:+--out-dir "$1"}
    ;;

  compare-screenshots)
    python3 tools/apps/screenshot_generator/compare_screenshots.py "$@"
    ;;

  # ---------------------------------------------------------------------------
  # Screen runner
  # ---------------------------------------------------------------------------

  install-screen-runner-deps)
    $SUDO apt-get update
    $SUDO apt-get install -y cmake build-essential libsdl2-dev libsdl2-ttf-dev imagemagick
    ;;

  build-screen-runner)
    cmake -S tools/apps/screen_runner -B tools/apps/screen_runner/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DDISPLAY_WIDTH=480 -DDISPLAY_HEIGHT=320 \
      ${@+"$@"}
    cmake --build tools/apps/screen_runner/build -j"$NPROC"
    ;;

  package-screen-runner)
    # Copy build outputs into a self-contained artifact directory.
    # Usage: ci.sh package-screen-runner [ARTIFACT_DIR]
    ARTIFACT_DIR="${1:-artifact}"
    mkdir -p "$ARTIFACT_DIR"
    if [ -f tools/apps/screen_runner/build/screen_runner.exe ]; then
      cp tools/apps/screen_runner/build/screen_runner.exe "$ARTIFACT_DIR/"
    else
      cp tools/apps/screen_runner/build/screen_runner "$ARTIFACT_DIR/"
    fi
    cp tools/apps/screen_runner/build/screen_runner_font_regular.ttf "$ARTIFACT_DIR/" 2>/dev/null || true
    cp tools/apps/screen_runner/build/screen_runner_font_semibold.ttf "$ARTIFACT_DIR/" 2>/dev/null || true
    cp tools/apps/screen_runner/build/screen_runner_logo.bmp "$ARTIFACT_DIR/" 2>/dev/null || true
    cp tools/scenarios/scenarios.json "$ARTIFACT_DIR/"
    ;;

  # ---------------------------------------------------------------------------
  # Web runner (WASM)
  # ---------------------------------------------------------------------------

  build-web-runner)
    # Build the browser (WASM) playground. Emscripten's emcmake/em++ must be on
    # PATH (the CI workflow provides them via mymindstorm/setup-emsdk). Optional
    # display dimensions can be passed through, e.g. -DDISPLAY_WIDTH=480 ...
    #
    # Guard: syntax-check the inline app script in shell.html first, so a broken
    # edit can't ship a shell where Module/ssOnReady never get defined.
    if command -v node >/dev/null 2>&1; then
      python3 - tools/apps/web_runner/shell.html > /tmp/web_runner_shell_app.js <<'PY'
import re, sys
html = open(sys.argv[1]).read()
m = re.search(r'<script type="text/javascript">(.*?)</script>\s*\{\{\{ SCRIPT \}\}\}', html, re.S)
sys.stdout.write(m.group(1) if m else 'throw new Error("app script block not found");')
PY
      node --check /tmp/web_runner_shell_app.js
    fi
    emcmake cmake -S tools/apps/web_runner -B tools/apps/web_runner/build-wasm \
      -DCMAKE_BUILD_TYPE=Release \
      -DDISPLAY_WIDTH=240 -DDISPLAY_HEIGHT=240 \
      ${@+"$@"}
    cmake --build tools/apps/web_runner/build-wasm -j"$NPROC"
    ;;

  package-web-runner)
    # Copy the single-file bundle into a self-contained directory for deploy.
    # Usage: ci.sh package-web-runner [DIR]
    WEB_DIR="${1:-web-site}"
    mkdir -p "$WEB_DIR"
    cp tools/apps/web_runner/build-wasm/index.html "$WEB_DIR/"
    ;;

  # ---------------------------------------------------------------------------
  # Combined GitHub Pages site (screenshot gallery + web runner)
  # ---------------------------------------------------------------------------

  assemble-site)
    # Assemble the full Pages site from already-built outputs:
    #   <site>/             screenshot gallery (manifest.json + img/)
    #   <site>/play/        the web runner playground (index.html)
    # Deployed atomically by the official Pages action. Usage: ci.sh assemble-site [DIR]
    SITE_DIR="${1:-site}"
    rm -rf "$SITE_DIR"
    mkdir -p "$SITE_DIR/play"
    if [ -d tools/apps/screenshot_generator/screenshots ]; then
      cp -r tools/apps/screenshot_generator/screenshots/. "$SITE_DIR/"
    fi
    cp tools/apps/web_runner/build-wasm/index.html "$SITE_DIR/play/"
    echo "Assembled site at $SITE_DIR (gallery at /, web runner at /play/)"
    ;;

  screenshot-diff-summary)
    # Print a Markdown summary of a screenshot comparison (for $GITHUB_STEP_SUMMARY).
    # Usage: ci.sh screenshot-diff-summary RESULT_JSON
    RESULT="${1:?Usage: screenshot-diff-summary RESULT_JSON}"
    echo "### SeedSigner LVGL screenshot regression"
    if [ -f "$RESULT" ]; then
      python3 -c "
import json, sys
s = json.load(open(sys.argv[1])).get('summary', {})
ch, nw, rm, un = s.get('changed',0), s.get('new',0), s.get('removed',0), s.get('unchanged',0)
if ch == 0 and nw == 0 and rm == 0:
    print(f'No screenshots affected by this PR ({un} unchanged).')
else:
    print(f'**{ch}** changed, **{nw}** new, **{rm}** removed, {un} unchanged.')
" "$RESULT"
    else
      echo "No comparison result was produced."
    fi
    echo ""
    echo "Download the **screenshot-diff** artifact (HTML report) and the **site** artifact (open \`index.html\` / \`play/index.html\` locally) to review."
    ;;


  # ---------------------------------------------------------------------------
  # Pages deployment
  # ---------------------------------------------------------------------------

  deploy-pages)
    # Deploy a directory to a git branch (for Pages hosting).
    # Usage: ci.sh deploy-pages SOURCE_DIR BRANCH [DEST_DIR]
    #
    # Requires git credentials to be configured (CI checkout action handles this).
    # DEST_DIR defaults to "." (full replace); set to e.g. "previews/pr-42" for partial.
    SOURCE_DIR="${1:?Usage: deploy-pages SOURCE_DIR BRANCH [DEST_DIR]}"
    PAGES_BRANCH="${2:?Usage: deploy-pages SOURCE_DIR BRANCH [DEST_DIR]}"
    DEST_DIR="${3:-.}"

    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK"' EXIT

    REMOTE_URL="$(git remote get-url origin)"

    # Try to clone the existing pages branch; create an orphan if it doesn't exist.
    if ! git clone --depth 1 --branch "$PAGES_BRANCH" "$REMOTE_URL" "$WORK" 2>/dev/null; then
      git init "$WORK"
      git -C "$WORK" checkout --orphan "$PAGES_BRANCH"
      git -C "$WORK" remote add origin "$REMOTE_URL"
    fi

    if [ "$DEST_DIR" = "." ]; then
      # Full deploy: clear everything, copy new content.
      find "$WORK" -maxdepth 1 ! -name '.git' ! -name '.' -exec rm -rf {} +
      cp -r "$SOURCE_DIR"/. "$WORK/"
    else
      # Partial deploy: replace only the target subdirectory.
      rm -rf "$WORK/$DEST_DIR"
      mkdir -p "$WORK/$DEST_DIR"
      cp -r "$SOURCE_DIR"/. "$WORK/$DEST_DIR/"
    fi

    git -C "$WORK" add -A
    if git -C "$WORK" diff --cached --quiet; then
      echo "No changes to deploy."
    else
      git -C "$WORK" \
        -c user.name="ci-bot" \
        -c user.email="ci-bot@noreply" \
        commit -m "Deploy screenshots"
      git -C "$WORK" push origin "$PAGES_BRANCH"
    fi
    ;;

  cleanup-pages)
    # Remove a subdirectory from the pages branch.
    # Usage: ci.sh cleanup-pages BRANCH SUBDIR
    PAGES_BRANCH="${1:?Usage: cleanup-pages BRANCH SUBDIR}"
    SUBDIR="${2:?Usage: cleanup-pages BRANCH SUBDIR}"

    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK"' EXIT

    REMOTE_URL="$(git remote get-url origin)"
    if ! git clone --depth 1 --branch "$PAGES_BRANCH" "$REMOTE_URL" "$WORK" 2>/dev/null; then
      echo "Pages branch '$PAGES_BRANCH' does not exist; nothing to clean."
      exit 0
    fi

    if [ -d "$WORK/$SUBDIR" ]; then
      rm -rf "$WORK/$SUBDIR"
      git -C "$WORK" add -A
      git -C "$WORK" \
        -c user.name="ci-bot" \
        -c user.email="ci-bot@noreply" \
        commit -m "Remove $SUBDIR"
      git -C "$WORK" push origin "$PAGES_BRANCH"
    else
      echo "Directory '$SUBDIR' not found on '$PAGES_BRANCH'; nothing to clean."
    fi
    ;;

  # ---------------------------------------------------------------------------
  # Help
  # ---------------------------------------------------------------------------

  help|*)
    echo "Usage: $0 COMMAND [ARGS...]"
    echo ""
    echo "Screenshot commands:"
    echo "  install-screenshot-deps          Install screenshot generator dependencies"
    echo "  build-screenshots                Build the screenshot generator"
    echo "  generate-screenshots [OUT_DIR]   Generate screenshots"
    echo "  compare-screenshots [ARGS...]    Compare before/after screenshots"
    echo ""
    echo "Screen runner commands:"
    echo "  install-screen-runner-deps       Install screen runner dependencies"
    echo "  build-screen-runner [CMAKE_ARGS] Build the screen runner"
    echo "  package-screen-runner [DIR]      Package build outputs into artifact dir"
    echo ""
    echo "Web runner (WASM) commands:"
    echo "  build-web-runner [CMAKE_ARGS]    Build the browser playground (needs emcmake on PATH)"
    echo "  package-web-runner [DIR]         Copy the single-file bundle into a deploy dir"
    echo ""
    echo "Pages commands:"
    echo "  deploy-pages SRC BRANCH [DEST]   Deploy directory to a git branch"
    echo "  cleanup-pages BRANCH SUBDIR      Remove a subdirectory from pages branch"
    exit 1
    ;;
esac
