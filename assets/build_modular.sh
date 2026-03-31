#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_FILE="${1:-$SCRIPT_DIR/64x64_modular/wizzard_64x64.aseprite}"
OUTPUT_DIR="${2:-$(dirname "$SOURCE_FILE")/$(basename "$SOURCE_FILE" .aseprite)_exports}"
EXPORT_SCRIPT="$SCRIPT_DIR/aseprite_export_layers.lua"

resolve_aseprite_bin() {
  if [[ -n "${ASEPRITE_BIN:-}" ]]; then
    printf '%s\n' "$ASEPRITE_BIN"
    return
  fi

  if [[ -x "/Applications/Aseprite.app/Contents/MacOS/aseprite" ]]; then
    printf '%s\n' "/Applications/Aseprite.app/Contents/MacOS/aseprite"
    return
  fi

  if [[ -x "$HOME/Applications/Aseprite.app/Contents/MacOS/aseprite" ]]; then
    printf '%s\n' "$HOME/Applications/Aseprite.app/Contents/MacOS/aseprite"
    return
  fi

  printf '%s\n' "aseprite"
}

ASEPRITE_BIN="$(resolve_aseprite_bin)"

if [[ ! -f "$SOURCE_FILE" ]]; then
  echo "Aseprite source file not found: $SOURCE_FILE" >&2
  exit 1
fi

if [[ ! -f "$EXPORT_SCRIPT" ]]; then
  echo "Aseprite export script not found: $EXPORT_SCRIPT" >&2
  exit 1
fi

if [[ "$ASEPRITE_BIN" = */* ]]; then
  if [[ ! -x "$ASEPRITE_BIN" ]]; then
    echo "Aseprite binary not found: $ASEPRITE_BIN" >&2
    exit 1
  fi
elif ! command -v "$ASEPRITE_BIN" >/dev/null 2>&1; then
  echo "Aseprite binary not found: $ASEPRITE_BIN" >&2
  echo "Set ASEPRITE_BIN=/path/to/aseprite or add it to PATH." >&2
  exit 1
fi

mkdir -p "$OUTPUT_DIR"

SOURCE_NAME="$(basename "$SOURCE_FILE" .aseprite)"

echo "Using Aseprite: $ASEPRITE_BIN"
echo "Source: $SOURCE_FILE"
echo "Output: $OUTPUT_DIR"

"$ASEPRITE_BIN" -b \
  --script-param "source_file=$SOURCE_FILE" \
  --script-param "output_dir=$OUTPUT_DIR" \
  --script-param "source_name=$SOURCE_NAME" \
  --script "$EXPORT_SCRIPT"

echo "Done."
