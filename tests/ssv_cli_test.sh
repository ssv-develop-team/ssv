#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail() {
    echo "test failed: $*" >&2
    exit 1
}

help_output="$(./ssv --help)"

grep -q "  clean" <<<"$help_output" || fail "help does not list clean"
grep -q "  run" <<<"$help_output" || fail "help does not list run"
grep -q "run --display" <<<"$help_output" || fail "help does not list run --display"
grep -q -- "--overlay" <<<"$help_output" || fail "help does not list --overlay"
grep -q -- "--sink" <<<"$help_output" || fail "help does not list --sink"
grep -q "  test" <<<"$help_output" || fail "help does not list test"
grep -q "运行代码测试和链路冒烟测试后退出" <<<"$help_output" || fail "help does not describe test as exit-style"

for legacy in "--m2" "--m2-mock" "--m3" "--m3-mock" "check" "all"; do
    if grep -q -- "$legacy" <<<"$help_output"; then
        fail "help still lists legacy command: $legacy"
    fi
done

grep -q 'SSV_RTSP_URL' .env.example || fail ".env.example does not document SSV_RTSP_URL"
grep -q 'config/ssv.yaml' scripts/lib.sh || fail "scripts/lib.sh does not search config/ssv.yaml"
grep -q 'config/ssv.yaml' .gitignore || fail ".gitignore does not ignore local config/ssv.yaml"
if rg -n 'SSV_RTSP_PROTOCOLS|SSV_RTSP_LATENCY|SSV_CHECK_TIMEOUT|SSV_DISPLAY_OVERLAY' .env.example >/tmp/ssv-env-runtime-matches.txt; then
    cat /tmp/ssv-env-runtime-matches.txt >&2
    fail ".env.example should not document YAML-owned runtime overrides"
fi
grep -q 'rtspsrc' scripts/pipeline.sh || fail "pipeline script does not use explicit rtspsrc"
grep -q 'protocols=\$RTSP_PROTOCOLS' scripts/pipeline.sh || fail "pipeline script does not pass RTSP transport"
grep -q 'application/x-rtp,media=video' scripts/pipeline.sh || fail "pipeline script does not filter RTSP video stream"
grep -q 'videorate' scripts/pipeline.sh || fail "pipeline script does not normalize RTSP framerate"
grep -q 'ssv_yaml_get pipeline.analysis_fps' scripts/pipeline.sh || fail "pipeline script does not read analysis fps from YAML"
grep -q 'ssv_yaml_get sources.0.protocols' scripts/pipeline.sh || fail "pipeline script does not read RTSP transport from YAML"
grep -q 'ssv_yaml_get sources.0.latency_ms' scripts/pipeline.sh || fail "pipeline script does not read RTSP latency from YAML"
grep -q 'display_source_pipeline' scripts/pipeline.sh || fail "display mode does not split before inference"
grep -q 'DISPLAY_OVERLAY' scripts/pipeline.sh || fail "display overlay is not controlled by an explicit switch"
if rg -n '^DISPLAY=' scripts/pipeline.sh >/tmp/ssv-display-var-matches.txt; then
    cat /tmp/ssv-display-var-matches.txt >&2
    fail "pipeline script must not overwrite the desktop DISPLAY environment variable"
fi
grep -q 'ssvoverlay' scripts/pipeline.sh || fail "display overlay mode does not enable detection overlay"
grep -q 'video/x-raw,format=BGRx' scripts/pipeline.sh || fail "display overlay branch does not use display-friendly BGRx format"
grep -q 'ssvoverlay ! videoconvert ! "video/x-raw,format=BGRx"' scripts/pipeline.sh || fail "display overlay branch does not force BGRx after overlay"
grep -q 'display_source_pipeline.*' scripts/pipeline.sh || fail "display source pipeline missing"
grep -q 'exec bash "$SCRIPTS_DIR/pipeline.sh" --run "$@"' ssv || fail "ssv does not pass run arguments through to pipeline script"
grep -q 'exec bash "$SCRIPTS_DIR/test.sh"' ssv || fail "ssv does not dispatch test command to the test orchestrator"
grep -q 'DISPLAY_SINK_OVERRIDE' scripts/pipeline.sh || fail "pipeline script does not use explicit display sink override"
grep -q 'gst-inspect-1.0 gtksink' scripts/pipeline.sh || fail "pipeline script does not prefer a GTK window sink"
grep -q '\$DISPLAY_SINK" != "gtksink"' scripts/pipeline.sh || fail "pipeline script should not force sync=false on gtksink"
if rg -n 'gtksink 使用单链路显示|\$DISPLAY_SINK" = "gtksink"' scripts/pipeline.sh >/tmp/ssv-gtksink-single-chain-matches.txt; then
    cat /tmp/ssv-gtksink-single-chain-matches.txt >&2
    fail "gtksink should use the same tee display path as other sinks"
