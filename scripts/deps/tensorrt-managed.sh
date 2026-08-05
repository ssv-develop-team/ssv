#!/usr/bin/env bash

SSV_TENSORRT_MANAGED_VERSION="10.16.1"
SSV_TENSORRT_MANAGED_PACKAGE_REVISION="10.16.1.11-1+cuda13.2"
SSV_TENSORRT_MANAGED_CUDNN_REVISION="9.25.0.15-1"
SSV_TENSORRT_MANAGED_CUDA_MAJOR="13"
SSV_TENSORRT_MANAGED_REPOSITORY="https://developer.download.nvidia.com/compute/cuda/repos/debian12/x86_64"

ssv_tensorrt_managed_package_manifest() {
    case "$(uname -m)" in
        x86_64|amd64) ;;
        *)
            ssv_deps_die "managed NVIDIA runtime supports Linux x86_64 only"
            return 1
            ;;
    esac
    printf '%s|%s\n' \
        "libnvinfer-headers-dev_${SSV_TENSORRT_MANAGED_PACKAGE_REVISION}_amd64.deb" \
        "4945a01b9be143091c89fd8366224159cd01648c25befabe7e1271ff6b9774ca" \
        "libnvinfer10_${SSV_TENSORRT_MANAGED_PACKAGE_REVISION}_amd64.deb" \
        "8232ccdc8be82815411879a589ba08df039b9f01a997effdb3f60d3b8a05e6bd" \
        "libnvonnxparsers10_${SSV_TENSORRT_MANAGED_PACKAGE_REVISION}_amd64.deb" \
        "1f70eb168ec0c5a2714585e2458464476d6529ef11fc54cad51fac2396a09e6f" \
        "libcudnn9-cuda-13_${SSV_TENSORRT_MANAGED_CUDNN_REVISION}_amd64.deb" \
        "5b2ef35d332c903fb81f000c883c36f41fd57de3972aa82e007f944478db0099"
}

ssv_tensorrt_validate_deb() {
    local archive="$1" expected_sha256="$2"
    local actual_sha256
    actual_sha256="$(sha256sum "$archive" 2>/dev/null | awk '{print $1}')"
    [ "$actual_sha256" = "$expected_sha256" ] || {
        ssv_deps_die "NVIDIA package checksum mismatch: $(basename -- "$archive")"
        return 1
    }
    dpkg-deb --info "$archive" >/dev/null 2>&1 || {
        ssv_deps_die "NVIDIA package is not a readable Debian archive: $(basename -- "$archive")"
        return 1
    }
}

ssv_tensorrt_install_managed_packages() {
    local destination="$1"
    local manifest
    manifest="$(ssv_tensorrt_managed_package_manifest)" || return 1
    ssv_deps_have_command sha256sum || { ssv_deps_die "sha256sum is required for managed NVIDIA packages"; return 1; }
    ssv_deps_have_command dpkg-deb || { ssv_deps_die "dpkg-deb is required for managed NVIDIA packages"; return 1; }
    local cache_dir="$SSV_ROOT/.deps/downloads/tensorrt/${SSV_TENSORRT_MANAGED_VERSION}-cuda13.2-cudnn9"
    mkdir -p -- "$cache_dir"
    local package_file expected_sha256 cache_file url
    while IFS='|' read -r package_file expected_sha256; do
        [ -n "$package_file" ] || continue
        cache_file="$cache_dir/$package_file"
        url="$SSV_TENSORRT_MANAGED_REPOSITORY/$package_file"
        ssv_deps_cached_download "$url" "$cache_file" >/dev/null || return 1
        ssv_deps_with_cache_retry "$cache_file" "$url" \
            ssv_tensorrt_validate_deb "$cache_file" "$expected_sha256" || return 1
        dpkg-deb --extract "$cache_file" "$destination" || {
            ssv_deps_die "failed to extract NVIDIA package: $package_file"
            return 1
        }
    done <<< "$manifest"
}

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
    ssv_tensorrt_find_unique_file "$root" NvInfer.h >/dev/null || return 1
    printf '%s\n' "$root"
}

ssv_tensorrt_library_soname() {
    local library="$1"
    readelf -d "$library" 2>/dev/null \
        | sed -n 's/.*Library soname: \[\([^]]*\)\].*/\1/p' \
        | head -n 1
}

ssv_tensorrt_find_soname_library() {
    local root="$1" expected_soname="$2"
    local stem="${expected_soname%%.so*}"
    local matches=()
    local item resolved actual_soname known
    while IFS= read -r -d '' item; do
        resolved="$(readlink -f -- "$item" 2>/dev/null)" || continue
        [ -f "$resolved" ] || continue
        actual_soname="$(ssv_tensorrt_library_soname "$resolved")"
        [ "$actual_soname" = "$expected_soname" ] || continue
        known=false
        local match
        for match in "${matches[@]}"; do [ "$match" = "$resolved" ] && known=true; done
        [ "$known" = true ] || matches+=("$resolved")
    done < <(find -H "$root" \( -type f -o -type l \) -name "${stem}.so*" -print0 2>/dev/null)
    if [ "${#matches[@]}" -gt 1 ]; then
        ssv_deps_die "multiple NVIDIA libraries provide $expected_soname under $root"
        return 1
    fi
    [ "${#matches[@]}" -eq 1 ] || return 1
    printf '%s\n' "${matches[0]}"
}

