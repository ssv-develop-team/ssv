#!/usr/bin/env bash

ssv_tensorrt_find_unique_file() {
    local root="$1" name="$2"
    local matches=()
    while IFS= read -r -d '' item; do matches+=("$item"); done < <(find "$root" -type f -name "$name" -print0 2>/dev/null)
    if [ "${#matches[@]}" -gt 1 ]; then
        ssv_deps_die "multiple TensorRT SDK candidates found for $name under $root"
        return 1
    fi
    [ "${#matches[@]}" -eq 1 ] || return 1
    printf '%s\n' "${matches[0]}"
}

ssv_tensorrt_sdk_roots() {
    local root="$1"
    local candidates=()
    if [ -f "$root/include/NvInfer.h" ] || [ -f "$root/NvInfer.h" ]; then candidates+=("$root"); fi
    local child
    for child in "$root"/TensorRT-*; do
        [ -d "$child" ] || continue
        if [ -f "$child/include/NvInfer.h" ] || [ -f "$child/NvInfer.h" ]; then candidates+=("$child"); fi
    done
    if [ "${#candidates[@]}" -gt 1 ]; then
        ssv_deps_die "multiple TensorRT SDK roots found under $root"
        return 1
    fi
    [ "${#candidates[@]}" -eq 1 ] || return 1
    printf '%s\n' "${candidates[0]}"
}

ssv_tensorrt_find_library() {
    local root="$1" pattern="$2"
    local matches=()
    while IFS= read -r -d '' item; do
        if [[ "$item" =~ /${pattern}$ ]]; then matches+=("$item"); fi
    done < <(find "$root" \( -type f -o -type l \) -print0 2>/dev/null)
    if [ "${#matches[@]}" -gt 1 ]; then
        # A versioned soname plus its unversioned linker name is expected; use
        # the unversioned file when available.
        local item
        for item in "${matches[@]}"; do [[ "$item" == *.so ]] && { printf '%s\n' "$item"; return 0; }; done
        ssv_deps_die "multiple TensorRT library candidates found for $pattern under $root"
        return 1
    fi
    [ "${#matches[@]}" -eq 1 ] || return 1
    printf '%s\n' "${matches[0]}"
}

ssv_tensorrt_resolve_version_macro() {
    local header="$1" macro="$2" value target
    value="$(awk -v macro="$macro" '$1 == "#define" && $2 == macro {print $3; exit}' "$header")"
    for _ in 1 2 3 4 5; do
        [[ "$value" =~ ^[0-9]+$ ]] && { printf '%s\n' "$value"; return 0; }
        [ -n "$value" ] || return 1
        target="$(awk -v macro="$value" '$1 == "#define" && $2 == macro {print $3; exit}' "$header")"
        [ -n "$target" ] || return 1
        value="$target"
    done
    return 1
}

ssv_tensorrt_version() {
    local header="$1"
    local major minor patch
    # Enterprise headers may expose NV_TENSORRT_* through TRT_* aliases.
    major="$(ssv_tensorrt_resolve_version_macro "$header" NV_TENSORRT_MAJOR || true)"
    minor="$(ssv_tensorrt_resolve_version_macro "$header" NV_TENSORRT_MINOR || true)"
    patch="$(ssv_tensorrt_resolve_version_macro "$header" NV_TENSORRT_PATCH || true)"
    if [ -n "$major" ] && [ -n "$minor" ] && [ -n "$patch" ]; then
        printf '%s.%s.%s\n' "$major" "$minor" "$patch"
        return 0
    fi
    local encoded
    encoded="$(awk '/NV_TENSORRT_VERSION_INT/ {print $3; exit}' "$header")"
    if [[ "$encoded" =~ ^[0-9]+$ ]]; then
        printf '%s\n' "$((encoded / 10000)).$(((encoded / 100) % 100)).$((encoded % 100))"
        return 0
    fi
    ssv_deps_die "unable to read TensorRT version from $header"
    return 1
}

