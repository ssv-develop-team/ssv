#!/bin/bash
# scripts/test.sh — 测试编排
set -euo pipefail
source "$(dirname "$0")/lib.sh"
source "$(dirname "$0")/deps.sh"
cd "$SSV_ROOT"

ssv_header "运行测试套件"

ssv_info "步骤 1/8: 运行依赖脚本测试"
bash "$SSV_ROOT/tests/ssv_deps_test.sh"

ssv_require_command "uv" "pip install uv" "All"

ssv_info "步骤 2/8: 运行模型准备契约测试"
uv run --isolated --script tests/ssv_prepare_model_test.py

ssv_info "步骤 3/8: 运行 TensorRT manifest 契约测试"
uv run --isolated --script tests/ssv_tensorrt_manifest_test.py

ssv_info "步骤 4/8: 编译 C++ runner、插件和测试"
bash "$SSV_ROOT/scripts/build.sh"

ssv_deps_load_runtime

ssv_info "步骤 5/8: 运行 Meson 测试"
meson test -C "$SSV_BUILD_DIR" --print-errorlogs

ssv_info "步骤 6/8: 运行 CLI 脚本测试"
bash "$SSV_ROOT/tests/ssv_cli_test.sh"

ssv_info "步骤 7/8: 运行 Python Agent 测试"
(cd "$SSV_ROOT/agent" && uv run --extra dev pytest)

if [ -n "$SSV_CONFIG" ] && [ -f "$SSV_CONFIG" ]; then
    ssv_require_command "timeout" \
        "sudo apt-get install coreutils" \
        "Debian/Ubuntu"
    SMOKE_TIMEOUT="30s"
    ssv_info "步骤 8/8: 运行 C++ runner 有界链路冒烟测试 (${SMOKE_TIMEOUT})"
    set +e
    timeout \
        --foreground \
        --signal=INT \
        --kill-after=5s \
        "$SMOKE_TIMEOUT" \
        bash "$SSV_ROOT/scripts/run.sh" \
            --config "$SSV_CONFIG" \
            --headless
    smoke_status=$?
    set -e
    if [ "$smoke_status" -ne 0 ] && [ "$smoke_status" -ne 124 ]; then
        if [ "${SSV_REQUIRE_SMOKE:-false}" = "true" ]; then
            exit "$smoke_status"
        fi
        ssv_warn "链路冒烟测试失败，已作为警告继续: status=$smoke_status"
    fi
else
    ssv_warn "跳过链路冒烟测试: 未找到本地运行配置"
fi

ssv_info "测试套件完成"
