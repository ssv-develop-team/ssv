#!/usr/bin/env bash

# OpenCV managed provider.
#
# The public contract is an installed OpenCV tree with a generated
# opencv4.pc. The provider owns the source, CMake build and install trees under
# one workspace, while the source archive is kept in the shared download cache.
SSV_OPENCV_SOURCE_URL="https://github.com/opencv/opencv/archive/refs/tags/%s.tar.gz"
SSV_OPENCV_REQUIRED_MODULES=(core imgproc video calib3d features2d flann dnn)

ssv_opencv_find_include_dir() {
    local root="$1"
    local header
    header="$(find "$root" -type f -path '*/opencv4/opencv2/core.hpp' -print -quit)"
    [ -n "$header" ] || return 1
    dirname -- "$(dirname -- "$header")"
}

ssv_opencv_find_lib_dir() {
    local root="$1"
    local library
    library="$(find "$root" -type f -name 'libopencv_core.so*' -print -quit)"
    [ -n "$library" ] || return 1
    dirname -- "$library"
}

ssv_opencv_find_library() {
    local lib_dir="$1" module="$2"
    local candidate
    for candidate in "$lib_dir/libopencv_${module}.so" "$lib_dir"/libopencv_"$module".so.*; do
        [ -e "$candidate" ] || continue
        printf '%s\n' "$candidate"
        return 0
    done
    return 1
}

ssv_opencv_validate_required_libraries() {
    local lib_dir="$1" module
    for module in "${SSV_OPENCV_REQUIRED_MODULES[@]}"; do
        ssv_opencv_find_library "$lib_dir" "$module" >/dev/null || {
            ssv_deps_die "OpenCV library missing: libopencv_${module}.so"
            return 1
        }
    done
}

ssv_opencv_validate_runtime_closure() {
    local lib_dir="$1"
    shift
    local modules=("$@")
    [ "${#modules[@]}" -gt 0 ] || modules=("${SSV_OPENCV_REQUIRED_MODULES[@]}")
    local module library needed base readelf_output ldd_output missing symbol_errors
    for module in "${modules[@]}"; do
        library="$(ssv_opencv_find_library "$lib_dir" "$module")" || return 1
        readelf_output="$(readelf -d "$library" 2>&1)" || {
            ssv_deps_die "OpenCV library is not a readable dynamic ELF: $(basename -- "$library"): $readelf_output"
            return 1
        }
        while read -r needed; do
            base="${needed#libopencv_}"
            base="${base%%.so*}"
            [ -n "$base" ] || continue
            if ! find "$lib_dir" -maxdepth 1 \( \
                -name "libopencv_${base}.so" -o \
                -name "libopencv_${base}.so.*" \
            \) -print -quit | grep -q .; then
                ssv_deps_die "OpenCV runtime closure is incomplete: $(basename -- "$library") needs $needed"
                return 1
            fi
        done < <(printf '%s\n' "$readelf_output" \
            | sed -n 's/.*Shared library: \[\(libopencv_[^]]*\)\].*/\1/p')
        ldd_output="$(LD_LIBRARY_PATH="$lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            ldd -r "$library" 2>&1)" || {
            ssv_deps_die "OpenCV runtime load check failed for $(basename -- "$library"): $ldd_output"
            return 1
        }
        missing="$(printf '%s\n' "$ldd_output" \
            | sed -n 's/^[[:space:]]*\([^[:space:]]*\) => not found$/\1/p' \
            | paste -sd, -)"
        symbol_errors="$(printf '%s\n' "$ldd_output" \
            | sed -n 's/.*undefined symbol: \([^[:space:]]*\).*/\1/p' \
            | paste -sd, -)"
        if [ -n "$missing" ] || [ -n "$symbol_errors" ]; then
            local unresolved="$missing"
            [ -z "$symbol_errors" ] || unresolved="${unresolved:+$unresolved,}$symbol_errors"
            ssv_deps_die "OpenCV runtime dependency is unresolved for $(basename -- "$library"): $unresolved"
            return 1
        fi
    done
}

ssv_opencv_validate_libraries() {
    local lib_dir="$1"
    ssv_opencv_validate_required_libraries "$lib_dir" || return 1
    ssv_opencv_validate_runtime_closure "$lib_dir" "${SSV_OPENCV_REQUIRED_MODULES[@]}"
}

