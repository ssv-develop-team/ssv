#!/usr/bin/env bash

# Package revision and URL details are provider internals. They deliberately
# do not form part of the public OpenCV version contract.
SSV_OPENCV_PACKAGE_REVISION="4.10.0+dfsg-5ubuntu1"
SSV_OPENCV_POOL_BASE="https://archive.ubuntu.com/ubuntu/pool/universe/o/opencv"
SSV_OPENCV_PROTOBUF_REVISION="3.21.12-11ubuntu3.1"
SSV_OPENCV_PROTOBUF_POOL_BASE="https://archive.ubuntu.com/ubuntu/pool/main/p/protobuf"
SSV_OPENCV_REQUIRED_MODULES=(core imgproc video calib3d features2d flann dnn)

ssv_opencv_platform() {
    case "$(uname -m)" in
        x86_64|amd64) printf 'amd64 x86_64-linux-gnu\n' ;;
        aarch64|arm64) printf 'arm64 aarch64-linux-gnu\n' ;;
        *) ssv_deps_die "managed OpenCV supports Linux x86_64 and aarch64, got $(uname -m)"; return 1 ;;
    esac
}

ssv_opencv_deb_member() {
    local archive="$1"
    local prefix="$2"
    ar t "$archive" | awk -v prefix="$prefix" '$0 ~ "^" prefix "\\." { print; exit }'
}

ssv_opencv_extract_tar_stream() {
    local archive="$1"
    local member="$2"
    local destination="$3"
    case "$member" in
        *.zst)
            if tar --help 2>/dev/null | grep -q -- '--zstd'; then
                ar p "$archive" "$member" | tar --zstd -xf - -C "$destination"
            elif ssv_deps_have_command zstd; then
                ar p "$archive" "$member" | zstd -dc | tar -xf - -C "$destination"
            else
                ssv_deps_die "tar cannot read zstd Debian members and zstd is unavailable"
                return 1
            fi
            ;;
        *.xz) ar p "$archive" "$member" | tar -xJf - -C "$destination" ;;
        *.gz) ar p "$archive" "$member" | tar -xzf - -C "$destination" ;;
        *.bz2) ar p "$archive" "$member" | tar -xjf - -C "$destination" ;;
        *.tar) ar p "$archive" "$member" | tar -xf - -C "$destination" ;;
        *) ssv_deps_die "unsupported Debian tar member: $member"; return 1 ;;
    esac
}

ssv_opencv_control_value() {
    local control_file="$1"
    local field="$2"
    awk -F': ' -v field="$field" '$1 == field { print substr($0, length(field) + 3); exit }' "$control_file"
}

ssv_opencv_validate_deb() {
    local archive="$1"
    local expected_package="$2"
    local expected_arch="$3"
    local expected_version_prefix="${4:-4.10.0}"
    local control_member
    control_member="$(ssv_opencv_deb_member "$archive" control.tar)"
    [ -n "$control_member" ] || { ssv_deps_die "OpenCV Debian archive has no control member: $archive"; return 1; }
    local control_dir
    control_dir="$(mktemp -d "${TMPDIR:-/tmp}/ssv-opencv-control.XXXXXX")"
    ssv_opencv_extract_tar_stream "$archive" "$control_member" "$control_dir" || { rm -rf -- "$control_dir"; return 1; }
    local control_file="$control_dir/control"
    [ -f "$control_file" ] || { rm -rf -- "$control_dir"; ssv_deps_die "OpenCV package control metadata is missing: $archive"; return 1; }
    local package version arch
    package="$(ssv_opencv_control_value "$control_file" Package)"
    version="$(ssv_opencv_control_value "$control_file" Version)"
    arch="$(ssv_opencv_control_value "$control_file" Architecture)"
    rm -rf -- "$control_dir"
    [ "$package" = "$expected_package" ] || { ssv_deps_die "OpenCV package name mismatch: expected $expected_package, got $package"; return 1; }
    [[ "$version" == "$expected_version_prefix"* ]] || { ssv_deps_die "package version mismatch: expected $expected_version_prefix, got $version"; return 1; }
    [ "$arch" = "$expected_arch" ] || { ssv_deps_die "OpenCV package architecture mismatch: expected $expected_arch, got $arch"; return 1; }
}

