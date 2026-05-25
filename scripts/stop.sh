#!/bin/bash
# scripts/stop.sh — 停止所有后台服务
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "停止所有服务"

if docker compose -f docker/compose.yaml ps --format json 2>/dev/null | grep -q '"running"'; then
    docker compose -f docker/compose.yaml down
    ssv_info "Redis 已停止"
else
    ssv_info "Redis 未在运行"
fi

ssv_info "完成"