ssv_tensorrt_make_pc() {
    local root="$1" version="$2" include_dir="$3" lib_dir="$4" cuda_include="$5" cuda_lib="$6"
    local pc_dir="$root/lib/pkgconfig"
    mkdir -p -- "$pc_dir"
    ssv_deps_write_pc "$pc_dir/nvinfer.pc" \
        "prefix=$root" \
        'exec_prefix=${prefix}' \
        "libdir=$lib_dir" \
        "includedir=$include_dir" \
        "cudaincludedir=$cuda_include" \
        "cudalibdir=$cuda_lib" \
        "" \
        "Name: nvinfer" \
        "Description: NVIDIA TensorRT and CUDA Runtime" \
        "Version: $version" \
        'Libs: -L${libdir} -lnvinfer -L${cudalibdir} -lcudart' \
        'Cflags: -I${includedir} -I${cudaincludedir}'
}

ssv_tensorrt_locate_cuda() {
    local root="$1"
    local cuda_home="${CUDA_HOME:-}"
    local candidate
    if [ -z "$cuda_home" ]; then
        for candidate in "$root" /usr/local/cuda /usr/local/cuda-*; do
            [ -d "$candidate" ] || continue
            if find -H "$candidate" -type f -name cuda_runtime_api.h -print -quit | grep -q .; then
                cuda_home="$candidate"
                break
            fi
        done
    fi
    local cuda_include="" cuda_lib=""
    if [ -n "$cuda_home" ]; then
        local cuda_header cuda_library
        cuda_header="$(find -H "$cuda_home" -type f -name cuda_runtime_api.h -print -quit 2>/dev/null)"
        cuda_library="$(find -H "$cuda_home" \( -type f -o -type l \) -name libcudart.so -print -quit 2>/dev/null)"
        [ -z "$cuda_header" ] || cuda_include="$(dirname -- "$cuda_header")"
        [ -z "$cuda_library" ] || cuda_lib="$(dirname -- "$cuda_library")"
    fi
    [ -n "$cuda_include" ] && [ -n "$cuda_lib" ] || return 1
    printf '%s\n%s\n' "$cuda_include" "$cuda_lib"
}

ssv_tensorrt_validate_layout() {
    local root="$1"
    local sdk_root
    sdk_root="$(ssv_tensorrt_sdk_roots "$root")" || { ssv_deps_die "TensorRT SDK root not found under $root"; return 1; }
    local header version_header lib_file
    header="$(ssv_tensorrt_find_unique_file "$sdk_root" NvInfer.h)" || { ssv_deps_die "TensorRT NvInfer.h not found under $sdk_root"; return 1; }
    version_header="$(ssv_tensorrt_find_unique_file "$sdk_root" NvInferVersion.h)" || { ssv_deps_die "TensorRT NvInferVersion.h not found under $sdk_root"; return 1; }
    lib_file="$(ssv_tensorrt_find_library "$sdk_root" 'libnvinfer\.so(\..*)?')" || { ssv_deps_die "TensorRT libnvinfer.so not found under $sdk_root"; return 1; }
    local cuda_info cuda_include cuda_lib
    cuda_info="$(ssv_tensorrt_locate_cuda "$sdk_root")" || { ssv_deps_die "CUDA Runtime (cuda_runtime_api.h and libcudart.so) not found for TensorRT"; return 1; }
    cuda_include="$(printf '%s\n' "$cuda_info" | sed -n '1p')"
    cuda_lib="$(printf '%s\n' "$cuda_info" | sed -n '2p')"
    local include_dir lib_dir
    include_dir="$(dirname -- "$header")"
    lib_dir="$(dirname -- "$lib_file")"
    local version
    version="$(ssv_tensorrt_version "$version_header")" || return 1
    ssv_tensorrt_make_pc "$root" "$version" "$include_dir" "$lib_dir" "$cuda_include" "$cuda_lib"
    local old_pkg_config_path="${PKG_CONFIG_PATH:-}"
    export PKG_CONFIG_PATH="$root/lib/pkgconfig${old_pkg_config_path:+:$old_pkg_config_path}"
    local pc_dir
    pc_dir="$(ssv_deps_pkgconfig_dir nvinfer)" || return 1
    [ "$pc_dir" = "$(cd -- "$root/lib/pkgconfig" && pwd -P)" ] || { ssv_deps_die "TensorRT pkg-config source mismatch: $pc_dir"; return 1; }
    ssv_deps_pkgconfig_version_at_least nvinfer "$version" || { ssv_deps_die "TensorRT pkg-config version check failed"; return 1; }
    local runtime_dirs
    runtime_dirs="$(ssv_deps_runtime_dirs "$lib_dir" "$cuda_lib")"
    ssv_deps_compile_probe \
        '#include <NvInfer.h>
#include <NvInferVersion.h>
#include <cuda_runtime_api.h>
int main() {
    int cuda_version = 0;
    if (cudaRuntimeGetVersion(&cuda_version) != cudaSuccess)
        return 1;
    return getInferLibVersion() > 0 && cuda_version > 0 ? 0 : 1;
}' \
        nvinfer "$runtime_dirs" || { ssv_deps_die "TensorRT/CUDA compile/load probe failed"; return 1; }
    printf 'version=%s\n' "$version"
    printf 'pkgconfig_dir=%s\n' "$pc_dir"
    printf 'runtime_dirs=%s\n' "$runtime_dirs"
}