fi
grep -q 'leaky=downstream' scripts/pipeline.sh || fail "display mode queues are not configured as leaky"
grep -q 'fakesink sync=false async=false' scripts/pipeline.sh || fail "display analysis branch fakesink should be async=false"
grep -q -- '--smoke' scripts/pipeline.sh || fail "pipeline script does not accept smoke mode"
grep -q -- '--skip-build' scripts/pipeline.sh || fail "pipeline script does not support skipping build for tests"
grep -q '#include <thread>' gst/ssv-infer/gstssvinfer.cpp || fail "ssvinfer does not use a worker thread"
grep -q 'PROP_ASYNC_INFER' gst/ssv-infer/gstssvinfer.cpp || fail "ssvinfer does not expose async inference"
grep -q 'latest_frame' gst/ssv-infer/gstssvinfer.cpp || fail "ssvinfer does not keep latest frame for async inference"
grep -q 'async=true' scripts/pipeline.sh || fail "pipeline script does not enable async inference"
grep -q 'ssv_yaml_get' scripts/lib.sh || fail "scripts/lib.sh does not expose YAML config reader"
grep -q 'ssv_yaml_get inference.model_path' scripts/pipeline.sh || fail "pipeline script does not read model path from YAML"
grep -q 'ssv_yaml_get pipeline.analysis_fps' scripts/pipeline.sh || fail "pipeline script does not read analysis fps from YAML"
grep -q 'analysis_rate_pipeline' scripts/pipeline.sh || fail "pipeline script does not build analysis rate branch"
grep -q 'ANALYSIS_FPS_LABEL="不限流"' scripts/pipeline.sh || fail "pipeline script does not support unlimited analysis fps"
grep -q 'framerate=\$ANALYSIS_FPS/1' scripts/pipeline.sh || fail "pipeline script does not keep capped analysis fps path"
grep -q 'ssv_yaml_get redis.stream_key' scripts/pipeline.sh || fail "pipeline script does not read Redis stream key from YAML"
if rg -n 'SSV_MODEL_PATH|SSV_TARGET_CLASS|SSV_LABEL_MAP|SSV_FRAME_WIDTH|SSV_FRAME_HEIGHT|SSV_ANALYSIS_FPS|SSV_CONF_THRESHOLD|SSV_INFER_RUNTIME|SSV_INFER_DEVICE|SSV_INFER_DEVICE_ID|SSV_INFER_PRECISION|SSV_MODEL_FAMILY|SSV_OUTPUT_FORMAT|SSV_RTSP_PROTOCOLS|SSV_RTSP_LATENCY|SSV_CHECK_TIMEOUT|SSV_DISPLAY_OVERLAY|SSV_DISPLAY_SINK|SSV_REDIS_STREAM_KEY|SSV_DISPLAY_FPS|SSV_CUDA_DEVICE_ID|SSV_CUDA_REQUIRED|cuda-device-id|cuda-required' scripts/pipeline.sh >/tmp/ssv-runtime-env-matches.txt; then
    cat /tmp/ssv-runtime-env-matches.txt >&2
    fail "pipeline script must not expose YAML-owned runtime environment overrides"
fi
if rg -n 'SSV_CUDA_DEVICE_ID|SSV_CUDA_REQUIRED|cuda-device-id|cuda-required' scripts/pipeline.sh >/tmp/ssv-cuda-config-matches.txt; then
    cat /tmp/ssv-cuda-config-matches.txt >&2
    fail "pipeline script must not expose CUDA-specific inference config"
fi
grep -q 'runtime=\$INFER_RUNTIME' scripts/pipeline.sh || fail "pipeline script does not pass runtime to ssvinfer"
grep -q 'device=\$INFER_DEVICE' scripts/pipeline.sh || fail "pipeline script does not pass device to ssvinfer"
grep -q 'device-id=\$INFER_DEVICE_ID' scripts/pipeline.sh || fail "pipeline script does not pass device id to ssvinfer"
grep -q 'precision=\$INFER_PRECISION' scripts/pipeline.sh || fail "pipeline script does not pass precision to ssvinfer"
grep -q 'model-family=\$MODEL_FAMILY' scripts/pipeline.sh || fail "pipeline script does not pass model family to ssvinfer"
grep -q 'output-format=\$OUTPUT_FORMAT' scripts/pipeline.sh || fail "pipeline script does not pass output format to ssvinfer"
grep -q 'source "$(dirname "$0")/deps.sh"' scripts/build.sh || fail "build script does not use the unified dependency entry point"
grep -q 'SSV_ONNXRUNTIME_VERSION=1.25.1-gpu' .env.example || fail ".env.example does not document the ONNX Runtime GPU version suffix"
grep -q 'managed 当前固定使用 OpenCV 4.10.0' .env.example || fail ".env.example does not document the managed OpenCV version"
grep -q 'opencv-managed.sh 的包来源、模块 SONAME 和运行库闭包' .env.example || fail ".env.example does not document managed OpenCV upgrade requirements"
grep -q 'SSV_TENSORRT_URL' .env.example || fail ".env.example does not document explicit TensorRT URL configuration"
grep -q 'SSV_TENSORRT_ARCHIVE' .env.example || fail ".env.example does not document TensorRT archive configuration"
grep -q 'SSV_DEPS_TENSORRT_MESON_MODE' scripts/build.sh || fail "build script does not pass the resolved TensorRT status to Meson"
if rg -n 'TensorRT-Enterprise|default_url' scripts/deps/tensorrt-managed.sh >/tmp/ssv-tensorrt-default-url-matches.txt; then
    cat /tmp/ssv-tensorrt-default-url-matches.txt >&2
    fail "build script must not choose a default TensorRT SDK URL"
