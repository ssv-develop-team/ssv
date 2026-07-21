#!/usr/bin/env bash

ssv_onnxruntime_normalize_version() {
    local version="$1"
    if [[ "$version" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(-gpu)?$ ]]; then
        printf '%s\n' "$version"
    else
        ssv_deps_die "SSV_ONNXRUNTIME_VERSION must be x.y.z or x.y.z-gpu: $version"
        return 1
    fi
}

ssv_onnxruntime_archive_info() {
    local version="$1"
    local machine
    machine="$(uname -m)"
    local arch
    case "$machine" in
        x86_64|amd64) arch="x64" ;;
        aarch64|arm64) arch="aarch64" ;;
        *) ssv_deps_die "managed ONNX Runtime supports Linux x86_64 and aarch64, got $machine"; return 1 ;;
    esac
    local base_version="${version%-gpu}"
    local variant=""
    [[ "$version" == *-gpu ]] && variant="-gpu"
    local archive="onnxruntime-linux-${arch}${variant}-${base_version}.tgz"
    printf '%s\n' "$archive" "https://github.com/microsoft/onnxruntime/releases/download/v${base_version}/${archive}"
}

ssv_onnxruntime_find_libdir() {
    local root="$1"
    local found
    found="$(find "$root" -type f \( -name 'libonnxruntime.so' -o -name 'libonnxruntime.so.*' \) -print -quit 2>/dev/null)"
    [ -n "$found" ] || return 1
    dirname -- "$found"
}

ssv_onnxruntime_make_pc() {
    local root="$1"
    local version="$2"
    local include_dir="$3"
    local lib_dir="$4"
    local pc_dir="$root/lib/pkgconfig"
    mkdir -p -- "$pc_dir"
    ssv_deps_write_pc "$pc_dir/onnxruntime.pc" \
        "prefix=$root" \
        'exec_prefix=${prefix}' \
        "libdir=$lib_dir" \
        "includedir=$include_dir" \
        "" \
        "Name: onnxruntime" \
        "Description: ONNX Runtime C/C++ inference runtime" \
        "Version: ${version%-gpu}" \
        'Libs: -L${libdir} -lonnxruntime' \
        'Cflags: -I${includedir}'
}