ssv_opencv_make_pc() {
    local root="$1" version="$2" include_dir="$3" lib_dir="$4"
    local pc_dir="${5:-$root/lib/pkgconfig}"
    local host_math_libs
    if pkg-config --exists cblas; then
        host_math_libs="$(pkg-config --libs cblas lapack blas)" || return 1
    else
        host_math_libs="$(pkg-config --libs lapack blas)" || return 1
    fi
    mkdir -p -- "$pc_dir"
    ssv_deps_write_pc "$pc_dir/opencv4.pc" \
        "prefix=$root" \
        'exec_prefix=${prefix}' \
        "libdir=$lib_dir" \
        "includedir=$include_dir" \
        "" \
        "Name: opencv4" \
        "Description: OpenCV runtime" \
        "Version: $version" \
        "Libs: -L\"\${libdir}\" -Wl,-rpath-link,\"\${libdir}\" -lopencv_calib3d -lopencv_video -lopencv_features2d -lopencv_flann -lopencv_imgproc -lopencv_core $host_math_libs" \
        'Cflags: -I"${includedir}"'
}

ssv_opencv_probe_version() {
    local expected_version="$1" lib_dir="$2"
    ssv_deps_pkgconfig_version_at_least opencv4 4.5 || return 1
    local runtime_dirs
    runtime_dirs="$(ssv_deps_runtime_dirs "$lib_dir")"
    ssv_deps_compile_probe \
        "#include <opencv2/core.hpp>
#include <string>
int main() {
    return std::string(CV_VERSION) == \"$expected_version\" &&
                   cv::getVersionString() == \"$expected_version\"
        ? 0 : 1;
}" \
        opencv4 "$runtime_dirs" || return 1
    printf '%s\n' "$runtime_dirs"
}

ssv_opencv_validate_layout() {
    local root="$1" expected_version="$2"
    local include_dir lib_dir
    include_dir="$(ssv_opencv_find_include_dir "$root")" || {
        ssv_deps_die "OpenCV headers not found under $root"
        return 1
    }
    lib_dir="$(ssv_opencv_find_lib_dir "$root")" || {
        ssv_deps_die "OpenCV libraries not found under $root"
        return 1
    }
    ssv_opencv_validate_libraries "$lib_dir" || return 1
    ssv_opencv_make_pc "$root" "$expected_version" "$include_dir" "$lib_dir"
    local old_pkg_config_path="${PKG_CONFIG_PATH:-}"
    export PKG_CONFIG_PATH="$root/lib/pkgconfig${old_pkg_config_path:+:$old_pkg_config_path}"
    local pc_dir
    pc_dir="$(ssv_deps_pkgconfig_dir opencv4)" || return 1
    [ "$pc_dir" = "$(cd -- "$root/lib/pkgconfig" && pwd -P)" ] || {
        ssv_deps_die "OpenCV pkg-config source mismatch: $pc_dir"
        return 1
    }
    local runtime_dirs
    runtime_dirs="$(ssv_opencv_probe_version "$expected_version" "$lib_dir")" || {
        ssv_deps_die "OpenCV compile/load probe failed (expected $expected_version)"
        return 1
    }
    local actual_version
    actual_version="$(pkg-config --modversion opencv4)"
    printf 'version=%s\n' "$actual_version"
    printf 'pkgconfig_dir=%s\n' "$pc_dir"
    printf 'runtime_dirs=%s\n' "$runtime_dirs"
}

ssv_opencv_managed_validate() {
    ssv_opencv_validate_layout "$1" "$2"
}

ssv_opencv_source_url() {
    local version="$1"
    printf '%s\n' "${SSV_OPENCV_SOURCE_URL//%s/$version}"
}

ssv_opencv_source_dir_is_ready() {
    local source_dir="$1"
    [ -f "$source_dir/CMakeLists.txt" ]
}

ssv_opencv_source_root_is_replaceable() {
    local source_dir="$1"
    if [ ! -e "$source_dir" ]; then
        return 0
    fi
    [ -d "$source_dir" ] || {
        ssv_deps_die "refusing to replace non-directory OpenCV source root: $source_dir"
        return 1
    }
    ssv_deps_is_empty_dir "$source_dir" && return 0
    ssv_opencv_source_dir_is_ready "$source_dir" && return 0
    ssv_deps_die "refusing to replace non-empty unrecognized OpenCV source root: $source_dir"
    return 1
}

ssv_opencv_build_root_is_replaceable() {
    local build_dir="$1"
    if [ ! -e "$build_dir" ]; then
        return 0
    fi
    [ -d "$build_dir" ] || {
        ssv_deps_die "refusing to replace non-directory OpenCV build root: $build_dir"
        return 1
    }
    ssv_deps_is_empty_dir "$build_dir" && return 0
    if [ -f "$build_dir/CMakeCache.txt" ] && [ -d "$build_dir/CMakeFiles" ]; then
        return 0
    fi
    ssv_deps_die "refusing to replace non-empty unrecognized OpenCV build root: $build_dir"
    return 1
}

