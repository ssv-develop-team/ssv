#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail() {
    echo "test failed: $*" >&2
    exit 1
}

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

write_dependency_snapshot() {
    local build_dir="$1"
    local runtime_path="${2:-}"
    (
        source "$ROOT/scripts/deps.sh"
        for key in "${SSV_DEPS_SNAPSHOT_KEYS[@]}"; do
            printf -v "$key" '%s' ''
        done
        SSV_DEPS_RUNTIME_PATH="$runtime_path"
        ssv_deps_write_env "$build_dir/ssv-deps.env"
    )
}

build_dispatch_bin="$TMP_DIR/build-dispatch-bin"
build_dispatch_capture="$TMP_DIR/build-dispatch.args"
mkdir -p "$build_dispatch_bin"
printf '%s\n' \
    '#!/bin/bash' \
    ': "${SSV_CAPTURE_PATH:?}"' \
    'printf "%s\n" "$@" > "$SSV_CAPTURE_PATH"' \
    > "$build_dispatch_bin/bash"
chmod +x "$build_dispatch_bin/bash"
SSV_CAPTURE_PATH="$build_dispatch_capture" PATH="$build_dispatch_bin:$PATH" \
    ./ssv build --profile intel
[ "$(sed -n '2p' "$build_dispatch_capture")" = --profile ] ||
    fail "ssv does not forward the build profile option"
[ "$(sed -n '3p' "$build_dispatch_capture")" = intel ] ||
    fail "ssv does not forward the build profile value"

build_parse_bin="$TMP_DIR/build-parse-bin"
build_parse_capture="$TMP_DIR/build-parse.flock"
mkdir -p "$build_parse_bin"
printf '%s\n' \
    '#!/bin/bash' \
    'printf "called\n" > "${SSV_CAPTURE_PATH:?}"' \
    'exit 99' \
    > "$build_parse_bin/flock"
chmod +x "$build_parse_bin/flock"

set +e
build_parse_output="$(SSV_CAPTURE_PATH="$build_parse_capture" \
    SSV_BUILD_DIR="$TMP_DIR/build-parse" PATH="$build_parse_bin:$PATH" \
    ./ssv build --profile bogus 2>&1)"
build_parse_status=$?
set -e
[ "$build_parse_status" -eq 2 ] ||
    fail "invalid build profile returned $build_parse_status instead of 2"
grep -Fq 'runtime profile must be auto, cpu, nvidia, intel, or amd: bogus' \
    <<<"$build_parse_output" || fail "invalid build profile has no precise error"
[ ! -e "$build_parse_capture" ] ||
    fail "invalid build profile reached dependency side effects"

assert_build_cli_rejected() {
    local expected="$1" output status
    shift
    rm -f "$build_parse_capture"
    set +e
    output="$(SSV_CAPTURE_PATH="$build_parse_capture" \
        SSV_BUILD_DIR="$TMP_DIR/build-parse" PATH="$build_parse_bin:$PATH" \
        ./ssv build "$@" 2>&1)"
    status=$?
    set -e
    [ "$status" -eq 2 ] ||
        fail "invalid build CLI returned $status instead of 2: $*"
    grep -Fq -- "$expected" <<<"$output" ||
        fail "invalid build CLI has no precise error: $expected"
    [ ! -e "$build_parse_capture" ] ||
        fail "invalid build CLI reached dependency side effects: $*"
}

assert_build_cli_rejected '--profile requires a value' --profile
assert_build_cli_rejected '--profile requires a value' --profile --help
assert_build_cli_rejected '--profile requires a value' --profile=
assert_build_cli_rejected '--profile may be specified only once' \
    --profile cpu --profile intel
assert_build_cli_rejected 'unknown build argument: --jobs' --jobs 2

rm -f "$build_parse_capture"
set +e
SSV_CAPTURE_PATH="$build_parse_capture" SSV_BUILD_DIR="$TMP_DIR/build-parse" \
    PATH="$build_parse_bin:$PATH" ./ssv build --profile=cpu >/dev/null 2>&1
