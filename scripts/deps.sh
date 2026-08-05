#!/usr/bin/env bash

# Unified dependency preparation entry point. Sourcing this file defines the
# public functions below and has no download or filesystem side effects.

: "${SSV_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

source "$SSV_ROOT/scripts/deps/common.sh"
source "$SSV_ROOT/scripts/deps/versions.sh"
source "$SSV_ROOT/scripts/deps/onnxruntime-profile.sh"
source "$SSV_ROOT/scripts/deps/onnxruntime-managed.sh"
source "$SSV_ROOT/scripts/deps/opencv-managed.sh"
source "$SSV_ROOT/scripts/deps/opencv-local.sh"
source "$SSV_ROOT/scripts/deps/tensorrt-managed.sh"

SSV_DEPS_SNAPSHOT_KEYS=(
    SSV_DEPS_SIGNATURE
    SSV_DEPS_PROFILE
    SSV_DEPS_PKG_CONFIG_PATH
    SSV_DEPS_RUNTIME_PATH
    SSV_DEPS_OPENCV_MODE
    SSV_DEPS_TENSORRT_MODE
    SSV_DEPS_ONNXRUNTIME_VERSION
    SSV_DEPS_ONNXRUNTIME_PCDIR
    SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS
    SSV_DEPS_ONNXRUNTIME_PROVIDERS
    SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES
    SSV_DEPS_OPENCV_PCDIR
    SSV_DEPS_OPENCV_RUNTIME_DIRS
    SSV_DEPS_TENSORRT_PCDIR
    SSV_DEPS_TENSORRT_RUNTIME_DIRS
)

SSV_DEPS_EXPLICIT_KEYS=(
    SSV_ONNXRUNTIME_SOURCE
    SSV_ONNXRUNTIME_VERSION
    SSV_ONNXRUNTIME_ROOT
    SSV_OPENCV_SOURCE
    SSV_OPENCV_MODE
    SSV_OPENCV_ROOT
    SSV_OPENCV_INCLUDE_DIR
    SSV_OPENCV_LIB_DIR
    SSV_TENSORRT_SOURCE
    SSV_TENSORRT_MODE
    SSV_TENSORRT_ROOT
    SSV_TENSORRT_ARCHIVE
    SSV_TENSORRT_URL
    CUDA_HOME
)

ssv_deps_var_is_explicit() {
    local name="$1"
    [ -n "${!name+x}" ]
}

ssv_deps_load_dotenv() {
    local env_file="$SSV_ROOT/.env"
    [ -f "$env_file" ] || return 0
    local line key value
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|'#'*) continue ;; esac
        key="${line%%=*}"
        value="${line#*=}"
        key="${key//[[:space:]]/}"
        case "$key" in
            SSV_ONNXRUNTIME_VERSION)
                ssv_deps_die "SSV_ONNXRUNTIME_VERSION is no longer configurable; select the runtime artifact with --profile"
                return 1
                ;;
            SSV_ONNXRUNTIME_SOURCE|SSV_ONNXRUNTIME_ROOT|\
            SSV_OPENCV_SOURCE|SSV_OPENCV_MODE|SSV_OPENCV_ROOT|SSV_OPENCV_INCLUDE_DIR|SSV_OPENCV_LIB_DIR|\
            SSV_TENSORRT_SOURCE|SSV_TENSORRT_MODE|SSV_TENSORRT_ROOT|\
            SSV_TENSORRT_ARCHIVE|SSV_TENSORRT_URL|CUDA_HOME|\
            SSV_EXTRA_PKG_CONFIG_PATH|SSV_BUILD_DIR)
                local marker="SSV_DEPS_EXPLICIT_${key}"
                if [ -z "${!key+x}" ]; then
                    export "$key=$value"
                    printf -v "$marker" '%s' true
                fi
                ;;
        esac
    done < "$env_file"
}

ssv_deps_capture_explicit_config() {
    local key marker
    for key in "${SSV_DEPS_EXPLICIT_KEYS[@]}"; do
        marker="SSV_DEPS_EXPLICIT_${key}"
        if [ -n "${!key+x}" ]; then printf -v "$marker" '%s' true; fi
    done
}