ssv_opencv_require_build_tools() {
    ssv_deps_require_command cmake || return 1
    local cxx="${CXX:-}" compiler
    if [ -n "$cxx" ]; then
        compiler="${cxx%%[[:space:]]*}"
        ssv_deps_require_command "$compiler" || return 1
    else
        compiler=""
        local candidate
        for candidate in c++ g++ clang++; do
            if ssv_deps_have_command "$candidate"; then
                compiler="$candidate"
                break
            fi
        done
        [ -n "$compiler" ] || {
            ssv_deps_die "OpenCV source build requires a C++ compiler (c++, g++, or clang++)"
            return 1
        }
        export CXX="$compiler"
    fi
    if ! ssv_deps_have_command make && ! ssv_deps_have_command ninja; then
        ssv_deps_die "OpenCV source build requires make or ninja"
        return 1
    fi
}

ssv_opencv_jobs() {
    local jobs="${SSV_OPENCV_BUILD_JOBS:-}"
    if [ -z "$jobs" ]; then
        jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')"
    fi
    case "$jobs" in
        ''|*[!0-9]*|0)
            ssv_deps_die "SSV_OPENCV_BUILD_JOBS must be a positive integer"
            return 1
            ;;
    esac
    printf '%s\n' "$jobs"
}

ssv_opencv_extract_source() {
    local archive="$1" source_parent="$2" version="$3"
    local stage
    stage="$(ssv_deps_make_candidate_dir "$source_parent/opencv-$version" "opencv-source-$version")" || return 1
    if ! ssv_deps_extract_archive "$archive" "$stage"; then
        rm -rf -- "$stage"
        return 1
    fi
    local extracted="$stage/opencv-$version"
    if [ ! -d "$extracted" ] || ! ssv_opencv_source_dir_is_ready "$extracted"; then
        rm -rf -- "$stage"
        ssv_deps_die "OpenCV source archive has no expected directory or CMakeLists.txt: opencv-$version"
        return 1
    fi
    printf '%s\n' "$extracted"
}

ssv_opencv_configure_and_build() {
    local source_dir="$1" build_dir="$2" install_dir="$3" install_prefix="$4" jobs="$5"
    local -a generator_args=()
    if ! ssv_deps_have_command make && ssv_deps_have_command ninja; then
        generator_args=(-G Ninja)
    fi
    local -a cmake_args=(
        -S "$source_dir"
        -B "$build_dir"
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX="$install_prefix"
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_opencv_core=ON
        -DBUILD_opencv_imgproc=ON
        -DBUILD_opencv_video=ON
        -DBUILD_opencv_calib3d=ON
        -DBUILD_opencv_features2d=ON
        -DBUILD_opencv_flann=ON
        -DBUILD_opencv_dnn=ON
        -DBUILD_opencv_imgcodecs=OFF
        -DBUILD_opencv_videoio=OFF
        -DBUILD_opencv_apps=OFF
        -DBUILD_opencv_gapi=OFF
        -DBUILD_opencv_highgui=OFF
        -DBUILD_opencv_ml=OFF
        -DBUILD_opencv_objdetect=OFF
        -DBUILD_opencv_photo=OFF
        -DBUILD_opencv_stitching=OFF
        -DBUILD_opencv_ts=OFF
        -DBUILD_PROTOBUF=ON
        -DBUILD_TESTS=OFF
        -DBUILD_PERF_TESTS=OFF
        -DBUILD_EXAMPLES=OFF
        -DBUILD_DOCS=OFF
        -DBUILD_JAVA=OFF
        -DBUILD_OBJC=OFF
        -DWITH_EIGEN=OFF
        -DWITH_1394=OFF
        -DWITH_FFMPEG=OFF
        -DWITH_GSTREAMER=OFF
        -DWITH_GTK=OFF
        -DWITH_CUDA=OFF
        -DWITH_CUDNN=OFF
        -DWITH_IPP=OFF
        -DWITH_ITT=OFF
        -DWITH_JPEG=OFF
        -DWITH_PNG=OFF
        -DWITH_TIFF=OFF
        -DWITH_WEBP=OFF
        -DWITH_OPENEXR=OFF
        -DWITH_OPENCL=OFF
        -DWITH_OPENGL=OFF
        -DWITH_OPENMP=OFF
        -DWITH_V4L=OFF
        -DWITH_VULKAN=OFF
        -DOPENCV_GENERATE_PKGCONFIG=OFF
        -DCPU_BASELINE=DETECT
        -DCV_TRACE=OFF
    )
    ssv_info "configuring managed OpenCV source" >&2
    local configure_log="$build_dir/ssv-cmake-configure.log"
    if ! cmake "${generator_args[@]}" "${cmake_args[@]}" >"$configure_log" 2>&1; then
        ssv_deps_die "OpenCV CMake configuration failed; see $configure_log"
        tail -n 80 "$configure_log" >&2 || true
        return 1
    fi
    ssv_info "building managed OpenCV source (${jobs} threads)" >&2
    local build_log="$build_dir/ssv-cmake-build.log"
    if ! cmake --build "$build_dir" --parallel "$jobs" >"$build_log" 2>&1; then
        ssv_deps_die "OpenCV compilation failed; see $build_log"
        tail -n 80 "$build_log" >&2 || true
        return 1
    fi
    ssv_info "installing managed OpenCV to $install_dir" >&2
    local install_log="$build_dir/ssv-cmake-install.log"
    if ! cmake --install "$build_dir" --prefix "$install_dir" >"$install_log" 2>&1; then
        ssv_deps_die "OpenCV installation failed; see $install_log"
        tail -n 80 "$install_log" >&2 || true
        return 1
    fi
    rm -f -- "$configure_log" "$build_log" "$install_log"
}

