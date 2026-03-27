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
    cmake -S tools/screenshot_generator -B tools/screenshot_generator/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DDISPLAY_WIDTH=480 -DDISPLAY_HEIGHT=320
    cmake --build tools/screenshot_generator/build -j"$(nproc)"
    ;;

  generate-screenshots)
    tools/screenshot_generator/build/screenshot_gen ${1:+--out-dir "$1"}
    ;;

  compare-screenshots)
    python3 tools/screenshot_generator/compare_screenshots.py "$@"
    ;;

  # ---------------------------------------------------------------------------
  # Screen runner
  # ---------------------------------------------------------------------------

  install-screen-runner-deps)
    $SUDO apt-get update
    $SUDO apt-get install -y cmake build-essential libsdl2-dev libsdl2-ttf-dev imagemagick
    ;;

  build-screen-runner)
    cmake -S tools/screen_runner -B tools/screen_runner/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DDISPLAY_WIDTH=480 -DDISPLAY_HEIGHT=320 \
      "${@}"
    cmake --build tools/screen_runner/build -j"$(nproc)"
    ;;

  package-screen-runner)
    # Copy build outputs into a self-contained artifact directory.
    # Usage: ci.sh package-screen-runner [ARTIFACT_DIR]
    ARTIFACT_DIR="${1:-artifact}"
    mkdir -p "$ARTIFACT_DIR"
    if [ -f tools/screen_runner/build/screen_runner.exe ]; then
      cp tools/screen_runner/build/screen_runner.exe "$ARTIFACT_DIR/"
    else
      cp tools/screen_runner/build/screen_runner "$ARTIFACT_DIR/"
    fi
    cp tools/screen_runner/build/screen_runner_font_regular.ttf "$ARTIFACT_DIR/" 2>/dev/null || true
    cp tools/screen_runner/build/screen_runner_font_semibold.ttf "$ARTIFACT_DIR/" 2>/dev/null || true
    cp tools/screen_runner/build/screen_runner_logo.bmp "$ARTIFACT_DIR/" 2>/dev/null || true
    cp tools/scenarios.json "$ARTIFACT_DIR/"
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
    echo "Pages commands:"
    echo "  deploy-pages SRC BRANCH [DEST]   Deploy directory to a git branch"
    echo "  cleanup-pages BRANCH SUBDIR      Remove a subdirectory from pages branch"
    exit 1
    ;;
esac
