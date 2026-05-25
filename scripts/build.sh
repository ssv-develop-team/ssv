#!/bin/bash
# scripts/build.sh — 编译 C++ GStreamer 插件
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "编译 GStreamer 插件"

mkdir -p "$SSV_ROOT/.deps"
ssv_require_command "flock" \
    "sudo apt-get install util-linux" \
    "Debian/Ubuntu"
exec 9>"$SSV_ROOT/.deps/build.lock"
ssv_info "等待构建锁..."
flock 9

ensure_onnxruntime() {
    if pkg-config --exists onnxruntime; then
        return 0
    fi

    local version="${SSV_ONNXRUNTIME_VERSION:-1.25.1}"
    local root="${SSV_ONNXRUNTIME_ROOT:-$SSV_ROOT/.deps/onnxruntime}"
    local pc_file="$root/lib/pkgconfig/onnxruntime.pc"

    if [ -f "$pc_file" ]; then
        export PKG_CONFIG_PATH="$root/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        export LD_LIBRARY_PATH="$root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        pkg-config --exists onnxruntime && return 0
    fi

    local arch
    case "$(uname -m)" in
        x86_64|amd64) arch="x64" ;;
        aarch64|arm64) arch="aarch64" ;;
        *)
            ssv_error "unsupported ONNX Runtime architecture: $(uname -m)"
            return 1
            ;;
    esac

    local archive="onnxruntime-linux-${arch}-${version}.tgz"
    local url="https://github.com/microsoft/onnxruntime/releases/download/v${version}/${archive}"
    local tmp_dir="$SSV_ROOT/.deps/tmp/onnxruntime-${version}"

    ssv_info "ONNX Runtime not found; downloading ${archive}"
    rm -rf "$tmp_dir"
    mkdir -p "$tmp_dir" "$(dirname "$root")"

    if command -v curl >/dev/null 2>&1; then
        curl -fL "$url" -o "$tmp_dir/$archive"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$tmp_dir/$archive" "$url"
    else
        ssv_error "curl or wget is required to download ONNX Runtime"
        return 1
    fi

    tar -xzf "$tmp_dir/$archive" -C "$tmp_dir"
    rm -rf "$root"
    mv "$tmp_dir/onnxruntime-linux-${arch}-${version}" "$root"
    rm -rf "$tmp_dir"

    mkdir -p "$root/lib/pkgconfig"
    cat > "$pc_file" <<EOF
prefix=$root
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: onnxruntime
Description: ONNX Runtime C/C++ inference runtime
Version: $version
Libs: -L\${libdir} -lonnxruntime
Cflags: -I\${includedir}
EOF

    export PKG_CONFIG_PATH="$root/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LD_LIBRARY_PATH="$root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    pkg-config --exists onnxruntime
}

missing_deps=()
for dep in gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0 yaml-cpp hiredis nlohmann_json; do
    if ! pkg-config --exists "$dep"; then
        missing_deps+=("$dep")
    fi
done

if [ "${#missing_deps[@]}" -gt 0 ]; then
    ssv_error "缺少 C/C++ 开发依赖: ${missing_deps[*]}"
    ssv_warn "Debian/Ubuntu: sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libyaml-cpp-dev libhiredis-dev nlohmann-json3-dev"
    ssv_warn "ONNX Runtime: ./ssv build can download a local CPU release after base dependencies are installed"
    ssv_warn "Arch Linux: sudo pacman -S gstreamer gst-plugins-base yaml-cpp hiredis nlohmann-json onnxruntime-cpu"
    exit 1
fi

ensure_onnxruntime

if ! pkg-config --exists onnxruntime; then
    ssv_error "缺少 C/C++ 开发依赖: onnxruntime"
    ssv_warn "ONNX Runtime: see README.md for automatic local CPU release installation details"
    exit 1
fi

if [ -f "$SSV_BUILD_DIR/build.ninja" ]; then
    ssv_info "使用已有 Meson 构建目录: ${SSV_BUILD_DIR#$SSV_ROOT/}"
else
    if [ -d "$SSV_BUILD_DIR" ]; then
        ssv_warn "构建目录存在但不是有效的 Meson build，重新创建: ${SSV_BUILD_DIR#$SSV_ROOT/}"
        rm -rf "$SSV_BUILD_DIR"
    fi
    meson setup "$SSV_BUILD_DIR"
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
        ssv_info "编译成功: ${p#$SSV_ROOT/}"
    else
        ssv_warn "插件未生成: ${p#$SSV_ROOT/} (可能缺少依赖)"
        ok=false
    fi
done

if [ "$ok" = false ]; then
    ssv_warn "部分插件未编译 (请安装依赖: onnxruntime-cpu hiredis nlohmann-json)"
fi
