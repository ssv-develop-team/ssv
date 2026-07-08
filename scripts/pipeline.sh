#!/bin/bash
# scripts/pipeline.sh — 运行整套 GStreamer 链路 / 有界 smoke
#   用法:
#     ./ssv run             RTSP → 推理 → 跟踪 → Redis
#     ./ssv run --display   同一链路额外打开视频观察窗口
#     ./ssv test            由 scripts/test.sh 调用的有界 smoke 路径
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

MODE="run"
SHOW_DISPLAY=false
DISPLAY_OVERLAY="$(ssv_yaml_get display.overlay false)"
DISPLAY_SINK_OVERRIDE=""
SKIP_BUILD=false

case "${1:-}" in
    "") ;;
    --run)
        MODE="run"
        shift
        ;;
    --smoke|--test)
        MODE="smoke"
        shift
        ;;
    --skip-build)
        SKIP_BUILD=true
        shift
        ;;
    *)
        ssv_error "未知 test/run 参数: $1"
        exit 1
        ;;
esac

while [ "$#" -gt 0 ]; do
    case "$1" in
        --display)
            SHOW_DISPLAY=true
            shift
            ;;
        --overlay)
            SHOW_DISPLAY=true
            DISPLAY_OVERLAY=true
            shift
            ;;
        --sink)
            if [ -z "${2:-}" ]; then
                ssv_error "--sink requires a sink name"
                exit 1
            fi
            SHOW_DISPLAY=true
            DISPLAY_SINK_OVERRIDE="$2"
            shift 2
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        *)
            ssv_error "未知 test/run 参数: $1"
            exit 1
            ;;
    esac
done

if [ "$SHOW_DISPLAY" = false ] && [ "$(ssv_yaml_get display.enabled false)" = "true" ]; then
    SHOW_DISPLAY=true
fi

ssv_header "检查 GStreamer Pipeline"

ssv_require_command "gst-launch-1.0" \
    "sudo apt-get install gstreamer1.0-tools" \
    "Debian/Ubuntu"

if [ "$MODE" = "smoke" ]; then
    ssv_require_command "timeout" \
        "sudo apt-get install coreutils" \
        "Debian/Ubuntu"
fi

if [ "$SKIP_BUILD" = false ]; then
    bash "$SSV_ROOT/scripts/build.sh"
fi

export_ssv_plugin_path

RTSP_URL="${SSV_RTSP_URL:-$(ssv_yaml_get sources.0.uri "")}"
if [ -z "$RTSP_URL" ]; then
    ssv_error "RTSP 视频源未配置"
    ssv_warn "在 ssv.yaml 设置 sources[0].uri，或临时设置 SSV_RTSP_URL"
    exit 1
fi

MODEL="$(ssv_yaml_get inference.model_path models/yolov8n.onnx)"
TARGET_CLASS="$(ssv_yaml_get inference.target_class person)"
LABEL_MAP="$(ssv_yaml_get inference.label_map config/model-labels/coco80.txt)"
if [ ! -f "$MODEL" ]; then
    ssv_error "模型文件不存在: $MODEL"
    ssv_warn "运行 ./ssv download-model 下载模型，或在 ssv.yaml 设置 inference.model_path"
    exit 1
fi

if [ -n "$LABEL_MAP" ] && [ ! -f "$LABEL_MAP" ]; then
    ssv_error "类别表文件不存在: $LABEL_MAP"
    ssv_warn "设置 inference.label_map，或使用默认 config/model-labels/coco80.txt"
    exit 1
fi

if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q '^ssv-redis$'; then
    ssv_warn "Redis 未运行，自动启动..."
    bash "$SSV_ROOT/scripts/redis.sh"
    sleep 2
fi

