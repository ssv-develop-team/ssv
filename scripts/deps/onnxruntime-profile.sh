#!/usr/bin/env bash

# Runtime-profile decisions are pure values. Artifact discovery and validation
# are layered below so callers never need to branch on individual EPs.

ssv_onnxruntime_validate_profile() {
    local profile="$1"
    case "$profile" in
        auto|cpu|nvidia|intel|amd) ;;
        *)
            ssv_deps_die "runtime profile must be auto, cpu, nvidia, intel, or amd: $profile"
            return 1
            ;;
    esac
}

ssv_onnxruntime_detect_gpu_vendors() {
    local sysfs_root="${1:-/sys}"
    local seen="|" vendor_file vendor
    for vendor_file in "$sysfs_root"/class/drm/card*/device/vendor; do
        [ -f "$vendor_file" ] || continue
        vendor="$(tr -d '[:space:]' < "$vendor_file")"
        vendor="${vendor,,}"
        vendor="${vendor#0x}"
        case "$vendor" in 10de|8086|1002) ;; *) continue ;; esac
        case "$seen" in *"|$vendor|"*) continue ;; esac
        seen="$seen$vendor|"
        printf '%s\n' "$vendor"
    done
}

ssv_onnxruntime_resolve_profile() {
    local requested="$1"
    shift
    ssv_onnxruntime_validate_profile "$requested" || return 1
    if [ "$requested" != auto ]; then
        printf '%s\n' "$requested"
        return 0
    fi

    local has_nvidia=false has_intel=false has_amd=false vendor
    for vendor in "$@"; do
        vendor="${vendor,,}"
        vendor="${vendor#0x}"
        case "$vendor" in
            10de) has_nvidia=true ;;
            8086) has_intel=true ;;
            1002) has_amd=true ;;
        esac
    done

    # A hybrid machine must resolve deterministically and activate one ORT
    # artifact. The order follows the public profile matrix, not sysfs order.
    if [ "$has_nvidia" = true ]; then
        printf 'nvidia\n'
    elif [ "$has_intel" = true ]; then
        printf 'intel\n'
    elif [ "$has_amd" = true ]; then
        printf 'amd\n'
    else
        printf 'cpu\n'
    fi
}

ssv_onnxruntime_expected_providers() {
    local profile="$1"
    case "$profile" in
        cpu) printf 'CPUExecutionProvider\n' ;;
        nvidia) printf '%s\n' TensorrtExecutionProvider CUDAExecutionProvider CPUExecutionProvider ;;
        intel) printf '%s\n' OpenVINOExecutionProvider CPUExecutionProvider ;;
        amd) printf '%s\n' MIGraphXExecutionProvider CPUExecutionProvider ;;
        *)
            ssv_deps_die "resolved runtime profile required for Provider selection: $profile"
            return 1
            ;;
    esac
}

ssv_onnxruntime_provider_library_names() {
    local profile="$1"
    case "$profile" in
        cpu) return 0 ;;
        nvidia)
            printf '%s\n' \
                libonnxruntime_providers_shared.so \
                libonnxruntime_providers_tensorrt.so \
                libonnxruntime_providers_cuda.so
            ;;
        intel)
            printf '%s\n' \
                libonnxruntime_providers_shared.so \
                libonnxruntime_providers_openvino.so
            ;;
        amd)
            printf '%s\n' \
                libonnxruntime_providers_shared.so \
                libonnxruntime_providers_migraphx.so
            ;;
        *)
            ssv_deps_die "resolved runtime profile required for Provider libraries: $profile"
            return 1
            ;;
    esac
}

