#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ssv-deps-test.XXXXXX")"
export ROOT TEST_DIR
trap 'rm -rf -- "$TEST_DIR"' EXIT

passed=0
failed=0

pass() {
    printf 'ok - %s\n' "$1"
    passed=$((passed + 1))
}

fail() {
    printf 'not ok - %s\n' "$1" >&2
    failed=$((failed + 1))
}

assert_eq() {
    local expected="$1" actual="$2" name="$3"
    if [ "$expected" = "$actual" ]; then pass "$name"; else
        printf 'expected: %q\nactual:   %q\n' "$expected" "$actual" >&2
        fail "$name"
    fi
}

assert_success() {
    local name="$1" output
    shift
    if output="$("$@" 2>&1)"; then
        pass "$name"
    else
        printf '%s\n' "$output" >&2
        fail "$name"
    fi
}

assert_failure() {
    local name="$1"
    shift
    if "$@" >/dev/null 2>&1; then fail "$name"; else pass "$name"; fi
}

assert_failure_contains() {
    local name="$1" expected="$2" output
    shift 2
    if output="$("$@" 2>&1)"; then
        fail "$name"
    elif grep -Fq "$expected" <<< "$output"; then
        pass "$name"
    else
        printf '%s\n' "$output" >&2
        fail "$name (missing error: $expected)"
    fi
}

assert_failure_contains_without_pattern() {
    local name="$1" expected="$2" unexpected_pattern="$3" output
    shift 3
    if output="$("$@" 2>&1)"; then
        fail "$name"
    elif ! grep -Fq "$expected" <<< "$output"; then
        printf '%s\n' "$output" >&2
        fail "$name (missing error: $expected)"
    elif grep -Eq "$unexpected_pattern" <<< "$output"; then
        printf '%s\n' "$output" >&2
        fail "$name (unexpected secondary error pattern: $unexpected_pattern)"
    else
        pass "$name"
    fi
}

run_clean_shell() {
    env \
        -u SSV_ONNXRUNTIME_SOURCE -u SSV_ONNXRUNTIME_VERSION -u SSV_ONNXRUNTIME_ROOT \
        -u SSV_OPENCV_SOURCE -u SSV_OPENCV_MODE -u SSV_OPENCV_ROOT \
        -u SSV_OPENCV_INCLUDE_DIR -u SSV_OPENCV_LIB_DIR \
        -u SSV_TENSORRT_SOURCE -u SSV_TENSORRT_MODE -u SSV_TENSORRT_ROOT \
        -u SSV_TENSORRT_ARCHIVE -u SSV_TENSORRT_URL -u CUDA_HOME \
        -u SSV_EXTRA_PKG_CONFIG_PATH \
        ROOT="$ROOT" TEST_DIR="$TEST_DIR" \
        SSV_ROOT="$ROOT" SSV_BUILD_DIR="$TEST_DIR/build" bash --noprofile --norc -c "$1"
}

build_fake_onnxruntime() {
    local sdk="$1" version="$2" providers="$3" profile="$4"
    mkdir -p "$sdk/include" "$sdk/lib"
    printf '%s\n' \
        '#pragma once' \
        '#include <string>' \
        '#include <vector>' \
        'extern "C" {' \
        'struct OrtApiBase { const char* (*GetVersionString)(); };' \
        'const OrtApiBase* OrtGetApiBase();' \
        'const char* SsvFakeOrtProviders();' \
        '}' \
        'namespace Ort {' \
        'inline std::vector<std::string> GetAvailableProviders() {' \
        '    std::vector<std::string> result;' \
        '    std::string csv = SsvFakeOrtProviders();' \
        '    std::size_t start = 0;' \
        '    while (start <= csv.size()) {' \
        "        const auto end = csv.find(',', start);" \
        '        result.push_back(csv.substr(start, end - start));' \
        '        if (end == std::string::npos) break;' \
        '        start = end + 1;' \
        '    }' \
        '    return result;' \
        '}' \
        '}' \
        > "$sdk/include/onnxruntime_cxx_api.h"
    printf '%s\n' \
        '#include <onnxruntime_cxx_api.h>' \
        "namespace { const char* version() { return \"$version\"; } }" \
        'static const OrtApiBase api{version};' \
        'extern "C" const OrtApiBase* OrtGetApiBase() { return &api; }' \
        "extern \"C\" const char* SsvFakeOrtProviders() { return \"$providers\"; }" \
        > "$sdk/onnxruntime.cpp"
    "${CXX:-c++}" -std=c++17 -fPIC -shared -I"$sdk/include" \
        "$sdk/onnxruntime.cpp" -Wl,-soname,libonnxruntime.so -o "$sdk/lib/libonnxruntime.so"
    printf '%s\n' "$version" > "$sdk/VERSION_NUMBER"

    local -a libraries=()
    case "$profile" in
        nvidia) libraries=(shared tensorrt cuda) ;;
        intel) libraries=(shared openvino) ;;
        amd) libraries=(shared migraphx) ;;
    esac
    local library
    for library in "${libraries[@]}"; do
        if [ "$library" = shared ]; then
            printf '%s\n' \
                '#include <onnxruntime_cxx_api.h>' \
                'extern "C" void* Provider_GetHost() {' \
                '    return const_cast<OrtApiBase*>(OrtGetApiBase());' \
                '}' \
                > "$sdk/provider-$library.cpp"
        else
            printf '%s\n' \
                'extern "C" void* Provider_GetHost();' \
                'extern "C" void* GetProvider() { return Provider_GetHost(); }' \
                > "$sdk/provider-$library.cpp"
        fi
        "${CXX:-c++}" -std=c++17 -fPIC -shared -I"$sdk/include" \
            "$sdk/provider-$library.cpp" -L"$sdk/lib" -lonnxruntime \
            -Wl,-rpath,'$ORIGIN' -o "$sdk/lib/libonnxruntime_providers_$library.so"
    done
}

defaults="$(run_clean_shell 'source scripts/deps.sh; ssv_deps_resolve_config; printf "%s|%s|%s|%s|%s|%s" "$SSV_ONNXRUNTIME_SOURCE" "$SSV_ONNXRUNTIME_VERSION" "$SSV_OPENCV_SOURCE" "$SSV_OPENCV_MODE" "$SSV_TENSORRT_SOURCE" "$SSV_TENSORRT_MODE"')"
assert_eq 'managed|1.25.1|managed|enabled|managed|auto' "$defaults" 'default dependency configuration is uniform'

nvidia_defaults="$(run_clean_shell 'source scripts/deps.sh; SSV_DEPS_PROFILE=nvidia; ssv_deps_resolve_config; printf "%s|%s|%s" "$SSV_ONNXRUNTIME_SOURCE" "$SSV_ONNXRUNTIME_VERSION" "$SSV_TENSORRT_MODE"')"
assert_eq 'managed|1.25.1-gpu|enabled' "$nvidia_defaults" 'NVIDIA profile selects the managed GPU artifact and strict TensorRT dependency'
intel_defaults="$(run_clean_shell 'source scripts/deps.sh; SSV_DEPS_PROFILE=intel; ssv_deps_resolve_config; printf "%s|%s" "$SSV_ONNXRUNTIME_SOURCE" "$SSV_ONNXRUNTIME_VERSION"')"
assert_eq 'managed|1.25.1' "$intel_defaults" 'Intel profile selects a managed source-build artifact by default'

assert_eq '1.25.1' "$(run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_normalize_version 1.25.1')" 'ONNX Runtime CPU version syntax'
assert_eq '1.25.1-gpu' "$(run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_normalize_version 1.25.1-gpu')" 'ONNX Runtime GPU version syntax'
assert_failure 'ONNX Runtime rejects flavor-like versions' run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_normalize_version gpu'

assert_eq 'nvidia' "$(run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_resolve_profile auto 0x8086 0x1002 0x10de')" 'hybrid GPU auto selection resolves exactly one profile'
assert_eq 'amd' "$(run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_resolve_profile amd 0x10de 0x8086')" 'explicit profile takes precedence over detected GPUs'
assert_failure 'runtime profile rejects an invalid enum' run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_resolve_profile rocm 0x1002'
assert_eq $'TensorrtExecutionProvider\nCUDAExecutionProvider\nCPUExecutionProvider' "$(run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_expected_providers nvidia')" 'NVIDIA profile defines one ordered Provider set'
assert_success 'Intel profile accepts its exact accelerated Provider set' run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_validate_provider_set intel CPUExecutionProvider,OpenVINOExecutionProvider'
assert_failure 'profile rejects Provider libraries from another accelerator' run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_validate_provider_set intel CPUExecutionProvider,OpenVINOExecutionProvider,CUDAExecutionProvider'
assert_failure 'profile rejects a missing required Provider' run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_validate_provider_set nvidia CPUExecutionProvider,CUDAExecutionProvider'
assert_failure 'profile rejects duplicate Providers' run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_validate_provider_set cpu CPUExecutionProvider,CPUExecutionProvider'
fake_sysfs="$TEST_DIR/sys"
mkdir -p \
    "$fake_sysfs/class/drm/card0/device" \
    "$fake_sysfs/class/drm/card1/device" \
    "$fake_sysfs/class/drm/card2/device" \
    "$fake_sysfs/class/drm/card3/device"