ssv_deps_resolve_config() {
    local onnx_version_explicit=false onnx_root_explicit=false
    local opencv_source_explicit=false opencv_root_explicit=false
    local opencv_include_explicit=false opencv_lib_explicit=false
    local tensorrt_source_explicit=false tensorrt_root_explicit=false
    [ "${SSV_DEPS_EXPLICIT_SSV_ONNXRUNTIME_VERSION:-false}" = true ] && onnx_version_explicit=true
    [ "${SSV_DEPS_EXPLICIT_SSV_ONNXRUNTIME_ROOT:-false}" = true ] && onnx_root_explicit=true
    [ "${SSV_DEPS_EXPLICIT_SSV_OPENCV_SOURCE:-false}" = true ] && opencv_source_explicit=true
    [ "${SSV_DEPS_EXPLICIT_SSV_OPENCV_ROOT:-false}" = true ] && opencv_root_explicit=true
    [ "${SSV_DEPS_EXPLICIT_SSV_OPENCV_INCLUDE_DIR:-false}" = true ] && opencv_include_explicit=true
    [ "${SSV_DEPS_EXPLICIT_SSV_OPENCV_LIB_DIR:-false}" = true ] && opencv_lib_explicit=true
    [ "${SSV_DEPS_EXPLICIT_SSV_TENSORRT_SOURCE:-false}" = true ] && tensorrt_source_explicit=true
    [ "${SSV_DEPS_EXPLICIT_SSV_TENSORRT_ROOT:-false}" = true ] && tensorrt_root_explicit=true

    if [ "$onnx_version_explicit" = true ]; then
        ssv_deps_die "SSV_ONNXRUNTIME_VERSION is no longer configurable; select the runtime artifact with --profile"
        return 1
    fi

    SSV_DEPS_PROFILE="${SSV_DEPS_PROFILE:-cpu}"
    ssv_onnxruntime_validate_profile "$SSV_DEPS_PROFILE" || return 1
    [ "$SSV_DEPS_PROFILE" != auto ] || {
        ssv_deps_die "runtime profile must be resolved before dependency configuration"
        return 1
    }
    local default_onnx_source=managed
    local default_onnx_version="$SSV_DEPS_DEFAULT_ONNXRUNTIME_VERSION"
    local default_tensorrt_mode=auto
    case "$SSV_DEPS_PROFILE" in
        nvidia)
            default_onnx_version="${SSV_DEPS_DEFAULT_ONNXRUNTIME_VERSION}-gpu"
            default_tensorrt_mode=enabled
            ;;
        intel|amd) default_onnx_source=system ;;
    esac

    SSV_ONNXRUNTIME_SOURCE="${SSV_ONNXRUNTIME_SOURCE:-$default_onnx_source}"
    SSV_ONNXRUNTIME_VERSION="$default_onnx_version"
    SSV_ONNXRUNTIME_ROOT="${SSV_ONNXRUNTIME_ROOT:-.deps/onnxruntime}"

    SSV_OPENCV_SOURCE="${SSV_OPENCV_SOURCE:-managed}"
    SSV_OPENCV_MODE="${SSV_OPENCV_MODE:-enabled}"
    SSV_OPENCV_ROOT="${SSV_OPENCV_ROOT:-.deps/opencv}"
    SSV_OPENCV_INCLUDE_DIR="${SSV_OPENCV_INCLUDE_DIR:-}"
    SSV_OPENCV_LIB_DIR="${SSV_OPENCV_LIB_DIR:-}"

    SSV_TENSORRT_SOURCE="${SSV_TENSORRT_SOURCE:-managed}"
    SSV_TENSORRT_MODE="${SSV_TENSORRT_MODE:-$default_tensorrt_mode}"
    SSV_TENSORRT_ROOT="${SSV_TENSORRT_ROOT:-.deps/tensorrt}"

    case "$SSV_ONNXRUNTIME_SOURCE" in managed|local|system) ;; *) ssv_deps_die "SSV_ONNXRUNTIME_SOURCE must be managed, local, or system"; return 1 ;; esac
    ssv_onnxruntime_normalize_version "$SSV_ONNXRUNTIME_VERSION" >/dev/null || return 1
    case "$SSV_OPENCV_SOURCE" in managed|local|system) ;; *) ssv_deps_die "SSV_OPENCV_SOURCE must be managed, local, or system"; return 1 ;; esac
    case "$SSV_OPENCV_MODE" in enabled|disabled) ;; *) ssv_deps_die "SSV_OPENCV_MODE must be enabled or disabled"; return 1 ;; esac
    case "$SSV_TENSORRT_SOURCE" in managed|system) ;; *) ssv_deps_die "SSV_TENSORRT_SOURCE must be managed or system"; return 1 ;; esac
    case "$SSV_TENSORRT_MODE" in auto|enabled|disabled) ;; *) ssv_deps_die "SSV_TENSORRT_MODE must be auto, enabled, or disabled"; return 1 ;; esac

    if [ "$SSV_ONNXRUNTIME_SOURCE" = managed ]; then
        case "$SSV_DEPS_PROFILE" in
            cpu)
                [[ "$SSV_ONNXRUNTIME_VERSION" != *-gpu ]] || {
                    ssv_deps_die "CPU profile requires a managed ONNX Runtime CPU package"
                    return 1
                }
                ;;
            nvidia)
                [[ "$SSV_ONNXRUNTIME_VERSION" == *-gpu ]] || {
                    ssv_deps_die "NVIDIA profile requires a managed ONNX Runtime GPU package"
                    return 1
                }
                ;;
            *)
                ssv_deps_die "$SSV_DEPS_PROFILE profile requires system or local ONNX Runtime"
                return 1
                ;;
        esac
    elif [ "$SSV_ONNXRUNTIME_SOURCE" = system ]; then
        if [ "$onnx_root_explicit" = true ]; then
            ssv_deps_die "system ONNX Runtime must not set managed ROOT"
            return 1
        fi
    fi
    if [ "$SSV_OPENCV_MODE" = disabled ]; then
        if [ "$opencv_source_explicit" = true ] || [ "$opencv_root_explicit" = true ] || \
            [ "$opencv_include_explicit" = true ] || [ "$opencv_lib_explicit" = true ]; then
            ssv_deps_die "disabled OpenCV must not set SOURCE, ROOT, INCLUDE_DIR, or LIB_DIR"
            return 1
        fi
    elif [ "$SSV_OPENCV_SOURCE" = system ]; then
        if [ "$opencv_root_explicit" = true ] || [ "$opencv_include_explicit" = true ] || [ "$opencv_lib_explicit" = true ]; then
            ssv_deps_die "system OpenCV must not set ROOT, INCLUDE_DIR, or LIB_DIR"
            return 1
        fi
    elif [ "$SSV_OPENCV_SOURCE" = managed ]; then
        if [ "$opencv_include_explicit" = true ] || [ "$opencv_lib_explicit" = true ]; then
            ssv_deps_die "managed OpenCV must not set INCLUDE_DIR or LIB_DIR"
            return 1
        fi
    else
        [ -n "${SSV_OPENCV_INCLUDE_DIR:-}" ] || {
            ssv_deps_die "local OpenCV requires SSV_OPENCV_INCLUDE_DIR"
            return 1
        }
        [ -n "${SSV_OPENCV_LIB_DIR:-}" ] || {
            ssv_deps_die "local OpenCV requires SSV_OPENCV_LIB_DIR"
            return 1
        }
        ssv_deps_validate_scalar SSV_OPENCV_INCLUDE_DIR "$SSV_OPENCV_INCLUDE_DIR" || return 1
        ssv_deps_validate_scalar SSV_OPENCV_LIB_DIR "$SSV_OPENCV_LIB_DIR" || return 1
        SSV_OPENCV_INCLUDE_DIR="$(ssv_deps_abs_path "$SSV_OPENCV_INCLUDE_DIR")"
        SSV_OPENCV_INCLUDE_DIR="$(ssv_deps_normalize_path "$SSV_OPENCV_INCLUDE_DIR")" || return 1
        SSV_OPENCV_LIB_DIR="$(ssv_deps_abs_path "$SSV_OPENCV_LIB_DIR")"
        SSV_OPENCV_LIB_DIR="$(ssv_deps_normalize_path "$SSV_OPENCV_LIB_DIR")" || return 1
        [ -d "$SSV_OPENCV_INCLUDE_DIR" ] || {
            ssv_deps_die "local OpenCV include directory does not exist: $SSV_OPENCV_INCLUDE_DIR"
            return 1
        }
        [ -d "$SSV_OPENCV_LIB_DIR" ] || {
            ssv_deps_die "local OpenCV library directory does not exist: $SSV_OPENCV_LIB_DIR"
            return 1
        }
    fi
    if [ "$SSV_TENSORRT_MODE" = disabled ]; then
        if [ "$tensorrt_source_explicit" = true ] || [ "$tensorrt_root_explicit" = true ] || \
            [ "${SSV_DEPS_EXPLICIT_SSV_TENSORRT_ARCHIVE:-false}" = true ] || \
            [ "${SSV_DEPS_EXPLICIT_SSV_TENSORRT_URL:-false}" = true ] || \
            [ "${SSV_DEPS_EXPLICIT_CUDA_HOME:-false}" = true ]; then
            ssv_deps_die "disabled TensorRT must not set SOURCE, ROOT, ARCHIVE, URL, or CUDA_HOME"
            return 1
        fi
    elif [ "$SSV_TENSORRT_SOURCE" = system ]; then
        if [ "$tensorrt_root_explicit" = true ] || \
            [ "${SSV_DEPS_EXPLICIT_SSV_TENSORRT_ARCHIVE:-false}" = true ] || \
            [ "${SSV_DEPS_EXPLICIT_SSV_TENSORRT_URL:-false}" = true ] || \
            [ "${SSV_DEPS_EXPLICIT_CUDA_HOME:-false}" = true ]; then
            ssv_deps_die "system TensorRT must not set ROOT, ARCHIVE, URL, or CUDA_HOME"
            return 1
        fi
    fi
    if [ -n "${SSV_TENSORRT_ARCHIVE:-}" ] && [ -n "${SSV_TENSORRT_URL:-}" ]; then
        ssv_deps_die "SSV_TENSORRT_ARCHIVE and SSV_TENSORRT_URL cannot both be set"
        return 1
    fi

    if [ "$SSV_DEPS_PROFILE" = nvidia ] && [ "$SSV_TENSORRT_MODE" != enabled ]; then
        ssv_deps_die "NVIDIA profile requires SSV_TENSORRT_MODE=enabled"
        return 1
    fi

    if [ "$SSV_ONNXRUNTIME_SOURCE" = managed ] || [ "$SSV_ONNXRUNTIME_SOURCE" = local ]; then
        SSV_ONNXRUNTIME_ROOT="$(ssv_deps_validate_root SSV_ONNXRUNTIME_ROOT "$SSV_ONNXRUNTIME_ROOT")" || return 1
    fi
    if [ "$SSV_OPENCV_MODE" = enabled ] && { [ "$SSV_OPENCV_SOURCE" = managed ] || [ "$SSV_OPENCV_SOURCE" = local ]; }; then
        SSV_OPENCV_ROOT="$(ssv_deps_validate_root SSV_OPENCV_ROOT "$SSV_OPENCV_ROOT")" || return 1
    fi
    if [ "$SSV_TENSORRT_MODE" != disabled ] && [ "$SSV_TENSORRT_SOURCE" = managed ]; then
        SSV_TENSORRT_ROOT="$(ssv_deps_validate_root SSV_TENSORRT_ROOT "$SSV_TENSORRT_ROOT")" || return 1
    fi
    [ -z "${SSV_TENSORRT_ARCHIVE:-}" ] || ssv_deps_validate_scalar SSV_TENSORRT_ARCHIVE "$SSV_TENSORRT_ARCHIVE" || return 1
    if [ -n "${SSV_TENSORRT_URL:-}" ]; then
        case "$SSV_TENSORRT_URL" in
            *$'\n'*|*$'\r'*)
                ssv_deps_die "SSV_TENSORRT_URL must not contain a newline"
                return 1
                ;;
        esac
    fi
    [ -z "${CUDA_HOME:-}" ] || ssv_deps_validate_scalar CUDA_HOME "$CUDA_HOME" || return 1
}

