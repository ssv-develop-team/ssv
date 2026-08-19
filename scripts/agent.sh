#!/bin/bash
# scripts/agent.sh — 启动 Python Agent 服务
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "启动 Python Agent 服务"

if [ ! -d agent/.venv ]; then
    ssv_info "安装 Python 依赖..."
    (cd agent && uv sync)
fi

config_args=()
if [ -n "$SSV_CONFIG" ]; then
    case "$SSV_CONFIG" in
        /*) ;;
        *) SSV_CONFIG="$SSV_ROOT/$SSV_CONFIG" ;;
    esac
    ssv_info "配置: $SSV_CONFIG"
    config_args=(--config "$SSV_CONFIG")
else
    ssv_info "配置: 未找到本地运行配置，使用 Agent 内置默认值"
fi
ssv_info "按 Ctrl+C 停止"
echo ""

cd agent
exec uv run python -m ssv_agent "${config_args[@]}"
