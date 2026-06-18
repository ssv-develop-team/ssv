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
    ssv_info "Docker Redis 未在运行"
fi

REDIS_PORT="${REDIS_PORT:-6379}"
REDIS_ROOT="$SSV_ROOT/.deps/redis"
REDIS_CLI="$REDIS_ROOT/usr/bin/redis-cli"
REDIS_LIB="$REDIS_ROOT/usr/lib/x86_64-linux-gnu"

if [ -x "$REDIS_CLI" ] && LD_LIBRARY_PATH="$REDIS_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" ping 2>/dev/null | grep -q PONG; then
    LD_LIBRARY_PATH="$REDIS_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" shutdown nosave >/dev/null 2>&1 || true
    ssv_info "本地 Redis 已停止: 127.0.0.1:$REDIS_PORT"
fi

ssv_info "完成"