ssv_deps_parse_provider_result() {
    local dep="$1"
    local output="$2"
    local line_count
    line_count="$(printf '%s\n' "$output" | awk 'NF {count++} END {print count+0}')"
    [ "$line_count" -eq 3 ] || { ssv_deps_die "$dep provider returned $line_count result lines, expected 3"; return 1; }
    local version="" pkgconfig_dir="" runtime_dirs="" line
    while IFS= read -r line; do
        case "$line" in
            version=*) [ -z "$version" ] || return 1; version="${line#version=}" ;;
            pkgconfig_dir=*) [ -z "$pkgconfig_dir" ] || return 1; pkgconfig_dir="${line#pkgconfig_dir=}" ;;
            runtime_dirs=*) [ -z "$runtime_dirs" ] || return 1; runtime_dirs="${line#runtime_dirs=}" ;;
            *) ssv_deps_die "$dep provider returned an unknown result line: $line"; return 1 ;;
        esac
    done <<< "$output"
    if [ -z "$version" ] || [ -z "$pkgconfig_dir" ]; then
        ssv_deps_die "$dep provider result is incomplete"
        return 1
    fi
    ssv_deps_validate_scalar "${dep}.version" "$version" || return 1
    ssv_deps_validate_scalar "${dep}.pkgconfig_dir" "$pkgconfig_dir" || return 1
    ssv_deps_validate_path_list "${dep}.runtime_dirs" "$runtime_dirs" || return 1
    printf '%s\n%s\n%s\n' "$version" "$pkgconfig_dir" "$runtime_dirs"
}