printf '0x8086\n' > "$fake_sysfs/class/drm/card0/device/vendor"
printf '0x10de\n' > "$fake_sysfs/class/drm/card1/device/vendor"
printf '0x10de\n' > "$fake_sysfs/class/drm/card2/device/vendor"
printf '0x1022\n' > "$fake_sysfs/class/drm/card3/device/vendor"
assert_eq $'8086\n10de' "$(run_clean_shell "source scripts/deps.sh; ssv_onnxruntime_detect_gpu_vendors '$fake_sysfs'")" 'GPU vendor detection normalizes and deduplicates sysfs values'
assert_eq 'cpu' "$(run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_resolve_profile auto 0x1022')" 'AMD CPU vendor does not select the AMD GPU profile'

assert_failure 'managed root rejects repository root' run_clean_shell 'source scripts/deps.sh; ssv_deps_validate_root ROOT "$SSV_ROOT"'
assert_failure 'managed root rejects .deps root' run_clean_shell 'source scripts/deps.sh; ssv_deps_validate_root ROOT .deps'
assert_failure 'managed root rejects colon' run_clean_shell 'source scripts/deps.sh; ssv_deps_validate_root ROOT ".deps/a:b"'
assert_success 'managed root accepts ordinary spaces' run_clean_shell 'source scripts/deps.sh; ssv_deps_validate_root ROOT ".deps/a b"'

assert_failure 'system ONNX Runtime rejects managed version' run_clean_shell 'SSV_ONNXRUNTIME_SOURCE=system SSV_ONNXRUNTIME_VERSION=1.25.1; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_success 'Intel profile accepts the managed ONNX Runtime source-build workspace' run_clean_shell 'SSV_DEPS_PROFILE=intel SSV_ONNXRUNTIME_SOURCE=managed; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config; [ "$SSV_ONNXRUNTIME_ROOT" = "$SSV_ROOT/.deps/onnxruntime-openvino" ]'
assert_failure 'CPU profile rejects a managed GPU package' run_clean_shell 'SSV_DEPS_PROFILE=cpu SSV_ONNXRUNTIME_VERSION=1.25.1-gpu; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_failure 'NVIDIA profile rejects a managed CPU package' run_clean_shell 'SSV_DEPS_PROFILE=nvidia SSV_ONNXRUNTIME_VERSION=1.25.1; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_failure 'Intel profile rejects a managed GPU package' run_clean_shell 'SSV_DEPS_PROFILE=intel SSV_ONNXRUNTIME_VERSION=1.25.1-gpu; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_failure 'NVIDIA profile rejects disabled TensorRT' run_clean_shell 'SSV_DEPS_PROFILE=nvidia SSV_TENSORRT_MODE=disabled; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_failure_contains 'explicit local GPU profile fails clearly when its artifact is missing' 'local ONNX Runtime artifact not found:' run_clean_shell 'SSV_DEPS_PROFILE=intel SSV_ONNXRUNTIME_SOURCE=local SSV_ONNXRUNTIME_ROOT="$TEST_DIR/missing-ort"; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config; SSV_DEPS_BASE_PKG_CONFIG_PATH=""; SSV_DEPS_PKG_CONFIG_PATH=""; PKG_CONFIG_PATH=""; ssv_deps_prepare_onnxruntime'

fake_intel_ort="$TEST_DIR/fake-intel-ort"
build_fake_onnxruntime "$fake_intel_ort" 1.25.1 'CPUExecutionProvider,OpenVINOExecutionProvider' intel
cp "$fake_intel_ort/lib/libonnxruntime_providers_openvino.so" \
    "$fake_intel_ort/lib/libonnxruntime_providers_cuda.so"
assert_success 'local Intel artifact is probed through the public dependency interface' run_clean_shell "source '$ROOT/scripts/deps.sh'
mkdir -p '$TEST_DIR/local-intel-project'
SSV_ROOT='$TEST_DIR/local-intel-project'
SSV_BUILD_DIR='$TEST_DIR/local-intel-project/build'
SSV_DEPS_PROFILE=intel
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_intel_ort'
ssv_deps_capture_explicit_config
ssv_deps_resolve_config
SSV_DEPS_BASE_PKG_CONFIG_PATH=''
SSV_DEPS_PKG_CONFIG_PATH=''
PKG_CONFIG_PATH=''
ssv_deps_prepare_onnxruntime
[ \"\$SSV_DEPS_ONNXRUNTIME_VERSION\" = 1.25.1 ] || { printf 'version=%s\n' \"\$SSV_DEPS_ONNXRUNTIME_VERSION\" >&2; exit 1; }
[ \"\$SSV_DEPS_ONNXRUNTIME_PROVIDERS\" = CPUExecutionProvider,OpenVINOExecutionProvider ] || { printf 'providers=%s\n' \"\$SSV_DEPS_ONNXRUNTIME_PROVIDERS\" >&2; exit 1; }
[ \"\$SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS\" = '$fake_intel_ort/lib' ] || { printf 'runtime_dirs=%s\n' \"\$SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS\" >&2; exit 1; }
case \"\$SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES\" in
  '$fake_intel_ort/lib/libonnxruntime_providers_shared.so:$fake_intel_ort/lib/libonnxruntime_providers_openvino.so') ;;
  *) printf 'provider_libraries=%s\n' \"\$SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES\" >&2; exit 1 ;;
esac
case \"\$SSV_DEPS_ONNXRUNTIME_PCDIR\" in \"\$SSV_BUILD_DIR\"/ssv-deps/*/lib/pkgconfig) ;; *) printf 'pcdir=%s\n' \"\$SSV_DEPS_ONNXRUNTIME_PCDIR\" >&2; exit 1 ;; esac
[ ! -e '$fake_intel_ort/lib/pkgconfig/onnxruntime.pc' ] || { printf 'local artifact was modified\n' >&2; exit 1; }"

fake_versioned_provider_ort="$TEST_DIR/fake-versioned-provider-ort"
build_fake_onnxruntime "$fake_versioned_provider_ort" 1.25.1 'CPUExecutionProvider,OpenVINOExecutionProvider' intel
printf '%s\n' \
    'VERS_1.0 {' \
    '    global: GetProvider;' \
    '    local: *;' \
    '};' > "$fake_versioned_provider_ort/provider-version.map"
"${CXX:-c++}" -std=c++17 -fPIC -shared -I"$fake_versioned_provider_ort/include" \
    "$fake_versioned_provider_ort/provider-openvino.cpp" \
    -L"$fake_versioned_provider_ort/lib" -lonnxruntime -Wl,-rpath,'$ORIGIN' \
    -Wl,--version-script="$fake_versioned_provider_ort/provider-version.map" \
    -Wl,--strip-all \
    -o "$fake_versioned_provider_ort/lib/libonnxruntime_providers_openvino.so"
assert_success 'local profile accepts a versioned Provider entry point' run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/versioned-provider-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_versioned_provider_ort'
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_MODE=disabled
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare intel"

fake_bad_provider_ort="$TEST_DIR/fake-bad-provider-ort"
build_fake_onnxruntime "$fake_bad_provider_ort" 1.25.1 'CPUExecutionProvider,OpenVINOExecutionProvider' intel
printf 'not an ELF\n' > "$fake_bad_provider_ort/lib/libonnxruntime_providers_openvino.so"
assert_failure_contains 'local profile rejects a wrong Provider library' 'Provider library is not a readable ELF' run_clean_shell "source scripts/deps.sh
SSV_DEPS_PROFILE=intel
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_bad_provider_ort'
ssv_deps_capture_explicit_config
ssv_deps_resolve_config
SSV_DEPS_BASE_PKG_CONFIG_PATH=''
SSV_DEPS_PKG_CONFIG_PATH=''
PKG_CONFIG_PATH=''
ssv_deps_prepare_onnxruntime"

fake_provider_abi_ort="$TEST_DIR/fake-provider-abi-ort"
build_fake_onnxruntime "$fake_provider_abi_ort" 1.25.1 'CPUExecutionProvider,OpenVINOExecutionProvider' intel
printf '%s\n' \
    'extern "C" void ssv_missing_provider_symbol();' \
    'extern "C" void* GetProvider() { ssv_missing_provider_symbol(); return nullptr; }' \
    > "$fake_provider_abi_ort/broken-provider.cpp"
"${CXX:-c++}" -fPIC -shared "$fake_provider_abi_ort/broken-provider.cpp" \
    -o "$fake_provider_abi_ort/lib/libonnxruntime_providers_openvino.so"
assert_failure_contains 'local profile rejects a Provider ABI mismatch' 'unresolved runtime symbols' run_clean_shell "source scripts/deps.sh
SSV_DEPS_PROFILE=intel
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_provider_abi_ort'
ssv_deps_capture_explicit_config
ssv_deps_resolve_config
SSV_DEPS_BASE_PKG_CONFIG_PATH=''
SSV_DEPS_PKG_CONFIG_PATH=''
PKG_CONFIG_PATH=''
ssv_deps_prepare_onnxruntime"

fake_abi_mismatch_ort="$TEST_DIR/fake-abi-mismatch-ort"
build_fake_onnxruntime "$fake_abi_mismatch_ort" 1.24.0 'CPUExecutionProvider,OpenVINOExecutionProvider' intel
printf '1.25.1\n' > "$fake_abi_mismatch_ort/VERSION_NUMBER"
assert_failure_contains 'local profile rejects an ORT runtime ABI mismatch' 'ONNX Runtime ABI mismatch: expected 1.25.1, got 1.24.0' run_clean_shell "source scripts/deps.sh
SSV_DEPS_PROFILE=intel
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_abi_mismatch_ort'
ssv_deps_capture_explicit_config
ssv_deps_resolve_config
SSV_DEPS_BASE_PKG_CONFIG_PATH=''
SSV_DEPS_PKG_CONFIG_PATH=''
PKG_CONFIG_PATH=''
ssv_deps_prepare_onnxruntime"

fake_mixed_provider_ort="$TEST_DIR/fake-mixed-provider-ort"
build_fake_onnxruntime "$fake_mixed_provider_ort" 1.25.1 'CPUExecutionProvider,CUDAExecutionProvider,OpenVINOExecutionProvider' intel
assert_failure_contains 'local profile rejects a mixed ORT Provider build' 'intel profile must not activate Provider: CUDAExecutionProvider' run_clean_shell "source scripts/deps.sh
SSV_DEPS_PROFILE=intel
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_mixed_provider_ort'
ssv_deps_capture_explicit_config
ssv_deps_resolve_config
SSV_DEPS_BASE_PKG_CONFIG_PATH=''
SSV_DEPS_PKG_CONFIG_PATH=''
PKG_CONFIG_PATH=''
ssv_deps_prepare_onnxruntime"

fake_intel_pc="$TEST_DIR/fake-intel-pc"
mkdir -p "$fake_intel_pc"
printf '%s\n' \
    "prefix=$fake_intel_ort" \
    "libdir=$fake_intel_ort/lib" \
    "includedir=$fake_intel_ort/include" \
    'Name: onnxruntime' \
    'Description: fake system ONNX Runtime' \
    'Version: 1.25.1' \
    'Libs: -L${libdir} -lonnxruntime' \
    'Cflags: -I${includedir}' \
    > "$fake_intel_pc/onnxruntime.pc"
system_intel_result="$(run_clean_shell "source scripts/deps.sh
SSV_DEPS_PROFILE=intel
SSV_ONNXRUNTIME_SOURCE=system
export PKG_CONFIG_PATH='$fake_intel_pc'
ssv_deps_resolve_config
SSV_DEPS_BASE_PKG_CONFIG_PATH=\"\$PKG_CONFIG_PATH\"
SSV_DEPS_PKG_CONFIG_PATH=''
ssv_deps_prepare_onnxruntime
printf '%s|%s|%s' \"\$SSV_DEPS_ONNXRUNTIME_SOURCE\" \"\$SSV_DEPS_ONNXRUNTIME_VERSION\" \"\$SSV_DEPS_ONNXRUNTIME_PROVIDERS\"")"
assert_eq 'system|1.25.1|CPUExecutionProvider,OpenVINOExecutionProvider' "$system_intel_result" 'system Intel artifact uses the same strict probe interface'

