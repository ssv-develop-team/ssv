#!/bin/bash
# scripts/build.sh — 编译 C++ GStreamer 插件
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "编译 GStreamer 插件"

mkdir -p "$SSV_ROOT/.deps"
ssv_require_command "flock" \
    "install util-linux with your system package manager" \
    "local system"
exec 9>"$SSV_ROOT/.deps/build.lock"
ssv_info "等待构建锁..."
flock 9

prepend_pkg_config_path() {
    local dir="$1"
    if [ -d "$dir" ]; then
        export PKG_CONFIG_PATH="$dir${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    fi
}

use_local_pkg_config_paths() {
    if [ -n "${SSV_EXTRA_PKG_CONFIG_PATH:-}" ]; then
        export PKG_CONFIG_PATH="$SSV_EXTRA_PKG_CONFIG_PATH${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    fi

    local dir
    for dir in \
        "$SSV_ROOT"/.deps/pkgdeps/*/lib/pkgconfig \
        "$SSV_ROOT"/.deps/pkgdeps/*/usr/lib/*/pkgconfig; do
        prepend_pkg_config_path "$dir"
    done
}

download_file() {
    local url="$1"
    local output="$2"

    if command -v curl >/dev/null 2>&1; then
        curl -fL "$url" -o "$output"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$output" "$url"
    else
        ssv_error "curl or wget is required to download $url"
        return 1
    fi
}

extract_archive() {
    local archive="$1"
    local dest="$2"

    rm -rf "$dest"
    mkdir -p "$dest"
    case "$archive" in
        *.tar.zst|*.tzst)
            if tar --help 2>/dev/null | grep -q -- '--zstd'; then
                tar --zstd -xf "$archive" -C "$dest"
            else
                ssv_require_command "zstd" \
                    "install zstd" \
                    "your package manager"
                zstd -dc "$archive" | tar -xf - -C "$dest"
            fi
            ;;
        *.tar.gz|*.tgz) tar -xzf "$archive" -C "$dest" ;;
        *.tar.xz|*.txz) tar -xJf "$archive" -C "$dest" ;;
        *.tar.bz2|*.tbz2) tar -xjf "$archive" -C "$dest" ;;
        *.tar) tar -xf "$archive" -C "$dest" ;;
        *.zip)
            ssv_require_command "unzip" \
                "install unzip" \
                "your package manager"
            unzip -q "$archive" -d "$dest"
            ;;
        *)
            ssv_error "unsupported archive format: $archive"
            return 1
            ;;
    esac
}

ensure_onnxruntime() {
    local version="${SSV_ONNXRUNTIME_VERSION:-1.25.1}"
    local flavor="${SSV_ONNXRUNTIME_FLAVOR:-cpu}"
    local default_root="$SSV_ROOT/.deps/onnxruntime"
    if [ "$flavor" = "gpu" ]; then
        default_root="$SSV_ROOT/.deps/onnxruntime-gpu"
    elif [ "$flavor" != "cpu" ]; then
        ssv_error "unsupported SSV_ONNXRUNTIME_FLAVOR: $flavor (expected cpu or gpu)"
        return 1
    fi
    local root="${SSV_ONNXRUNTIME_ROOT:-$default_root}"
    local pc_file="$root/lib/pkgconfig/onnxruntime.pc"

    if [ "$flavor" = "cpu" ] && pkg-config --exists onnxruntime; then
        return 0
    fi

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

    local archive
    if [ "$flavor" = "gpu" ]; then
        archive="onnxruntime-linux-${arch}-gpu-${version}.tgz"
    else
        archive="onnxruntime-linux-${arch}-${version}.tgz"
    fi
    local url="https://github.com/microsoft/onnxruntime/releases/download/v${version}/${archive}"
    local tmp_dir="$SSV_ROOT/.deps/tmp/onnxruntime-${flavor}-${version}"

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
    if [ "$flavor" = "gpu" ]; then
        mv "$tmp_dir/onnxruntime-linux-${arch}-gpu-${version}" "$root"
    else
        mv "$tmp_dir/onnxruntime-linux-${arch}-${version}" "$root"
    fi
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

ensure_tensorrt() {
    local mode="${SSV_TENSORRT:-auto}"
    SSV_TENSORRT_MESON_MODE="$mode"
    case "$mode" in
        auto|enabled|disabled) ;;
        *)
            ssv_error "unsupported SSV_TENSORRT: $mode (expected auto, enabled, or disabled)"
            return 1
            ;;
    esac

    if [ "$mode" = "disabled" ]; then
        SSV_TENSORRT_MESON_MODE="disabled"
        return 0
    fi

    local root="${SSV_TENSORRT_ROOT:-$SSV_ROOT/.deps/tensorrt}"
    local pc_dir="$root/lib/pkgconfig"
    local pc_file="$pc_dir/nvinfer.pc"

    if [ -f "$pc_file" ]; then
        export PKG_CONFIG_PATH="$pc_dir${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        local existing_lib_dir
        existing_lib_dir="$(pkg-config --variable=libdir nvinfer 2>/dev/null || true)"
        if [ -n "$existing_lib_dir" ]; then
            export LD_LIBRARY_PATH="$existing_lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        fi
        if pkg-config --exists nvinfer; then
            SSV_TENSORRT_MESON_MODE="enabled"
            return 0
        fi
    fi

    if [ -n "${SSV_TENSORRT_ARCHIVE:-}" ]; then
        if [ ! -f "$SSV_TENSORRT_ARCHIVE" ]; then
            ssv_error "SSV_TENSORRT_ARCHIVE does not exist: $SSV_TENSORRT_ARCHIVE"
            return 1
        fi
        ssv_info "Extracting TensorRT archive to ${root#$SSV_ROOT/}"
        extract_archive "$SSV_TENSORRT_ARCHIVE" "$root"
    elif [ -n "${SSV_TENSORRT_URL:-}" ]; then
        local archive_name
        archive_name="${SSV_TENSORRT_URL##*/}"
        local archive_path="$SSV_ROOT/.deps/downloads/$archive_name"
        mkdir -p "$(dirname "$archive_path")"
        if [ ! -f "$archive_path" ]; then
            ssv_info "Downloading TensorRT archive to ${archive_path#$SSV_ROOT/}"
            download_file "$SSV_TENSORRT_URL" "$archive_path"
        fi
        ssv_info "Extracting TensorRT archive to ${root#$SSV_ROOT/}"
        extract_archive "$archive_path" "$root"
    fi

    local include_dir=""
    local lib_dir=""
    local found_path=""
    found_path="$(find "$root" -type f -name NvInfer.h -print -quit 2>/dev/null || true)"
    if [ -n "$found_path" ]; then
        include_dir="$(dirname "$found_path")"
    fi
    found_path="$(find "$root" -type f \( -name 'libnvinfer.so' -o -name 'libnvinfer.so.*' \) -print -quit 2>/dev/null || true)"
    if [ -n "$found_path" ]; then
        lib_dir="$(dirname "$found_path")"
    fi

    if [ "$mode" = "auto" ]; then
        if [ -n "$include_dir" ] && [ -n "$lib_dir" ]; then
            :
        else
            SSV_TENSORRT_MESON_MODE="disabled"
            return 0
        fi
    fi

    if [ -z "$include_dir" ] || [ -z "$lib_dir" ]; then
        ssv_error "TensorRT SDK not found under $root"
        ssv_warn "Set SSV_TENSORRT_ROOT to an unpacked TensorRT SDK, or explicitly set SSV_TENSORRT_ARCHIVE/SSV_TENSORRT_URL."
        if [ "$mode" = "enabled" ]; then
            return 1
        fi
        SSV_TENSORRT_MESON_MODE="disabled"
        return 0
    fi

    if [ ! -e "$lib_dir/libnvinfer.so" ]; then
        local versioned_lib
        versioned_lib="$(find "$lib_dir" -maxdepth 1 -type f -name 'libnvinfer.so.*' -print -quit)"
        if [ -n "$versioned_lib" ] && [ -w "$lib_dir" ]; then
            ln -sf "$(basename "$versioned_lib")" "$lib_dir/libnvinfer.so"
        fi
    fi

    if [ ! -e "$lib_dir/libnvinfer.so" ]; then
        ssv_error "TensorRT SDK under $root does not provide linkable libnvinfer.so"
        return 1
    fi

    local cuda_root="${CUDA_HOME:-}"
    local candidate
    if [ -z "$cuda_root" ]; then
        for candidate in /usr/local/cuda /usr/local/cuda-*; do
            if [ -f "$candidate/targets/x86_64-linux/include/cuda_runtime_api.h" ] || [ -f "$candidate/include/cuda_runtime_api.h" ]; then
                cuda_root="$candidate"
                break
            fi
        done
    fi

    local cuda_include=""
    local cuda_lib=""
    for candidate in \
        "$cuda_root/targets/x86_64-linux/include" \
        "$cuda_root/include"; do
        if [ -f "$candidate/cuda_runtime_api.h" ]; then
            cuda_include="$candidate"
            break
        fi
    done
    for candidate in \
        "$cuda_root/targets/x86_64-linux/lib" \
        "$cuda_root/lib64" \
        "$cuda_root/lib"; do
        if [ -e "$candidate/libcudart.so" ]; then
            cuda_lib="$candidate"
            break
        fi
    done

    if [ ! -f "$cuda_include/cuda_runtime_api.h" ] || [ ! -e "$cuda_lib/libcudart.so" ]; then
        ssv_error "CUDA toolkit not found; set CUDA_HOME before building TensorRT"
        return 1
    fi

    local version="${SSV_TENSORRT_VERSION:-local}"
    mkdir -p "$pc_dir"
    cat > "$pc_file" <<EOF