build_parse_status=$?
set -e
if [ "$build_parse_status" -ne 99 ] || [ ! -e "$build_parse_capture" ]; then
    fail "valid --profile=cpu did not reach dependency preparation"
fi

run_build_cache_reset_case() {
    local mode="$1" expected_option="$2"
    local sandbox="$TMP_DIR/build-cache-$mode"
    local fake_bin="$sandbox/bin"
    local build_dir="$sandbox/build"
    local meson_calls="$sandbox/meson.calls"
    mkdir -p "$sandbox/scripts" "$fake_bin" "$build_dir"
    cp ssv "$sandbox/ssv"
    cp scripts/build.sh scripts/lib.sh "$sandbox/scripts/"
    printf '%s\n' \
        '#!/bin/bash' \
        'ssv_deps_die() { printf "[SSV] %s\\n" "$*" >&2; return 1; }' \
        'ssv_onnxruntime_validate_profile() {' \
        '    case "$1" in auto|cpu|nvidia|intel|amd) ;; *) return 1 ;; esac' \
        '}' \
        'ssv_deps_write_env() {' \
        '    mkdir -p -- "$(dirname -- "$1")"' \
        '    printf "SSV_DEPS_SIGNATURE=%s\\n" "$SSV_DEPS_SIGNATURE" > "$1"' \
        '}' \
        'ssv_deps_prepare() {' \
        '    SSV_DEPS_PROFILE="$1"' \
        '    SSV_DEPS_SIGNATURE="${FAKE_DEP_SIGNATURE:?}"' \
        '    SSV_DEPS_OPENCV_MODE=disabled' \
        '    SSV_DEPS_TENSORRT_MODE=disabled' \
        '    SSV_DEPS_RUNTIME_PATH=' \
        '    SSV_DEPS_PKG_CONFIG_PATH=' \
        '    ssv_deps_write_env "$SSV_BUILD_DIR/ssv-deps.env.pending"' \
        '}' \
        > "$sandbox/scripts/deps.sh"
    printf '%s\n' '#!/bin/bash' 'exit 0' > "$fake_bin/pkg-config"
    printf '%s\n' \
        '#!/bin/bash' \
        'set -e' \
        ': "${FAKE_MESON_CALLS:?}"' \
        'if [ "$1" = setup ] && [ "${2:-}" = --help ]; then' \
        '    [ "${FAKE_MESON_CLEARCACHE:-false}" = true ] && printf "%s\\n" --clearcache' \
        '    exit 0' \
        'fi' \
        'printf "%s\\n" "$*" >> "$FAKE_MESON_CALLS"' \
        'if [ "$1" = setup ]; then' \
        '    build_dir="$2"' \
        '    case " $* " in' \
        '        *" --wipe "*) rm -rf -- "$build_dir"; mkdir -p "$build_dir" ;;' \
        '    esac' \
        '    : > "$build_dir/build.ninja"' \
        'elif [ "$1" = compile ]; then' \
        '    build_dir="$3"' \
        '    [ -f "$build_dir/ssv-deps.env.pending" ] || exit 45' \
        '    for plugin in ssv-template ssv-infer ssv-track ssv-pub ssv-overlay; do' \
        '        mkdir -p "$build_dir/gst/$plugin"' \
        '        : > "$build_dir/gst/$plugin/libgst${plugin//-/}.so"' \
        '    done' \
        'fi' \
        > "$fake_bin/meson"
    chmod +x "$sandbox/ssv" "$fake_bin/pkg-config" "$fake_bin/meson"
    : > "$build_dir/build.ninja"
    printf 'SSV_DEPS_SIGNATURE=old\n' > "$build_dir/ssv-deps.env"

    FAKE_DEP_SIGNATURE=new \
    FAKE_MESON_CALLS="$meson_calls" \
    FAKE_MESON_CLEARCACHE="$mode" \
    SSV_BUILD_DIR="$build_dir" \
    PATH="$fake_bin:$PATH" \
        "$sandbox/ssv" build --profile cpu >/dev/null
    grep -Fq -- "$expected_option" "$meson_calls" ||
        fail "dependency signature change did not reset Meson cache with $expected_option"
    grep -Fxq 'SSV_DEPS_SIGNATURE=new' "$build_dir/ssv-deps.env" ||
        fail "cache reset did not publish the new successful dependency snapshot"
}