fake_managed_cpu_ort="$TEST_DIR/fake-managed-cpu-ort"
build_fake_onnxruntime "$fake_managed_cpu_ort" 1.25.1 CPUExecutionProvider cpu
managed_cpu_result="$(run_clean_shell "source scripts/deps.sh; ssv_onnxruntime_managed_validate '$fake_managed_cpu_ort' 1.25.1 cpu")"
case "$managed_cpu_result" in
    *'providers=CPUExecutionProvider'*) pass 'managed CPU artifact uses the profile probe' ;;
    *) printf '%s\n' "$managed_cpu_result" >&2; fail 'managed CPU artifact uses the profile probe' ;;
esac
assert_success 'managed CPU result supports an empty Provider library list' run_clean_shell "SSV_DEPS_PROFILE=cpu
SSV_ONNXRUNTIME_ROOT='$fake_managed_cpu_ort'
source scripts/deps.sh
ssv_deps_capture_explicit_config
ssv_deps_resolve_config
SSV_DEPS_BASE_PKG_CONFIG_PATH=''
SSV_DEPS_PKG_CONFIG_PATH=''
PKG_CONFIG_PATH=''
ssv_deps_prepare_onnxruntime
[ \"\$SSV_DEPS_ONNXRUNTIME_PROVIDERS\" = CPUExecutionProvider ]
[ -z \"\$SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES\" ]"

fake_cached_wrong_abi_ort="$TEST_DIR/fake-cached-wrong-abi-ort"
build_fake_onnxruntime "$fake_cached_wrong_abi_ort" 1.24.0 CPUExecutionProvider cpu
printf '1.25.1\n' > "$fake_cached_wrong_abi_ort/VERSION_NUMBER"
managed_cache_project="$TEST_DIR/managed-cache-validation-project"
managed_cache_dir="$managed_cache_project/.deps/downloads/onnxruntime/1.25.1"
managed_cache_file="$managed_cache_dir/onnxruntime-linux-x64-1.25.1.tgz"
mkdir -p "$managed_cache_dir"
tar -C "$TEST_DIR" -czf "$managed_cache_file" "$(basename -- "$fake_cached_wrong_abi_ort")"
managed_cache_fake_bin="$TEST_DIR/managed-cache-fake-bin"
managed_cache_curl_count="$TEST_DIR/managed-cache-curl-count"
mkdir -p "$managed_cache_fake_bin"
printf '%s\n' \
    '#!/usr/bin/env bash' \
    'printf "called\n" > "${FAKE_CURL_COUNT:?}"' \
    'exit 1' > "$managed_cache_fake_bin/curl"
chmod +x "$managed_cache_fake_bin/curl"
managed_validation_retries_bad_cache_test() {
    local output
    if output="$(PATH="$managed_cache_fake_bin:$PATH" \
        FAKE_CURL_COUNT="$managed_cache_curl_count" \
        run_clean_shell "source scripts/deps.sh
SSV_ROOT='$managed_cache_project'
SSV_BUILD_DIR='${managed_cache_project}/build'
SSV_ONNXRUNTIME_SOURCE=managed
SSV_ONNXRUNTIME_ROOT='${managed_cache_project}/.deps/onnxruntime'
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_MODE=disabled
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare cpu" 2>&1)"; then
        printf 'managed artifact with the wrong ABI unexpectedly passed\n' >&2
        return 1
    fi
    grep -Fq 'ONNX Runtime ABI mismatch: expected 1.25.1, got 1.24.0' <<< "$output" || {
        printf '%s\n' "$output" >&2
        return 1
    }
    [ ! -e "$managed_cache_file" ] || {
        printf 'managed cache was preserved after an artifact validation failure\n' >&2
        return 1
    }
    [ -s "$managed_cache_curl_count" ] || {
        printf 'managed artifact validation did not retry the download\n' >&2
        return 1
    }
}
assert_success 'managed artifact validation failure removes and retries the bad cache' \
    managed_validation_retries_bad_cache_test

assert_success 'dependency snapshot exposes one compact resolved interface' run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/compact-snapshot-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_managed_cpu_ort'
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_MODE=disabled
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare cpu >/dev/null
snapshot=\"\$SSV_BUILD_DIR/ssv-deps.env.pending\"
actual=\"\$(sed 's/=.*//' \"\$snapshot\")\"
expected='SSV_DEPS_SIGNATURE
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
SSV_DEPS_TENSORRT_RUNTIME_DIRS'
[ \"\$actual\" = \"\$expected\" ] || {
    printf 'snapshot keys:\n%s\n' \"\$actual\" >&2
    exit 1
}
[ \"\$(sed -n 's/^SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS=//p' \"\$snapshot\")\" = '$fake_managed_cpu_ort/lib' ]
case \"\$(sed -n 's/^SSV_DEPS_ONNXRUNTIME_PCDIR=//p' \"\$snapshot\")\" in
    \"\$SSV_BUILD_DIR\"/ssv-deps/*/lib/pkgconfig) ;;
    *) exit 1 ;;
esac
[ \"\$(sed -n 's/^SSV_DEPS_TENSORRT_MODE=//p' \"\$snapshot\")\" = disabled ]"

assert_success 'TensorRT snapshot mode is resolved rather than requested' run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/resolved-mode-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_managed_cpu_ort'
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_MODE=auto
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare cpu >/dev/null
snapshot=\"\$SSV_BUILD_DIR/ssv-deps.env.pending\"
[ \"\$(sed -n 's/^SSV_DEPS_TENSORRT_MODE=//p' \"\$snapshot\")\" = disabled ]"

assert_failure_contains 'runtime profile owns the fixed ONNX Runtime version' \
    'SSV_ONNXRUNTIME_VERSION is no longer configurable; select the runtime artifact with --profile' \
    run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/fixed-ort-version-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_ONNXRUNTIME_SOURCE=managed
SSV_ONNXRUNTIME_ROOT='$fake_managed_cpu_ort'
SSV_ONNXRUNTIME_VERSION=1.25.1
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_MODE=disabled
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare cpu"

fake_managed_nvidia_ort="$TEST_DIR/fake-managed-nvidia-ort"
build_fake_onnxruntime "$fake_managed_nvidia_ort" 1.25.1 'CPUExecutionProvider,CUDAExecutionProvider,TensorrtExecutionProvider' nvidia
managed_nvidia_result="$(run_clean_shell "source scripts/deps.sh; ssv_onnxruntime_managed_validate '$fake_managed_nvidia_ort' 1.25.1-gpu nvidia")"
case "$managed_nvidia_result" in
    *'providers=CPUExecutionProvider,CUDAExecutionProvider,TensorrtExecutionProvider'*) pass 'managed NVIDIA artifact requires CUDA and TensorRT Providers' ;;
    *) printf '%s\n' "$managed_nvidia_result" >&2; fail 'managed NVIDIA artifact requires CUDA and TensorRT Providers' ;;
esac

managed_intel_result="$(run_clean_shell "source scripts/deps.sh; ssv_onnxruntime_managed_validate '$fake_intel_ort' 1.25.1 intel")"
case "$managed_intel_result" in
    *'providers=CPUExecutionProvider,OpenVINOExecutionProvider'*) pass 'managed Intel artifact uses the profile probe' ;;
    *) printf '%s\n' "$managed_intel_result" >&2; fail 'managed Intel artifact uses the profile probe' ;;
