#!/usr/bin/env bash
# Quick single-machine verification: 3 relay processes + one receiver.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"

cmake -S "$ROOT" -B "$BUILD" -DONION_BUILD_TESTS=ON
cmake --build "$BUILD" -j"$(nproc)"
ctest --test-dir "$BUILD" --output-on-failure \
    -R 'unit_tests|local_topology|tamper_rejection'
