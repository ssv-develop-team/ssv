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
DISPLAY=false
DISPLAY_OVERLAY="${SSV_DISPLAY_OVERLAY:-false}"
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
            DISPLAY=true
            shift
            ;;
        --overlay)
            DISPLAY=true
            DISPLAY_OVERLAY=true
            shift
            ;;
        --sink)
            if [ -z "${2:-}" ]; then
                ssv_error "--sink requires a sink name"
                exit 1
            fi
            DISPLAY=true
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

RTSP_URL="${SSV_RTSP_URL:-}"
if [ -z "$RTSP_URL" ]; then
    ssv_error "SSV_RTSP_URL 未设置"
    ssv_warn "在 .env 中设置: SSV_RTSP_URL=rtsp://user:pass@host:554/stream"
    exit 1
fi

MODEL="${SSV_MODEL_PATH:-models/yolov8n.onnx}"
if [ ! -f "$MODEL" ]; then
    ssv_error "模型文件不存在: $MODEL"
    ssv_warn "运行 ./ssv download-model 下载模型，或设置 SSV_MODEL_PATH"
    exit 1
fi

if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q '^ssv-redis$'; then
    ssv_warn "Redis 未运行，自动启动..."
    bash "$SSV_ROOT/scripts/redis.sh"
    sleep 2
fi

FRAME_WIDTH="${SSV_FRAME_WIDTH:-640}"
FRAME_HEIGHT="${SSV_FRAME_HEIGHT:-480}"
DISPLAY_FPS="${SSV_DISPLAY_FPS:-30}"
ANALYSIS_FPS="${SSV_ANALYSIS_FPS:-5}"
CONF_THRESHOLD="${SSV_CONF_THRESHOLD:-0.5}"
RTSP_PROTOCOLS="${SSV_RTSP_PROTOCOLS:-tcp}"
RTSP_LATENCY="${SSV_RTSP_LATENCY:-200}"
REDIS_HOST="${REDIS_HOST:-localhost}"
REDIS_PORT="${REDIS_PORT:-6379}"
REDIS_STREAM_KEY="${SSV_REDIS_STREAM_KEY:-ssv:events}"
CHECK_TIMEOUT="${SSV_CHECK_TIMEOUT:-30s}"

resolve_display_sink() {
    if [ -n "$DISPLAY_SINK_OVERRIDE" ]; then
        echo "$DISPLAY_SINK_OVERRIDE"
        return 0
    fi

    if [ -n "${SSV_DISPLAY_SINK:-}" ]; then
        echo "$SSV_DISPLAY_SINK"
        return 0
    fi

    if [ -n "${WAYLAND_DISPLAY:-}" ] && gst-inspect-1.0 waylandsink >/dev/null 2>&1; then
        echo "waylandsink"
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

rtsp_decode_pipeline=(
    rtspsrc "location=$RTSP_URL" "protocols=$RTSP_PROTOCOLS" "latency=$RTSP_LATENCY"
    ! application/x-rtp,media=video
    ! decodebin
    ! queue "leaky=downstream" "max-size-buffers=2"
    ! videoconvert
)

analysis_pipeline=(
    ! videoscale
    ! videorate
    ! "video/x-raw,width=$FRAME_WIDTH,height=$FRAME_HEIGHT,framerate=$ANALYSIS_FPS/1,format=BGR"
    ! ssvtemplate
    ! ssvinfer "model-path=$MODEL" "conf-threshold=$CONF_THRESHOLD" "async=true"
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
ssv_info "显示帧率: ${DISPLAY_FPS}fps, 分析帧率: ${ANALYSIS_FPS}fps"
ssv_info "模型: $MODEL"
ssv_info "Redis Stream: $REDIS_STREAM_KEY"

if [ "$DISPLAY" = true ]; then
    ssv_info "模式: 实时链路 + 视频观察窗口 (sink: $DISPLAY_SINK)"
    ssv_info "关闭视频窗口即退出"
    if [ "$DISPLAY_OVERLAY" = true ]; then
        ssv_warn "检测框 overlay 当前为实验路径；如窗口异常，去掉 --overlay"
        GST_DEBUG="${GST_DEBUG:-ssv*:4}" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            ! tee name=t \
              t. ! queue "leaky=downstream" "max-size-buffers=2" \
                 "${display_source_pipeline[@]}" \
                 ! videoconvert ! "video/x-raw,format=BGRx" ! ssvoverlay ! videoconvert ! "$DISPLAY_SINK" sync=false \
              t. ! queue "leaky=downstream" "max-size-buffers=2" \
                 "${analysis_pipeline[@]}" \
                 ! fakesink sync=false
    else
        GST_DEBUG="${GST_DEBUG:-ssv*:4}" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            ! tee name=t \
              t. ! queue "leaky=downstream" "max-size-buffers=2" \
                 "${display_source_pipeline[@]}" \
                 ! videoconvert ! "$DISPLAY_SINK" sync=false \
              t. ! queue "leaky=downstream" "max-size-buffers=2" \
                 "${analysis_pipeline[@]}" \
                 ! fakesink sync=false
    fi
else
    if [ "$MODE" = "smoke" ]; then
        ssv_info "模式: 链路冒烟测试 (timeout: $CHECK_TIMEOUT)"
    else
        ssv_info "模式: 实时链路无头运行"
    fi
    if [ "$MODE" = "smoke" ]; then
        set +e
        GST_DEBUG="${GST_DEBUG:-ssv*:4}" \
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
        GST_DEBUG="${GST_DEBUG:-ssv*:4}" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            "${analysis_pipeline[@]}" \
            ! fakesink sync=false
    fi
fi

EVENT_COUNT=$(docker exec ssv-redis redis-cli XLEN "$REDIS_STREAM_KEY" 2>/dev/null || echo "?")
ssv_info "Redis $REDIS_STREAM_KEY 中累计 $EVENT_COUNT 条事件"