ssv_deps_parse_onnxruntime_result() {
    local output="$1"
    local line_count
    line_count="$(printf '%s\n' "$output" | awk 'NF { count++ } END { print count+0 }')"
    [ "$line_count" -eq 5 ] || {
        ssv_deps_die "ONNXRUNTIME provider returned $line_count result lines, expected 5"
        return 1
    }
    local version="" pkgconfig_dir="" runtime_dirs="" providers="" provider_libraries="" line
    local seen_version=false seen_pkgconfig=false seen_runtime=false seen_providers=false seen_libraries=false
    while IFS= read -r line; do
        case "$line" in
            version=*) [ "$seen_version" = false ] || return 1; seen_version=true; version="${line#version=}" ;;
            pkgconfig_dir=*) [ "$seen_pkgconfig" = false ] || return 1; seen_pkgconfig=true; pkgconfig_dir="${line#pkgconfig_dir=}" ;;
            runtime_dirs=*) [ "$seen_runtime" = false ] || return 1; seen_runtime=true; runtime_dirs="${line#runtime_dirs=}" ;;
            providers=*) [ "$seen_providers" = false ] || return 1; seen_providers=true; providers="${line#providers=}" ;;
            provider_libraries=*) [ "$seen_libraries" = false ] || return 1; seen_libraries=true; provider_libraries="${line#provider_libraries=}" ;;
            *) ssv_deps_die "ONNXRUNTIME provider returned an unknown result line: $line"; return 1 ;;
        esac
    done <<< "$output"
    if [ "$seen_version" != true ] || [ "$seen_pkgconfig" != true ] || [ "$seen_runtime" != true ] || \
        [ "$seen_providers" != true ] || [ "$seen_libraries" != true ]; then
        ssv_deps_die "ONNXRUNTIME provider result is incomplete"
        return 1
    fi
    if [ -z "$version" ] || [ -z "$pkgconfig_dir" ] || [ -z "$providers" ]; then
        ssv_deps_die "ONNXRUNTIME provider result has empty required fields"
        return 1
    fi
    ssv_deps_validate_scalar ONNXRUNTIME.version "$version" || return 1
    ssv_deps_validate_scalar ONNXRUNTIME.pkgconfig_dir "$pkgconfig_dir" || return 1
    ssv_deps_validate_path_list ONNXRUNTIME.runtime_dirs "$runtime_dirs" || return 1
    ssv_deps_validate_scalar ONNXRUNTIME.providers "$providers" || return 1
    ssv_deps_validate_path_list ONNXRUNTIME.provider_libraries "$provider_libraries" || return 1
    printf '%s\n%s\n%s\n%s\n%s\n' \
        "$version" "$pkgconfig_dir" "$runtime_dirs" "$providers" "$provider_libraries"
}

