#!/bin/bash
# scripts/build.sh — 编译 C++ GStreamer 插件
set -euo pipefail
source "$(dirname "$0")/lib.sh"
source "$(dirname "$0")/deps.sh"

ssv_build_usage() {
    printf 'usage: ./ssv build [--profile auto|cpu|nvidia|intel|amd]\n'
}

ssv_build_parse_args() {
    requested_profile=auto
    profile_seen=false
    show_build_help=false
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --profile)
                [ "$profile_seen" = false ] || {
                    ssv_deps_die "--profile may be specified only once"
                    return 1
                }
                if [ "$#" -lt 2 ] || [[ "${2:-}" == -* ]]; then
                    ssv_deps_die "--profile requires a value"
                    return 1
                fi
                requested_profile="$2"
                profile_seen=true
                shift 2
                ;;
            --profile=*)
                [ "$profile_seen" = false ] || {
                    ssv_deps_die "--profile may be specified only once"
                    return 1
                }
                requested_profile="${1#--profile=}"
                [ -n "$requested_profile" ] || {
                    ssv_deps_die "--profile requires a value"
                    return 1
                }
                profile_seen=true
                shift
                ;;
            -h|--help)
                show_build_help=true
                shift
                ;;
            *)
                ssv_deps_die "unknown build argument: $1"
                return 1
                ;;
        esac
    done
    ssv_onnxruntime_validate_profile "$requested_profile"
}

if ! ssv_build_parse_args "$@"; then
    ssv_build_usage >&2
    exit 2
fi
if [ "$show_build_help" = true ]; then
    ssv_build_usage
    exit 0
fi

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
ssv_deps_prepare "$requested_profile"

previous_dependency_signature=""
if [ -f "$SSV_BUILD_DIR/ssv-deps.env" ]; then
    previous_dependency_signature="$(
        sed -n 's/^SSV_DEPS_SIGNATURE=//p' \
            "$SSV_BUILD_DIR/ssv-deps.env"
    )"
fi
dependency_cache_changed=false
if [ "$previous_dependency_signature" != "$SSV_DEPS_SIGNATURE" ]; then
    dependency_cache_changed=true
fi

meson_opencv_args=(-Dopencv_mode="$SSV_DEPS_OPENCV_MODE")
meson_tensorrt_args=(-Dtensorrt_mode="$SSV_DEPS_TENSORRT_MODE")
meson_profile_args=(
    -Donnxruntime_profile="$SSV_DEPS_PROFILE"
    -Donnxruntime_dependency_signature="$SSV_DEPS_SIGNATURE"
)
meson_runtime_args=(-Ddeps_runtime_path="$SSV_DEPS_RUNTIME_PATH")
meson_pkg_config_args=()
if [ -n "$SSV_DEPS_PKG_CONFIG_PATH" ]; then
    meson_pkg_config_args=(-Dpkg_config_path="$SSV_DEPS_PKG_CONFIG_PATH")
fi

if [ -f "$SSV_BUILD_DIR/build.ninja" ]; then
    ssv_info "使用已有 Meson 构建目录: ${SSV_BUILD_DIR#"$SSV_ROOT"/}"
    # Refresh project option definitions before applying options introduced
    # after this build directory was first configured.
    meson setup "$SSV_BUILD_DIR" --reconfigure
    if [ "$dependency_cache_changed" = true ]; then
        if meson setup --help 2>/dev/null | grep -Fq -- '--clearcache'; then
            meson setup "$SSV_BUILD_DIR" --reconfigure --clearcache \
                "${meson_opencv_args[@]}" "${meson_tensorrt_args[@]}" \
                "${meson_profile_args[@]}" "${meson_runtime_args[@]}" \
                "${meson_pkg_config_args[@]}"
        else
            # Meson 1.1-1.2 has no supported dependency-cache reset. Its
            # documented wipe path is required when resolved SDKs change.
            meson setup "$SSV_BUILD_DIR" --wipe \
                "${meson_opencv_args[@]}" "${meson_tensorrt_args[@]}" \
                "${meson_profile_args[@]}" "${meson_runtime_args[@]}" \
                "${meson_pkg_config_args[@]}"
            ssv_deps_write_env "$pending_env"
        fi
    else
        meson setup "$SSV_BUILD_DIR" --reconfigure \
            "${meson_opencv_args[@]}" "${meson_tensorrt_args[@]}" \
            "${meson_profile_args[@]}" "${meson_runtime_args[@]}" \
            "${meson_pkg_config_args[@]}"
    fi
else
    meson setup "$SSV_BUILD_DIR" "${meson_opencv_args[@]}" "${meson_tensorrt_args[@]}" "${meson_profile_args[@]}" "${meson_runtime_args[@]}" "${meson_pkg_config_args[@]}"
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