FRAME_WIDTH="$(ssv_yaml_get pipeline.frame_width 640)"
FRAME_HEIGHT="$(ssv_yaml_get pipeline.frame_height 480)"
DISPLAY_FPS="$(ssv_yaml_get display.fps 30)"
ANALYSIS_FPS="$(ssv_yaml_get pipeline.analysis_fps 5)"
CONF_THRESHOLD="$(ssv_yaml_get inference.confidence_threshold 0.5)"
INFER_RUNTIME="$(ssv_yaml_get inference.runtime auto)"
INFER_DEVICE="$(ssv_yaml_get inference.device auto)"
INFER_DEVICE_ID="$(ssv_yaml_get inference.device_id 0)"
INFER_PRECISION="$(ssv_yaml_get inference.precision auto)"
MODEL_FAMILY="$(ssv_yaml_get inference.model_family yolo)"
OUTPUT_FORMAT="$(ssv_yaml_get inference.output_format auto)"
RTSP_PROTOCOLS="$(ssv_yaml_get sources.0.protocols tcp)"
RTSP_LATENCY="$(ssv_yaml_get sources.0.latency_ms 200)"
REDIS_HOST="${REDIS_HOST:-$(ssv_yaml_get redis.host localhost)}"
REDIS_PORT="${REDIS_PORT:-$(ssv_yaml_get redis.port 6379)}"
REDIS_STREAM_KEY="$(ssv_yaml_get redis.stream_key ssv:events)"
CHECK_TIMEOUT="$(ssv_yaml_get pipeline.check_timeout 30s)"
GST_DEBUG_LEVEL="${GST_DEBUG:-$(ssv_yaml_get logging.cpp_debug_level "ssv*:4")}"

if [[ ! "$ANALYSIS_FPS" =~ ^[0-9]+$ ]]; then
    ssv_error "pipeline.analysis_fps 必须是非负整数: $ANALYSIS_FPS"
    exit 1
fi

resolve_display_sink() {
    if [ -n "$DISPLAY_SINK_OVERRIDE" ]; then
        echo "$DISPLAY_SINK_OVERRIDE"
        return 0
    fi

    local yaml_sink
    yaml_sink="$(ssv_yaml_get display.sink "")"
    if [ -n "$yaml_sink" ]; then
        echo "$yaml_sink"
        return 0
    fi

    if [ -n "${DISPLAY:-}" ] && gst-inspect-1.0 gtksink >/dev/null 2>&1; then
        echo "gtksink"
        return 0
    fi

    if [ -n "${WAYLAND_DISPLAY:-}" ] && gst-inspect-1.0 waylandsink >/dev/null 2>&1; then
        echo "waylandsink"
        return 0
    fi

    if [ -n "${DISPLAY:-}" ] && gst-inspect-1.0 ximagesink >/dev/null 2>&1; then
        echo "ximagesink"
        return 0
    fi

    if [ -n "${DISPLAY:-}" ] && gst-inspect-1.0 xvimagesink >/dev/null 2>&1; then
        echo "xvimagesink"
        return 0
    fi

    if [ -n "${DISPLAY:-}" ] && gst-inspect-1.0 glimagesink >/dev/null 2>&1; then
        echo "glimagesink"
        return 0
    fi

    if gst-inspect-1.0 autovideosink >/dev/null 2>&1; then
        echo "autovideosink"
        return 0
    fi

    ssv_error "no usable video sink found"
    exit 1
}

DISPLAY_SINK="$(resolve_display_sink)"
display_sink_args=("$DISPLAY_SINK")
if [ "$DISPLAY_SINK" != "gtksink" ]; then
    display_sink_args+=("sync=false")
fi

rtsp_decode_pipeline=(
    rtspsrc "location=$RTSP_URL" "protocols=$RTSP_PROTOCOLS" "latency=$RTSP_LATENCY"
    ! application/x-rtp,media=video
    ! decodebin
    ! queue "leaky=downstream" "max-size-buffers=2"
    ! videoconvert
)

infer_props=(
    ssvinfer
    "runtime=$INFER_RUNTIME"
    "model-path=$MODEL"
    "conf-threshold=$CONF_THRESHOLD"
    "label-map=$LABEL_MAP"
    "device=$INFER_DEVICE"
    "device-id=$INFER_DEVICE_ID"
    "precision=$INFER_PRECISION"
    "model-family=$MODEL_FAMILY"
    "output-format=$OUTPUT_FORMAT"
    "async=true"
)
if [ -n "$TARGET_CLASS" ]; then
    infer_props+=("target-class=$TARGET_CLASS")
fi

analysis_rate_pipeline=(
    ! videoscale
)
if [ "$ANALYSIS_FPS" -gt 0 ]; then
    analysis_rate_pipeline+=(
        ! videorate
        ! "video/x-raw,width=$FRAME_WIDTH,height=$FRAME_HEIGHT,framerate=$ANALYSIS_FPS/1,format=BGR"
    )
    ANALYSIS_FPS_LABEL="${ANALYSIS_FPS}fps"
