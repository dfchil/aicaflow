#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && /bin/pwd -P)"
ROOT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && /bin/pwd -P)"
cd "$ROOT_DIR"

ARTIFACT_DIR="tests/artifacts"
mkdir -p "$ARTIFACT_DIR"

echo "[integration] building host tools"
make -C src/tools >/dev/null

echo "[integration] generating test .afx"
AFTMP="$ARTIFACT_DIR/test_suite_output.afx"
INFOTMP="$ARTIFACT_DIR/test_suite_info.txt"
./tools/midi2afx --trim input/bwv1007.mid "$AFTMP" input/wavetables.map >/dev/null

echo "[integration] inspecting output"
./tools/afx_info "$AFTMP" input/wavetables.map > "$INFOTMP"

grep -q "Magic:[[:space:]]*0xA1CAF200" "$INFOTMP"
grep -q "Version:[[:space:]]*1" "$INFOTMP"
grep -q "Sample Descriptors" "$INFOTMP"
grep -Eq "(Flow Command) Register Distribution" "$INFOTMP"

echo "PASS: integration tests"
