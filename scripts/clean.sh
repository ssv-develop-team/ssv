#!/bin/bash
# scripts/clean.sh — 清理 Meson 构建目录
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "清理构建目录"

if [ -d "$SSV_BUILD_DIR" ]; then
    rm -rf "$SSV_BUILD_DIR"
    ssv_info "已删除: ${SSV_BUILD_DIR#$SSV_ROOT/}"
else
    ssv_info "构建目录不存在: ${SSV_BUILD_DIR#$SSV_ROOT/}"
fi
