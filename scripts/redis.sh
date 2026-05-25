#!/bin/bash
# scripts/redis.sh — 启动 Docker Redis (后台运行)
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "启动 Docker Redis"

ssv_require_command "docker" \
    "sudo apt-get install docker.io docker-compose-plugin" \
    "Debian/Ubuntu"

if ! docker compose version >/dev/null 2>&1; then
    ssv_error "docker compose not available"
    ssv_warn "Debian/Ubuntu: sudo apt-get install docker-compose-plugin"
    exit 1
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