ssv_tensorrt_find_library() {
    ssv_tensorrt_find_soname_library "$@"
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

ssv_tensorrt_validate_required_version() {
    local version="$1"
    case "$version" in
        10.*) ;;
        *)
            ssv_deps_die "NVIDIA runtime requires TensorRT ABI major 10, got $version"
            return 1
            ;;
    esac
}

ssv_tensorrt_cuda_version() {
    local header="$1" encoded
    encoded="$(awk '$1 == "#define" && $2 == "CUDART_VERSION" {print $3; exit}' "$header")"
    [[ "$encoded" =~ ^[0-9]+$ ]] || {
        ssv_deps_die "unable to read CUDA Runtime version from $header"
        return 1
    }
    printf '%s.%s\n' "$((encoded / 1000))" "$(((encoded % 1000) / 10))"
}

ssv_tensorrt_make_pc() {
    local root="$1" version="$2" include_dir="$3" lib_dir="$4" cuda_include="$5" cuda_lib="$6"
    local nvinfer_soname="$7"
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
        "Libs: -L\${libdir} -l:${nvinfer_soname} -L\${cudalibdir} -lcudart" \
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

ssv_tensorrt_validate_runtime_closure() {
    local runtime_dirs="$1"
    shift
    local library ldd_output
    for library in "$@"; do
        ldd_output="$(LD_LIBRARY_PATH="$runtime_dirs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd -r "$library" 2>&1)" || {
            ssv_deps_die "NVIDIA runtime load check failed for $(basename -- "$library"): $ldd_output"
            return 1
        }
        if grep -Eq 'not found|undefined symbol:' <<< "$ldd_output"; then
            ssv_deps_die "NVIDIA runtime closure is incomplete for $(basename -- "$library"): $ldd_output"
            return 1
        fi
    done
}

ssv_tensorrt_validate_layout() {
    local root="$1"
    ssv_deps_have_command readelf || { ssv_deps_die "readelf is required to validate NVIDIA libraries"; return 1; }
    ssv_deps_have_command ldd || { ssv_deps_die "ldd is required to validate NVIDIA libraries"; return 1; }
    local sdk_root
    sdk_root="$(ssv_tensorrt_sdk_roots "$root")" || { ssv_deps_die "TensorRT SDK root not found under $root"; return 1; }
    local header version_header
    header="$(ssv_tensorrt_find_unique_file "$sdk_root" NvInfer.h)" || { ssv_deps_die "TensorRT NvInfer.h not found under $sdk_root"; return 1; }
    version_header="$(ssv_tensorrt_find_unique_file "$sdk_root" NvInferVersion.h)" || { ssv_deps_die "TensorRT NvInferVersion.h not found under $sdk_root"; return 1; }
    local version
    version="$(ssv_tensorrt_version "$version_header")" || return 1
    ssv_tensorrt_validate_required_version "$version" || return 1
    local nvinfer_file nvonnxparser_file cudnn_file
    nvinfer_file="$(ssv_tensorrt_find_library "$sdk_root" libnvinfer.so.10)" \
        || { ssv_deps_die "TensorRT libnvinfer.so.10 not found under $sdk_root"; return 1; }
    nvonnxparser_file="$(ssv_tensorrt_find_soname_library "$sdk_root" libnvonnxparser.so.10)" \
        || { ssv_deps_die "TensorRT libnvonnxparser.so.10 not found under $sdk_root"; return 1; }
    cudnn_file="$(ssv_tensorrt_find_soname_library "$sdk_root" libcudnn.so.9)" \
        || { ssv_deps_die "NVIDIA runtime libcudnn.so.9 not found under $sdk_root"; return 1; }
    local cuda_info cuda_include cuda_lib
    cuda_info="$(ssv_tensorrt_locate_cuda "$sdk_root")" || { ssv_deps_die "CUDA Runtime (cuda_runtime_api.h and libcudart.so) not found for TensorRT"; return 1; }
    cuda_include="$(printf '%s\n' "$cuda_info" | sed -n '1p')"
    cuda_lib="$(printf '%s\n' "$cuda_info" | sed -n '2p')"
    local cuda_version
    cuda_version="$(ssv_tensorrt_cuda_version "$cuda_include/cuda_runtime_api.h")" || return 1
    case "$cuda_version" in
        "${SSV_TENSORRT_MANAGED_CUDA_MAJOR}."*) ;;
        *)
            ssv_deps_die "NVIDIA runtime requires CUDA ABI major ${SSV_TENSORRT_MANAGED_CUDA_MAJOR}, got $cuda_version"
            return 1
            ;;
    esac
    local cudart_file
    cudart_file="$(ssv_tensorrt_find_soname_library "$cuda_lib" "libcudart.so.${SSV_TENSORRT_MANAGED_CUDA_MAJOR}")" \
        || { ssv_deps_die "CUDA Runtime libcudart.so.${SSV_TENSORRT_MANAGED_CUDA_MAJOR} not found under $cuda_lib"; return 1; }
    local include_dir lib_dir
    include_dir="$(dirname -- "$header")"
    lib_dir="$(dirname -- "$nvinfer_file")"
    ssv_tensorrt_make_pc "$root" "$version" "$include_dir" "$lib_dir" \
        "$cuda_include" "$cuda_lib" libnvinfer.so.10
    local old_pkg_config_path="${PKG_CONFIG_PATH:-}"
    export PKG_CONFIG_PATH="$root/lib/pkgconfig${old_pkg_config_path:+:$old_pkg_config_path}"
    local pc_dir
    pc_dir="$(ssv_deps_pkgconfig_dir nvinfer)" || return 1
    [ "$pc_dir" = "$(cd -- "$root/lib/pkgconfig" && pwd -P)" ] || { ssv_deps_die "TensorRT pkg-config source mismatch: $pc_dir"; return 1; }
    ssv_deps_pkgconfig_version_at_least nvinfer "$version" || { ssv_deps_die "TensorRT pkg-config version check failed"; return 1; }
    local runtime_dirs
    runtime_dirs="$(ssv_deps_runtime_dirs \
        "$lib_dir" \
        "$(dirname -- "$nvonnxparser_file")" \
        "$(dirname -- "$cudnn_file")" \
        "$cuda_lib")"
    ssv_tensorrt_validate_runtime_closure "$runtime_dirs" \
        "$nvinfer_file" "$nvonnxparser_file" "$cudnn_file" "$cudart_file" || return 1
    ssv_deps_compile_probe \
        '#include <NvInfer.h>
