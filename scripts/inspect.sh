#!/bin/bash
# scripts/inspect.sh — 检查 ssvtemplate 插件信息
set -euo pipefail
source "$(dirname "$0")/lib.sh"
source "$(dirname "$0")/deps.sh"

ssv_header "检查插件: ssvtemplate"

ssv_require_command "gst-inspect-1.0" \
    "sudo apt-get install gstreamer1.0-tools" \
    "Debian/Ubuntu"

ssv_deps_load_runtime
export_ssv_plugin_path
GST_DEBUG="ssv*:5" gst-inspect-1.0 ssvtemplate