run_build_cache_reset_case true --clearcache
run_build_cache_reset_case false --wipe

run_dispatch_build="$TMP_DIR/run-dispatch-build"
run_dispatch_capture="$TMP_DIR/run-dispatch.args"
run_dispatch_runtime="$TMP_DIR/run-dispatch-runtime"
write_dependency_snapshot "$run_dispatch_build" "$run_dispatch_runtime"
mkdir -p "$run_dispatch_build/runner"
printf '%s\n' \
    '#!/bin/bash' \
    ': "${SSV_CAPTURE_PATH:?}"' \
    'printf "%s\n" "$@" > "$SSV_CAPTURE_PATH"' \
    'printf "ENV:LD_LIBRARY_PATH=%s\n" "${LD_LIBRARY_PATH:-}" >> "$SSV_CAPTURE_PATH"' \
    'printf "ENV:GST_PLUGIN_PATH=%s\n" "${GST_PLUGIN_PATH:-}" >> "$SSV_CAPTURE_PATH"' \
    'exit 23' \
    > "$run_dispatch_build/runner/ssv-runner"
chmod +x "$run_dispatch_build/runner/ssv-runner"

set +e
SSV_BUILD_DIR="$run_dispatch_build" SSV_CAPTURE_PATH="$run_dispatch_capture" \
    ./ssv run \
        --config config/custom.yaml \
        --display \
        --overlay \
        --display-backend gtkglsink
run_dispatch_status=$?
set -e
[ "$run_dispatch_status" -eq 23 ] ||
    fail "ssv run did not preserve the runner exit status"
if [ "$(sed -n '1p' "$run_dispatch_capture")" != --config ] ||
    [ "$(sed -n '2p' "$run_dispatch_capture")" != config/custom.yaml ] ||
    [ "$(sed -n '3p' "$run_dispatch_capture")" != --display ] ||
    [ "$(sed -n '4p' "$run_dispatch_capture")" != --overlay ] ||
    [ "$(sed -n '5p' "$run_dispatch_capture")" != --display-backend ] ||
    [ "$(sed -n '6p' "$run_dispatch_capture")" != gtkglsink ]; then
    fail "ssv run did not forward the public runner CLI unchanged"
fi
grep -Fq "ENV:LD_LIBRARY_PATH=$run_dispatch_build/gst/ssv-common:$run_dispatch_runtime" \
    "$run_dispatch_capture" ||
    fail "ssv run did not load the dependency snapshot runtime path"
grep -Fq "ENV:GST_PLUGIN_PATH=$run_dispatch_build/gst/ssv-template" \
    "$run_dispatch_capture" ||
    fail "ssv run did not export the project plugin path"

run_headless_capture="$TMP_DIR/run-headless.args"
set +e
SSV_BUILD_DIR="$run_dispatch_build" SSV_CAPTURE_PATH="$run_headless_capture" \
    ./ssv run --headless
run_headless_status=$?
set -e
[ "$run_headless_status" -eq 23 ] ||
    fail "ssv run --headless did not preserve the runner exit status"
[ "$(sed -n '1p' "$run_headless_capture")" = --headless ] ||
    fail "ssv run did not forward the headless override"

missing_snapshot_build="$TMP_DIR/missing-snapshot-build"
set +e
missing_snapshot_output="$(SSV_BUILD_DIR="$missing_snapshot_build" \
    ./ssv run --headless 2>&1)"
missing_snapshot_status=$?
set -e
[ "$missing_snapshot_status" -ne 0 ] ||
    fail "ssv run accepted a missing successful dependency snapshot"
grep -Fq 'dependency snapshot not found:' <<<"$missing_snapshot_output" ||
    fail "missing dependency snapshot has no precise error"
grep -Fq 'run ./ssv build first' <<<"$missing_snapshot_output" ||
    fail "missing dependency snapshot does not tell the user to build first"

[ ! -e scripts/pipeline.sh ] ||
    fail "the legacy long-running shell pipeline still exists"
