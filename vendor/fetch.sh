#!/bin/sh
# vendor/fetch.sh — Download bundled header-only dependencies
# Run from the repo root: sh vendor/fetch.sh

set -e

VENDOR_DIR="$(dirname "$0")"

# ── nlohmann/json ────────────────────────────────────────────────────────────
# https://github.com/nlohmann/json (MIT license)
JSON_VERSION="3.11.3"
JSON_URL="https://github.com/nlohmann/json/releases/download/v${JSON_VERSION}/json.hpp"
JSON_PATH="${VENDOR_DIR}/nlohmann/json.hpp"

if [ ! -f "${JSON_PATH}" ]; then
    echo "Downloading nlohmann/json v${JSON_VERSION}..."
    mkdir -p "${VENDOR_DIR}/nlohmann"
    curl -L "${JSON_URL}" -o "${JSON_PATH}"
    echo "  → ${JSON_PATH}"
else
    echo "nlohmann/json already present at ${JSON_PATH}"
fi

echo ""
echo "All vendor dependencies ready."
