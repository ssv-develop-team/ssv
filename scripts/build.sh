#!/bin/bash
# scripts/build.sh — 编译 C++ GStreamer 插件
set -euo pipefail
source "$(dirname "$0")/lib.sh"
source "$(dirname "$0")/deps.sh"
cd "$SSV_ROOT"

ssv_header "编译 GStreamer 插件"

mkdir -p "$SSV_ROOT/.deps"
ssv_require_command "flock" \
    "install util-linux with your system package manager" \
    "local system"
exec 9>"$SSV_ROOT/.deps/build.lock"
ssv_info "等待构建锁..."
flock 9

if [ -n "${SSV_EXTRA_PKG_CONFIG_PATH:-}" ]; then
    export PKG_CONFIG_PATH="$SSV_EXTRA_PKG_CONFIG_PATH${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi

missing_deps=()
for dep in gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0 yaml-cpp hiredis nlohmann_json blas lapack; do
    if ! pkg-config --exists "$dep"; then
        missing_deps+=("$dep")
    fi
done

if [ "${#missing_deps[@]}" -gt 0 ]; then
    ssv_error "缺少 C/C++ 开发依赖: ${missing_deps[*]}"
    ssv_warn "Install GStreamer, yaml-cpp, hiredis, nlohmann-json, BLAS, and LAPACK development packages with your system package manager."
    ssv_warn "If pkg-config files live outside the default search path, set SSV_EXTRA_PKG_CONFIG_PATH or PKG_CONFIG_PATH."
    ssv_warn "ONNX Runtime: ./ssv build can download a local CPU release after base dependencies are installed"
    exit 1
fi

if [ -d "$SSV_BUILD_DIR" ] && [ ! -f "$SSV_BUILD_DIR/build.ninja" ]; then
    ssv_warn "构建目录存在但不是有效的 Meson build，重新创建: ${SSV_BUILD_DIR#"$SSV_ROOT"/}"
    rm -rf -- "$SSV_BUILD_DIR"
fi
mkdir -p "$SSV_BUILD_DIR"
pending_env="$SSV_BUILD_DIR/ssv-deps.env.pending"
cleanup_pending_env() {
    rm -f -- "$pending_env"
}
trap cleanup_pending_env EXIT
ssv_deps_prepare
old_signature="$(sed -n 's/^SSV_DEPS_SIGNATURE=//p' "$SSV_BUILD_DIR/ssv-deps.env" 2>/dev/null || true)"

meson_opencv_args=(-Dopencv_mode="$SSV_DEPS_OPENCV_MODE")
meson_tensorrt_args=(-Dtensorrt_mode="$SSV_DEPS_TENSORRT_MESON_MODE")
meson_runtime_args=(-Ddeps_runtime_path="$SSV_DEPS_RUNTIME_PATH")
meson_pkg_config_args=()
if [ -n "$SSV_DEPS_PKG_CONFIG_PATH" ]; then
    meson_pkg_config_args=(-Dpkg_config_path="$SSV_DEPS_PKG_CONFIG_PATH")
fi

if [ -f "$SSV_BUILD_DIR/build.ninja" ]; then
    ssv_info "使用已有 Meson 构建目录: ${SSV_BUILD_DIR#"$SSV_ROOT"/}"
    if [ "$old_signature" != "$SSV_DEPS_SIGNATURE" ]; then
        meson setup "$SSV_BUILD_DIR" --reconfigure "${meson_opencv_args[@]}" "${meson_tensorrt_args[@]}" "${meson_runtime_args[@]}" "${meson_pkg_config_args[@]}"
    fi
else
    meson setup "$SSV_BUILD_DIR" "${meson_opencv_args[@]}" "${meson_tensorrt_args[@]}" "${meson_runtime_args[@]}" "${meson_pkg_config_args[@]}"
fi

meson compile -C "$SSV_BUILD_DIR"

# 检查所有插件产物
plugins=(
    "$SSV_BUILD_DIR/gst/ssv-template/libgstssvtemplate.so"
    "$SSV_BUILD_DIR/gst/ssv-infer/libgstssvinfer.so"
    "$SSV_BUILD_DIR/gst/ssv-track/libgstssvtrack.so"
    "$SSV_BUILD_DIR/gst/ssv-pub/libgstssvpub.so"
    "$SSV_BUILD_DIR/gst/ssv-overlay/libgstssvoverlay.so"
)

ok=true
for p in "${plugins[@]}"; do
    if [ -f "$p" ]; then
        ssv_info "编译成功: ${p#"$SSV_ROOT"/}"
    else
        ssv_warn "插件未生成: ${p#"$SSV_ROOT"/} (可能缺少依赖)"
        ok=false
    fi
done

if [ "$ok" = false ]; then
    ssv_error "部分插件未生成，构建结果不完整"
    exit 1
fi

mv -f -- "$pending_env" "$SSV_BUILD_DIR/ssv-deps.env"
trap - EXIT