if rg -n 'gst-launch-1\.0' ssv scripts; then
    fail "a production script still executes gst-launch-1.0"
fi
if rg -n 'ssv_yaml_get|import[[:space:]]+yaml' ssv scripts --glob '*.sh'; then
    fail "shell scripts still parse runtime YAML"
fi

test_sandbox="$TMP_DIR/test-command"
test_capture="$TMP_DIR/test-timeout.args"
test_bin="$TMP_DIR/test-bin"
mkdir -p \
    "$test_sandbox/scripts" \
    "$test_sandbox/tests" \
    "$test_sandbox/agent" \
    "$test_bin"
cp ssv "$test_sandbox/ssv"
cp scripts/lib.sh scripts/deps.sh scripts/run.sh scripts/test.sh \
    "$test_sandbox/scripts/"
cp -R scripts/deps "$test_sandbox/scripts/deps"
for test_script in ssv_deps_test.sh ssv_cli_test.sh; do
    printf '%s\n' '#!/bin/bash' 'exit 0' \
        > "$test_sandbox/tests/$test_script"
    chmod +x "$test_sandbox/tests/$test_script"
done
printf '%s\n' \
    '#!/bin/bash' \
    'source "$(dirname "$0")/lib.sh"' \
    'source "$(dirname "$0")/deps.sh"' \
    'for key in "${SSV_DEPS_SNAPSHOT_KEYS[@]}"; do' \
    '    printf -v "$key" "%s" ""' \
    'done' \
    'ssv_deps_write_env "$SSV_BUILD_DIR/ssv-deps.env"' \
    > "$test_sandbox/scripts/build.sh"
chmod +x "$test_sandbox/scripts/build.sh"
printf '%s\n' '#!/bin/bash' 'exit 0' > "$test_bin/uv"
printf '%s\n' '#!/bin/bash' 'exit 0' > "$test_bin/meson"
printf '%s\n' \
    '#!/bin/bash' \
    ': "${SSV_CAPTURE_PATH:?}"' \
    'printf "%s\n" "$@" > "$SSV_CAPTURE_PATH"' \
    'exit 124' \
    > "$test_bin/timeout"
chmod +x "$test_bin/uv" "$test_bin/meson" "$test_bin/timeout"

test_config="$test_sandbox/ssv.yaml"
printf '%s\n' \
    'version: "2.0"' \
    'sources:' \
    '  - id: smoke-source' \
    '    uri: rtsp://127.0.0.1/test' \
    > "$test_config"
test_config_before="$(sha256sum "$test_config")"
SSV_CONFIG_PATH="$test_config" \
SSV_BUILD_DIR="$test_sandbox/build" \
SSV_CAPTURE_PATH="$test_capture" \
PATH="$test_bin:$PATH" \
    "$test_sandbox/ssv" test >/dev/null
[ -f "$test_capture" ] ||
    fail "ssv test did not invoke an external runner timeout"
grep -Fxq '30s' "$test_capture" ||
    fail "ssv test does not use the bounded smoke duration"
grep -Fxq "$test_sandbox/scripts/run.sh" "$test_capture" ||
    fail "ssv test smoke does not use the production runner wrapper"
grep -Fxq -- '--config' "$test_capture" ||
    fail "ssv test smoke does not pass the selected config"
grep -Fxq -- '--headless' "$test_capture" ||
    fail "ssv test smoke is not explicitly headless"
[ "$(sha256sum "$test_config")" = "$test_config_before" ] ||
    fail "ssv test modified the runtime YAML"

help_output="$(./ssv --help)"
grep -q "  clean" <<<"$help_output" || fail "help does not list clean"
grep -q "  run" <<<"$help_output" || fail "help does not list run"
grep -q "run --display" <<<"$help_output" ||
    fail "help does not list run --display"
grep -q -- "--overlay" <<<"$help_output" || fail "help does not list --overlay"
grep -q "run --config PATH" <<<"$help_output" ||
    fail "help does not list the config override"
grep -q -- "--headless" <<<"$help_output" ||
    fail "help does not list the headless override"
grep -q -- "--display-backend gtkglsink|gtksink" <<<"$help_output" ||
    fail "help does not list the constrained display backend override"