ssv_onnxruntime_validate_provider_set() {
    local profile="$1" providers="$2"
    ssv_deps_validate_scalar onnxruntime.providers "$providers" || return 1
    [ -n "$providers" ] || {
        ssv_deps_die "$profile profile returned no ONNX Runtime Providers"
        return 1
    }

    local expected_lines
    expected_lines="$(ssv_onnxruntime_expected_providers "$profile")" || return 1
    local seen="|" provider
    local -a actual_providers=()
    local IFS=,
    read -r -a actual_providers <<< "$providers"
    for provider in "${actual_providers[@]}"; do
        [ -n "$provider" ] || {
            ssv_deps_die "$profile profile returned an empty ONNX Runtime Provider name"
            return 1
        }
        case "$seen" in
            *"|$provider|"*)
                ssv_deps_die "$profile profile returned duplicate Provider: $provider"
                return 1
                ;;
        esac
        seen="$seen$provider|"
        case "$provider" in
            CPUExecutionProvider|AzureExecutionProvider) ;;
            TensorrtExecutionProvider)
                [ "$profile" = nvidia ] || {
                    ssv_deps_die "$profile profile must not activate Provider: $provider"
                    return 1
                }
                ;;
            CUDAExecutionProvider)
                [ "$profile" = nvidia ] || {
                    ssv_deps_die "$profile profile must not activate Provider: $provider"
                    return 1
                }
                ;;
            OpenVINOExecutionProvider)
                [ "$profile" = intel ] || {
                    ssv_deps_die "$profile profile must not activate Provider: $provider"
                    return 1
                }
                ;;
            MIGraphXExecutionProvider)
                [ "$profile" = amd ] || {
                    ssv_deps_die "$profile profile must not activate Provider: $provider"
                    return 1
                }
                ;;
            *)
                ssv_deps_die "$profile profile returned an unsupported Provider: $provider"
                return 1
                ;;
        esac
    done

    local expected
    while IFS= read -r expected; do
        case "$seen" in
            *"|$expected|"*) ;;
            *)
                ssv_deps_die "$profile profile is missing required Provider: $expected"
                return 1
                ;;
        esac
    done <<< "$expected_lines"
}

ssv_onnxruntime_collect_provider_libraries() {
    local profile="$1" lib_dir="$2"
    local result="" library_name library readelf_output
    while IFS= read -r library_name; do
        [ -n "$library_name" ] || continue
        library="$lib_dir/$library_name"
        [ -f "$library" ] || {
            ssv_deps_die "$profile profile is missing Provider library: $library_name"
            return 1
        }
        readelf_output="$(readelf -h "$library" 2>&1)" || {
            ssv_deps_die "$profile profile Provider library is not a readable ELF: $library_name: $readelf_output"
            return 1
        }
        if [ "$library_name" != libonnxruntime_providers_shared.so ]; then
            # Official release libraries version this exported ABI symbol, for
            # example GetProvider@@VERS_1.0. Match its base name while still
            # requiring a defined, externally visible dynamic symbol.
            if ! readelf --dyn-syms --wide "$library" | awk '
                $5 == "GLOBAL" && $6 == "DEFAULT" && $7 != "UND" {
                    symbol = $8
                    sub(/@.*/, "", symbol)
                    if (symbol == "GetProvider") found = 1
                }
                END { exit !found }
            '; then
                ssv_deps_die "$profile profile Provider library has no GetProvider entry point: $library_name"
                return 1
            fi
        fi
        result="$(ssv_deps_join_unique "$result${result:+:}$library")"
    done < <(ssv_onnxruntime_provider_library_names "$profile")
    printf '%s\n' "$result"
}

