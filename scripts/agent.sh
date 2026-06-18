#!/bin/bash
# scripts/agent.sh — 启动 Python Agent 服务
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "启动 Python Agent 服务"

vision_enabled=false
for arg in "$@"; do
    if [ "$arg" = "--no-mock" ]; then
        vision_enabled=true
    fi
done
if [ "${SSV_AGENT_MOCK_PROVIDER:-}" = "false" ] || [ "${SSV_AGENT_PROVIDER:-}" = "local_yolo" ]; then
    vision_enabled=true
fi

if [ ! -d agent/.venv ]; then
    ssv_info "安装 Python 依赖..."
    if [ "$vision_enabled" = true ]; then
        (cd agent && uv sync --extra vision)
    else
        (cd agent && uv sync)
    fi
fi

ssv_info "配置: $SSV_CONFIG"
if [ "$vision_enabled" = true ]; then
    ssv_info "Agent provider: local_yolo (vision extra)"
fi
ssv_info "按 Ctrl+C 停止"
echo ""

cd agent
if [ "$vision_enabled" = true ]; then
    exec uv run --extra vision python -m ssv_agent --config "$SSV_CONFIG" "$@"
else
    exec uv run python -m ssv_agent --config "$SSV_CONFIG" "$@"
fi