if grep -q -- "--sink" <<<"$help_output"; then
    fail "help still exposes arbitrary GStreamer element injection"
fi
grep -q "  test" <<<"$help_output" || fail "help does not list test"
grep -q "  prepare-model" <<<"$help_output" ||
    fail "help does not list prepare-model"
grep -q "运行代码测试和链路冒烟测试后退出" <<<"$help_output" ||
    fail "help does not describe test as exit-style"

grep -Fq './ssv -> scripts/run.sh -> build/runner/ssv-runner' README.md ||
    fail "README does not describe the production runner entry"
grep -Fq './ssv run --headless' README.md ||
    fail "README does not document the headless override"
grep -Fq -- '--display-backend gtkglsink' README.md ||
    fail "README does not document the constrained display backend override"
if rg -n 'scripts/pipeline\.sh|gst-launch-1\.0|--sink|pipeline\.check_timeout' \
    README.md; then
    fail "README still documents the removed shell runtime contract"
fi

for legacy in "--m2" "--m2-mock" "--m3" "--m3-mock" "check" "all"; do
    if grep -q -- "$legacy" <<<"$help_output"; then
        fail "help still lists legacy command: $legacy"
    fi
done

grep -Fq "./ssv prepare-model \\" README.md ||
    fail "README does not document the prepare-model command"
grep -Fq 'rgba_u8_nhwc_v1' README.md ||
    fail "README does not document the wrapper input contract"
grep -Fq -- '--force' README.md ||
    fail "README does not document protected wrapper replacement"
if grep -Fq '| `SSV_ONNXRUNTIME_VERSION` |' README.md; then
    fail "README still exposes the profile-owned ONNX Runtime version"
fi
if grep -Eq '^[[:space:]]*#[[:space:]]*SSV_ONNXRUNTIME_VERSION=' .env.example; then
    fail ".env.example still exposes the profile-owned ONNX Runtime version"
fi

grep -q 'SSV_RTSP_URL' .env.example ||
    fail ".env.example does not document SSV_RTSP_URL"
grep -q 'config/ssv.yaml' scripts/lib.sh ||
    fail "scripts/lib.sh does not search config/ssv.yaml"
grep -q 'config/ssv.yaml' .gitignore ||
    fail ".gitignore does not ignore local config/ssv.yaml"
if rg -n 'python -m ssv_agent --config "\$SSV_CONFIG"' scripts/agent.sh; then
    fail "scripts/agent.sh always passes an empty default config path"
fi
if rg -n \
    'SSV_RTSP_PROTOCOLS|SSV_RTSP_LATENCY|SSV_CHECK_TIMEOUT|SSV_DISPLAY_OVERLAY' \
    .env.example; then
    fail ".env.example documents YAML-owned runtime overrides"
fi

grep -q '不参与默认配置搜索' config/ssv.example.yaml ||
    fail "example YAML does not declare its template-only boundary"
python3 - config/ssv.example.yaml <<'PY'
import sys
from pathlib import Path

import yaml

config = yaml.safe_load(Path(sys.argv[1]).read_text(encoding="utf-8"))
assert config["version"] == "2.0"
assert len(config["sources"]) == 1
assert config["sources"][0]["id"]
assert config["sources"][0]["codec"] == "h264"
assert "decoder" not in config["sources"][0]
assert config["sources"][0]["decode"]["mode"] == "auto"
assert "pipeline" not in config
assert "sink" not in config["display"]
assert config["display"]["backend"] == "auto"
assert config["display"]["overlay"]["font"]["face"] == "regular"
assert config["display"]["overlay"]["font"]["size"] == 12
assert config["display"]["overlay"]["motion_prediction"]["max_horizon_ms"] == 300
assert "model_path" not in config["inference"]
assert config["inference"]["analysis_fps"] == 15
assert config["inference"]["runtime"]["type"] == "onnxruntime"
assert config["inference"]["runtime"]["providers"]["mode"] == "auto"
assert config["inference"]["runtime"]["cache"]["directory"] == ""
assert "frame_rate" not in config["tracking"]
PY

