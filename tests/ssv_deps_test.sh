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
    local name="$1"
    shift
    if "$@" >/dev/null 2>&1; then pass "$name"; else fail "$name"; fi
}

assert_failure() {
    local name="$1"
    shift
    if "$@" >/dev/null 2>&1; then fail "$name"; else pass "$name"; fi
}

run_clean_shell() {
    env \
        -u SSV_ONNXRUNTIME_SOURCE -u SSV_ONNXRUNTIME_VERSION -u SSV_ONNXRUNTIME_ROOT \
        -u SSV_OPENCV_SOURCE -u SSV_OPENCV_MODE -u SSV_OPENCV_ROOT \
        -u SSV_TENSORRT_SOURCE -u SSV_TENSORRT_MODE -u SSV_TENSORRT_ROOT \
        -u SSV_TENSORRT_ARCHIVE -u SSV_TENSORRT_URL -u CUDA_HOME \
        -u SSV_EXTRA_PKG_CONFIG_PATH \
        ROOT="$ROOT" TEST_DIR="$TEST_DIR" \
        SSV_ROOT="$ROOT" SSV_BUILD_DIR="$TEST_DIR/build" bash --noprofile --norc -c "$1"
}

defaults="$(run_clean_shell 'source scripts/deps.sh; ssv_deps_resolve_config; printf "%s|%s|%s|%s|%s|%s" "$SSV_ONNXRUNTIME_SOURCE" "$SSV_ONNXRUNTIME_VERSION" "$SSV_OPENCV_SOURCE" "$SSV_OPENCV_MODE" "$SSV_TENSORRT_SOURCE" "$SSV_TENSORRT_MODE"')"
assert_eq 'managed|1.25.1|managed|enabled|managed|auto' "$defaults" 'default dependency configuration is uniform'

assert_eq '1.25.1' "$(run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_normalize_version 1.25.1')" 'ONNX Runtime CPU version syntax'
assert_eq '1.25.1-gpu' "$(run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_normalize_version 1.25.1-gpu')" 'ONNX Runtime GPU version syntax'
assert_failure 'ONNX Runtime rejects flavor-like versions' run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_normalize_version gpu'

assert_failure 'managed root rejects repository root' run_clean_shell 'source scripts/deps.sh; ssv_deps_validate_root ROOT "$SSV_ROOT"'
assert_failure 'managed root rejects .deps root' run_clean_shell 'source scripts/deps.sh; ssv_deps_validate_root ROOT .deps'
assert_failure 'managed root rejects colon' run_clean_shell 'source scripts/deps.sh; ssv_deps_validate_root ROOT ".deps/a:b"'
assert_success 'managed root accepts ordinary spaces' run_clean_shell 'source scripts/deps.sh; ssv_deps_validate_root ROOT ".deps/a b"'

assert_failure 'system ONNX Runtime rejects managed version' run_clean_shell 'SSV_ONNXRUNTIME_SOURCE=system SSV_ONNXRUNTIME_VERSION=1.25.1; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_failure 'disabled OpenCV rejects source' run_clean_shell 'SSV_OPENCV_MODE=disabled SSV_OPENCV_SOURCE=managed; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_failure 'disabled TensorRT rejects source' run_clean_shell 'SSV_TENSORRT_MODE=disabled SSV_TENSORRT_SOURCE=managed; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'
assert_failure 'TensorRT rejects archive and URL together' run_clean_shell 'source scripts/deps.sh; SSV_TENSORRT_ARCHIVE=/tmp/a.tar; SSV_TENSORRT_URL=https://example.invalid/a.tar; ssv_deps_resolve_config'
assert_success 'TensorRT accepts a direct HTTPS URL' run_clean_shell 'SSV_TENSORRT_MODE=enabled SSV_TENSORRT_URL=https://example.invalid/a.tar; source scripts/deps.sh; ssv_deps_capture_explicit_config; ssv_deps_resolve_config'

assert_eq '/opt/a:/opt/b' "$(run_clean_shell 'source scripts/deps.sh; ssv_deps_join_unique "/opt/a:/opt/b:/opt/a"')" 'runtime path deduplicates in first-seen order'
assert_eq "$TEST_DIR/custom-lib" "$(run_clean_shell 'source scripts/deps.sh; mkdir -p "$TEST_DIR/custom-lib"; ssv_deps_runtime_dirs /usr/lib "$TEST_DIR/custom-lib"')" 'system runtime directories are filtered'

provider_result="$(run_clean_shell 'source scripts/deps.sh; ssv_deps_parse_provider_result X $'\''version=1.2.3\npkgconfig_dir=/tmp/pc\nruntime_dirs=/tmp/lib'\''')"
assert_eq $'1.2.3\n/tmp/pc\n/tmp/lib' "$provider_result" 'provider result protocol has exactly three fields'
assert_failure 'provider result rejects extra fields' run_clean_shell 'source scripts/deps.sh; ssv_deps_parse_provider_result X $'\''version=1\npkgconfig_dir=/p\nruntime_dirs=/l\nextra=x'\'''

