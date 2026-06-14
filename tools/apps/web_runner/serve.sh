#!/usr/bin/env bash
# Serve the built web_runner bundle to the local browser AND external clients on
# the LAN and the tailnet. Binds 0.0.0.0 (all interfaces) so the page is reachable
# from a phone/laptop on the same network or any device on your tailnet — the
# intended remote-development loop (build on this machine, view from elsewhere).
#
# Usage:
#   bash tools/apps/web_runner/serve.sh           # port 8000
#   bash tools/apps/web_runner/serve.sh 9000       # custom port
#
# If a firewall is active, allow the port (e.g. `sudo ufw allow <port>/tcp`).
# Tailnet access also requires the device to be on your tailnet.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVE_DIR="${SCRIPT_DIR}/build-wasm"
PORT="${1:-8000}"

if [ ! -f "${SERVE_DIR}/index.html" ]; then
  echo "No build found at ${SERVE_DIR}/index.html — run 'bash tools/apps/web_runner/build.sh' first." >&2
  exit 1
fi

# Pick a likely LAN IPv4 (skip loopback, docker bridges, and tailnet 100.64/10).
lan_ip="$(hostname -I 2>/dev/null | tr ' ' '\n' \
  | grep -E '^(192\.168|10\.|172\.(1[6-9]|2[0-9]|3[01]))\.' \
  | grep -vE '^172\.1[78]\.' | head -1 || true)"

echo "Serving ${SERVE_DIR} on 0.0.0.0:${PORT}"
echo "  Local:   http://localhost:${PORT}/index.html"
[ -n "$lan_ip" ] && echo "  LAN:     http://${lan_ip}:${PORT}/index.html"

# Tailnet URLs (MagicDNS name preferred, IP as fallback).
if command -v tailscale >/dev/null 2>&1; then
  ts_name="$(tailscale status --self --json 2>/dev/null \
    | python3 -c "import sys,json; print(json.load(sys.stdin).get('Self',{}).get('DNSName','').rstrip('.'))" 2>/dev/null || true)"
  ts_ip="$(tailscale ip -4 2>/dev/null | head -1 || true)"
  [ -n "$ts_name" ] && echo "  Tailnet: http://${ts_name}:${PORT}/index.html"
  [ -n "$ts_ip" ]   && echo "  Tailnet: http://${ts_ip}:${PORT}/index.html"
fi
echo "Press Ctrl-C to stop."

# --bind 0.0.0.0 is the default for http.server, but make it explicit.
exec python3 -m http.server "${PORT}" --bind 0.0.0.0 --directory "${SERVE_DIR}"