grep -q 'source "$(dirname "$0")/deps.sh"' scripts/build.sh ||
    fail "build script does not use the unified dependency entry point"
grep -q '版本由 profile 固定派生' .env.example ||
    fail ".env.example does not document profile-owned ORT versions"
grep -q './ssv build --profile auto|cpu|nvidia|intel|amd' .env.example ||
    fail ".env.example does not document runtime profiles"
grep -q 'SSV_ONNXRUNTIME_SOURCE=local' .env.example ||
    fail ".env.example does not document local ORT artifacts"
grep -q 'managed 当前固定使用 OpenCV 4.10.0' .env.example ||
    fail ".env.example does not document the managed OpenCV version"
grep -q 'opencv-managed.sh 的源码归档、模块清单和运行库闭包' .env.example ||
    fail ".env.example does not document managed OpenCV upgrade requirements"
grep -q 'SSV_TENSORRT_URL' .env.example ||
    fail ".env.example does not document explicit TensorRT URL configuration"
grep -q 'SSV_TENSORRT_ARCHIVE' .env.example ||
    fail ".env.example does not document TensorRT archive configuration"
grep -q 'tensorrt_mode="$SSV_DEPS_TENSORRT_MODE"' scripts/build.sh ||
    fail "build script does not pass the resolved TensorRT mode to Meson"
grep -q 'onnxruntime_profile="$SSV_DEPS_PROFILE"' scripts/build.sh ||
    fail "build script does not pass the resolved ORT profile to Meson"
grep -q 'onnxruntime_dependency_signature="$SSV_DEPS_SIGNATURE"' scripts/build.sh ||
    fail "build script does not pass the dependency signature to Meson"
grep -q "option('onnxruntime_profile'" meson.options ||
    fail "Meson does not expose the resolved ORT profile"
grep -q "option('onnxruntime_dependency_signature'" meson.options ||
    fail "Meson does not expose the dependency signature"
if rg -n 'TensorRT-Enterprise|default_url' scripts/deps/tensorrt-managed.sh; then
    fail "build script must not choose a default TensorRT SDK URL"
fi
grep -q 'ssv_deps_load_runtime' scripts/run.sh ||
    fail "run does not load the successful dependency snapshot"
grep -q 'ssv_deps_load_runtime' scripts/inspect.sh ||
    fail "inspect does not load the successful dependency snapshot"
if rg -n 'site-packages|nvidia/.*/lib|nvidia/\*/lib' scripts/lib.sh; then
    fail "runtime script must not scan Python NVIDIA wheel paths"
fi

grep -q 'config/model-labels/coco80.txt' config/ssv.example.yaml ||
    fail "YAML config does not default to the COCO label map"
grep -q 'scripts/model/verify_helmet_models.py' docs/安全帽模型验证说明.md ||
    fail "helmet verification docs do not use scripts/model path"
grep -q 'class SsvSourceMeta' gst/ssv-common/include/ssv_meta.hpp ||
    fail "source-scoped perception metadata contract is missing"
grep -q 'std::make_shared<const SsvTrackedFrame>' gst/ssv-common/meta/ssv_meta.cpp ||
    fail "immutable tracked snapshot sharing is missing"
for legacy_contract in \
    gst/ssv-common/include/ssv_frame_types.hpp \
    gst/ssv-common/include/ssv_timeline.hpp \
    gst/ssv-common/include/ssv_result_channels.hpp \
    gst/ssv-common/include/ssv_result_exchange.hpp \
    gst/ssv-common/ssv_result_channels.cpp \
    gst/ssv-common/ssv_result_exchange.cpp; do
    [ ! -e "$legacy_contract" ] ||
        fail "legacy perception metadata contract still exists: $legacy_contract"
done
grep -q "subdir('ssv-overlay')" gst/meson.build ||
    fail "overlay plugin is not included in Meson"
grep -q 'GST_ELEMENT_REGISTER_DEFINE(ssv_overlay, "ssvoverlay"' \
    gst/ssv-overlay/gstssvoverlay.cpp || fail "ssvoverlay plugin is missing"

