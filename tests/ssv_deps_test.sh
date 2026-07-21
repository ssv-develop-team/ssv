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
        -u SSV_OPENCV_INCLUDE_DIR -u SSV_OPENCV_LIB_DIR \
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

assert_success 'TensorRT auto without SDK resolves to stub' run_clean_shell 'source scripts/deps.sh; SSV_TENSORRT_MODE=auto; SSV_TENSORRT_SOURCE=managed; SSV_TENSORRT_ROOT="$TEST_DIR/no-sdk"; ssv_deps_prepare_tensorrt; [ "$SSV_DEPS_TENSORRT_STATUS" = stub ]'
assert_success 'TensorRT disabled resolves to stub' run_clean_shell 'source scripts/deps.sh; SSV_TENSORRT_MODE=disabled; ssv_deps_resolve_config; ssv_deps_prepare_tensorrt; [ "$SSV_DEPS_TENSORRT_STATUS" = stub ]'

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
build_fake_local_opencv "$fake_managed_workspace/managed/usr" 4.10.0
mkdir -p "$fake_managed_workspace/local/source"
printf 'keep local source\n' > "$fake_managed_workspace/local/source/.ssv-test-sentinel"
assert_success 'managed OpenCV publishes only inside the shared workspace managed subtree' run_clean_shell "source scripts/deps.sh
result=\"\$(ssv_opencv_managed_prepare '$fake_managed_workspace' 4.10.0)\"
case \"\$result\" in *'pkgconfig_dir=$fake_managed_workspace/managed/lib/pkgconfig'*) ;; *) exit 1 ;; esac
[ -f '$fake_managed_workspace/managed/lib/pkgconfig/opencv4.pc' ]
[ ! -e '$fake_managed_workspace/lib/pkgconfig/opencv4.pc' ]
grep -Fqx 'keep local source' '$fake_managed_workspace/local/source/.ssv-test-sentinel'"

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
LD_LIBRARY_PATH='$TEST_DIR/host-lib'
ssv_deps_load_runtime
[ \"\$LD_LIBRARY_PATH\" = '$TEST_DIR/custom-lib:$TEST_DIR/host-lib' ]"
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
