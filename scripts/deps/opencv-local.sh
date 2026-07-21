#!/usr/bin/env bash

# OpenCV built outside this repository. The provider owns only a generated pc
# file under the shared OpenCV root; it never copies or mutates the user's
# headers or libraries.

ssv_opencv_local_include_dir() {
    local requested="$1"
    local candidate
    for candidate in "$requested" "$requested/opencv4"; do
        if [ -f "$candidate/opencv2/core.hpp" ]; then
            (cd -- "$candidate" && pwd -P)
            return 0
        fi
    done
    ssv_deps_die "local OpenCV headers not found under: $requested"
    return 1
}

ssv_opencv_local_prepare() {
    local requested_include="$1" lib_dir="$2" expected_version="$3"
    local include_dir
    include_dir="$(ssv_opencv_local_include_dir "$requested_include")" || return 1
    lib_dir="$(cd -- "$lib_dir" && pwd -P)" || {
        ssv_deps_die "local OpenCV library directory does not exist: $2"
        return 1
    }
    ssv_opencv_validate_libraries "$lib_dir" || return 1

    local workspace_root="${SSV_OPENCV_ROOT:-$SSV_ROOT/.deps/opencv}"
    local root="$workspace_root/local"
    local pc_dir="$root/lib/pkgconfig"
    local candidate
    candidate="$(ssv_deps_make_candidate_dir "$pc_dir" opencv-local-pkgconfig)"
    ssv_opencv_make_pc "$root" "$expected_version" "$include_dir" "$lib_dir" "$candidate"
    # The generated pkg-config directory is the only managed artifact. Publish
    # only that subtree so source, build, and install remain untouched.
    local old_pkg_config_path="${PKG_CONFIG_PATH:-}"
    export PKG_CONFIG_PATH="$candidate${old_pkg_config_path:+:$old_pkg_config_path}"
    local runtime_dirs
    runtime_dirs="$(ssv_opencv_probe_version "$expected_version" "$lib_dir")" || {
        rm -rf -- "$candidate"
        ssv_deps_die "local OpenCV compile/load probe failed (expected $expected_version)"
        return 1
    }
    ssv_deps_atomic_replace_dir "$candidate" "$pc_dir" || return 1
    pc_dir="$(cd -- "$pc_dir" && pwd -P)"
    printf 'version=%s\n' "$expected_version"
    printf 'pkgconfig_dir=%s\n' "$pc_dir"
    printf 'runtime_dirs=%s\n' "$runtime_dirs"
}
