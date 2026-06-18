#!/bin/bash
# scripts/t4-demo.sh — 运行 T4 Agent 闭环演示
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "T4 Agent 链路演示"

if ! command -v uv >/dev/null 2>&1; then
    ssv_error "uv not found"
    ssv_warn "请先安装 uv，例如: sudo snap install astral-uv --classic"
    exit 1
fi

ssv_info "启动/检查 Redis..."
bash "$SSV_ROOT/scripts/redis.sh"

if [ ! -d agent/.venv ]; then
    ssv_info "安装 Agent Python 依赖（包含 vision extra）..."
    (cd agent && uv sync --extra dev --extra vision)
fi

export SSV_AGENT_MODEL_PATH="${SSV_AGENT_MODEL_PATH:-/home/lzy/work-code/comp-2-freeze10.pt}"
export SSV_AGENT_PROVIDER="${SSV_AGENT_PROVIDER:-local_yolo}"

ssv_info "模型: $SSV_AGENT_MODEL_PATH"
ssv_info "运行 demo..."
echo ""

uv run --project agent --extra vision python -m ssv_agent.demos.internal \
    --config "$SSV_CONFIG" \
    "$@"