ssv_opencv_extract_deb() {
    local archive="$1"
    local destination="$2"
    local data_member
    data_member="$(ssv_opencv_deb_member "$archive" data.tar)"
    [ -n "$data_member" ] || { ssv_deps_die "OpenCV Debian archive has no data member: $archive"; return 1; }
    ssv_opencv_extract_tar_stream "$archive" "$data_member" "$destination"
}

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
    local lib_dir="$1"
    local module="$2"
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
            if ! find "$lib_dir" -maxdepth 1 \( -name "libopencv_${base}.so" -o -name "libopencv_${base}.so.*" \) -print -quit | grep -q .; then
                ssv_deps_die "OpenCV runtime closure is incomplete: $(basename -- "$library") needs $needed"
                return 1
            fi
        done < <(printf '%s\n' "$readelf_output" | sed -n 's/.*Shared library: \[\(libopencv_[^]]*\)\].*/\1/p')
        ldd_output="$(LD_LIBRARY_PATH="$lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd -r "$library" 2>&1)" || {
            ssv_deps_die "OpenCV runtime load check failed for $(basename -- "$library"): $ldd_output"
            return 1
        }
        missing="$(printf '%s\n' "$ldd_output" | sed -n 's/^[[:space:]]*\([^[:space:]]*\) => not found$/\1/p' | paste -sd, -)"
        symbol_errors="$(printf '%s\n' "$ldd_output" | sed -n 's/.*undefined symbol: \([^[:space:]]*\).*/\1/p' | paste -sd, -)"
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
    include_dir="$(ssv_opencv_find_include_dir "$root")" || { ssv_deps_die "OpenCV headers not found under $root"; return 1; }
    lib_dir="$(ssv_opencv_find_lib_dir "$root")" || { ssv_deps_die "OpenCV libraries not found under $root"; return 1; }
    ssv_opencv_validate_libraries "$lib_dir" || return 1
    ssv_opencv_make_pc "$root" "$expected_version" "$include_dir" "$lib_dir"
    local old_pkg_config_path="${PKG_CONFIG_PATH:-}"
    export PKG_CONFIG_PATH="$root/lib/pkgconfig${old_pkg_config_path:+:$old_pkg_config_path}"
    local pc_dir
    pc_dir="$(ssv_deps_pkgconfig_dir opencv4)" || return 1
    [ "$pc_dir" = "$(cd -- "$root/lib/pkgconfig" && pwd -P)" ] || { ssv_deps_die "OpenCV pkg-config source mismatch: $pc_dir"; return 1; }
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

