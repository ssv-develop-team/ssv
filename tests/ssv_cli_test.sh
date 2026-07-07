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
grep -q 'SSV_RTSP_PROTOCOLS=tcp' .env.example || fail ".env.example does not default RTSP transport to TCP"
grep -q 'rtspsrc' scripts/pipeline.sh || fail "pipeline script does not use explicit rtspsrc"
grep -q 'protocols=\$RTSP_PROTOCOLS' scripts/pipeline.sh || fail "pipeline script does not pass RTSP transport"
grep -q 'application/x-rtp,media=video' scripts/pipeline.sh || fail "pipeline script does not filter RTSP video stream"
grep -q 'videorate' scripts/pipeline.sh || fail "pipeline script does not normalize RTSP framerate"
grep -q 'SSV_ANALYSIS_FPS' scripts/pipeline.sh || fail "pipeline script does not expose analysis fps"
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
grep -q 'SSV_RUNTIME_BACKEND' scripts/pipeline.sh || fail "pipeline script does not expose runtime backend"
grep -q 'SSV_RUNTIME_DEVICE' scripts/pipeline.sh || fail "pipeline script does not expose runtime device"
grep -q 'RUNTIME_BACKEND="${SSV_RUNTIME_BACKEND:-onnx}"' scripts/pipeline.sh || fail "pipeline script does not default runtime backend to onnx"
grep -q 'runtime=\$RUNTIME_BACKEND' scripts/pipeline.sh || fail "pipeline script does not pass runtime backend to ssvinfer"
grep -q 'device=\$RUNTIME_DEVICE' scripts/pipeline.sh || fail "pipeline script does not pass runtime device to ssvinfer"
grep -q '^runtime:' config/ssv.default.yaml || fail "default config does not expose top-level runtime section"
grep -q 'backend: "onnx"' config/ssv.default.yaml || fail "default config does not default runtime backend to onnx"
grep -q 'device: "cpu"' config/ssv.default.yaml || fail "default config does not default runtime device to cpu"
if rg -n '^[[:space:]]+runtime:' config/ssv.default.yaml config/ssv.example.yaml >/tmp/ssv-nested-runtime-matches.txt; then
    cat /tmp/ssv-nested-runtime-matches.txt >&2
    fail "runtime must not be nested under inference"
fi
if rg -n 'SSV_CUDA_REQUIRED|cuda-required|device: "auto"|device=auto|str_equal\(device, "auto"\)|OrtCUDAProviderOptions|AppendExecutionProvider_CUDA|SSV_RUNTIME_DEVICE=auto|SSV_INFER_CUDA_DEVICE_ID=auto|runtime-device|onnx-cuda-device-id|cuda_device_id|cuda-device-id' scripts/pipeline.sh config/ssv.default.yaml config/ssv.example.yaml gst/ssv-infer/gstssvinfer.cpp >/tmp/ssv-cuda-coupling-matches.txt; then
    cat /tmp/ssv-cuda-coupling-matches.txt >&2
    fail "GPU entry must use runtime/device without legacy device naming or direct CUDA provider code in gstssvinfer"
fi
if rg -n 'SSV_CUDA_REQUIRED|SSV_INFER_RUNTIME_DEVICE|SSV_ONNX_CUDA_DEVICE_ID|SSV_RUNTIME_DEVICE=auto|SSV_INFER_CUDA_DEVICE_ID=auto|auto 优先 CUDA' .env.example >/tmp/ssv-env-cuda-matches.txt; then
    cat /tmp/ssv-env-cuda-matches.txt >&2
    fail ".env.example must document runtime/device based selection"
fi
grep -q 'SSV_ONNXRUNTIME_FLAVOR' scripts/build.sh || fail "build script does not expose ONNX Runtime flavor"
grep -q 'scripts/runtime/onnxruntime.sh' scripts/lib.sh || fail "runtime script does not source ONNX Runtime helper"
grep -q 'scripts/runtime/cuda.sh' scripts/lib.sh || fail "runtime script does not source CUDA helper"
grep -q 'SSV_ONNXRUNTIME_FLAVOR' scripts/runtime/onnxruntime.sh || fail "ONNX Runtime helper does not expose flavor"
grep -q 'onnxruntime-gpu' scripts/runtime/onnxruntime.sh || fail "ONNX Runtime helper does not select GPU path"
grep -q 'site-packages' scripts/runtime/cuda.sh || fail "CUDA helper does not discover Python site-packages"
grep -q 'nvidia/.*/lib' scripts/runtime/cuda.sh || grep -q 'nvidia/\*/lib' scripts/runtime/cuda.sh || fail "CUDA helper does not add NVIDIA wheel lib directories"
grep -q 'SSV_TARGET_CLASS' scripts/pipeline.sh || fail "pipeline script does not expose target class"
grep -q 'TARGET_CLASS="${SSV_TARGET_CLASS-person}"' scripts/pipeline.sh || fail "pipeline script does not preserve empty target class for all-class inference"
grep -q 'SSV_LABEL_MAP' scripts/pipeline.sh || fail "pipeline script does not expose label map"
grep -q 'if \[ -n "\$TARGET_CLASS" \]' scripts/pipeline.sh || fail "pipeline script does not omit empty target class"
grep -q 'infer_props+=("target-class=\$TARGET_CLASS")' scripts/pipeline.sh || fail "pipeline script does not pass non-empty target class to ssvinfer"
grep -q 'label-map=\$LABEL_MAP' scripts/pipeline.sh || fail "pipeline script does not pass label map to ssvinfer"
grep -q 'config/model-labels/coco80.txt' scripts/pipeline.sh || fail "pipeline script does not default to config/model-labels/coco80.txt"
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