ssv_onnxruntime_runtime_dirs() {
    local profile="$1" lib_dir="$2" provider_libraries="$3"
    local shared_provider_library="$lib_dir/libonnxruntime_providers_shared.so"
    local -a libraries=("$lib_dir/libonnxruntime.so")
    local -a provider_items=()
    local IFS=:
    read -r -a provider_items <<< "$provider_libraries"
    libraries+=("${provider_items[@]}")

    local search_path="$lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    local library preload_library ldd_output missing symbol_errors dependency
    local -a dependency_dirs=()
    for library in "${libraries[@]}"; do
        [ -f "$library" ] || {
            ssv_deps_die "ONNX Runtime link library not found: $library"
            return 1
        }
        if ! readelf -h "$library" >/dev/null 2>&1; then
            ssv_deps_die "ONNX Runtime library is not a readable ELF: $(basename -- "$library")"
            return 1
        fi
        preload_library=""
        case "$library" in
            "$lib_dir"/libonnxruntime_providers_*.so)
                [ "$library" = "$shared_provider_library" ] || \
                    preload_library="$shared_provider_library"
                ;;
        esac
        if [ -n "$preload_library" ]; then
            # ORT loads the shared Provider before an execution Provider. The
            # official EP libraries intentionally leave Provider_GetHost for
            # that loader-owned boundary instead of declaring DT_NEEDED.
            ldd_output="$(
                LD_LIBRARY_PATH="$search_path" \
                    LD_PRELOAD="$preload_library${LD_PRELOAD:+:$LD_PRELOAD}" \
                    ldd -r "$library" 2>&1
            )" || {
                ssv_deps_die "$profile profile runtime load check failed for $(basename -- "$library"): $ldd_output"
                return 1
            }
        else
            ldd_output="$(LD_LIBRARY_PATH="$search_path" ldd -r "$library" 2>&1)" || {
                ssv_deps_die "$profile profile runtime load check failed for $(basename -- "$library"): $ldd_output"
                return 1
            }
        fi
        missing="$(printf '%s\n' "$ldd_output" | sed -n 's/^[[:space:]]*\([^[:space:]]*\) => not found$/\1/p' | paste -sd, -)"
        [ -z "$missing" ] || {
            ssv_deps_die "$profile profile has unresolved runtime dependencies for $(basename -- "$library"): $missing"
            return 1
        }
        symbol_errors="$(printf '%s\n' "$ldd_output" | sed -n 's/.*undefined symbol: \([^[:space:]]*\).*/\1/p' | sort -u | paste -sd, -)"
        [ -z "$symbol_errors" ] || {
            ssv_deps_die "$profile profile has unresolved runtime symbols for $(basename -- "$library"): $symbol_errors"
            return 1
        }
        while IFS= read -r dependency; do
            [ -n "$dependency" ] || continue
            dependency_dirs+=("$(dirname -- "$dependency")")
        done < <(printf '%s\n' "$ldd_output" | awk '
            $2 == "=>" && $3 ~ /^\// { print $3; next }
            $1 ~ /^\// { print $1 }
        ')
    done
    ssv_deps_runtime_dirs "$lib_dir" "${dependency_dirs[@]}"
}

ssv_onnxruntime_probe() {
    local runtime_dirs="$1"
    ssv_deps_compile_probe \
        '#include <algorithm>
#include <iostream>
#include <onnxruntime_cxx_api.h>
int main() {
    auto providers = Ort::GetAvailableProviders();
    std::sort(providers.begin(), providers.end());
    std::cout << "version=" << OrtGetApiBase()->GetVersionString() << "\nproviders=";
    for (std::size_t i = 0; i < providers.size(); ++i) {
        if (i != 0) std::cout << ",";
        std::cout << providers[i];
    }
    std::cout << "\n";
    return 0;
}' \
        onnxruntime "$runtime_dirs"
}

ssv_onnxruntime_validate_artifact() {
    local profile="$1" expected_version="$2" include_dir="$3" lib_dir="$4" expected_pc_dir="$5"
    [ -f "$include_dir/onnxruntime_cxx_api.h" ] || {
        ssv_deps_die "ONNX Runtime header not found: $include_dir/onnxruntime_cxx_api.h"
        return 1
    }
    [ -f "$lib_dir/libonnxruntime.so" ] || {
        ssv_deps_die "ONNX Runtime link library not found: $lib_dir/libonnxruntime.so"
        return 1
    }

    local pc_dir pc_version
    pc_dir="$(ssv_deps_pkgconfig_dir onnxruntime)" || return 1
    [ "$pc_dir" = "$expected_pc_dir" ] || {
        ssv_deps_die "onnxruntime pkg-config source mismatch: expected $expected_pc_dir, got $pc_dir"
        return 1
    }
    pc_version="$(pkg-config --modversion onnxruntime)" || return 1
    [ "$pc_version" = "$expected_version" ] || {
        ssv_deps_die "ONNX Runtime pkg-config ABI mismatch: expected $expected_version, got $pc_version"
        return 1
    }

    local provider_libraries runtime_dirs probe_output line_count
    provider_libraries="$(ssv_onnxruntime_collect_provider_libraries "$profile" "$lib_dir")" || return 1
    runtime_dirs="$(ssv_onnxruntime_runtime_dirs "$profile" "$lib_dir" "$provider_libraries")" || return 1
    probe_output="$(ssv_onnxruntime_probe "$runtime_dirs")" || {
        ssv_deps_die "ONNX Runtime compile/load probe failed for profile=$profile"
        return 1
    }
    line_count="$(printf '%s\n' "$probe_output" | awk 'NF { count++ } END { print count+0 }')"
    [ "$line_count" -eq 2 ] || {
        ssv_deps_die "ONNX Runtime probe returned $line_count lines, expected 2"
        return 1
    }

    local runtime_version="" providers="" line
    while IFS= read -r line; do
        case "$line" in
            version=*) [ -z "$runtime_version" ] || return 1; runtime_version="${line#version=}" ;;
            providers=*) [ -z "$providers" ] || return 1; providers="${line#providers=}" ;;
            *) ssv_deps_die "ONNX Runtime probe returned an unknown line: $line"; return 1 ;;
        esac
    done <<< "$probe_output"
    [ "$runtime_version" = "$expected_version" ] || {
        ssv_deps_die "ONNX Runtime ABI mismatch: expected $expected_version, got ${runtime_version:-empty}"
        return 1
    }
    ssv_onnxruntime_validate_provider_set "$profile" "$providers" || return 1

    printf 'version=%s\n' "$runtime_version"
    printf 'pkgconfig_dir=%s\n' "$pc_dir"
    printf 'runtime_dirs=%s\n' "$runtime_dirs"
    printf 'providers=%s\n' "$providers"
    printf 'provider_libraries=%s\n' "$provider_libraries"
}