ssv_opencv_managed_prepare() {
    local workspace_root="$1" expected_version="$2"
    local root="$workspace_root/managed"
    local result_file error_file
    result_file="$(mktemp "${TMPDIR:-/tmp}/ssv-opencv-result.XXXXXX")"
    error_file="$(mktemp "${TMPDIR:-/tmp}/ssv-opencv-error.XXXXXX")"
    if [ -d "$root" ] && ssv_opencv_managed_validate "$root" "$expected_version" >"$result_file" 2>"$error_file"; then
        cat "$result_file"; rm -f -- "$result_file" "$error_file"
        return 0
    fi
    rm -f -- "$result_file" "$error_file"
    ssv_deps_require_replaceable_root "$root" ssv_opencv_managed_validate "$expected_version" || return 1
    local deb_arch
    read -r deb_arch _ < <(ssv_opencv_platform) || return 1
    [ -n "$deb_arch" ] || return 1
    local cache_dir="$SSV_ROOT/.deps/downloads/opencv/$expected_version"
    mkdir -p -- "$cache_dir"
    local module kind filename package cache_file
    local packages=()
    for module in core imgproc video calib3d features2d flann; do
        for kind in dev runtime; do
            if [ "$kind" = dev ]; then
                package="libopencv-${module}-dev"
                filename="${package}_${SSV_OPENCV_PACKAGE_REVISION}_${deb_arch}.deb"
            else
                package="libopencv-${module}410"
                filename="${package}_${SSV_OPENCV_PACKAGE_REVISION}_${deb_arch}.deb"
            fi
            packages+=("$package|$filename")
        done
    done
    package="libopencv-dnn410"
    filename="${package}_${SSV_OPENCV_PACKAGE_REVISION}_${deb_arch}.deb"
    packages+=("$package|$filename")
    # The dnn runtime has a direct SONAME dependency on protobuf. Carry the
    # matching runtime privately so managed OpenCV is loadable across hosts
    # with a different protobuf SONAME; it is not exposed in opencv4.pc.
    package="libprotobuf32t64"
    filename="${package}_${SSV_OPENCV_PROTOBUF_REVISION}_${deb_arch}.deb"
    packages+=("$package|$filename")
    for entry in "${packages[@]}"; do
        package="${entry%%|*}"; filename="${entry#*|}"
        cache_file="$cache_dir/$filename"
        local pool_base="$SSV_OPENCV_POOL_BASE"
        local version_prefix="4.10.0"
        [ "$package" != libprotobuf32t64 ] || pool_base="$SSV_OPENCV_PROTOBUF_POOL_BASE"
        [ "$package" != libprotobuf32t64 ] || version_prefix="3.21.12"
        ssv_deps_cached_download "$pool_base/$filename" "$cache_file" >/dev/null || return 1
        ssv_deps_with_cache_retry "$cache_file" "$pool_base/$filename" \
            ssv_opencv_validate_deb "$cache_file" "$package" "$deb_arch" "$version_prefix" || return 1
    done
    local candidate
    candidate="$(ssv_deps_make_candidate_dir "$root" "opencv-${expected_version}")"
    for entry in "${packages[@]}"; do
        package="${entry%%|*}"
        filename="${entry#*|}"
        local pool_base="$SSV_OPENCV_POOL_BASE"
        [ "$package" != libprotobuf32t64 ] || pool_base="$SSV_OPENCV_PROTOBUF_POOL_BASE"
        ssv_deps_with_cache_retry "$cache_dir/$filename" "$pool_base/$filename" \
            ssv_opencv_extract_deb "$cache_dir/$filename" "$candidate" || { rm -rf -- "$candidate"; return 1; }
    done
    local include_dir lib_dir
    include_dir="$(ssv_opencv_find_include_dir "$candidate")" || { rm -rf -- "$candidate"; return 1; }
    lib_dir="$(ssv_opencv_find_lib_dir "$candidate")" || { rm -rf -- "$candidate"; return 1; }
    mkdir -p -- "$candidate/lib/pkgconfig"
    ssv_opencv_make_pc "$candidate" "$expected_version" "$include_dir" "$lib_dir"
    result_file="$(mktemp "${TMPDIR:-/tmp}/ssv-opencv-result.XXXXXX")"
    error_file="$(mktemp "${TMPDIR:-/tmp}/ssv-opencv-error.XXXXXX")"
    ssv_opencv_validate_layout "$candidate" "$expected_version" >"$result_file" 2>"$error_file" || {
        cat "$error_file" >&2 || true
        rm -f -- "$result_file" "$error_file"
        rm -rf -- "$candidate"
        return 1
    }
    rm -f -- "$result_file" "$error_file"
    ssv_deps_atomic_replace_dir "$candidate" "$root" || return 1
    ssv_opencv_managed_validate "$root" "$expected_version"
}