prefix=$root
exec_prefix=\${prefix}
libdir=$lib_dir
includedir=$include_dir
cudaincludedir=$cuda_include
cudalibdir=$cuda_lib

Name: nvinfer
Description: NVIDIA TensorRT inference runtime
Version: ${version%%-*}
Libs: -L\${libdir} -lnvinfer -L\${cudalibdir} -lcudart
Cflags: -I\${includedir} -I\${cudaincludedir}
EOF

    export PKG_CONFIG_PATH="$pc_dir${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LD_LIBRARY_PATH="$lib_dir:$cuda_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    SSV_TENSORRT_MESON_MODE="enabled"
    pkg-config --exists nvinfer
}

use_local_pkg_config_paths

missing_deps=()
for dep in gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0 yaml-cpp hiredis nlohmann_json; do
    if ! pkg-config --exists "$dep"; then
        missing_deps+=("$dep")
    fi
done

if [ "${#missing_deps[@]}" -gt 0 ]; then
    ssv_error "缺少 C/C++ 开发依赖: ${missing_deps[*]}"
    ssv_warn "Install GStreamer, yaml-cpp, hiredis, and nlohmann-json development packages with your system package manager."
    ssv_warn "If pkg-config files live outside the default search path, set SSV_EXTRA_PKG_CONFIG_PATH or PKG_CONFIG_PATH."
    ssv_warn "ONNX Runtime: ./ssv build can download a local CPU release after base dependencies are installed"
    exit 1
