#!/bin/bash
# scripts/inspect.sh — 检查 ssvtemplate 插件信息
set -euo pipefail
source "$(dirname "$0")/lib.sh"

ssv_header "检查插件: ssvtemplate"

ssv_require_command "gst-inspect-1.0" \
    "sudo apt-get install gstreamer1.0-tools" \
    "Debian/Ubuntu"

export GST_PLUGIN_PATH="$SSV_PLUGIN_DIR:${GST_PLUGIN_PATH:-}"
GST_DEBUG="ssv*:5" gst-inspect-1.0 ssvtemplate
