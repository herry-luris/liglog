#!/usr/bin/env bash
# Run both benchmarks and print a side-by-side comparison table.
#
# Usage:
#   ./compare.sh [build_dir]
#
# build_dir defaults to ./build.

set -euo pipefail

BUILD_DIR="${1:-./build}"
LIGLOG_BIN="${BUILD_DIR}/benchmarks/bench_liglog"
SPDLOG_BIN="${BUILD_DIR}/benchmarks/bench_spdlog"

if [[ ! -x "$LIGLOG_BIN" ]]; then
    echo "error: $LIGLOG_BIN not found. Build first." >&2
    exit 1
fi
if [[ ! -x "$SPDLOG_BIN" ]]; then
    echo "error: $SPDLOG_BIN not found. Enable spdlog in CMake and rebuild." >&2
    exit 1
fi

# Console sinks write to stderr; silence them so only the tables remain.
echo "==> liglog"
"$LIGLOG_BIN" 2>/dev/null | tee /tmp/bench_liglog.txt

echo
echo "==> spdlog"
"$SPDLOG_BIN" 2>/dev/null | tee /tmp/bench_spdlog.txt

echo
echo "==> comparison"
python3 - <<'PY'
import re

def parse(path):
    rows = {}
    for line in open(path):
        m = re.match(r"^(\S+)\s+(\d+\.\d+)\s+\d+\s+[\d.]+M\s*$", line.strip())
        if m:
            rows[m.group(1)] = float(m.group(2))
    return rows

lig = parse("/tmp/bench_liglog.txt")
spd = parse("/tmp/bench_spdlog.txt")

scenarios = [
    "FrontendSuppressed",
    "FrontendEnabled_NoArgs",
    "FrontendEnabled_IntArg",
    "FrontendEnabled_MixedArgs",
    "SyncTransportFrom",
    "AsyncTransportFrom",
    "AsyncTransportFromFlush",
    "EndToEnd_Sync_Minimal",
    "EndToEnd_Async",
    "MultiThread_Sync/threads:1",
    "MultiThread_Sync/threads:2",
    "MultiThread_Sync/threads:4",
    "MultiThread_Sync/threads:8",
    "MultiThread_Async/threads:1",
    "MultiThread_Async/threads:2",
    "MultiThread_Async/threads:4",
    "MultiThread_Async/threads:8",
    "TextFormatter_Full",
    "TextFormatter_Minimal",
    "MixedWorkload",
]

print(f"{'scenario':<38} {'liglog(ns)':>12} {'spdlog(ns)':>12} {'spdlog/liglog':>15}")
print("-" * 80)
for s in scenarios:
    if s in lig and s in spd:
        ratio = spd[s] / lig[s]
        # ratio > 1 means spdlog is slower -> liglog wins
        print(f"{s:<38} {lig[s]:>12.2f} {spd[s]:>12.2f} {ratio:>14.2f}x")
    elif s in lig:
        print(f"{s:<38} {lig[s]:>12.2f} {'-':>12} {'-':>15}")
    elif s in spd:
        print(f"{s:<38} {'-':>12} {spd[s]:>12.2f} {'-':>15}")
PY