esac

fake_amd_ort="$TEST_DIR/fake-amd-ort"
build_fake_onnxruntime "$fake_amd_ort" 1.25.1 'CPUExecutionProvider,MIGraphXExecutionProvider' amd
assert_success 'local AMD artifact requires the MIGraphX Provider' run_clean_shell "source scripts/deps.sh
SSV_DEPS_PROFILE=amd
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_amd_ort'
ssv_deps_capture_explicit_config
ssv_deps_resolve_config
SSV_DEPS_BASE_PKG_CONFIG_PATH=''
SSV_DEPS_PKG_CONFIG_PATH=''
PKG_CONFIG_PATH=''
ssv_deps_prepare_onnxruntime
[ \"\$SSV_DEPS_ONNXRUNTIME_PROVIDERS\" = CPUExecutionProvider,MIGraphXExecutionProvider ]"
assert_failure 'disabled OpenCV rejects source' run_clean_shell 'SSV_OPENCV_MODE=disabled SSV_OPENCV_SOURCE=managed; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_success 'local OpenCV source accepts explicit include and library paths' run_clean_shell 'local_include="$TEST_DIR/local/include"; local_lib="$TEST_DIR/local/lib"; mkdir -p "$local_include" "$local_lib"; SSV_OPENCV_SOURCE=local SSV_OPENCV_INCLUDE_DIR="$local_include" SSV_OPENCV_LIB_DIR="$local_lib"; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config; [ "$SSV_OPENCV_SOURCE" = local ]'
assert_failure 'local OpenCV requires an include path' run_clean_shell 'SSV_OPENCV_SOURCE=local SSV_OPENCV_LIB_DIR="$TEST_DIR/local/lib"; mkdir -p "$SSV_OPENCV_LIB_DIR"; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_failure 'local OpenCV requires a library path' run_clean_shell 'SSV_OPENCV_SOURCE=local SSV_OPENCV_INCLUDE_DIR="$TEST_DIR/local/include"; mkdir -p "$SSV_OPENCV_INCLUDE_DIR"; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_success 'local OpenCV accepts the shared workspace root' run_clean_shell 'SSV_OPENCV_SOURCE=local SSV_OPENCV_ROOT=.deps/opencv-custom SSV_OPENCV_INCLUDE_DIR="$TEST_DIR/include" SSV_OPENCV_LIB_DIR="$TEST_DIR/lib"; mkdir -p "$SSV_OPENCV_INCLUDE_DIR" "$SSV_OPENCV_LIB_DIR"; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config; [ "$SSV_OPENCV_ROOT" = "$SSV_ROOT/.deps/opencv-custom" ]'
assert_failure 'managed OpenCV rejects local include path' run_clean_shell 'SSV_OPENCV_SOURCE=managed SSV_OPENCV_INCLUDE_DIR="$TEST_DIR/include"; mkdir -p "$SSV_OPENCV_INCLUDE_DIR"; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_failure 'disabled TensorRT rejects source' run_clean_shell 'SSV_TENSORRT_MODE=disabled SSV_TENSORRT_SOURCE=managed; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_failure 'TensorRT rejects archive and URL together' run_clean_shell 'source scripts/deps.sh; SSV_TENSORRT_ARCHIVE=/tmp/a.tar; SSV_TENSORRT_URL=https://example.invalid/a.tar; ssv_deps_resolve_config'
assert_success 'TensorRT accepts a direct HTTPS URL' run_clean_shell 'SSV_TENSORRT_MODE=enabled SSV_TENSORRT_URL=https://example.invalid/a.tar; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'

assert_eq '/opt/a:/opt/b' "$(run_clean_shell 'source scripts/deps.sh; ssv_deps_join_unique "/opt/a:/opt/b:/opt/a"')" 'runtime path deduplicates in first-seen order'
assert_eq "$TEST_DIR/custom-lib" "$(run_clean_shell 'source scripts/deps.sh; mkdir -p "$TEST_DIR/custom-lib"; ssv_deps_runtime_dirs /usr/lib "$TEST_DIR/custom-lib"')" 'system runtime directories are filtered'

provider_result="$(run_clean_shell 'source scripts/deps.sh; ssv_deps_parse_provider_result X $'\''version=1.2.3\npkgconfig_dir=/tmp/pc\nruntime_dirs=/tmp/lib'\''')"
assert_eq $'1.2.3\n/tmp/pc\n/tmp/lib' "$provider_result" 'provider result protocol has exactly three fields'
assert_failure 'provider result rejects extra fields' run_clean_shell 'source scripts/deps.sh; ssv_deps_parse_provider_result X $'\''version=1\npkgconfig_dir=/p\nruntime_dirs=/l\nextra=x'\'''

assert_success 'TensorRT auto without SDK resolves to stub' run_clean_shell 'source scripts/deps.sh; SSV_TENSORRT_MODE=auto; SSV_TENSORRT_SOURCE=managed; SSV_TENSORRT_ROOT="$TEST_DIR/no-sdk"; ssv_deps_prepare_tensorrt; [ "$SSV_DEPS_TENSORRT_MODE" = disabled ]'
assert_success 'TensorRT disabled resolves to stub' run_clean_shell 'source scripts/deps.sh; SSV_TENSORRT_MODE=disabled; ssv_deps_resolve_config; ssv_deps_prepare_tensorrt; [ "$SSV_DEPS_TENSORRT_MODE" = disabled ]'

build_fake_local_opencv() {
    local sdk="$1" version="$2" module
    mkdir -p "$sdk/include/opencv4/opencv2" "$sdk/lib"
    printf '%s\n' \
        '#pragma once' \
        '#include <string>' \
        "#define CV_VERSION \"$version\"" \
        'namespace cv { std::string getVersionString(); }' \
        > "$sdk/include/opencv4/opencv2/core.hpp"
    printf '%s\n' \
        '#include <opencv2/core.hpp>' \
        "namespace cv { std::string getVersionString() { return \"$version\"; } }" \
        > "$sdk/core.cpp"
    "${CXX:-c++}" -std=c++17 -fPIC -shared \
        -I"$sdk/include/opencv4" "$sdk/core.cpp" -o "$sdk/lib/libopencv_core.so"
    for module in imgproc video calib3d features2d flann dnn; do
        ln -s libopencv_core.so "$sdk/lib/libopencv_${module}.so"
    done
}

fake_local_opencv="$TEST_DIR/local-opencv-sdk"
build_fake_local_opencv "$fake_local_opencv" 4.10.0

fake_managed_workspace="$TEST_DIR/managed-opencv-project/.deps/opencv"
fake_source_dir="$fake_managed_workspace/source/opencv-4.10.0"
mkdir -p "$fake_source_dir" "$fake_managed_workspace/local/source" "$fake_managed_workspace/managed"
: > "$fake_source_dir/CMakeLists.txt"
printf 'keep local source\n' > "$fake_managed_workspace/local/source/.ssv-test-sentinel"
printf 'keep legacy managed\n' > "$fake_managed_workspace/managed/.ssv-test-sentinel"
fake_cmake_bin="$TEST_DIR/fake-cmake-bin"
mkdir -p "$fake_cmake_bin"
printf '%s\n' \
    '#!/usr/bin/env bash' \
    'set -euo pipefail' \
    'build_dir=""' \
    'install_dir=""' \
    'for ((index = 1; index <= $#; index++)); do' \
    '    arg="${!index}"' \
    '    case "$arg" in' \
    '        -B) next=$((index + 1)); build_dir="${!next}" ;;' \
    '        -DCMAKE_INSTALL_PREFIX=*) install_dir="${arg#*=}" ;;' \
    '    esac' \
    'done' \
    'if [[ "${1:-}" == --build ]]; then exit 0; fi' \
    'if [[ "${1:-}" == --install ]]; then' \
    '    build_dir="${2:?}"' \
    '    install_dir="$(<"$build_dir/.install-prefix")"' \
    '    for ((index = 3; index <= $#; index++)); do' \
    '        arg="${!index}"' \
    '        [[ "$arg" == --prefix ]] || continue' \
    '        next=$((index + 1)); install_dir="${!next}"' \
    '    done' \
    '    mkdir -p "$install_dir"' \
    '    cp -a "${FAKE_OPENCV_SDK:?}/include" "$install_dir/"' \
    '    cp -a "${FAKE_OPENCV_SDK:?}/lib" "$install_dir/"' \
    '    exit 0' \
    'fi' \
    'mkdir -p "$build_dir"' \
    'printf "%s\\n" "$install_dir" > "$build_dir/.install-prefix"' \
    > "$fake_cmake_bin/cmake"
chmod +x "$fake_cmake_bin/cmake"
assert_success 'managed OpenCV builds into source/build/install workspace directories' run_clean_shell "export PATH='$fake_cmake_bin':\$PATH
export FAKE_OPENCV_SDK='$fake_local_opencv'
source scripts/deps.sh
result=\"\$(ssv_opencv_managed_prepare '$fake_managed_workspace' 4.10.0)\"
case \"\$result\" in *'pkgconfig_dir=$fake_managed_workspace/install/lib/pkgconfig'*) ;; *) exit 1 ;; esac
[ -f '$fake_managed_workspace/install/lib/pkgconfig/opencv4.pc' ]
[ -f '$fake_managed_workspace/build/.install-prefix' ]
grep -Fqx 'keep local source' '$fake_managed_workspace/local/source/.ssv-test-sentinel'
grep -Fqx 'keep legacy managed' '$fake_managed_workspace/managed/.ssv-test-sentinel'"

fake_invalid_workspace="$TEST_DIR/managed-opencv-invalid/.deps/opencv"
mkdir -p "$fake_invalid_workspace/source/opencv-4.10.0" "$fake_invalid_workspace/install"
: > "$fake_invalid_workspace/source/opencv-4.10.0/CMakeLists.txt"
cp -a "$fake_local_opencv/include" "$fake_invalid_workspace/install/"
cp -a "$fake_local_opencv/lib" "$fake_invalid_workspace/install/"
rm -f "$fake_invalid_workspace/install/lib/libopencv_dnn.so"
printf 'broken\n' > "$fake_invalid_workspace/install/lib/libopencv_dnn.so"
printf 'keep invalid install\n' > "$fake_invalid_workspace/install/.ssv-test-sentinel"
fake_failing_cmake_bin="$TEST_DIR/fake-failing-cmake-bin"
mkdir -p "$fake_failing_cmake_bin"
printf '%s\n' '#!/usr/bin/env bash' 'exit 1' > "$fake_failing_cmake_bin/cmake"
chmod +x "$fake_failing_cmake_bin/cmake"
assert_failure 'managed OpenCV preserves an invalid install when candidate configuration fails' run_clean_shell "export PATH='$fake_failing_cmake_bin':$PATH
source scripts/deps.sh
if ssv_opencv_managed_prepare '$fake_invalid_workspace' 4.10.0; then exit 1; fi
grep -Fqx 'keep invalid install' '$fake_invalid_workspace/install/.ssv-test-sentinel'
exit 1"

assert_success 'local OpenCV provider generates and probes a private pkg-config package' run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/local-provider-project'
mkdir -p \"\$SSV_ROOT/.deps/opencv/local/source\"
printf 'keep source\n' > \"\$SSV_ROOT/.deps/opencv/local/source/.ssv-test-sentinel\"
result=\"\$(ssv_opencv_local_prepare '$fake_local_opencv/include' '$fake_local_opencv/lib' 4.10.0)\"
case \"\$result\" in *'version=4.10.0'*) ;; *) exit 1 ;; esac
pc=\"\$SSV_ROOT/.deps/opencv/local/lib/pkgconfig/opencv4.pc\"
grep -Fqx 'includedir=$fake_local_opencv/include/opencv4' \"\$pc\"
grep -Fqx 'libdir=$fake_local_opencv/lib' \"\$pc\"
grep -Fqx 'keep source' \"\$SSV_ROOT/.deps/opencv/local/source/.ssv-test-sentinel\"
[ \"\$(find \"\$SSV_ROOT/.deps\" -mindepth 1 -maxdepth 1 -type d -name '*opencv*' -printf '%f\\n')\" = opencv ]"
assert_success 'local OpenCV snapshot records normalized local paths' run_clean_shell "source scripts/deps.sh
SSV_OPENCV_SOURCE=local
SSV_OPENCV_INCLUDE_DIR='$fake_local_opencv/include'
SSV_OPENCV_LIB_DIR='$fake_local_opencv/lib'
ssv_deps_resolve_config
SSV_DEPS_PKG_CONFIG_PATH=''
SSV_DEPS_BASE_PKG_CONFIG_PATH=''
PKG_CONFIG_PATH=''
ssv_deps_prepare_opencv
[ \"\$SSV_DEPS_OPENCV_INCLUDE_DIR\" = '$fake_local_opencv/include/opencv4' ]
[ \"\$SSV_DEPS_OPENCV_LIB_DIR\" = '$fake_local_opencv/lib' ]"

fake_local_opencv_spaces="$TEST_DIR/local opencv sdk"
build_fake_local_opencv "$fake_local_opencv_spaces" 4.10.0
assert_success 'local OpenCV provider supports include and library paths with spaces' run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/local-provider-spaces'
ssv_opencv_local_prepare '$fake_local_opencv_spaces/include' '$fake_local_opencv_spaces/lib' 4.10.0"

fake_broken_opencv="$TEST_DIR/local-opencv-broken"
build_fake_local_opencv "$fake_broken_opencv" 4.10.0
printf 'not an ELF\n' > "$fake_broken_opencv/lib/libopencv_dnn.so"
assert_failure 'local OpenCV provider rejects an unreadable required library' run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/local-provider-broken'
ssv_opencv_local_prepare '$fake_broken_opencv/include' '$fake_broken_opencv/lib' 4.10.0"

fake_unresolved_opencv="$TEST_DIR/local-opencv-unresolved"
build_fake_local_opencv "$fake_unresolved_opencv" 4.10.0
printf '%s\n' 'extern "C" void ssv_missing_symbol();' 'extern "C" void ssv_call_missing_symbol() { ssv_missing_symbol(); }' \
    > "$fake_unresolved_opencv/unresolved.cpp"
"${CXX:-c++}" -fPIC -shared "$fake_unresolved_opencv/unresolved.cpp" \
    -o "$fake_unresolved_opencv/lib/libopencv_dnn.so"
assert_failure 'local OpenCV provider rejects an unresolved runtime symbol' run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/local-provider-unresolved'
ssv_opencv_local_prepare '$fake_unresolved_opencv/include' '$fake_unresolved_opencv/lib' 4.10.0"

fake_wrong_opencv="$TEST_DIR/local-opencv-wrong-version"
build_fake_local_opencv "$fake_wrong_opencv" 4.9.0
assert_failure 'local OpenCV provider rejects a runtime version other than 4.10.0' run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/local-provider-wrong-version'
ssv_opencv_local_prepare '$fake_wrong_opencv/include/opencv4' '$fake_wrong_opencv/lib' 4.10.0"

fake_build_bin="$TEST_DIR/fake-build-bin"
mkdir -p "$fake_build_bin"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$fake_build_bin/git"
chmod +x "$fake_build_bin/git"
assert_failure 'managed roots with spaces fail before download' run_clean_shell "PATH='$fake_build_bin':\$PATH SSV_ONNXRUNTIME_ROOT='$TEST_DIR/onnx runtime' SSV_ONNXRUNTIME_SOURCE=system; source scripts/deps.sh; ssv_deps_prepare"

build_fake_tensorrt() {
    local sdk="$1" trt_major="$2" trt_minor="$3" trt_patch="$4" cuda_version="$5"
    mkdir -p "$sdk/include" "$sdk/lib" "$sdk/cuda/include" "$sdk/cuda/lib" "$sdk/lib/pkgconfig"
    printf '%s\n' \
        '#pragma once' \
        '#include <NvInferVersion.h>' \
        'extern "C" int getInferLibVersion();' \
        > "$sdk/include/NvInfer.h"
    printf '%s\n' \
        "#define NV_TENSORRT_MAJOR $trt_major" \
        "#define NV_TENSORRT_MINOR $trt_minor" \
        "#define NV_TENSORRT_PATCH $trt_patch" > "$sdk/include/NvInferVersion.h"
    printf '%s\n' \
        '#pragma once' \
        "#define CUDART_VERSION $cuda_version" \
        'typedef int cudaError_t;' \
        'enum { cudaSuccess = 0 };' \
        'extern "C" cudaError_t cudaRuntimeGetVersion(int *version);' \
        > "$sdk/cuda/include/cuda_runtime_api.h"
    printf '%s\n' \
        "extern \"C\" int getInferLibVersion() { return $((trt_major * 10000 + trt_minor * 100 + trt_patch)); }" \
        > "$sdk/nvinfer.cpp"
    printf '%s\n' \
        '#include <cuda_runtime_api.h>' \
        'extern "C" cudaError_t cudaRuntimeGetVersion(int *version) {' \
        "    *version = $cuda_version;" \
        '    return cudaSuccess;' \
        '}' \
        > "$sdk/cudart.cpp"
    printf '%s\n' \
        'extern "C" int ssv_fake_nvonnxparser() { return 1; }' \
        > "$sdk/nvonnxparser.cpp"
    printf '%s\n' \
        'extern "C" int ssv_fake_cudnn() { return 1; }' \
        > "$sdk/cudnn.cpp"
    "${CXX:-c++}" -fPIC -shared "$sdk/nvinfer.cpp" \
        -Wl,-soname,"libnvinfer.so.$trt_major" \
        -o "$sdk/lib/libnvinfer.so.$trt_major"
    ln -s "libnvinfer.so.$trt_major" "$sdk/lib/libnvinfer.so"
    "${CXX:-c++}" -fPIC -shared "$sdk/nvonnxparser.cpp" \
        -Wl,-soname,"libnvonnxparser.so.$trt_major" \
        -o "$sdk/lib/libnvonnxparser.so.$trt_major"
    "${CXX:-c++}" -fPIC -shared "$sdk/cudnn.cpp" \
        -Wl,-soname,libcudnn.so.9 \
        -o "$sdk/lib/libcudnn.so.9"
    "${CXX:-c++}" -fPIC -shared -I"$sdk/cuda/include" \
        "$sdk/cudart.cpp" -Wl,-soname,"libcudart.so.$((cuda_version / 1000))" \
        -o "$sdk/cuda/lib/libcudart.so.$((cuda_version / 1000))"
    ln -s "libcudart.so.$((cuda_version / 1000))" "$sdk/cuda/lib/libcudart.so"
}

fake_tensorrt="$TEST_DIR/fake-tensorrt"
build_fake_tensorrt "$fake_tensorrt" 10 16 1 13020
assert_eq '10.16.1' "$(run_clean_shell "source scripts/deps.sh; ssv_tensorrt_version '$fake_tensorrt/include/NvInferVersion.h'")" 'TensorRT version comes from NvInferVersion.h'

fake_aarch64_bin="$TEST_DIR/fake-aarch64-bin"
mkdir -p "$fake_aarch64_bin"
printf '%s\n' \
    '#!/usr/bin/env bash' \
    'if [ "${1:-}" = -m ]; then printf "aarch64\\n"; else /usr/bin/uname "$@"; fi' \
    > "$fake_aarch64_bin/uname"
chmod +x "$fake_aarch64_bin/uname"
assert_failure_contains_without_pattern 'managed NVIDIA packages reject non-x86_64 precisely' \
    'managed NVIDIA runtime supports Linux x86_64 only' \
    'TensorRT SDK root not found' \
    run_clean_shell "export PATH='$fake_aarch64_bin':\$PATH
source scripts/deps.sh
SSV_ROOT='$TEST_DIR/non-x86-tensorrt-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_SOURCE=managed
SSV_TENSORRT_ROOT='${TEST_DIR}/non-x86-tensorrt-target'
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare nvidia"
assert_failure_contains_without_pattern 'managed NVIDIA ONNX Runtime rejects non-x86_64 precisely' \
    'managed NVIDIA ONNX Runtime supports Linux x86_64 only' \
    '\[SSV\] downloading|unbound variable' \
    run_clean_shell "export PATH='$fake_aarch64_bin':\$PATH
source scripts/deps.sh
SSV_ROOT='$TEST_DIR/non-x86-onnxruntime-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_ONNXRUNTIME_SOURCE=managed
SSV_ONNXRUNTIME_ROOT='${TEST_DIR}/non-x86-onnxruntime-target'
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_SOURCE=managed
SSV_TENSORRT_MODE=enabled
SSV_TENSORRT_ROOT='$fake_tensorrt'
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare nvidia"

fake_enterprise_version="$TEST_DIR/NvInferVersion-enterprise.h"
printf '%s\n' \
    '#define TRT_MAJOR_ENTERPRISE 11' \
    '#define TRT_MINOR_ENTERPRISE 1' \
    '#define TRT_PATCH_ENTERPRISE 0' \
    '#define NV_TENSORRT_MAJOR TRT_MAJOR_ENTERPRISE' \
    '#define NV_TENSORRT_MINOR TRT_MINOR_ENTERPRISE' \
    '#define NV_TENSORRT_PATCH TRT_PATCH_ENTERPRISE' > "$fake_enterprise_version"
assert_eq '11.1.0' "$(run_clean_shell "source scripts/deps.sh; ssv_tensorrt_version '$fake_enterprise_version'")" 'TensorRT version resolves Enterprise header aliases'

fake_cuda_link="$TEST_DIR/fake-cuda-link"
ln -s "$fake_tensorrt/cuda" "$fake_cuda_link"
assert_success 'TensorRT follows a symlinked CUDA_HOME' run_clean_shell "CUDA_HOME='$fake_cuda_link'; source scripts/deps.sh; ssv_tensorrt_locate_cuda '$fake_tensorrt'"

assert_success 'public dependency prepare snapshots resolved library paths' run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/resolved-library-path-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_managed_cpu_ort'
SSV_OPENCV_SOURCE=local
SSV_OPENCV_INCLUDE_DIR='$fake_local_opencv/include'
SSV_OPENCV_LIB_DIR='$fake_local_opencv/lib'
SSV_TENSORRT_SOURCE=managed
SSV_TENSORRT_MODE=enabled
SSV_TENSORRT_ROOT='$fake_tensorrt'
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare cpu >/dev/null
snapshot=\"\$SSV_BUILD_DIR/ssv-deps.env.pending\"
opencv_pc=\"\$(sed -n 's/^SSV_DEPS_OPENCV_PCDIR=//p' \"\$snapshot\")\"
opencv_runtime=\"\$(sed -n 's/^SSV_DEPS_OPENCV_RUNTIME_DIRS=//p' \"\$snapshot\")\"
tensorrt_pc=\"\$(sed -n 's/^SSV_DEPS_TENSORRT_PCDIR=//p' \"\$snapshot\")\"
tensorrt_runtime=\"\$(sed -n 's/^SSV_DEPS_TENSORRT_RUNTIME_DIRS=//p' \"\$snapshot\")\"
[ \"\$opencv_pc\" = \"\$SSV_ROOT/.deps/opencv/local/lib/pkgconfig\" ] || {
    printf 'opencv pcdir=%s\n' \"\$opencv_pc\" >&2
    exit 1
}
[ \"\$opencv_runtime\" = '$fake_local_opencv/lib' ] || {
    printf 'opencv runtime_dirs=%s\n' \"\$opencv_runtime\" >&2
    exit 1
}
[ \"\$tensorrt_pc\" = '$fake_tensorrt/lib/pkgconfig' ] || {
    printf 'tensorrt pcdir=%s\n' \"\$tensorrt_pc\" >&2
    exit 1
}
[ \"\$tensorrt_runtime\" = '$fake_tensorrt/lib:$fake_tensorrt/cuda/lib' ] || {
    printf 'tensorrt runtime_dirs=%s\n' \"\$tensorrt_runtime\" >&2
    exit 1
}"

assert_success 'NVIDIA dependency prepare closes the TensorRT CUDA and cuDNN runtime set' run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/nvidia-closure-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_managed_nvidia_ort'
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_SOURCE=managed
SSV_TENSORRT_MODE=enabled
SSV_TENSORRT_ROOT='$fake_tensorrt'
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare nvidia >/dev/null
snapshot=\"\$SSV_BUILD_DIR/ssv-deps.env.pending\"
[ \"\$(sed -n 's/^SSV_DEPS_ONNXRUNTIME_PROVIDERS=//p' \"\$snapshot\")\" = CPUExecutionProvider,CUDAExecutionProvider,TensorrtExecutionProvider ]
[ \"\$(sed -n 's/^SSV_DEPS_TENSORRT_MODE=//p' \"\$snapshot\")\" = enabled ]
runtime=\"\$(sed -n 's/^SSV_DEPS_TENSORRT_RUNTIME_DIRS=//p' \"\$snapshot\")\"
[ \"\$runtime\" = '$fake_tensorrt/lib:$fake_tensorrt/cuda/lib' ]"

fake_tensorrt_11="$TEST_DIR/fake-tensorrt-11"
build_fake_tensorrt "$fake_tensorrt_11" 11 1 0 13020
printf '%s\n' \
    "prefix=$fake_tensorrt_11" \
    'libdir=${prefix}/lib' \
    'includedir=${prefix}/include' \
    'cudalibdir=${prefix}/cuda/lib' \
    'cudaincludedir=${prefix}/cuda/include' \
    'Name: nvinfer' \
    'Description: incompatible fake TensorRT' \
    'Version: 11.1.0' \
    'Libs: -L${libdir} -lnvinfer -L${cudalibdir} -lcudart' \
    'Cflags: -I${includedir} -I${cudaincludedir}' \
    > "$fake_tensorrt_11/lib/pkgconfig/nvinfer.pc"
assert_failure_contains 'public dependency prepare rejects TensorRT ABI 11' \
    'NVIDIA runtime requires TensorRT ABI major 10, got 11.1.0' \
    run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/incompatible-tensorrt-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_managed_cpu_ort'
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_SOURCE=system
SSV_TENSORRT_MODE=enabled
export PKG_CONFIG_PATH='$fake_tensorrt_11/lib/pkgconfig'
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare cpu"

fake_tensorrt_cuda12="$TEST_DIR/fake-tensorrt-cuda12"
fake_tensorrt_cuda12_archive="$TEST_DIR/fake-tensorrt-cuda12.tgz"
build_fake_tensorrt "$fake_tensorrt_cuda12" 10 16 1 12090
tar -C "$TEST_DIR" -czf "$fake_tensorrt_cuda12_archive" "$(basename -- "$fake_tensorrt_cuda12")"
assert_failure_contains 'public dependency prepare rejects CUDA ABI 12' \
    'NVIDIA runtime requires CUDA ABI major 13, got 12.9' \
    run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/incompatible-cuda-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_managed_cpu_ort'
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_SOURCE=managed
SSV_TENSORRT_MODE=enabled
SSV_TENSORRT_ROOT='${TEST_DIR}/cuda12-target'
SSV_TENSORRT_ARCHIVE='$fake_tensorrt_cuda12_archive'
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare cpu"

fake_tensorrt_10_15="$TEST_DIR/fake-tensorrt-10.15"
fake_tensorrt_10_15_archive="$TEST_DIR/fake-tensorrt-10.15.tgz"
build_fake_tensorrt "$fake_tensorrt_10_15" 10 15 0 13020
tar -C "$TEST_DIR" -czf "$fake_tensorrt_10_15_archive" \
    "$(basename -- "$fake_tensorrt_10_15")"
assert_failure_contains 'managed TensorRT rejects another ABI-compatible release' \
    'managed TensorRT version mismatch: expected 10.16.1, got 10.15.0' \
    run_clean_shell "source scripts/deps.sh
SSV_ROOT='$TEST_DIR/inexact-managed-tensorrt-project'
SSV_BUILD_DIR=\"\$SSV_ROOT/build\"
SSV_ONNXRUNTIME_SOURCE=local
SSV_ONNXRUNTIME_ROOT='$fake_managed_cpu_ort'
SSV_OPENCV_MODE=disabled
SSV_TENSORRT_SOURCE=managed
SSV_TENSORRT_MODE=enabled
SSV_TENSORRT_ROOT='${TEST_DIR}/inexact-managed-tensorrt-target'
SSV_TENSORRT_ARCHIVE='$fake_tensorrt_10_15_archive'
mkdir -p \"\$SSV_BUILD_DIR\"
ssv_deps_prepare cpu"

multi_tensorrt="$TEST_DIR/multi-tensorrt"
mkdir -p "$multi_tensorrt/TensorRT-a/include" "$multi_tensorrt/TensorRT-b/include"
: > "$multi_tensorrt/TensorRT-a/include/NvInfer.h"
: > "$multi_tensorrt/TensorRT-b/include/NvInfer.h"
assert_failure 'TensorRT rejects multiple SDK roots' run_clean_shell "source scripts/deps.sh; ssv_tensorrt_sdk_roots '$multi_tensorrt'"

fake_onnx_info="$(run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_archive_info 1.25.1-gpu')"
case "$fake_onnx_info" in
    *onnxruntime-linux-x64-gpu_cuda13-1.25.1.tgz*) pass 'NVIDIA profile selects the official CUDA 13 ONNX Runtime archive' ;;
    *) fail 'NVIDIA profile selects the official CUDA 13 ONNX Runtime archive' ;;
esac

fake_pc="$TEST_DIR/fake-pc"
fake_pkg_bin="$TEST_DIR/fake-pkg-bin"
fake_cuda_lib="$TEST_DIR/fake-cuda-lib"
mkdir -p "$fake_pc" "$fake_pkg_bin" "$fake_cuda_lib"
printf '%s\n' 'Name: nvinfer' 'Description: fake' 'Version: 11.2.3' 'Libs:' 'Cflags:' > "$fake_pc/nvinfer.pc"
printf '%s\n' '#!/usr/bin/env bash' \
    'case "$*" in' \
    '  "--variable=pcfiledir nvinfer") printf "%s\\n" "${FAKE_PC_DIR:?}" ;;' \
    '  "--variable=libdir nvinfer") printf "/usr/lib\\n" ;;' \
    '  "--modversion nvinfer") printf "11.2.3\\n" ;;' \
    '  "--libs nvinfer") printf "%s\\n" "-L/usr/lib -L${FAKE_CUDA_LIB:?} -lnvinfer -lcudart" ;;' \
    '  "--exists nvinfer"|"--atleast-version=1.0 nvinfer"|"--cflags --libs nvinfer"|"--cflags --libs --static nvinfer") exit 0 ;;' \
    '  *) exit 1 ;;' \
    'esac' > "$fake_pkg_bin/pkg-config"
printf '%s\n' '#!/usr/bin/env bash' \
    'output=""' \
    'while [ "$#" -gt 0 ]; do case "$1" in -o) output="$2"; shift 2 ;; *) shift ;; esac; done' \
    'printf "%s\\n" "#!/usr/bin/env bash" "exit 0" > "$output"' \
    'chmod +x "$output"' > "$fake_pkg_bin/c++"
chmod +x "$fake_pkg_bin/pkg-config" "$fake_pkg_bin/c++"
system_tensorrt_result="$(run_clean_shell "export PATH='$fake_pkg_bin':\$PATH FAKE_PC_DIR='$fake_pc' FAKE_CUDA_LIB='$fake_cuda_lib' CXX=c++; source scripts/deps.sh; ssv_deps_system_result nvinfer 1.0")"
case "$system_tensorrt_result" in
    *"runtime_dirs=$fake_cuda_lib"*) pass 'system TensorRT uses pkg-config and captures CUDA runtime path' ;;
    *) fail 'system TensorRT uses pkg-config and captures CUDA runtime path' ;;
esac

snapshot_test() {
    local snapshot="$TEST_DIR/ssv-deps.env"
    run_clean_shell "source scripts/deps.sh
for key in \"\${SSV_DEPS_SNAPSHOT_KEYS[@]}\"; do printf -v \"\$key\" %s value; done
SSV_DEPS_PROFILE=intel
SSV_DEPS_ONNXRUNTIME_PCDIR='/opt/onnxruntime/lib/pkgconfig'
SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS='/opt/onnxruntime/lib'
SSV_DEPS_ONNXRUNTIME_PROVIDERS='CPUExecutionProvider,OpenVINOExecutionProvider'
SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES='/tmp/libonnxruntime_providers_openvino.so'
SSV_DEPS_OPENCV_PCDIR='/opt/opencv/lib/pkgconfig'
SSV_DEPS_OPENCV_RUNTIME_DIRS='/opt/opencv/lib'
SSV_DEPS_TENSORRT_PCDIR='/opt/tensorrt/lib/pkgconfig'
SSV_DEPS_TENSORRT_RUNTIME_DIRS='/opt/tensorrt/lib:/opt/cuda/lib64'
SSV_DEPS_RUNTIME_PATH='/tmp/a path:/tmp/b'
ssv_deps_write_env '$snapshot'
unset SSV_DEPS_ONNXRUNTIME_PCDIR SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS
unset SSV_DEPS_OPENCV_PCDIR SSV_DEPS_OPENCV_RUNTIME_DIRS
unset SSV_DEPS_TENSORRT_PCDIR SSV_DEPS_TENSORRT_RUNTIME_DIRS
ssv_deps_load_env '$snapshot'
[ \"\$SSV_DEPS_RUNTIME_PATH\" = '/tmp/a path:/tmp/b' ]
[ \"\$SSV_DEPS_ONNXRUNTIME_PCDIR\" = '/opt/onnxruntime/lib/pkgconfig' ]
[ \"\$SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS\" = '/opt/onnxruntime/lib' ]
[ \"\$SSV_DEPS_OPENCV_PCDIR\" = '/opt/opencv/lib/pkgconfig' ]
[ \"\$SSV_DEPS_OPENCV_RUNTIME_DIRS\" = '/opt/opencv/lib' ]
[ \"\$SSV_DEPS_TENSORRT_PCDIR\" = '/opt/tensorrt/lib/pkgconfig' ]
[ \"\$SSV_DEPS_TENSORRT_RUNTIME_DIRS\" = '/opt/tensorrt/lib:/opt/cuda/lib64' ]
bash -c '[ \"\$SSV_DEPS_PROFILE\" = intel ] && [ \"\$SSV_DEPS_OPENCV_PCDIR\" = /opt/opencv/lib/pkgconfig ]'"
}
assert_success 'dependency snapshot round-trips whitelisted values' snapshot_test

signature_profile_test() {
    run_clean_shell 'source scripts/deps.sh
for key in "${SSV_DEPS_SNAPSHOT_KEYS[@]}"; do printf -v "$key" %s ""; done
SSV_DEPS_PROFILE=cpu
SSV_DEPS_ONNXRUNTIME_PROVIDERS=CPUExecutionProvider
cpu_signature="$(ssv_deps_compute_signature)"
SSV_DEPS_PROFILE=intel
SSV_DEPS_ONNXRUNTIME_PROVIDERS=CPUExecutionProvider,OpenVINOExecutionProvider
SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES=/bin/true
intel_signature="$(ssv_deps_compute_signature)"
[ "$cpu_signature" != "$intel_signature" ]'
}
assert_success 'dependency signature includes profile and Provider libraries' signature_profile_test

dependency_content_signature_test() {
    local library_dir="$TEST_DIR/signature-libraries"
    mkdir -p "$library_dir"
    run_clean_shell "source scripts/deps.sh
printf '%s\n' 'extern \"C\" int dependency_version() { return 1; }' > '$library_dir/dependency.cpp'
\"\${CXX:-c++}\" -shared -fPIC -Wl,-soname,libsignature_dependency.so \\
    '$library_dir/dependency.cpp' -o '$library_dir/libsignature_dependency.so'
printf '%s\n' \\
    'extern \"C\" int dependency_version();' \\
    'extern \"C\" int provider_version() { return dependency_version(); }' \\
    > '$library_dir/provider.cpp'
\"\${CXX:-c++}\" -shared -fPIC '$library_dir/provider.cpp' \\
    -L'$library_dir' -lsignature_dependency \\
    -o '$library_dir/libsignature_provider.so'
for key in \"\${SSV_DEPS_SNAPSHOT_KEYS[@]}\"; do printf -v \"\$key\" %s ''; done
SSV_DEPS_PROFILE=intel
SSV_DEPS_ONNXRUNTIME_VERSION=1.25.1
SSV_DEPS_ONNXRUNTIME_PROVIDERS=CPUExecutionProvider,OpenVINOExecutionProvider
SSV_DEPS_ONNXRUNTIME_PROVIDER_LIBRARIES='$library_dir/libsignature_provider.so'
SSV_DEPS_ONNXRUNTIME_RUNTIME_DIRS='$library_dir'
before=\"\$(ssv_deps_compute_signature)\"
printf '%s\n' 'extern \"C\" int dependency_version() { return 2; }' > '$library_dir/dependency.cpp'
\"\${CXX:-c++}\" -shared -fPIC -Wl,-soname,libsignature_dependency.so \\
    '$library_dir/dependency.cpp' -o '$library_dir/libsignature_dependency.so'
after=\"\$(ssv_deps_compute_signature)\"
[ \"\$before\" != \"\$after\" ]"
}
assert_success 'dependency signature changes with linked library content' dependency_content_signature_test

custom_build_snapshot_test() {
    local custom_build="$TEST_DIR/custom-build"
    run_clean_shell "source scripts/deps.sh
mkdir -p '$custom_build'
for key in \"\${SSV_DEPS_SNAPSHOT_KEYS[@]}\"; do printf -v \"\$key\" %s value; done
SSV_DEPS_RUNTIME_PATH='$TEST_DIR/custom-lib'
ssv_deps_write_env '$custom_build/ssv-deps.env'
SSV_BUILD_DIR='$custom_build'
LD_LIBRARY_PATH='$TEST_DIR/host-lib'
ssv_deps_load_runtime
[ \"\$LD_LIBRARY_PATH\" = '$TEST_DIR/custom-lib:$TEST_DIR/host-lib' ]"
}
assert_success 'runtime snapshot follows custom build directory' custom_build_snapshot_test

printf 'SSV_DEPS_SIGNATURE=value\nBAD_KEY=value\n' > "$TEST_DIR/bad.env"
assert_failure 'dependency snapshot rejects unknown variables' run_clean_shell "source scripts/deps.sh; ssv_deps_load_env '$TEST_DIR/bad.env'"

assert_failure_contains 'dependency snapshot requires the resolved profile' 'missing variable in dependency snapshot: SSV_DEPS_PROFILE' run_clean_shell "source scripts/deps.sh
snapshot='$TEST_DIR/missing-profile.env'
for key in \"\${SSV_DEPS_SNAPSHOT_KEYS[@]}\"; do
    [ \"\$key\" = SSV_DEPS_PROFILE ] || printf '%s=%q\\n' \"\$key\" value >> \"\$snapshot\"
done
ssv_deps_load_env \"\$snapshot\""

unknown_root="$TEST_DIR/unknown-root"
mkdir -p "$unknown_root"
printf 'keep\n' > "$unknown_root/user-data"
assert_failure 'unknown non-empty managed root is protected' run_clean_shell "source scripts/deps.sh; ssv_deps_require_replaceable_root '$unknown_root' ssv_onnxruntime_managed_validate 1.25.1"
[ -f "$unknown_root/user-data" ] && pass 'unknown root contents remain untouched' || fail 'unknown root contents remain untouched'

fake_bin="$TEST_DIR/fake-bin"
mkdir -p "$fake_bin" "$TEST_DIR/downloads"
printf '%s\n' '#!/usr/bin/env bash' \
    'set -e' \
    'count_file="${FAKE_CURL_COUNT:?}"' \
    'count=0' \
    '[ ! -f "$count_file" ] || count="$(cat "$count_file")"' \
    'printf "%s\\n" "$((count + 1))" > "$count_file"' \
    'output=""' \
    'while [ "$#" -gt 0 ]; do' \
    '    case "$1" in -o) output="$2"; shift 2 ;; *) shift ;; esac' \
    'done' \
    'printf "archive\\n" > "$output"' > "$fake_bin/curl"
chmod +x "$fake_bin/curl"
download_count="$TEST_DIR/curl-count"
PATH="$fake_bin:$PATH" FAKE_CURL_COUNT="$download_count" run_clean_shell "source scripts/deps.sh; ssv_deps_cached_download https://example.invalid/a '$TEST_DIR/downloads/a' >/dev/null; ssv_deps_cached_download https://example.invalid/a '$TEST_DIR/downloads/a' >/dev/null"
assert_eq '1' "$(cat "$download_count")" 'cached download avoids a second network request'

if grep -Fq '$SSV_ROOT/.deps/downloads/onnxruntime/' "$ROOT/scripts/deps/onnxruntime-managed.sh" && \
    grep -Fq '$SSV_ROOT/.deps/downloads/opencv/' "$ROOT/scripts/deps/opencv-managed.sh" && \
    grep -Fq '$SSV_ROOT/.deps/downloads/tensorrt/' "$ROOT/scripts/deps/tensorrt-managed.sh"; then
    pass 'managed archives share the central .deps/downloads tree'
else
    fail 'managed archives share the central .deps/downloads tree'
fi

if rg -n 'dpkg-deb' "$ROOT/scripts/deps/opencv-managed.sh" >/dev/null; then fail 'OpenCV provider has no dpkg-deb dependency'; else pass 'OpenCV provider has no dpkg-deb dependency'; fi
if rg -n 'libcblas-dev' "$ROOT/.github/workflows/ci.yml" "$ROOT/README.md" >/dev/null || \
    rg -n 'for dep .* cblas' "$ROOT/scripts/build.sh" >/dev/null; then
    fail 'OpenCV host math dependencies use portable BLAS and LAPACK names'
else
    pass 'OpenCV host math dependencies use portable BLAS and LAPACK names'
fi
grep -q 'nlohmann-json cblas blas lapack' "$ROOT/README.md" \
    && pass 'Arch install instructions include its separate CBLAS package' \
    || fail 'Arch install instructions include its separate CBLAS package'
grep -q 'pkg-config --exists cblas' "$ROOT/scripts/deps/opencv-managed.sh" \
    && pass 'OpenCV provider adapts to a separate system CBLAS package' \
    || fail 'OpenCV provider adapts to a separate system CBLAS package'
if rg -n 'Libs\.private:.*host_math_libs' "$ROOT/scripts/deps/opencv-managed.sh" >/dev/null; then
    fail 'OpenCV provider exposes host math libraries to dynamic consumers'
else
    pass 'OpenCV provider exposes host math libraries to dynamic consumers'
fi
if rg -n 'SSV_ONNXRUNTIME_FLAVOR|SSV_TENSORRT_VERSION' "$ROOT/scripts" "$ROOT/.env.example" >/dev/null; then fail 'legacy dependency variables are absent'; else pass 'legacy dependency variables are absent'; fi

singular_dep_matches="$TEST_DIR/singular-dep-matches"
singular_dep_root='.de''p'
if rg -n --hidden -F "$singular_dep_root/" \
    --glob '!.git/**' --glob '!.deps/**' --glob "!$singular_dep_root/**" --glob '!build/**' \
    "$ROOT" >"$singular_dep_matches"; then
    cat "$singular_dep_matches" >&2
    fail 'dependency paths use only the .deps root'
else
    pass 'dependency paths use only the .deps root'
fi

printf '%s tests passed, %s failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
