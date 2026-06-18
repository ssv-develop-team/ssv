#!/bin/bash
# scripts/t4-yolo-compare.sh — OpenCV 对比显示安全帽 YOLO 检测
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

VIDEO="/home/lzy/work-code/30sec.mp4"
MODEL="/home/lzy/work-code/comp-2-freeze10.pt"
OUTPUT_DIR="$SSV_ROOT/output/t4-demo"

usage() {
    cat <<'EOF'
用法:
  ./ssv t4-yolo-compare [options]

默认:
  使用 /home/lzy/work-code/30sec.mp4 和 /home/lzy/work-code/comp-2-freeze10.pt
  左侧显示原视频，右侧显示 YOLO 标注结果
  红框 = 不带头盔/head，绿框 = helmet
  输出保存到 output/t4-demo/

选项:
  --video PATH       视频路径
  --model PATH       YOLO .pt 模型路径
  --output-dir PATH  输出目录，默认 output/t4-demo
  --conf FLOAT       置信度阈值，默认 0.25
  --device DEVICE    cpu / cuda:0，默认 cpu
  --stride N         每 N 帧推理一次，默认 1
  --max-frames N     最多处理 N 帧，默认 0 表示完整视频
  --no-display       不弹 OpenCV 窗口，只保存视频和图片
  -h, --help         显示帮助
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
        --conf|--device|--stride|--max-frames|--no-display)
            break
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

ssv_header "T4 YOLO 视频对比检测"

if ! command -v uv >/dev/null 2>&1; then
    ssv_error "uv not found"
    ssv_warn "请先安装 uv，例如: sudo snap install astral-uv --classic"
    exit 1
fi

if [ ! -d agent/.venv ]; then
    ssv_info "安装 Agent Python 依赖（包含 vision extra）..."
    (cd agent && uv sync --extra dev --extra vision)
fi

ssv_info "视频: $VIDEO"
ssv_info "模型: $MODEL"
ssv_info "输出目录: $OUTPUT_DIR"
ssv_info "红框 = 不带头盔/head；绿框 = helmet；窗口内按 q 或 ESC 退出"

uv run --project agent --extra vision python -m ssv_agent.demos.yolo_compare \
    --video "$VIDEO" \
    --model "$MODEL" \
    --output-dir "$OUTPUT_DIR" \
    "$@"