ssv_onnxruntime_validate_layout() {
    local root="$1"
    local expected_version="$2"
    local version_file="$root/VERSION_NUMBER"
    local include_dir="$root/include"
    local lib_dir
    lib_dir="$(ssv_onnxruntime_find_libdir "$root")" || {
        ssv_deps_die "ONNX Runtime library not found under $root"
        return 1
    }
    [ -f "$include_dir/onnxruntime_cxx_api.h" ] || {
        ssv_deps_die "ONNX Runtime header not found under $root"
        return 1
    }
    [ -f "$version_file" ] || {
        ssv_deps_die "ONNX Runtime VERSION_NUMBER not found under $root"
        return 1
    }
    local actual_version
    actual_version="$(tr -d '[:space:]' < "$version_file")"
    local expected_base="${expected_version%-gpu}"
    [ "$actual_version" = "$expected_base" ] || {
        ssv_deps_die "ONNX Runtime version mismatch: expected $expected_base, got $actual_version"
        return 1
    }
    local cuda_provider="$lib_dir/libonnxruntime_providers_cuda.so"
    if [[ "$expected_version" == *-gpu ]]; then
        [ -f "$cuda_provider" ] || {
            ssv_deps_die "ONNX Runtime GPU package is missing libonnxruntime_providers_cuda.so"
            return 1
        }
    elif [ -f "$cuda_provider" ]; then
        ssv_deps_die "ONNX Runtime CPU package unexpectedly contains the CUDA provider"
        return 1
    fi
    [ -f "$lib_dir/libonnxruntime.so" ] || {
        local versioned
        versioned="$(find "$lib_dir" -maxdepth 1 -type f -name 'libonnxruntime.so.*' -print -quit)"
        [ -n "$versioned" ] || { ssv_deps_die "ONNX Runtime link library not found under $lib_dir"; return 1; }
        ln -s "$(basename -- "$versioned")" "$lib_dir/libonnxruntime.so"
    }
    ssv_onnxruntime_make_pc "$root" "$expected_version" "$include_dir" "$lib_dir"

    local old_pkg_config_path="${PKG_CONFIG_PATH:-}"
    export PKG_CONFIG_PATH="$root/lib/pkgconfig${old_pkg_config_path:+:$old_pkg_config_path}"
    local pc_dir
    pc_dir="$(ssv_deps_pkgconfig_dir onnxruntime)" || return 1
    [ "$pc_dir" = "$(cd -- "$root/lib/pkgconfig" && pwd -P)" ] || {
        ssv_deps_die "ONNX Runtime pkg-config source mismatch: $pc_dir"
        return 1
    }
    ssv_deps_pkgconfig_version_at_least onnxruntime "$expected_base" || {
        ssv_deps_die "ONNX Runtime pkg-config version check failed"
        return 1
    }
    local runtime_dirs
    runtime_dirs="$(ssv_deps_runtime_dirs "$lib_dir")"
    if [[ "$expected_version" == *-gpu ]]; then
        local library
        for library in "$lib_dir"/*.so*; do
            [ -e "$library" ] || continue
            if LD_LIBRARY_PATH="$lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$library" 2>/dev/null | grep -q 'not found'; then
                ssv_deps_die "ONNX Runtime GPU library has unresolved CUDA/cuDNN dependencies: $(basename -- "$library")"
                return 1
            fi
        done
    fi
    ssv_deps_compile_probe \
        "#include <cstring>
#include <onnxruntime_cxx_api.h>
int main() { return std::strcmp(OrtGetApiBase()->GetVersionString(), \"$expected_base\"); }" \
        onnxruntime "$runtime_dirs" || {
        ssv_deps_die "ONNX Runtime compile/load probe failed"
        return 1
    }
    printf 'version=%s\n' "$actual_version"
    printf 'pkgconfig_dir=%s\n' "$pc_dir"
    printf 'runtime_dirs=%s\n' "$runtime_dirs"
}

ssv_onnxruntime_managed_validate() {
    local root="$1"
    local expected_version="$2"
    ssv_onnxruntime_validate_layout "$root" "$expected_version"
}

ssv_onnxruntime_managed_prepare() {
    local root="$1"
    local expected_version="$2"
    expected_version="$(ssv_onnxruntime_normalize_version "$expected_version")" || return 1

    local result_file error_file
    result_file="$(mktemp "${TMPDIR:-/tmp}/ssv-onnx-result.XXXXXX")"
    error_file="$(mktemp "${TMPDIR:-/tmp}/ssv-onnx-error.XXXXXX")"
    if [ -d "$root" ] && ssv_onnxruntime_managed_validate "$root" "$expected_version" >"$result_file" 2>"$error_file"; then
        cat "$result_file"
        rm -f -- "$result_file" "$error_file"
        return 0
    fi
    rm -f -- "$result_file" "$error_file"
    ssv_deps_require_replaceable_root "$root" ssv_onnxruntime_managed_validate "$expected_version" || return 1

    local archive_url archive
    mapfile -t archive_url < <(ssv_onnxruntime_archive_info "$expected_version") || return 1
    archive="${archive_url[0]}"
    local url="${archive_url[1]}"
    local cache_dir="$SSV_ROOT/.deps/downloads/onnxruntime/$expected_version"
    local cache_file="$cache_dir/$archive"
    ssv_deps_cached_download "$url" "$cache_file" >/dev/null || return 1

    local candidate
    candidate="$(ssv_deps_make_candidate_dir "$root" "onnxruntime-${expected_version}")"
    if ! ssv_deps_extract_archive "$cache_file" "$candidate"; then
        rm -rf -- "$candidate"
        rm -f -- "$cache_file"
        ssv_deps_cached_download "$url" "$cache_file" >/dev/null || return 1
        candidate="$(ssv_deps_make_candidate_dir "$root" "onnxruntime-${expected_version}")"
        ssv_deps_extract_archive "$cache_file" "$candidate" || { rm -rf -- "$candidate"; return 1; }
    fi

    local extracted
    extracted="$(find "$candidate" -mindepth 1 -maxdepth 4 -type f -name onnxruntime_cxx_api.h -print -quit)"
    [ -n "$extracted" ] || { rm -rf -- "$candidate"; ssv_deps_die "ONNX Runtime archive has no expected header"; return 1; }
    local extracted_root
    extracted_root="$(dirname -- "$(dirname -- "$extracted")")"
    # Archives normally contain one top-level directory. Move its contents to
    # the stable root so every provider exposes the same layout.
    if [ "$extracted_root" != "$candidate" ]; then
        local normalized
        normalized="$(ssv_deps_make_candidate_dir "$root" onnxruntime-normalize)"
        cp -a "$extracted_root"/. "$normalized"/
        rm -rf -- "$candidate"
        candidate="$normalized"
    fi
    result_file="$(mktemp "${TMPDIR:-/tmp}/ssv-onnx-result.XXXXXX")"
    error_file="$(mktemp "${TMPDIR:-/tmp}/ssv-onnx-error.XXXXXX")"
    if ! ssv_onnxruntime_validate_layout "$candidate" "$expected_version" >"$result_file" 2>"$error_file"; then
        cat "$error_file" >&2 || true
        rm -f -- "$result_file" "$error_file"
        rm -rf -- "$candidate"
        rm -f -- "$cache_file"
        ssv_deps_cached_download "$url" "$cache_file" >/dev/null || return 1
        candidate="$(ssv_deps_make_candidate_dir "$root" "onnxruntime-${expected_version}")"
        ssv_deps_extract_archive "$cache_file" "$candidate" || { rm -rf -- "$candidate"; return 1; }
        extracted="$(find "$candidate" -mindepth 1 -maxdepth 4 -type f -name onnxruntime_cxx_api.h -print -quit)"
        [ -n "$extracted" ] || { rm -rf -- "$candidate"; return 1; }
        extracted_root="$(dirname -- "$(dirname -- "$extracted")")"
        if [ "$extracted_root" != "$candidate" ]; then
            normalized="$(ssv_deps_make_candidate_dir "$root" onnxruntime-normalize)"
            cp -a "$extracted_root"/. "$normalized"/
            rm -rf -- "$candidate"
            candidate="$normalized"
        fi
        result_file="$(mktemp "${TMPDIR:-/tmp}/ssv-onnx-result.XXXXXX")"
        error_file="$(mktemp "${TMPDIR:-/tmp}/ssv-onnx-error.XXXXXX")"
        ssv_onnxruntime_validate_layout "$candidate" "$expected_version" >"$result_file" 2>"$error_file" || {
            cat "$error_file" >&2 || true
            rm -f -- "$result_file" "$error_file"
            rm -rf -- "$candidate"
            return 1
        }
    fi
    rm -f -- "$result_file" "$error_file"
    ssv_deps_atomic_replace_dir "$candidate" "$root" || return 1
    # Re-run validation after replacement so pcfiledir contains the stable
    # path rather than the temporary candidate path.
    ssv_onnxruntime_validate_layout "$root" "$expected_version"
}
