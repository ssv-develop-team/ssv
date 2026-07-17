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

if [ -n "${SSV_CONFIG_PATH:-}" ]; then
    SSV_CONFIG="$SSV_CONFIG_PATH"
elif [ -f "$SSV_ROOT/ssv.yaml" ]; then
    SSV_CONFIG="$SSV_ROOT/ssv.yaml"
elif [ -f "$SSV_ROOT/config/ssv.yaml" ]; then
    SSV_CONFIG="$SSV_ROOT/config/ssv.yaml"
else
    SSV_CONFIG="$SSV_ROOT/config/ssv.example.yaml"
fi
SSV_ONNXRUNTIME_FLAVOR="${SSV_ONNXRUNTIME_FLAVOR:-cpu}"
SSV_ONNXRUNTIME_DEFAULT_ROOT="$SSV_ROOT/.deps/onnxruntime"
if [ "$SSV_ONNXRUNTIME_FLAVOR" = "gpu" ]; then
    SSV_ONNXRUNTIME_DEFAULT_ROOT="$SSV_ROOT/.deps/onnxruntime-gpu"
elif [ "$SSV_ONNXRUNTIME_FLAVOR" != "cpu" ]; then
    echo "[SSV] unsupported SSV_ONNXRUNTIME_FLAVOR: $SSV_ONNXRUNTIME_FLAVOR (expected cpu or gpu)" >&2
    exit 1
fi
SSV_ONNXRUNTIME_ROOT="${SSV_ONNXRUNTIME_ROOT:-$SSV_ONNXRUNTIME_DEFAULT_ROOT}"
SSV_TENSORRT_ROOT="${SSV_TENSORRT_ROOT:-$SSV_ROOT/.deps/tensorrt}"
SSV_OPENCV_ROOT="${SSV_OPENCV_ROOT:-$SSV_ROOT/.deps/opencv}"
SSV_BUILD_DIR="${SSV_BUILD_DIR:-$SSV_ROOT/build}"
SSV_PLUGIN_DIR="$SSV_BUILD_DIR/gst/ssv-template"

ssv_yaml_get() {
    local key_path="$1"
    local default_value="${2:-}"
    if [ ! -f "$SSV_CONFIG" ]; then
        printf '%s' "$default_value"
        return 0
    fi

    python3 - "$SSV_CONFIG" "$key_path" "$default_value" <<'PY'
import sys
from pathlib import Path

import yaml

config_path, key_path, default_value = sys.argv[1], sys.argv[2], sys.argv[3]
with open(Path(config_path), encoding="utf-8") as f:
    data = yaml.safe_load(f) or {}

value = data
for part in key_path.split("."):
    if isinstance(value, dict):
        if part not in value:
            print(default_value, end="")
            raise SystemExit(0)
        value = value[part]
    elif isinstance(value, list) and part.isdigit():
        index = int(part)
        if index >= len(value):
            print(default_value, end="")
            raise SystemExit(0)
        value = value[index]
    else:
        print(default_value, end="")
        raise SystemExit(0)

if value is None:
    print(default_value, end="")
elif isinstance(value, bool):
    print("true" if value else "false", end="")
else:
    print(value, end="")
PY
}

# 所有插件目录 (用于 GST_PLUGIN_PATH)
SSV_PLUGIN_PATHS="$SSV_BUILD_DIR/gst/ssv-template"
SSV_PLUGIN_PATHS="$SSV_PLUGIN_PATHS:$SSV_BUILD_DIR/gst/ssv-infer"
SSV_PLUGIN_PATHS="$SSV_PLUGIN_PATHS:$SSV_BUILD_DIR/gst/ssv-track"
SSV_PLUGIN_PATHS="$SSV_PLUGIN_PATHS:$SSV_BUILD_DIR/gst/ssv-pub"
SSV_PLUGIN_PATHS="$SSV_PLUGIN_PATHS:$SSV_BUILD_DIR/gst/ssv-overlay"

append_ld_path_if_dir() {
    local dir="$1"
    if [ -d "$dir" ]; then
        SSV_LD_PATHS="$SSV_LD_PATHS:$dir"
    fi
}

append_nvidia_wheel_libs() {
    local site_packages
    for site_packages in \
        "$SSV_ROOT"/.venv/lib/python*/site-packages \
        "$SSV_ROOT"/.deps/python-site-packages \
        "$SSV_ROOT"/.deps/venvs/*/lib/python*/site-packages; do
        [ -d "$site_packages/nvidia" ] || continue
        local lib_dir
        for lib_dir in "$site_packages"/nvidia/*/lib; do
            append_ld_path_if_dir "$lib_dir"
        done
    done
}

# 导出 GST_PLUGIN_PATH 和 LD_LIBRARY_PATH
export_ssv_plugin_path() {
    export GST_PLUGIN_PATH="$SSV_PLUGIN_PATHS"
    # ssv-common 是共享库，需要让动态链接器能找到
    SSV_LD_PATHS="$SSV_BUILD_DIR/gst/ssv-common"
    append_ld_path_if_dir "$SSV_OPENCV_ROOT"/usr/lib/*
    append_ld_path_if_dir "$SSV_ONNXRUNTIME_ROOT/lib"
    append_ld_path_if_dir "$SSV_TENSORRT_ROOT/usr/lib/x86_64-linux-gnu"
    append_ld_path_if_dir "$SSV_TENSORRT_ROOT/lib"
    append_ld_path_if_dir "$SSV_TENSORRT_ROOT/TensorRT-11.1.0.106/lib"
    append_nvidia_wheel_libs
    export LD_LIBRARY_PATH="$SSV_LD_PATHS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
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
