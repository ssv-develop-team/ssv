#!/bin/bash
# scripts/test.sh — 测试编排
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "运行测试套件"

ssv_info "步骤 1/5: 编译 C++ 插件"
bash "$SSV_ROOT/scripts/build.sh"

ssv_info "步骤 2/5: 运行 Meson 测试"
meson test -C "$SSV_BUILD_DIR"

ssv_info "步骤 3/5: 运行 CLI 脚本测试"
bash "$SSV_ROOT/tests/ssv_cli_test.sh"

ssv_info "步骤 4/5: 运行 Python Agent 测试"
ssv_require_command "uv" "pip install uv" "All"
(cd "$SSV_ROOT/agent" && uv run --extra dev pytest)

MODEL="${SSV_MODEL_PATH:-models/yolov8n.onnx}"
if [ -n "${SSV_RTSP_URL:-}" ] && [ -f "$MODEL" ]; then
    ssv_info "步骤 5/5: 运行链路冒烟测试"
    set +e
    bash "$SSV_ROOT/scripts/pipeline.sh" --smoke --skip-build
    smoke_status=$?
    set -e
    if [ "$smoke_status" -ne 0 ] && [ "$smoke_status" -ne 124 ]; then
        if [ "${SSV_REQUIRE_SMOKE:-false}" = "true" ]; then
            exit "$smoke_status"
        fi
        ssv_warn "链路冒烟测试失败，已作为警告继续: status=$smoke_status"
    fi
else
    ssv_warn "跳过链路冒烟测试: 需要 SSV_RTSP_URL 和可用模型文件"
fi

ssv_info "测试套件完成"