fi
grep -q 'ssv_deps_load_runtime' scripts/pipeline.sh || fail "pipeline does not load the successful dependency snapshot"
grep -q 'ssv_deps_load_runtime' scripts/inspect.sh || fail "inspect does not load the successful dependency snapshot"
if rg -n 'site-packages|nvidia/.*/lib|nvidia/\*/lib' scripts/lib.sh; then
    fail "runtime script must not scan Python NVIDIA wheel paths"
fi
grep -q 'if \[ -n "\$TARGET_CLASS" \]' scripts/pipeline.sh || fail "pipeline script does not omit empty target class"
grep -q 'infer_props+=("target-class=\$TARGET_CLASS")' scripts/pipeline.sh || fail "pipeline script does not pass non-empty target class to ssvinfer"
grep -q 'label-map=\$LABEL_MAP' scripts/pipeline.sh || fail "pipeline script does not pass label map to ssvinfer"
grep -q 'config/model-labels/coco80.txt' config/ssv.example.yaml || fail "YAML config does not default to config/model-labels/coco80.txt"
grep -q 'scripts/model/verify_helmet_models.py' docs/安全帽模型验证说明.md || fail "helmet verification docs do not use scripts/model path"
grep -q 'Run YOLO ONNX inference on video frames' gst/ssv-infer/gstssvinfer.cpp || fail "ssvinfer metadata changed unexpectedly"
grep -q 'peek_latest' gst/ssv-common/include/ssv_meta.hpp || fail "detection store does not expose latest results for overlay"
grep -q "subdir('ssv-overlay')" gst/meson.build || fail "overlay plugin is not included in Meson"
grep -q 'GST_ELEMENT_REGISTER_DEFINE(ssv_overlay, "ssvoverlay"' gst/ssv-overlay/gstssvoverlay.cpp || fail "ssvoverlay plugin is missing"

if rg -n 'builddir' ssv scripts README.md .env.example >/tmp/ssv-builddir-matches.txt; then
    cat /tmp/ssv-builddir-matches.txt >&2
    fail "scripts or docs still reference builddir"
fi

grep -q 'SSV_BUILD_DIR.*build' scripts/lib.sh || fail "scripts/lib.sh does not define SSV_BUILD_DIR"
grep -q 'rm -rf.*SSV_BUILD_DIR' scripts/clean.sh || fail "scripts/clean.sh does not remove SSV_BUILD_DIR"

grep -q 'ssv_deps_prepare' scripts/build.sh || fail 'build script does not prepare dependencies through deps.sh'
awk '
    /rm -rf -- "\$SSV_BUILD_DIR"/ { cleanup = NR }
    /^[[:space:]]*ssv_deps_prepare$/ { prepare = NR }
    END { exit !(cleanup && prepare && cleanup < prepare) }
' scripts/build.sh || fail 'build script must reset an invalid Meson directory before writing the pending dependency snapshot'
grep -q 'downloads/opencv' scripts/deps/opencv-managed.sh || fail 'OpenCV packages are not cached by its provider'
grep -q 'SSV_OPENCV_PACKAGE_REVISION' scripts/deps/opencv-managed.sh || fail 'OpenCV package details are not private to its provider'
grep -q 'pc_dir/opencv4.pc' scripts/deps/opencv-managed.sh || fail 'OpenCV provider does not generate opencv4.pc'
if rg -n 'dpkg-deb' scripts/deps/opencv-managed.sh; then fail 'OpenCV provider must use ar and tar, not dpkg-deb'; fi
grep -q "option('opencv_mode'" meson.options || fail 'Meson does not expose the unified OpenCV mode'
grep -q "opencv_mode = get_option('opencv_mode')" meson.build || fail 'Meson does not read the unified OpenCV mode'
grep -q 'opencv_enabled' meson.build || fail 'Meson does not gate OpenCV discovery by build mode'
grep -q 'opencv_enabled' gst/ssv-track/meson.build || fail 'track does not gate GMC on OpenCV mode'
grep -q 'opencv_enabled' gst/tests/meson.build || fail 'tests do not gate OpenCV on build mode'

if rg -n 'SSV_ONNXRUNTIME_FLAVOR|SSV_TENSORRT_VERSION|SSV_OPENCV=|SSV_TENSORRT=' scripts .env.example README.md; then
    fail 'legacy dependency variable names remain in active scripts or user docs'
fi
if rg -n "option\('(opencv|tensorrt)'|-D(opencv|tensorrt)=" meson.options meson.build gst scripts README.md .env.example; then
    fail 'legacy Meson dependency options remain'
fi
if rg -n "method *: *'cmake'|find_library|\.deps.*onnxruntime|TensorRT-[0-9].*/lib" meson.build gst/tests/meson.build; then
    fail 'Meson still contains SDK discovery fallbacks or fixed paths'
fi