#include <NvInferVersion.h>
#include <cuda_runtime_api.h>
int main() {
    int cuda_version = 0;
    if (cudaRuntimeGetVersion(&cuda_version) != cudaSuccess)
        return 1;
    return getInferLibVersion() > 0 && cuda_version >= 13000 && cuda_version < 14000 ? 0 : 1;
}' \
        nvinfer "$runtime_dirs" || { ssv_deps_die "TensorRT/CUDA compile/load probe failed"; return 1; }
    printf 'version=%s\n' "$version"
    printf 'pkgconfig_dir=%s\n' "$pc_dir"
    printf 'runtime_dirs=%s\n' "$runtime_dirs"
}

ssv_tensorrt_managed_validate() {
    local result version
    result="$(ssv_tensorrt_validate_layout "$1")" || return 1
    version="$(printf '%s\n' "$result" | sed -n 's/^version=//p')"
    [ "$version" = "$SSV_TENSORRT_MANAGED_VERSION" ] || {
        ssv_deps_die "managed TensorRT version mismatch: expected $SSV_TENSORRT_MANAGED_VERSION, got $version"
        return 1
    }
    printf '%s\n' "$result"
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
        local existing_result
        if existing_result="$(ssv_tensorrt_managed_validate "$root" 2>/dev/null)"; then
            printf '%s\n' "$existing_result"
            return 0
        fi
    fi
    ssv_deps_require_replaceable_root "$root" ssv_tensorrt_managed_validate || return 1
    local candidate
    candidate="$(ssv_deps_make_candidate_dir "$root" tensorrt)"
    if [ -z "$archive" ] && [ -z "$url" ]; then
        ssv_tensorrt_install_managed_packages "$candidate" || { rm -rf -- "$candidate"; return 1; }
    else
        local source_archive="$archive"
        if [ -n "$url" ]; then
            local identity
            identity="$(printf '%s' "$url" | sha256sum | awk '{print $1}')"
            local filename="${url##*/}"
            source_archive="$SSV_ROOT/.deps/downloads/tensorrt/${identity}-${filename%%\?*}"
            ssv_deps_cached_download "$url" "$source_archive" >/dev/null || { rm -rf -- "$candidate"; return 1; }
        fi
        if [ -z "$source_archive" ] || [ ! -f "$source_archive" ]; then
            rm -rf -- "$candidate"
            ssv_deps_die "TensorRT archive does not exist: $source_archive"
            return 1
        fi
        ssv_deps_extract_archive "$source_archive" "$candidate" \
            || { rm -rf -- "$candidate"; return 1; }
    fi
    local result_file error_file
    result_file="$(mktemp "${TMPDIR:-/tmp}/ssv-tensorrt-result.XXXXXX")"
    error_file="$(mktemp "${TMPDIR:-/tmp}/ssv-tensorrt-error.XXXXXX")"
    ssv_tensorrt_managed_validate "$candidate" >"$result_file" 2>"$error_file" || {
        cat "$error_file" >&2 || true
        rm -f -- "$result_file" "$error_file"
        rm -rf -- "$candidate"
        return 1
    }
    rm -f -- "$result_file" "$error_file"
    ssv_deps_atomic_replace_dir "$candidate" "$root" || return 1
    ssv_tensorrt_managed_validate "$root"
}
