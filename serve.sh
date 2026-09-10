#!/usr/bin/env bash
# Rebuild the site and serve it locally.
set -euo pipefail
cd "$(dirname "$0")"
PORT="${1:-8000}"
python3 build.py
echo "Serving http://localhost:${PORT}/  (Ctrl-C to stop)"
python3 -m http.server "$PORT" --bind 127.0.0.1