assert_success 'TensorRT auto without SDK resolves to stub' run_clean_shell 'source scripts/deps.sh; SSV_TENSORRT_MODE=auto; SSV_TENSORRT_SOURCE=managed; SSV_TENSORRT_ROOT="$TEST_DIR/no-sdk"; ssv_deps_prepare_tensorrt; [ "$SSV_DEPS_TENSORRT_STATUS" = stub ]'
assert_success 'TensorRT disabled resolves to stub' run_clean_shell 'source scripts/deps.sh; SSV_TENSORRT_MODE=disabled; ssv_deps_resolve_config; ssv_deps_prepare_tensorrt; [ "$SSV_DEPS_TENSORRT_STATUS" = stub ]'

fake_build_bin="$TEST_DIR/fake-build-bin"
mkdir -p "$fake_build_bin"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$fake_build_bin/git"
chmod +x "$fake_build_bin/git"
assert_failure 'managed roots with spaces fail before download' run_clean_shell "PATH='$fake_build_bin':\$PATH SSV_ONNXRUNTIME_ROOT='$TEST_DIR/onnx runtime'; source scripts/deps.sh; ssv_deps_prepare"

fake_tensorrt="$TEST_DIR/fake-tensorrt"
mkdir -p "$fake_tensorrt/include" "$fake_tensorrt/lib" "$fake_tensorrt/cuda/include" "$fake_tensorrt/cuda/lib" "$fake_tensorrt/lib/pkgconfig"
printf '%s\n' '#pragma once' > "$fake_tensorrt/include/NvInfer.h"
printf '%s\n' \
    '#define NV_TENSORRT_MAJOR 11' \
    '#define NV_TENSORRT_MINOR 2' \
    '#define NV_TENSORRT_PATCH 3' > "$fake_tensorrt/include/NvInferVersion.h"
printf '%s\n' '#pragma once' > "$fake_tensorrt/cuda/include/cuda_runtime_api.h"
: > "$fake_tensorrt/lib/libnvinfer.so"
: > "$fake_tensorrt/cuda/lib/libcudart.so"
assert_eq '11.2.3' "$(run_clean_shell "source scripts/deps.sh; ssv_tensorrt_version '$fake_tensorrt/include/NvInferVersion.h'")" 'TensorRT version comes from NvInferVersion.h'

multi_tensorrt="$TEST_DIR/multi-tensorrt"
mkdir -p "$multi_tensorrt/TensorRT-a/include" "$multi_tensorrt/TensorRT-b/include"
: > "$multi_tensorrt/TensorRT-a/include/NvInfer.h"
: > "$multi_tensorrt/TensorRT-b/include/NvInfer.h"
assert_failure 'TensorRT rejects multiple SDK roots' run_clean_shell "source scripts/deps.sh; ssv_tensorrt_sdk_roots '$multi_tensorrt'"

fake_onnx_info="$(run_clean_shell 'source scripts/deps.sh; ssv_onnxruntime_archive_info 1.25.1-gpu')"
case "$fake_onnx_info" in
    *onnxruntime-linux-x64-gpu-1.25.1.tgz*) pass 'ONNX Runtime GPU suffix selects GPU archive' ;;
    *) fail 'ONNX Runtime GPU suffix selects GPU archive' ;;
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
for key in \"\${SSV_DEPS_ENV_KEYS[@]}\"; do printf -v \"\$key\" %s value; done
SSV_DEPS_RUNTIME_PATH='/tmp/a path:/tmp/b'
ssv_deps_write_env '$snapshot'
ssv_deps_load_env '$snapshot'
[ \"\$SSV_DEPS_RUNTIME_PATH\" = '/tmp/a path:/tmp/b' ]"
}
assert_success 'dependency snapshot round-trips whitelisted values' snapshot_test

custom_build_snapshot_test() {
    local custom_build="$TEST_DIR/custom-build"
    run_clean_shell "source scripts/deps.sh
mkdir -p '$custom_build'
for key in \"\${SSV_DEPS_ENV_KEYS[@]}\"; do printf -v \"\$key\" %s value; done
SSV_DEPS_RUNTIME_PATH='$TEST_DIR/custom-lib'
ssv_deps_write_env '$custom_build/ssv-deps.env'
SSV_BUILD_DIR='$custom_build'
ssv_deps_load_runtime
[ \"\$LD_LIBRARY_PATH\" = '$TEST_DIR/custom-lib' ]"
}
assert_success 'runtime snapshot follows custom build directory' custom_build_snapshot_test

printf 'SSV_DEPS_SIGNATURE=value\nBAD_KEY=value\n' > "$TEST_DIR/bad.env"
assert_failure 'dependency snapshot rejects unknown variables' run_clean_shell "source scripts/deps.sh; ssv_deps_load_env '$TEST_DIR/bad.env'"

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

if rg -n 'dpkg-deb' "$ROOT/scripts/deps/opencv-managed.sh" >/dev/null; then fail 'OpenCV provider has no dpkg-deb dependency'; else pass 'OpenCV provider has no dpkg-deb dependency'; fi
if rg -n 'SSV_ONNXRUNTIME_FLAVOR|SSV_TENSORRT_VERSION' "$ROOT/scripts" "$ROOT/.env.example" >/dev/null; then fail 'legacy dependency variables are absent'; else pass 'legacy dependency variables are absent'; fi

printf '%s tests passed, %s failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
