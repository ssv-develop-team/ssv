#!/bin/bash
# scripts/download-model.sh — 下载 YOLOv8n ONNX 模型
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$SSV_ROOT"

ssv_header "下载 YOLO ONNX 模型"

MODEL_DIR="$SSV_ROOT/models"
MODEL_FILE="$MODEL_DIR/yolov8n.onnx"

if [ -f "$MODEL_FILE" ]; then
    ssv_info "模型已存在: $MODEL_FILE"
    exit 0
fi

mkdir -p "$MODEL_DIR"

# 方式一: 使用 ultralytics Python 包导出 (推荐)
if command -v uv &>/dev/null; then
    ssv_info "尝试使用 ultralytics 导出 YOLOv8n ONNX..."
    if uv run --with ultralytics python -c "
from ultralytics import YOLO
model = YOLO('yolov8n.pt')
model.export(format='onnx', imgsz=640, simplify=True)
import shutil, os
src = 'yolov8n.onnx'
if os.path.exists(src):
    shutil.move(src, '$MODEL_FILE')
    print('OK')
else:
    print('EXPORT_FAILED')
" 2>/dev/null | grep -q OK; then
        ssv_info "导出成功: $MODEL_FILE"
        exit 0
    fi
fi

# 方式二: 使用 pip + ultralytics
if command -v pip &>/dev/null; then
    ssv_info "尝试 pip 安装 ultralytics..."
    pip install -q ultralytics 2>/dev/null || true
    if python3 -c "
from ultralytics import YOLO
model = YOLO('yolov8n.pt')
model.export(format='onnx', imgsz=640, simplify=True)
import shutil, os
src = 'yolov8n.onnx'
if os.path.exists(src):
    shutil.move(src, '$MODEL_FILE')
    print('OK')
" 2>/dev/null | grep -q OK; then
        ssv_info "导出成功: $MODEL_FILE"
        exit 0
    fi
fi

ssv_error "自动下载失败"
ssv_warn "请手动下载 YOLOv8n ONNX 模型并放置到: $MODEL_FILE"
ssv_warn "推荐方式: pip install ultralytics && yolo export model=yolov8n.pt format=onnx imgsz=640"
exit 1