else
    analysis_rate_pipeline+=(
        ! "video/x-raw,width=$FRAME_WIDTH,height=$FRAME_HEIGHT,format=BGR"
    )
    ANALYSIS_FPS_LABEL="不限流"
fi

analysis_pipeline=(
    "${analysis_rate_pipeline[@]}"
    ! ssvtemplate
    ! "${infer_props[@]}"
    ! ssvtrack
    ! ssvpub "redis-host=$REDIS_HOST" "redis-port=$REDIS_PORT" "stream-key=$REDIS_STREAM_KEY"
)

display_source_pipeline=(
    ! videoscale
    ! videorate
    ! "video/x-raw,framerate=$DISPLAY_FPS/1"
)

ssv_info "输入: $RTSP_URL"
ssv_info "RTSP transport: $RTSP_PROTOCOLS, latency: ${RTSP_LATENCY}ms"
ssv_info "显示帧率: ${DISPLAY_FPS}fps, 分析帧率: ${ANALYSIS_FPS_LABEL}"
ssv_info "模型: $MODEL"
ssv_info "推理运行时: $INFER_RUNTIME"
ssv_info "推理设备: $INFER_DEVICE (device-id=$INFER_DEVICE_ID, precision=$INFER_PRECISION)"
ssv_info "模型家族: $MODEL_FAMILY, 输出格式: $OUTPUT_FORMAT"
ssv_info "目标类别: ${TARGET_CLASS:-全部类别}"
ssv_info "类别表: ${LABEL_MAP:-内置 COCO}"
ssv_info "Redis Stream: $REDIS_STREAM_KEY"

if [ "$SHOW_DISPLAY" = true ]; then
    ssv_info "模式: 实时链路 + 视频观察窗口 (sink: $DISPLAY_SINK)"
    ssv_info "关闭视频窗口即退出"
    if [ "$DISPLAY_OVERLAY" = true ]; then
        ssv_warn "检测框 overlay 当前为实验路径；如窗口异常，去掉 --overlay"
        GST_DEBUG="$GST_DEBUG_LEVEL" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            ! tee name=t \
              t. ! queue "leaky=downstream" "max-size-buffers=2" \
                 "${display_source_pipeline[@]}" \
                 ! videoconvert ! "video/x-raw,format=BGRx" ! ssvoverlay ! videoconvert ! "video/x-raw,format=BGRx" ! "${display_sink_args[@]}" \
              t. ! queue "leaky=downstream" "max-size-buffers=2" \
                 "${analysis_pipeline[@]}" \
                 ! fakesink sync=false async=false
    else
        GST_DEBUG="$GST_DEBUG_LEVEL" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            ! tee name=t \
              t. ! queue "leaky=downstream" "max-size-buffers=2" \
                 "${display_source_pipeline[@]}" \
                 ! videoconvert ! "video/x-raw,format=BGRx" ! "${display_sink_args[@]}" \
              t. ! queue "leaky=downstream" "max-size-buffers=2" \
                 "${analysis_pipeline[@]}" \
                 ! fakesink sync=false async=false
    fi
else
    if [ "$MODE" = "smoke" ]; then
        ssv_info "模式: 链路冒烟测试 (timeout: $CHECK_TIMEOUT)"
    else
        ssv_info "模式: 实时链路无头运行"
    fi
    if [ "$MODE" = "smoke" ]; then
        set +e
        GST_DEBUG="$GST_DEBUG_LEVEL" \
        timeout --foreground "$CHECK_TIMEOUT" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            "${analysis_pipeline[@]}" \
            ! fakesink sync=false
        status=$?
        set -e
        if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
            exit "$status"
        fi
        ssv_info "链路冒烟测试完成"
    else
        GST_DEBUG="$GST_DEBUG_LEVEL" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            "${analysis_pipeline[@]}" \
            ! fakesink sync=false
    fi
fi

EVENT_COUNT=$(docker exec ssv-redis redis-cli XLEN "$REDIS_STREAM_KEY" 2>/dev/null || echo "?")
ssv_info "Redis $REDIS_STREAM_KEY 中累计 $EVENT_COUNT 条事件"