ssv_onnxruntime_local_prepare() {
    local root="$1" expected_version="$2" profile="$3"
    [ -d "$root" ] || {
        ssv_deps_die "local ONNX Runtime artifact not found: $root"
        return 1
    }
    local version_file="$root/VERSION_NUMBER"
    [ -f "$version_file" ] || {
        ssv_deps_die "local ONNX Runtime VERSION_NUMBER not found: $root"
        return 1
    }
    local artifact_version
    artifact_version="$(tr -d '[:space:]' < "$version_file")"
    [ "$artifact_version" = "$expected_version" ] || {
        ssv_deps_die "local ONNX Runtime version mismatch: expected $expected_version, got $artifact_version"
        return 1
    }

    local include_dir="$root/include" lib_dir
    lib_dir="$(ssv_onnxruntime_find_libdir "$root")" || {
        ssv_deps_die "local ONNX Runtime library not found under $root"
        return 1
    }
    lib_dir="$(cd -- "$lib_dir" && pwd -P)"
    local adapter_root="${SSV_BUILD_DIR:-$SSV_ROOT/build}/ssv-deps/onnxruntime-$profile"
    ssv_onnxruntime_make_pc "$adapter_root" "$expected_version" "$include_dir" "$lib_dir"
    local pc_dir="$adapter_root/lib/pkgconfig"
    pc_dir="$(cd -- "$pc_dir" && pwd -P)"
    local old_pkg_config_path="${PKG_CONFIG_PATH:-}"
    export PKG_CONFIG_PATH="$pc_dir${old_pkg_config_path:+:$old_pkg_config_path}"
    ssv_onnxruntime_validate_artifact "$profile" "$expected_version" "$include_dir" "$lib_dir" "$pc_dir"
}

ssv_onnxruntime_system_prepare() {
    local expected_version="$1" profile="$2"
    pkg-config --exists onnxruntime || {
        ssv_deps_die "system ONNX Runtime artifact is required for profile=$profile"
        return 1
    }
    local pc_dir include_dir lib_dir
    pc_dir="$(ssv_deps_pkgconfig_dir onnxruntime)" || return 1
    case "$pc_dir" in
        "$SSV_ROOT/.deps"/*|"${SSV_BUILD_DIR:-$SSV_ROOT/build}"/*)
            ssv_deps_die "system ONNX Runtime resolved to a project-local artifact: $pc_dir"
            return 1
            ;;
    esac
    include_dir="$(pkg-config --variable=includedir onnxruntime)"
    lib_dir="$(pkg-config --variable=libdir onnxruntime)"
    if [ ! -d "$include_dir" ] || [ ! -d "$lib_dir" ]; then
        ssv_deps_die "system ONNX Runtime pkg-config must define existing includedir and libdir"
        return 1
    fi
    ssv_onnxruntime_validate_artifact "$profile" "$expected_version" "$include_dir" "$lib_dir" "$pc_dir"
}
