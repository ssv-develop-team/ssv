#!/bin/bash
# scripts/t4-video-demo.sh — 从视频/流生成事件并演示 T4 Agent 复核闭环
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "T4 视频链路演示"

if ! command -v uv >/dev/null 2>&1; then
    ssv_error "uv not found"
    ssv_warn "请先安装 uv，例如: sudo snap install astral-uv --classic"
    exit 1
fi

for arg in "$@"; do
    if [ "$arg" = "--help" ] || [ "$arg" = "-h" ]; then
        uv run --project agent --extra vision python -m ssv_agent.demos.video "$@"
        exit 0
    fi
done

ssv_info "启动/检查 Redis..."
bash "$SSV_ROOT/scripts/redis.sh"

if [ ! -d agent/.venv ]; then
    ssv_info "安装 Agent Python 依赖（包含 vision extra）..."
    (cd agent && uv sync --extra dev --extra vision)
fi

ssv_info "运行视频 demo..."
echo ""

uv run --project agent --extra vision python -m ssv_agent.demos.video \
    --config "$SSV_CONFIG" \
    "$@"