ssv_deps_system_result() {
    local package_name="$1" minimum="$2"
    ssv_deps_pkgconfig_version_at_least "$package_name" "$minimum" || {
        ssv_deps_die "system $package_name >= $minimum is required"
        return 1
    }
    local version pc_dir lib_dir runtime_dirs link_flags flag
    local link_dirs=()
    version="$(pkg-config --modversion "$package_name")"
    pc_dir="$(ssv_deps_pkgconfig_dir "$package_name")" || return 1
    case "$pc_dir" in
        "$SSV_ROOT/.deps"/*)
            ssv_deps_die "system $package_name resolved to a managed project path: $pc_dir"
            return 1
            ;;
    esac
    lib_dir="$(pkg-config --variable=libdir "$package_name" 2>/dev/null || true)"
    [ -z "$lib_dir" ] || link_dirs+=("$lib_dir")
    link_flags="$(pkg-config --libs "$package_name" 2>/dev/null || true)"
    for flag in $link_flags; do
        case "$flag" in -L?*) link_dirs+=("${flag#-L}") ;; esac
    done
    runtime_dirs="$(ssv_deps_runtime_dirs "${link_dirs[@]}")"
    local probe_source
    case "$package_name" in
        opencv4)
            probe_source='#include <opencv2/core.hpp>
int main() { return CV_VERSION[0] == '\''\0'\''; }'
            ;;
        nvinfer)
            probe_source='#include <NvInfer.h>
#include <cuda_runtime_api.h>
int main() { return NV_TENSORRT_MAJOR < 0; }'
            ;;
        *) ssv_deps_die "no system dependency probe is defined for $package_name"; return 1 ;;
    esac
    ssv_deps_compile_probe "$probe_source" "$package_name" "$runtime_dirs" || {
        ssv_deps_die "system $package_name compile/load probe failed"
        return 1
    }
    printf 'version=%s\npkgconfig_dir=%s\nruntime_dirs=%s\n' "$version" "$pc_dir" "$runtime_dirs"
}

ssv_deps_assign_result() {
    local prefix="$1" output="$2"
    local parsed
    parsed="$(ssv_deps_parse_provider_result "$prefix" "$output")" || return 1
    local version pc_dir runtime_dirs
    version="$(printf '%s\n' "$parsed" | sed -n '1p')"
    pc_dir="$(printf '%s\n' "$parsed" | sed -n '2p')"
    runtime_dirs="$(printf '%s\n' "$parsed" | sed -n '3p')"
    printf -v "SSV_DEPS_${prefix}_VERSION" '%s' "$version"
    printf -v "SSV_DEPS_${prefix}_PCDIR" '%s' "$pc_dir"
    printf -v "SSV_DEPS_${prefix}_RUNTIME_DIRS" '%s' "$runtime_dirs"
}

ssv_deps_assign_onnxruntime_result() {
    local output="$1" parsed
    parsed="$(ssv_deps_parse_onnxruntime_result "$output")" || return 1
    SSV_DEPS_ONNXRUNTIME_VERSION="$(printf '%s\n' "$parsed" | sed -n '1p')"
    SSV_DEPS_ONNXRUNTIME_PCDIR="$(printf '%s\n' "$parsed" | sed -n '2p')"
    SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS="$(printf '%s\n' "$parsed" | sed -n '3p')"
    SSV_DEPS_ONNXRUNTIME_PROVIDERS="$(printf '%s\n' "$parsed" | sed -n '4p')"
    SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES="$(printf '%s\n' "$parsed" | sed -n '5p')"
}

ssv_deps_prepend_pkgconfig() {
    local dir="$1"
    [ -n "$dir" ] || return 0
    SSV_DEPS_PKG_CONFIG_PATH="$(ssv_deps_join_unique "$dir${SSV_DEPS_PKG_CONFIG_PATH:+:$SSV_DEPS_PKG_CONFIG_PATH}")"
    export PKG_CONFIG_PATH="$SSV_DEPS_PKG_CONFIG_PATH${SSV_DEPS_BASE_PKG_CONFIG_PATH:+:$SSV_DEPS_BASE_PKG_CONFIG_PATH}"
}

ssv_deps_prepare_onnxruntime() {
    local output
    if [ "$SSV_ONNXRUNTIME_SOURCE" = managed ]; then
        output="$(ssv_onnxruntime_managed_prepare "$SSV_ONNXRUNTIME_ROOT" "$SSV_ONNXRUNTIME_VERSION" "$SSV_DEPS_PROFILE")" || return 1
    elif [ "$SSV_ONNXRUNTIME_SOURCE" = local ]; then
        output="$(ssv_onnxruntime_local_prepare "$SSV_ONNXRUNTIME_ROOT" "${SSV_ONNXRUNTIME_VERSION%-gpu}" "$SSV_DEPS_PROFILE")" || return 1
    else
        output="$(ssv_onnxruntime_system_prepare "${SSV_ONNXRUNTIME_VERSION%-gpu}" "$SSV_DEPS_PROFILE")" || return 1
    fi
    ssv_deps_assign_onnxruntime_result "$output" || return 1
    SSV_DEPS_ONNXRUNTIME_SOURCE="$SSV_ONNXRUNTIME_SOURCE"
    if [ "$SSV_ONNXRUNTIME_SOURCE" = managed ]; then
        SSV_DEPS_ONNXRUNTIME_CONFIG_VERSION="$SSV_ONNXRUNTIME_VERSION"
    else
        SSV_DEPS_ONNXRUNTIME_CONFIG_VERSION="$SSV_ONNXRUNTIME_SOURCE"
    fi
    ssv_deps_prepend_pkgconfig "$SSV_DEPS_ONNXRUNTIME_PCDIR"
    local hit
    hit="$(ssv_deps_pkgconfig_dir onnxruntime)" || return 1
    [ "$hit" = "$SSV_DEPS_ONNXRUNTIME_PCDIR" ] || { ssv_deps_die "onnxruntime pkg-config source mismatch: expected $SSV_DEPS_ONNXRUNTIME_PCDIR, got $hit"; return 1; }
}

ssv_deps_prepare_opencv() {
    SSV_DEPS_OPENCV_MODE="$SSV_OPENCV_MODE"
    if [ "$SSV_OPENCV_MODE" = disabled ]; then
        SSV_DEPS_OPENCV_SOURCE=disabled
        SSV_DEPS_OPENCV_VERSION=disabled
        SSV_DEPS_OPENCV_PCDIR=""
        SSV_DEPS_OPENCV_RUNTIME_DIRS=""
        SSV_DEPS_OPENCV_INCLUDE_DIR=""
        SSV_DEPS_OPENCV_LIB_DIR=""
        return 0
    fi
    local output
    if [ "$SSV_OPENCV_SOURCE" = managed ]; then
        output="$(ssv_opencv_managed_prepare "$SSV_OPENCV_ROOT" "$SSV_DEPS_DEFAULT_OPENCV_VERSION")" || return 1
    elif [ "$SSV_OPENCV_SOURCE" = local ]; then
        output="$(ssv_opencv_local_prepare "$SSV_OPENCV_INCLUDE_DIR" "$SSV_OPENCV_LIB_DIR" "$SSV_DEPS_DEFAULT_OPENCV_VERSION")" || return 1
    else
        output="$(ssv_deps_system_result opencv4 4.5)" || return 1
    fi
    ssv_deps_assign_result OPENCV "$output" || return 1
    SSV_DEPS_OPENCV_SOURCE="$SSV_OPENCV_SOURCE"
    if [ "$SSV_OPENCV_SOURCE" = local ]; then
        SSV_DEPS_OPENCV_INCLUDE_DIR="$SSV_OPENCV_INCLUDE_DIR"
        [ -f "$SSV_DEPS_OPENCV_INCLUDE_DIR/opencv2/core.hpp" ] || SSV_DEPS_OPENCV_INCLUDE_DIR="$SSV_DEPS_OPENCV_INCLUDE_DIR/opencv4"
        SSV_DEPS_OPENCV_LIB_DIR="$SSV_OPENCV_LIB_DIR"
    else
        SSV_DEPS_OPENCV_INCLUDE_DIR=""
        SSV_DEPS_OPENCV_LIB_DIR=""
    fi
    ssv_deps_prepend_pkgconfig "$SSV_DEPS_OPENCV_PCDIR"
    local hit
    hit="$(ssv_deps_pkgconfig_dir opencv4)" || return 1
    [ "$hit" = "$SSV_DEPS_OPENCV_PCDIR" ] || { ssv_deps_die "opencv4 pkg-config source mismatch: expected $SSV_DEPS_OPENCV_PCDIR, got $hit"; return 1; }
}

ssv_deps_prepare_tensorrt() {
    if [ "$SSV_TENSORRT_MODE" = disabled ]; then
        SSV_DEPS_TENSORRT_SOURCE=disabled
        SSV_DEPS_TENSORRT_VERSION=disabled
        SSV_DEPS_TENSORRT_MODE=disabled
        SSV_DEPS_TENSORRT_PCDIR=""
        SSV_DEPS_TENSORRT_RUNTIME_DIRS=""
        return 0
    fi

    local output="" status=0
    if [ "$SSV_TENSORRT_MODE" = auto ] && [ -z "${SSV_TENSORRT_ARCHIVE:-}" ] && [ -z "${SSV_TENSORRT_URL:-}" ]; then
        if [ "$SSV_TENSORRT_SOURCE" = managed ]; then
            if [ ! -e "$SSV_TENSORRT_ROOT" ] || ssv_deps_is_empty_dir "$SSV_TENSORRT_ROOT"; then
                SSV_DEPS_TENSORRT_SOURCE=managed
                SSV_DEPS_TENSORRT_VERSION=unavailable
                SSV_DEPS_TENSORRT_MODE=disabled
                SSV_DEPS_TENSORRT_PCDIR=""
                SSV_DEPS_TENSORRT_RUNTIME_DIRS=""
                return 0
            fi
            ssv_deps_require_replaceable_root "$SSV_TENSORRT_ROOT" ssv_tensorrt_managed_validate || return 1
            if ! ssv_tensorrt_managed_validate "$SSV_TENSORRT_ROOT" >/dev/null 2>&1; then
                SSV_DEPS_TENSORRT_SOURCE=managed
                SSV_DEPS_TENSORRT_VERSION=unavailable
                SSV_DEPS_TENSORRT_MODE=disabled
                SSV_DEPS_TENSORRT_PCDIR=""
                SSV_DEPS_TENSORRT_RUNTIME_DIRS=""
                return 0
            fi
        elif ! pkg-config --exists nvinfer; then
            SSV_DEPS_TENSORRT_SOURCE=system
            SSV_DEPS_TENSORRT_VERSION=unavailable
            SSV_DEPS_TENSORRT_MODE=disabled
            SSV_DEPS_TENSORRT_PCDIR=""
            SSV_DEPS_TENSORRT_RUNTIME_DIRS=""
            return 0
        fi
    fi
    if [ "$SSV_TENSORRT_SOURCE" = managed ]; then
        if output="$(ssv_tensorrt_managed_prepare "$SSV_TENSORRT_ROOT")"; then status=0; else status=$?; fi
    else
        if output="$(ssv_deps_system_result nvinfer 1.0)"; then status=0; else status=$?; fi
    fi
    if [ "$status" -ne 0 ]; then
        if [ "$SSV_TENSORRT_MODE" = auto ] && [ -z "${SSV_TENSORRT_ARCHIVE:-}" ] && [ -z "${SSV_TENSORRT_URL:-}" ]; then
            SSV_DEPS_TENSORRT_SOURCE="$SSV_TENSORRT_SOURCE"
            SSV_DEPS_TENSORRT_VERSION=unavailable
            SSV_DEPS_TENSORRT_MODE=disabled
            SSV_DEPS_TENSORRT_PCDIR=""
            SSV_DEPS_TENSORRT_RUNTIME_DIRS=""
            return 0
        fi
        ssv_deps_die "TensorRT is unavailable for mode=$SSV_TENSORRT_MODE source=$SSV_TENSORRT_SOURCE"
        return 1
    fi
    ssv_deps_assign_result TENSORRT "$output" || return 1
    if [ "$SSV_TENSORRT_MODE" = auto ]; then
        if ! ssv_tensorrt_validate_required_version "$SSV_DEPS_TENSORRT_VERSION" >/dev/null 2>&1; then
            SSV_DEPS_TENSORRT_SOURCE="$SSV_TENSORRT_SOURCE"
            SSV_DEPS_TENSORRT_VERSION=unavailable
            SSV_DEPS_TENSORRT_MODE=disabled
            SSV_DEPS_TENSORRT_PCDIR=""
            SSV_DEPS_TENSORRT_RUNTIME_DIRS=""
            return 0
        fi
    else
        ssv_tensorrt_validate_required_version "$SSV_DEPS_TENSORRT_VERSION" || return 1
    fi
    SSV_DEPS_TENSORRT_SOURCE="$SSV_TENSORRT_SOURCE"
    SSV_DEPS_TENSORRT_MODE=enabled
    ssv_deps_prepend_pkgconfig "$SSV_DEPS_TENSORRT_PCDIR"
    local hit
    hit="$(ssv_deps_pkgconfig_dir nvinfer)" || return 1
    [ "$hit" = "$SSV_DEPS_TENSORRT_PCDIR" ] || { ssv_deps_die "nvinfer pkg-config source mismatch: expected $SSV_DEPS_TENSORRT_PCDIR, got $hit"; return 1; }
}

ssv_deps_onnxruntime_signature_roots() {
    local pc_dir="${SSV_DEPS_ONNXRUNTIME_PCDIR:-}"
    if [ -n "$pc_dir" ]; then
        local lib_dir
        lib_dir="$(pkg-config --variable=libdir onnxruntime 2>/dev/null)" || {
            ssv_deps_die "ONNX Runtime pkg-config did not resolve a library directory for signature"
            return 1
        }
        [ -d "$lib_dir" ] || {
            ssv_deps_die "ONNX Runtime signature library directory is missing: $lib_dir"
            return 1
        }
        lib_dir="$(cd -- "$lib_dir" && pwd -P)"
        local link_library="$lib_dir/libonnxruntime.so"
        [ -f "$link_library" ] || {
            ssv_deps_die "ONNX Runtime signature root is missing: $link_library"
            return 1
        }
        printf '%s\n' "$link_library"
    fi

    local library
    local -a provider_libraries=()
    local IFS=:
    read -r -a provider_libraries <<< "${SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES:-}"
    for library in "${provider_libraries[@]}"; do
        [ -n "$library" ] || continue
        [ -f "$library" ] || {
            ssv_deps_die "ONNX Runtime Provider signature root is missing: $library"
            return 1
        }
        printf '%s\n' "$library"
    done
}

ssv_deps_linked_library_paths() {
    local roots
    roots="$(ssv_deps_onnxruntime_signature_roots)" || return 1
    [ -n "$roots" ] || return 0

    local search_path
    search_path="$(ssv_deps_join_unique \
        "${SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS:-}:${SSV_DEPS_TENSORRT_RUNTIME_DIRS:-}:${LD_LIBRARY_PATH:-}")"
    local root resolved ldd_output line dependency
    local -A seen_paths=()
    while IFS= read -r root; do
        [ -n "$root" ] || continue
        resolved="$(readlink -f -- "$root")" || {
            ssv_deps_die "failed to resolve dependency signature root: $root"
            return 1
        }
        seen_paths["$resolved"]=true
        ldd_output="$(LD_LIBRARY_PATH="$search_path" ldd "$resolved" 2>&1)" || {
            ssv_deps_die "failed to inspect linked libraries for dependency signature: $resolved"
            return 1
        }
        while IFS= read -r line; do
            case "$line" in
                *" => not found"*)
                    ssv_deps_die "dependency signature has an unresolved library for $resolved: $line"
                    return 1
                    ;;
                *" => /"*)
                    dependency="${line#* => }"
                    dependency="${dependency% (*}"
                    ;;
                [[:space:]]/*" ("*)
                    dependency="${line#"${line%%[![:space:]]*}"}"
                    dependency="${dependency% (*}"
                    ;;
                *) continue ;;
            esac
            dependency="$(readlink -f -- "$dependency")" || {
                ssv_deps_die "failed to resolve linked dependency for signature: $dependency"
                return 1
            }
            [ -f "$dependency" ] || {
                ssv_deps_die "linked dependency for signature is not a file: $dependency"
                return 1
            }
            seen_paths["$dependency"]=true
        done <<< "$ldd_output"
    done <<< "$roots"
    printf '%s\n' "${!seen_paths[@]}" | LC_ALL=C sort
}

ssv_deps_file_content_identity() {
    local path="$1" build_id digest
    build_id="$(LC_ALL=C readelf -n "$path" 2>/dev/null \
        | awk '/Build ID:/ { print $3; exit }')"
    if [ -n "$build_id" ]; then
        printf '%s|build-id=%s\n' "$path" "$build_id"
        return 0
    fi
    if ssv_deps_have_command sha256sum; then
        digest="$(sha256sum -- "$path" | awk '{print $1}')" || return 1
    else
        digest="$(shasum -a 256 "$path" | awk '{print $1}')" || return 1
    fi
    [ -n "$digest" ] || {
        ssv_deps_die "failed to identify dependency library contents: $path"
        return 1
    }
    printf '%s|sha256=%s\n' "$path" "$digest"
}

ssv_deps_dependency_content_identity() {
    local libraries path
    libraries="$(ssv_deps_linked_library_paths)" || return 1
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        ssv_deps_file_content_identity "$path" || return 1
    done <<< "$libraries"
}

ssv_deps_compute_signature() {
    local payload dependency_content_identity
    dependency_content_identity="$(ssv_deps_dependency_content_identity)" || return 1
    payload="$(printf '%s\n' \
        "$SSV_DEPS_PROFILE|$SSV_DEPS_ONNXRUNTIME_SOURCE|$SSV_DEPS_ONNXRUNTIME_CONFIG_VERSION|$SSV_DEPS_ONNXRUNTIME_VERSION|$SSV_DEPS_ONNXRUNTIME_PCDIR|$SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS|$SSV_DEPS_ONNXRUNTIME_PROVIDERS|$SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES" \
        "$SSV_DEPS_OPENCV_SOURCE|$SSV_DEPS_OPENCV_MODE|$SSV_DEPS_OPENCV_VERSION|$SSV_DEPS_OPENCV_PCDIR|$SSV_DEPS_OPENCV_RUNTIME_DIRS|$SSV_DEPS_OPENCV_INCLUDE_DIR|$SSV_DEPS_OPENCV_LIB_DIR" \
        "$SSV_DEPS_TENSORRT_SOURCE|$SSV_TENSORRT_MODE|$SSV_DEPS_TENSORRT_MODE|$SSV_DEPS_TENSORRT_VERSION|$SSV_DEPS_TENSORRT_PCDIR|$SSV_DEPS_TENSORRT_RUNTIME_DIRS" \
        "$dependency_content_identity")"
    if ssv_deps_have_command sha256sum; then
        printf '%s' "$payload" | sha256sum | awk '{print $1}'
    else
        printf '%s' "$payload" | shasum -a 256 | awk '{print $1}'
    fi
}

ssv_deps_write_env() {
    local path="$1"
    mkdir -p -- "$(dirname -- "$path")"
    local temporary
    temporary="$(mktemp "$(dirname -- "$path")/.ssv-deps-env.XXXXXX")" || return 1
    local key value
    for key in "${SSV_DEPS_SNAPSHOT_KEYS[@]}"; do
        value="${!key-}"
        printf '%s=%q\n' "$key" "$value" >> "$temporary" || {
            rm -f -- "$temporary"
            return 1
        }
    done
    mv -f -- "$temporary" "$path" || {
        rm -f -- "$temporary"
        return 1
    }
}

ssv_deps_validate_env_file() {
    local path="$1"
    [ -f "$path" ] || { ssv_deps_die "dependency snapshot not found: $path; run ./ssv build first"; return 1; }
    local line key valid
    local seen="|"
    while IFS= read -r line || [ -n "$line" ]; do
        [[ "$line" =~ ^([A-Z0-9_]+)= ]] || { ssv_deps_die "invalid dependency snapshot line: $line"; return 1; }
        case "$line" in
            *$'\n'*|*$'\r'*|*';'*|*'`'*|*'&'*|*'|'*|*'<'*|*'>'*|*'$('*)
                ssv_deps_die "unsafe dependency snapshot line: $line"
                return 1
                ;;
        esac
        key="${BASH_REMATCH[1]}"
        case "$seen" in *"|$key|"*) ssv_deps_die "duplicate variable in dependency snapshot: $key"; return 1 ;; esac
        seen="$seen$key|"
        valid=false
        local allowed
        for allowed in "${SSV_DEPS_SNAPSHOT_KEYS[@]}"; do [ "$allowed" = "$key" ] && { valid=true; break; }; done
        [ "$valid" = true ] || { ssv_deps_die "unknown variable in dependency snapshot: $key"; return 1; }
    done < "$path"
    local required
    for required in "${SSV_DEPS_SNAPSHOT_KEYS[@]}"; do
        case "$seen" in *"|$required|"*) ;; *) ssv_deps_die "missing variable in dependency snapshot: $required"; return 1 ;; esac
    done
}

ssv_deps_load_env() {
    local path="$1"
    ssv_deps_validate_env_file "$path" || return 1
    # The strict whitelist and shell-token check above make this source safe
    # while preserving paths containing ordinary spaces via printf %q.
    # shellcheck disable=SC1090
    source "$path"
    export "${SSV_DEPS_SNAPSHOT_KEYS[@]}"
}

ssv_deps_load_build() {
    local path="${1:-${SSV_BUILD_DIR:-$SSV_ROOT/build}/ssv-deps.env.pending}"
    ssv_deps_load_env "$path" || return 1
    if [ -n "$SSV_DEPS_PKG_CONFIG_PATH" ]; then
        PKG_CONFIG_PATH="$(ssv_deps_join_unique "$SSV_DEPS_PKG_CONFIG_PATH${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}")"
        export PKG_CONFIG_PATH
    fi
}

ssv_deps_load_runtime() {
    local path="${1:-${SSV_BUILD_DIR:-$SSV_ROOT/build}/ssv-deps.env}"
    ssv_deps_load_env "$path" || return 1
    if [ -n "$SSV_DEPS_RUNTIME_PATH" ]; then
        local resolved_runtime_path
        resolved_runtime_path="$(ssv_deps_join_unique "$SSV_DEPS_RUNTIME_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}")"
        export LD_LIBRARY_PATH="$resolved_runtime_path"
    fi
}

ssv_deps_display_path() {
    local path="$1"
    case "$path" in "$SSV_ROOT"/*) printf '%s\n' "${path#"$SSV_ROOT"/}" ;; *) printf '%s\n' "$path" ;; esac
}

ssv_deps_print_summary() {
    local onnx_where opencv_where trt_where
    if [ "$SSV_DEPS_ONNXRUNTIME_SOURCE" = managed ]; then onnx_where=" root=$(ssv_deps_display_path "$SSV_ONNXRUNTIME_ROOT")"; else onnx_where=" pcdir=$SSV_DEPS_ONNXRUNTIME_PCDIR"; fi
    local onnx_display_version="$SSV_DEPS_ONNXRUNTIME_VERSION"
    [ "$SSV_DEPS_ONNXRUNTIME_SOURCE" != managed ] || onnx_display_version="$SSV_DEPS_ONNXRUNTIME_CONFIG_VERSION"
    printf 'ONNX Runtime  profile=%s source=%s version=%s providers=%s%s\n' \
        "$SSV_DEPS_PROFILE" "$SSV_DEPS_ONNXRUNTIME_SOURCE" "$onnx_display_version" \
        "$SSV_DEPS_ONNXRUNTIME_PROVIDERS" "$onnx_where"
    if [ "$SSV_DEPS_OPENCV_MODE" = disabled ]; then
        printf 'OpenCV        mode=disabled\n'
    else
        if [ "$SSV_DEPS_OPENCV_SOURCE" = managed ]; then opencv_where=" root=$(ssv_deps_display_path "$SSV_OPENCV_ROOT")"; else opencv_where=" pcdir=$SSV_DEPS_OPENCV_PCDIR"; fi
        printf 'OpenCV        source=%s mode=%s version=%s%s\n' "$SSV_DEPS_OPENCV_SOURCE" "$SSV_DEPS_OPENCV_MODE" "$SSV_DEPS_OPENCV_VERSION" "$opencv_where"
    fi
    if [ "$SSV_DEPS_TENSORRT_MODE" = disabled ]; then
        printf 'TensorRT      source=%s mode=%s status=stub\n' "$SSV_DEPS_TENSORRT_SOURCE" "$SSV_DEPS_TENSORRT_MODE"
    else
        if [ "$SSV_DEPS_TENSORRT_SOURCE" = managed ]; then trt_where=" root=$(ssv_deps_display_path "$SSV_TENSORRT_ROOT")"; else trt_where=" pcdir=$SSV_DEPS_TENSORRT_PCDIR"; fi
        printf 'TensorRT      source=%s mode=%s version=%s%s\n' "$SSV_DEPS_TENSORRT_SOURCE" "$SSV_DEPS_TENSORRT_MODE" "$SSV_DEPS_TENSORRT_VERSION" "$trt_where"
    fi
}

ssv_deps_prepare() {
    local requested_profile="${1:-auto}"
    ssv_onnxruntime_validate_profile "$requested_profile" || return 1
    ssv_deps_capture_explicit_config
    ssv_deps_load_dotenv || return 1
    local -a detected_vendors=()
    if [ "$requested_profile" = auto ]; then
        mapfile -t detected_vendors < <(ssv_onnxruntime_detect_gpu_vendors /sys)
    fi
    SSV_DEPS_PROFILE="$(ssv_onnxruntime_resolve_profile "$requested_profile" "${detected_vendors[@]}")" || return 1
    ssv_deps_resolve_config || return 1
    SSV_DEPS_BASE_PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}"
    if [ -n "${SSV_EXTRA_PKG_CONFIG_PATH:-}" ]; then
        SSV_DEPS_BASE_PKG_CONFIG_PATH="$SSV_EXTRA_PKG_CONFIG_PATH${SSV_DEPS_BASE_PKG_CONFIG_PATH:+:$SSV_DEPS_BASE_PKG_CONFIG_PATH}"
    fi
    SSV_DEPS_PKG_CONFIG_PATH=""
    export PKG_CONFIG_PATH="$SSV_DEPS_BASE_PKG_CONFIG_PATH"
    if [ "$SSV_DEPS_PROFILE" = nvidia ]; then
        # TensorRT/CUDA must be loadable before the ORT provider probe. This
        # still prepares one ORT artifact and fails before Meson on closure gaps.
        ssv_deps_prepare_tensorrt || return 1
        if [ -n "$SSV_DEPS_TENSORRT_RUNTIME_DIRS" ]; then
            LD_LIBRARY_PATH="$(ssv_deps_join_unique "$SSV_DEPS_TENSORRT_RUNTIME_DIRS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}")"
            export LD_LIBRARY_PATH
        fi
    fi
    ssv_deps_prepare_onnxruntime || return 1
    ssv_deps_prepare_opencv || return 1
    [ "$SSV_DEPS_PROFILE" = nvidia ] || ssv_deps_prepare_tensorrt || return 1
    SSV_DEPS_PKG_CONFIG_PATH="$(ssv_deps_join_unique "$SSV_DEPS_PKG_CONFIG_PATH")"
    SSV_DEPS_RUNTIME_PATH="$(ssv_deps_join_unique "$SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS:$SSV_DEPS_OPENCV_RUNTIME_DIRS:$SSV_DEPS_TENSORRT_RUNTIME_DIRS")"
    SSV_DEPS_SIGNATURE="$(ssv_deps_compute_signature)" || return 1
    local pending="$SSV_BUILD_DIR/ssv-deps.env.pending"
    ssv_deps_write_env "$pending" || return 1
    ssv_deps_print_summary
}
