#!/bin/bash
# scripts/t4-full-demo.sh — 一键运行 T4 完整演示（汇报友好模式）
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

VIDEO="/home/lzy/work-code/30sec.mp4"
MODEL="/home/lzy/work-code/comp-2-freeze10.pt"
OUTPUT_DIR="$SSV_ROOT/output/t4-full-demo"
RUN_INTERNAL=true
RUN_OPENAI=auto
OPEN_REPORT=true
VERBOSE=false

usage() {
    cat <<'EOF'
用法:
  ./ssv t4-full-demo [options]

默认演示:
  1. T4 内部闭环 mock demo
  2. 真实视频 + 本地 YOLO provider 复核
  3. 真实视频 + openai_compatible provider 复核（如果 .env 已配置）
  4. 生成并尝试打开 HTML 演示页

选项:
  --video PATH          演示视频路径，默认 /home/lzy/work-code/30sec.mp4
  --model PATH          安全帽 YOLO .pt 模型路径，默认 /home/lzy/work-code/comp-2-freeze10.pt
  --output-dir PATH     输出目录，默认 output/t4-full-demo
  --skip-internal       跳过 T4 内部 mock 闭环
  --skip-openai         跳过外部 AI provider 演示
  --require-openai      外部 AI provider 未配置或失败时直接失败
  --no-open-report      不自动打开 HTML 演示页
  --no-open-image       兼容旧参数；等同于 --no-open-report
  --verbose             打印子命令完整日志
  -h, --help            显示帮助
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --video)
            VIDEO="${2:?missing value for --video}"
            shift 2
            ;;
        --model)
            MODEL="${2:?missing value for --model}"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="${2:?missing value for --output-dir}"
            shift 2
            ;;
        --skip-internal)
            RUN_INTERNAL=false
            shift
            ;;
        --skip-openai)
            RUN_OPENAI=skip
            shift
            ;;
        --require-openai)
            RUN_OPENAI=require
            shift
            ;;
        --no-open-report|--no-open-image)
            OPEN_REPORT=false
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            ssv_error "未知参数: $1"
            usage
            exit 1
            ;;
    esac
done

run_step() {
    local log_file="$1"
    shift
    mkdir -p "$(dirname "$log_file")"
    if [ "$VERBOSE" = true ]; then
        "$@" | tee "$log_file"
    else
        if ! "$@" >"$log_file" 2>&1; then
            ssv_error "步骤失败，最近日志如下: $log_file"
            tail -80 "$log_file" >&2 || true
            return 1
        fi
    fi
}

open_file() {
    local target="$1"
    [ "$OPEN_REPORT" = true ] || return 0
    if [ ! -e "$target" ]; then
        ssv_warn "无法打开，文件不存在: $target"
        return 0
    fi
    if command -v xdg-open >/dev/null 2>&1 && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
        ssv_info "正在打开演示页: $target"
        nohup xdg-open "$target" >/dev/null 2>&1 &
        return 0
    fi
    if command -v gio >/dev/null 2>&1 && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
        ssv_info "正在打开演示页: $target"
        nohup gio open "$target" >/dev/null 2>&1 &
        return 0
    fi
    ssv_warn "当前环境没有可用图形显示，无法自动弹出；请手动打开: $target"
}

print_case_summary() {
    local title="$1"
    local folder="$2"
    uv run --project agent python -m ssv_agent.demos.report case-summary "$title" "$folder"
}

generate_report() {
    uv run --project agent python -m ssv_agent.demos.report generate "$OUTPUT_DIR"
}

ssv_header "T4 一键完整演示"

if ! command -v uv >/dev/null 2>&1; then
    ssv_error "uv not found"
    ssv_warn "请先安装 uv，例如: sudo snap install astral-uv --classic"
    exit 1
fi

if [ ! -f "$VIDEO" ]; then
    ssv_error "演示视频不存在: $VIDEO"
    exit 1
fi

if [ ! -f "$MODEL" ]; then
    ssv_error "模型文件不存在: $MODEL"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
rm -rf "$OUTPUT_DIR/internal" "$OUTPUT_DIR/local_yolo" "$OUTPUT_DIR/openai"
rm -f "$OUTPUT_DIR/redis.log" "$OUTPUT_DIR/internal.log" "$OUTPUT_DIR/local_yolo.log" \
    "$OUTPUT_DIR/openai.log" "$OUTPUT_DIR/summary.md" "$OUTPUT_DIR/report.html"

ssv_info "视频: $VIDEO"
ssv_info "模型: $MODEL"
ssv_info "输出目录: $OUTPUT_DIR"
ssv_info "详细日志会写入输出目录，终端只显示演示摘要。"

ssv_info "启动/检查 Redis..."
bash "$SSV_ROOT/scripts/redis.sh" >"$OUTPUT_DIR/redis.log" 2>&1
ssv_info "Redis 已就绪"

if [ ! -d agent/.venv ]; then
    ssv_info "安装 Agent Python 依赖（包含 vision extra）..."
    (cd agent && uv sync --extra dev --extra vision)
fi

if [ "$RUN_INTERNAL" = true ]; then
    ssv_header "阶段 1/3: T4 内部闭环"
    run_step "$OUTPUT_DIR/internal.log" \
        bash "$SSV_ROOT/scripts/t4-demo.sh" \
            --skip-local-yolo \
            --output-dir "$OUTPUT_DIR/internal"
    ssv_info "T4 内部闭环完成：passed 2/2"
else
    ssv_warn "跳过 T4 内部闭环"
fi

ssv_header "阶段 2/3: 真实视频 + 本地 YOLO provider"
run_step "$OUTPUT_DIR/local_yolo.log" \
    bash "$SSV_ROOT/scripts/t4-video-demo.sh" \
        --video "$VIDEO" \
        --model "$MODEL" \
        --output-dir "$OUTPUT_DIR/local_yolo"
print_case_summary "真实视频 + 本地 YOLO provider" "$OUTPUT_DIR/local_yolo"

OPENAI_READY="$(uv run --project agent python -m ssv_agent.demos.report openai-ready --env-file "$SSV_ROOT/.env")"

if [ "$RUN_OPENAI" != "skip" ] && [ "$OPENAI_READY" = "yes" ]; then
    ssv_header "阶段 3/3: 真实视频 + 外部 AI provider"
    run_step "$OUTPUT_DIR/openai.log" \
        bash "$SSV_ROOT/scripts/t4-video-demo.sh" \
            --video "$VIDEO" \
            --model "$MODEL" \
            --agent-provider openai_compatible \
            --output-dir "$OUTPUT_DIR/openai"
    print_case_summary "真实视频 + 外部 AI provider" "$OUTPUT_DIR/openai"
elif [ "$RUN_OPENAI" = "require" ]; then
    ssv_error "外部 AI provider 未配置完整，无法继续"
    exit 1
else
    ssv_header "阶段 3/3: 外部 AI provider"
    if [ "$RUN_OPENAI" = "skip" ]; then
        ssv_warn "按参数要求跳过 openai_compatible 演示"
    else
        ssv_warn "未配置完整 .env，跳过 openai_compatible 演示"
    fi
fi

ssv_header "生成演示汇总"
generate_report

open_file "$OUTPUT_DIR/report.html"

ssv_info "T4 一键演示完成"
