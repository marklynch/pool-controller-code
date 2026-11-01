#!/usr/bin/env bash
#
# Preview the poolcontrol.io site locally, exactly as GitHub Pages will serve it.
#
#   ./website/preview.sh                      # latest 5 releases
#   ./website/preview.sh --limit 2            # fewer, for a quicker download
#   ./website/preview.sh --bin build/pool-controller-full.bin --version dev
#
# Builds the site with website/build-site.sh — the same script the deploy
# workflow uses — and serves it over http://localhost, which counts as a secure
# context, so the flasher genuinely works here.
#
# Opening website/install/index.html straight from disk does NOT work:
# manifest.json and versions.json are generated, and file:// can't fetch them anyway.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$(cd "$SRC/.." && pwd)/.site-preview"
PORT="${PORT:-8001}"

case "${1:-}" in
  -h|--help) sed -n '2,15p' "${BASH_SOURCE[0]}"; exit 0 ;;
esac

"$SRC/build-site.sh" --out "$OUT" "$@"

echo
echo "Serving at http://localhost:${PORT}/  (Ctrl-C to stop)"
echo "Installer at http://localhost:${PORT}/install/ — use Chrome or Edge, Web Serial is required."
echo
exec python3 -m http.server "$PORT" --directory "$OUT" --bind 127.0.0.1
