#!/usr/bin/env bash
# Rebuild everything and run the checks.
#   ./test.sh           build + compiler test-suite in host mode + native self-hosting check
#   ./test.sh --qemu    the same, plus the test-suite and the self-hosting chain inside QEMU
set -euo pipefail
cd "$(dirname "$0")"
./build.sh >/dev/null && echo "build ok"
python3 tests/run_tests.py --markdown
tools/selfhost_check.sh ${1:-}
[ "${1:-}" = "--qemu" ] && python3 tests/run_tests.py --qemu --markdown
exit 0