ssv_tensorrt_managed_validate() {
    ssv_tensorrt_validate_layout "$1"
}

ssv_tensorrt_managed_prepare() {
    local root="$1"
    local archive="${SSV_TENSORRT_ARCHIVE:-}"
    local url="${SSV_TENSORRT_URL:-}"
    if [ -n "$archive" ] && [ -n "$url" ]; then
        ssv_deps_die "SSV_TENSORRT_ARCHIVE and SSV_TENSORRT_URL cannot both be set"
        return 1
    fi
    if [ -z "$archive" ] && [ -z "$url" ] && [ -d "$root" ]; then
        ssv_tensorrt_managed_validate "$root"
        return $?
    fi
    ssv_deps_require_replaceable_root "$root" ssv_tensorrt_managed_validate || return 1
    local source_archive="$archive"
    if [ -n "$url" ]; then
        local identity
        identity="$(printf '%s' "$url" | sha256sum | awk '{print $1}')"
        local filename="${url##*/}"
        source_archive="$SSV_ROOT/.deps/downloads/tensorrt/${identity}-${filename%%\?*}"
        ssv_deps_cached_download "$url" "$source_archive" >/dev/null || return 1
    fi
    [ -n "$source_archive" ] && [ -f "$source_archive" ] || { ssv_deps_die "TensorRT archive does not exist: $source_archive"; return 1; }
    local candidate
    candidate="$(ssv_deps_make_candidate_dir "$root" tensorrt)"
    ssv_deps_extract_archive "$source_archive" "$candidate" || { rm -rf -- "$candidate"; return 1; }
    local result_file error_file
    result_file="$(mktemp "${TMPDIR:-/tmp}/ssv-tensorrt-result.XXXXXX")"
    error_file="$(mktemp "${TMPDIR:-/tmp}/ssv-tensorrt-error.XXXXXX")"
    ssv_tensorrt_validate_layout "$candidate" >"$result_file" 2>"$error_file" || {
        cat "$error_file" >&2 || true
        rm -f -- "$result_file" "$error_file"
        rm -rf -- "$candidate"
        return 1
    }
    rm -f -- "$result_file" "$error_file"
    ssv_deps_atomic_replace_dir "$candidate" "$root" || return 1
    ssv_tensorrt_managed_validate "$root"
}