fi

ensure_onnxruntime
ensure_tensorrt

if ! pkg-config --exists onnxruntime; then
    ssv_error "缺少 C/C++ 开发依赖: onnxruntime"
    ssv_warn "ONNX Runtime: see README.md for automatic local CPU release installation details"
    exit 1
fi

meson_tensorrt_args=()
case "${SSV_TENSORRT_MESON_MODE:-${SSV_TENSORRT:-auto}}" in
    enabled) meson_tensorrt_args=(-Dtensorrt=enabled) ;;
    disabled) meson_tensorrt_args=(-Dtensorrt=disabled) ;;
    *) meson_tensorrt_args=(-Dtensorrt=auto) ;;
esac
meson_pkg_config_args=()
if [ -n "${PKG_CONFIG_PATH:-}" ]; then
    meson_pkg_config_args=(-Dpkg_config_path="$PKG_CONFIG_PATH")
fi

if [ -f "$SSV_BUILD_DIR/build.ninja" ]; then
    ssv_info "使用已有 Meson 构建目录: ${SSV_BUILD_DIR#$SSV_ROOT/}"
    meson setup "$SSV_BUILD_DIR" --reconfigure "${meson_tensorrt_args[@]}" "${meson_pkg_config_args[@]}"
else
    if [ -d "$SSV_BUILD_DIR" ]; then
        ssv_warn "构建目录存在但不是有效的 Meson build，重新创建: ${SSV_BUILD_DIR#$SSV_ROOT/}"
        rm -rf "$SSV_BUILD_DIR"
    fi
    meson setup "$SSV_BUILD_DIR" "${meson_tensorrt_args[@]}" "${meson_pkg_config_args[@]}"
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
