#!/bin/bash
# scripts/run.sh — 准备已验证的运行环境并交接给 C++ runner
set -euo pipefail
source "$(dirname "$0")/lib.sh"
source "$(dirname "$0")/deps.sh"
cd "$SSV_ROOT"

ssv_deps_load_runtime
export_ssv_plugin_path

RUNNER_PATH="$SSV_BUILD_DIR/runner/ssv-runner"
if [ ! -x "$RUNNER_PATH" ]; then
    ssv_error "runner 不存在或不可执行: $RUNNER_PATH; 请先运行 ./ssv build"
    exit 1
fi

exec "$RUNNER_PATH" "$@"
