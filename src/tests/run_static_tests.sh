#!/usr/bin/env bash
set -euo pipefail

# Kept for compatibility with the older test bundle. The consolidated runner now
# includes the former static tests plus the current unit, sync, backend, and packer tests.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT/tests/run_tests.sh" "$@"
