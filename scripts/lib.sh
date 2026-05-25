#!/bin/bash
# scripts/lib.sh — 共享函数，被其他脚本 source
# 用法: source "$(dirname "$0")/lib.sh"

set -euo pipefail

# ─── 项目根目录 ───────────────────────────────────────────────
SSV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ─── 加载 .env (不覆盖已有环境变量) ───────────────────────────
if [ -f "$SSV_ROOT/.env" ]; then
    while IFS='=' read -r key value; do
        [[ -z "$key" || "$key" =~ ^[[:space:]]*# ]] && continue
        key="$(echo "$key" | xargs)"
        if [ -z "${!key+x}" ]; then
            export "$key=$value"
        fi
    done < "$SSV_ROOT/.env"
fi

SSV_CONFIG="${SSV_CONFIG_PATH:-$SSV_ROOT/config/ssv.default.yaml}"
SSV_ONNXRUNTIME_ROOT="${SSV_ONNXRUNTIME_ROOT:-$SSV_ROOT/.deps/onnxruntime}"
SSV_BUILD_DIR="${SSV_BUILD_DIR:-$SSV_ROOT/build}"
SSV_PLUGIN_DIR="$SSV_BUILD_DIR/gst/ssv-template"

# 所有插件目录 (用于 GST_PLUGIN_PATH)
SSV_PLUGIN_PATHS="$SSV_BUILD_DIR/gst/ssv-template"
SSV_PLUGIN_PATHS="$SSV_PLUGIN_PATHS:$SSV_BUILD_DIR/gst/ssv-infer"
SSV_PLUGIN_PATHS="$SSV_PLUGIN_PATHS:$SSV_BUILD_DIR/gst/ssv-track"
SSV_PLUGIN_PATHS="$SSV_PLUGIN_PATHS:$SSV_BUILD_DIR/gst/ssv-pub"
SSV_PLUGIN_PATHS="$SSV_PLUGIN_PATHS:$SSV_BUILD_DIR/gst/ssv-overlay"

# 导出 GST_PLUGIN_PATH 和 LD_LIBRARY_PATH
export_ssv_plugin_path() {
    export GST_PLUGIN_PATH="$SSV_PLUGIN_PATHS${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
    # ssv-common 是共享库，需要让动态链接器能找到
    export LD_LIBRARY_PATH="$SSV_BUILD_DIR/gst/ssv-common${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    if [ -d "$SSV_ONNXRUNTIME_ROOT/lib" ]; then
        export LD_LIBRARY_PATH="$SSV_ONNXRUNTIME_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    fi
}

ssv_require_command() {
    local cmd="$1"
    local package_hint="$2"
    local distro_hint="$3"

    if command -v "$cmd" >/dev/null 2>&1; then
        return 0
    fi

    ssv_error "$cmd not found"
    if [ -n "$package_hint" ]; then
        ssv_warn "$distro_hint: $package_hint"
    fi
    return 1
}

# ─── 颜色 ─────────────────────────────────────────────────────
_RED='\033[0;31m'
_GREEN='\033[0;32m'
_YELLOW='\033[0;33m'
_CYAN='\033[0;36m'
_BOLD='\033[1m'
_NC='\033[0m'

ssv_info()  { echo -e "${_GREEN}[SSV]${_NC} $*"; }
ssv_warn()  { echo -e "${_YELLOW}[SSV]${_NC} $*"; }
ssv_error() { echo -e "${_RED}[SSV]${_NC} $*" >&2; }
ssv_header(){ echo -e "\n${_BOLD}${_CYAN}── $* ──${_NC}\n"; }