if rg -n 'builddir' ssv scripts README.md .env.example; then
    fail "scripts or docs still reference builddir"
fi
grep -q 'SSV_BUILD_DIR.*build' scripts/lib.sh ||
    fail "scripts/lib.sh does not define SSV_BUILD_DIR"
grep -q 'rm -rf.*SSV_BUILD_DIR' scripts/clean.sh ||
    fail "scripts/clean.sh does not remove SSV_BUILD_DIR"

grep -q 'ssv_deps_prepare' scripts/build.sh ||
    fail "build script does not prepare dependencies through deps.sh"
awk '
    /rm -rf -- "\$SSV_BUILD_DIR"/ { cleanup = NR }
    /^[[:space:]]*ssv_deps_prepare / { prepare = NR }
    END { exit !(cleanup && prepare && cleanup < prepare) }
' scripts/build.sh ||
    fail "build must reset invalid Meson state before dependency preparation"
grep -q 'downloads/opencv' scripts/deps/opencv-managed.sh ||
    fail "OpenCV source archive is not cached by its provider"
grep -q 'SSV_OPENCV_SOURCE_URL' scripts/deps/opencv-managed.sh ||
    fail "OpenCV source URL is not owned by its provider"
grep -q 'ssv_opencv_configure_and_build' scripts/deps/opencv-managed.sh ||
    fail "OpenCV provider does not own the CMake build flow"
grep -q -- '-DCMAKE_BUILD_TYPE=Release' scripts/deps/opencv-managed.sh ||
    fail "OpenCV provider does not configure a Release build"
grep -q 'source_dir=.*opencv-' scripts/deps/opencv-managed.sh ||
    fail "OpenCV provider does not keep a versioned source tree"
grep -q 'workspace_root/build' scripts/deps/opencv-managed.sh ||
    fail "OpenCV provider does not keep a dedicated build tree"
grep -q 'workspace_root/install' scripts/deps/opencv-managed.sh ||
    fail "OpenCV provider does not keep a dedicated install tree"
grep -q 'SSV_OPENCV_BUILD_JOBS' scripts/deps/opencv-managed.sh ||
    fail "OpenCV provider does not expose build parallelism"
grep -q 'ssv_deps_atomic_replace_dir' scripts/deps/opencv-managed.sh ||
    fail "OpenCV provider does not publish candidates atomically"
grep -q 'ssv_deps_require_replaceable_root' scripts/deps/opencv-managed.sh ||
    fail "OpenCV provider does not protect existing roots"
grep -q 'pc_dir/opencv4.pc' scripts/deps/opencv-managed.sh ||
    fail "OpenCV provider does not generate opencv4.pc"
if rg -n 'dpkg-deb' scripts/deps/opencv-managed.sh; then
    fail "OpenCV provider must not depend on Debian package extraction"
fi
grep -q "option('opencv_mode'" meson.options ||
    fail "Meson does not expose the unified OpenCV mode"
grep -q "opencv_mode = get_option('opencv_mode')" meson.build ||
    fail "Meson does not read the unified OpenCV mode"
grep -q 'opencv_enabled' meson.build ||
    fail "Meson does not gate OpenCV discovery by build mode"
grep -q 'opencv_enabled' gst/ssv-track/meson.build ||
    fail "track does not gate GMC on OpenCV mode"
grep -q 'opencv_enabled' gst/tests/meson.build ||
    fail "tests do not gate OpenCV on build mode"

if rg -n \
    'SSV_ONNXRUNTIME_FLAVOR|SSV_TENSORRT_VERSION|SSV_OPENCV=|SSV_TENSORRT=' \
    scripts .env.example README.md; then
    fail "legacy dependency variable names remain"
fi
if rg -n "option\\('(opencv|tensorrt)'|-D(opencv|tensorrt)=" \
    meson.options meson.build gst scripts README.md .env.example; then
    fail "legacy Meson dependency options remain"
fi
if rg -n "method *: *'cmake'|find_library|\\.deps.*onnxruntime|TensorRT-[0-9].*/lib" \
    meson.build gst/tests/meson.build; then
    fail "Meson still contains SDK discovery fallbacks or fixed paths"
fi