ssv_opencv_managed_prepare() {
    local workspace_root="$1" expected_version="$2"
    local source_parent="$workspace_root/source"
    local source_dir="$source_parent/opencv-$expected_version"
    local build_dir="$workspace_root/build"
    local install_dir="$workspace_root/install"

    if [ -d "$install_dir" ]; then
        local existing_result
        if existing_result="$(ssv_opencv_managed_validate "$install_dir" "$expected_version" 2>/dev/null)"; then
            printf '%s\n' "$existing_result"
            return 0
        fi
    fi

    ssv_deps_require_replaceable_root "$install_dir" ssv_opencv_managed_validate "$expected_version" || return 1
    ssv_opencv_build_root_is_replaceable "$build_dir" || return 1
    ssv_opencv_source_root_is_replaceable "$source_dir" || return 1
    ssv_opencv_require_build_tools || return 1
    mkdir -p -- "$source_parent"
    if ! ssv_opencv_source_dir_is_ready "$source_dir"; then
        local cache_dir="$SSV_ROOT/.deps/downloads/opencv/$expected_version"
        local archive="$cache_dir/opencv-$expected_version.tar.gz"
        local url
        url="$(ssv_opencv_source_url "$expected_version")"
        ssv_deps_cached_download "$url" "$archive" >/dev/null || return 1
        local source_candidate
        if ! source_candidate="$(ssv_opencv_extract_source "$archive" "$source_parent" "$expected_version")"; then
            ssv_warn "cached OpenCV source archive is invalid; downloading it once more: $(basename -- "$archive")"
            rm -f -- "$archive"
            ssv_deps_cached_download "$url" "$archive" >/dev/null || return 1
            source_candidate="$(ssv_opencv_extract_source "$archive" "$source_parent" "$expected_version")" || return 1
        fi
        if ! ssv_deps_atomic_replace_dir "$source_candidate" "$source_dir"; then
            rm -rf -- "$(dirname -- "$source_candidate")"
            return 1
        fi
        rm -rf -- "$(dirname -- "$source_candidate")"
    fi

    local jobs
    jobs="$(ssv_opencv_jobs)" || return 1
    local build_candidate install_candidate
    build_candidate="$(ssv_deps_make_candidate_dir "$build_dir" "opencv-build-$expected_version")" || return 1
    install_candidate="$(ssv_deps_make_candidate_dir "$install_dir" "opencv-install-$expected_version")" || {
        rm -rf -- "$build_candidate"
        return 1
    }
    if ! ssv_opencv_configure_and_build \
        "$source_dir" "$build_candidate" "$install_candidate" "$install_dir" "$jobs"; then
        rm -rf -- "$build_candidate" "$install_candidate"
        return 1
    fi
    if ! ssv_opencv_managed_validate "$install_candidate" "$expected_version" >/dev/null; then
        rm -rf -- "$build_candidate" "$install_candidate"
        return 1
    fi
    if ! ssv_deps_atomic_replace_dir "$build_candidate" "$build_dir" || \
        ! ssv_deps_atomic_replace_dir "$install_candidate" "$install_dir"; then
        rm -rf -- "$build_candidate" "$install_candidate"
        return 1
    fi
    ssv_opencv_managed_validate "$install_dir" "$expected_version"
}
