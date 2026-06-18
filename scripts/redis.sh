#!/bin/bash
# scripts/redis.sh — 启动 Docker Redis (后台运行)
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "启动 Redis"

REDIS_PORT="${REDIS_PORT:-6379}"
REDIS_ROOT="$SSV_ROOT/.deps/redis"
REDIS_APT_DIR="$SSV_ROOT/.deps/apt-redis"
REDIS_BIN="$REDIS_ROOT/usr/bin/redis-server"
REDIS_CLI="$REDIS_ROOT/usr/bin/redis-cli"
REDIS_LIB="$REDIS_ROOT/usr/lib/x86_64-linux-gnu"
REDIS_PID="$REDIS_ROOT/ssv-redis.pid"
REDIS_LOG="$REDIS_ROOT/ssv-redis.log"
REDIS_DATA="$REDIS_ROOT/data"

start_local_redis() {
    mkdir -p "$REDIS_ROOT" "$REDIS_APT_DIR" "$REDIS_DATA"

    if [ ! -x "$REDIS_BIN" ] || [ ! -x "$REDIS_CLI" ]; then
        if ! command -v apt >/dev/null 2>&1 || ! command -v dpkg-deb >/dev/null 2>&1; then
            ssv_error "docker 不可用，且本机缺少 apt/dpkg-deb，无法自动准备本地 Redis"
            exit 1
        fi
        ssv_info "Docker 不可用，下载用户态 Redis 到 .deps/redis"
        (
            cd "$REDIS_APT_DIR"
            apt download redis-server redis-tools liblzf1 libjemalloc2 liblua5.1-0
            for deb in *.deb; do
                dpkg-deb -x "$deb" "$REDIS_ROOT"
            done
        )
    fi

    if LD_LIBRARY_PATH="$REDIS_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" ping 2>/dev/null | grep -q PONG; then
        ssv_info "Redis 已在运行: 127.0.0.1:$REDIS_PORT"
        exit 0
    fi

    ssv_info "启动本地 Redis: 127.0.0.1:$REDIS_PORT"
    LD_LIBRARY_PATH="$REDIS_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$REDIS_BIN" \
        --bind 127.0.0.1 \
        --port "$REDIS_PORT" \
        --dir "$REDIS_DATA" \
        --pidfile "$REDIS_PID" \
        --logfile "$REDIS_LOG" \
        --daemonize yes

    retries=0
    while [ $retries -lt 15 ]; do
        if LD_LIBRARY_PATH="$REDIS_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" ping 2>/dev/null | grep -q PONG; then
            ssv_info "Redis 已就绪: 127.0.0.1:$REDIS_PORT"
            exit 0
        fi
        sleep 1
        retries=$((retries + 1))
    done

    ssv_error "本地 Redis 启动超时，日志: $REDIS_LOG"
    exit 1
}

if ! command -v docker >/dev/null 2>&1; then
    start_local_redis
fi

if ! docker compose version >/dev/null 2>&1; then
    start_local_redis
fi

if docker compose -f docker/compose.yaml ps --format json 2>/dev/null | grep -q '"running"'; then
    ssv_info "Redis 已在运行"
    exit 0
fi

docker compose -f docker/compose.yaml up -d
ssv_info "等待 Redis 就绪..."

retries=0
while [ $retries -lt 15 ]; do
    if docker exec ssv-redis redis-cli ping &>/dev/null; then
        ssv_info "Redis 已就绪"
        exit 0
    fi
    sleep 1
    retries=$((retries + 1))
done

ssv_error "Redis 启动超时"
exit 1
