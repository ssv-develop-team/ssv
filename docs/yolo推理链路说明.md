# YOLO 推理链路说明

本文说明当前 `runner`、`ssvinfer` 和推理服务之间的真实边界。当前正式模型源文件是原始社区 ONNX；旧的 wrapper/prepare 方案不再属于运行时流程。

## 结论

- `ssvinfer` 是 GStreamer 适配器，不直接创建 ONNX Runtime session，也不负责读取模型配置。
- 正式 ONNX 推理接受唯一输入为静态 `float32 [1,3,H,W]`、NCHW 的原始 ONNX，并要求输出为静态 float32 tensor。
- runner 分析分支先生成模型尺寸的 RGBA canvas；推理服务按 `inference.model.preprocess` 将它转换为复用的 float32 NCHW buffer。
- ONNX Runtime backend 负责 session 和执行；直接 TensorRT backend 只加载已有 engine 和 schema v2 manifest。
- `YoloOutputParser` 支持 `yolov5`、`yolov8` 和 `yolo_nx6` 三种输出配置，输出框在模型画布上执行过滤、类别筛选和 NMS 后映射回源图。

## 数据流

```text
RTSP H.264
  -> rtspsrc / depay / parser / decoder
  -> analysis queue / videorate
  -> resize / letterbox / color conversion
  -> model-sized RGBA canvas
  -> SsvInferenceService
  -> SsvImagePreprocessor
  -> float32 NCHW host buffer
  -> ONNX Runtime or TensorRT backend
  -> YoloOutputParser
  -> model-canvas filtering and NMS
  -> source-coordinate detections
  -> SsvSourceMeta
  -> optional ssvtrack / ssvpub
```

分析分支由 `runner/pipeline/ssv_pipeline_builder.cpp` 组装。显示分支与分析分支从 decoded tee 分开，显示窗口不是模型输入的所有权方。

## `ssvinfer` 的边界

插件文件为 `gst/ssv-infer/plugin/gstssvinfer.cpp`。插件的主要职责是：

1. 绑定 `source-id` 和 `SsvSourceContext`。
2. 接收已经协商好的 RGBA buffer 和 frame timing。
3. 调用 `SsvInferenceService::create_analysis_frame()` 创建分析帧。
4. 提交 `SsvInferenceRequest`，等待完成、替换、取消或失败状态。
5. 将完成的 `SsvDetectionFrame` 发布到 source metadata。

插件不拥有 ONNX Runtime session、TensorRT engine、模型路径、类别表、预处理计划或后处理参数。`mock-detect` 只用于测试插件和 metadata 流转，不能验证真实模型契约或输出数值。

## 原始 ONNX 输入契约

`ssv_model_input_contract_validate()` 在推理服务启动阶段读取模型 I/O 并检查：

| 项目 | 要求 |
| --- | --- |
| 输入数量 | 恰好一个 graph input |
| 输入 dtype | `float32` |
| 输入 layout | `NCHW` |
| 输入 shape | 静态 `[1,3,H,W]`，`H`、`W` 为正数 |
| 输出数量 | 至少一个 |
| 输出 dtype | 全部为 `float32` |
| custom metadata | 不要求 `ssv.wrapper.*` |

不满足契约的模型会在 session、scheduler 创建前失败；runner 不会根据文件名、producer 名称或 ONNX metadata 猜测输入语义。

## 预处理配置

模型语义由 YAML 显式声明：

```yaml
inference:
  model:
    path: "models/yolov8n.onnx"
    family: "yolo"
    output_format: "yolov8"
    label_map: "config/model-labels/coco80.txt"
    preprocess:
      color_order: "rgb"
      resize: "letterbox"
      execution: "auto"
      normalization:
        scale: 0.00392156862745098
        mean: [0.0, 0.0, 0.0]
        std: [1.0, 1.0, 1.0]
```

字段含义：

- `color_order`：`rgb` 或 `bgr`，决定 RGBA canvas 的三个颜色通道写入顺序。
- `resize`：`letterbox` 或 `stretch`，决定视频前端生成模型 canvas 时的几何策略。
- `normalization.scale`：像素先乘以该值；旧的 `/255` 语义是 `0.00392156862745098`。
- `normalization.mean/std`：逐通道执行 `(pixel * scale - mean) / std`。
- `execution`：`cpu`、`cuda` 或 `auto`。CPU 始终可用；`auto` 只在直接 TensorRT CUDA adapter 可用时使用硬件预处理。

运行时不会修改 ONNX 图，也不会创建中间 wrapper 文件。ONNX Runtime（包括 CUDA/TensorRT EP）当前使用 CPU 预处理；直接 TensorRT 可按能力使用 CUDA 预处理，但不承诺零拷贝。

## Letterbox 与坐标

当 `resize: letterbox` 时，前端按源图和模型尺寸计算保持比例的 canvas，并保存 `PreprocessTransform`。模型输出框先在模型画布坐标中执行 NMS，再映射回源图：

```text
source_x = (model_x - pad_left) / scale
source_y = (model_y - pad_top)  / scale
```

最后按源图宽高归一化到 `[0,1]`，裁剪到有效范围；完全无效或宽高不为正的框会被丢弃。`stretch` 不添加 padding，但仍使用相同的源图坐标映射边界。

## 推理服务生命周期

`SsvInferenceService` 启动时按以下顺序工作：

1. 校验 `inference` 配置和模型路径。
2. 读取 `inference.model.label_map`。
3. 创建 ONNX Runtime 或 TensorRT backend。
4. 读取模型输入、输出和 tensor specs。
5. 校验原始 ONNX 的 float32 NCHW 输入契约。
6. 创建 `PreprocessPlan`，分配并复用 host float buffer。
7. 创建 session/engine、warmup，并启动 `SsvLatestFrameScheduler` worker。

模型路径、runtime、输出格式、类别表、阈值和预处理参数都属于 YAML 的 `inference` 段，不属于 `ssvinfer` GObject 属性。

## 最新帧调度

`SsvLatestFrameScheduler` 保留一个 in-flight task 和一个 pending task：

```text
submit A -> worker 执行 A
submit B -> B 等待
submit C -> B 标记为 Replaced，C 等待
worker 完成 A -> 执行 C
```

分析分支还会按 `inference.analysis_fps` 做 drop-only 限流；推理速度不足时，旧 pending 帧会被替换。被替换或取消的 frame 不会发布真实检测结果，完成结果保留自己的 `frame_id`、`source_id` 和 timing。

推理异常由 scheduler 记录为失败，插件发布当前 frame 的空检测并保持 pipeline 流动；模型契约、caps、source geometry 或 metadata 绑定错误属于启动/提交错误，可能使 pipeline 失败。

## 输出格式

`inference.model.output_format` 必须与实际输出 shape 和语义一致。

### `yolov8`

期望 shape 为：

```text
[1, 4 + num_classes, num_anchors]
```

每个 anchor 按 channel-first 读取，取最高类别分数作为 confidence，不单独读取 objectness。坐标可以是归一化坐标或模型画布像素坐标，parser 会转换为角点。

### `yolov5`

期望 shape 为：

```text
[1, num_anchors, 5 + num_classes]
```

每行前四项是 `cx, cy, w, h`，第 5 项是 objectness，类别 confidence 为 `objectness * class_score`。

### `yolo_nx6`

期望 shape 为：

```text
[1, num_detections, 6]
```

每行按以下顺序读取：

```text
[x1, y1, x2, y2, score, class_id]
```

这是端到端检测输出时的配置，不应仅根据模型文件名选择。

## 类别表、置信度与 NMS

`inference.model.label_map` 是逐行类别表，空行和 `#` 开头的行会跳过，剩余行号就是 `class_id`。

- `target_class: ""`：不过滤类别。
- 非空 `target_class`：必须精确匹配 label map 中的一行，否则服务启动失败。
- `yolov8` 和 `yolov5`：输出类别数必须等于 label map 行数。
- `yolo_nx6`：类别 ID 由输出提供，label map 用于命名和目标类别过滤。

候选框先执行 confidence 和类别过滤，再按 confidence 降序做同类别 NMS：

```text
IoU(candidate, selected) > 0.45 && same class -> suppress
```

不同类别之间不会互相抑制，最终最多保留 `50` 个框。这两个值目前是 parser 内部常量，不是 YAML 配置项。

## TensorRT manifest

TensorRT backend 只加载已经构建好的 `.engine`。engine 应在最终运行机器上从同一个原始 ONNX 构建，再生成 schema v2 manifest：

```bash
trtexec \
  --onnx=models/yolov8n.onnx \
  --saveEngine=models/yolov8n.engine \
  --fp16

./ssv model manifest \
  --model models/yolov8n.onnx \
  --engine models/yolov8n.engine \
  --output models/yolov8n.engine.json \
  --precision fp16 \
  --tensorrt-version 10.16.1.11 \
  --cuda-runtime-version 13020 \
  --compute-capability 8.9
```

manifest 记录 engine SHA-256、原始 ONNX SHA-256、精度、TensorRT/CUDA 版本、compute capability 和 engine 输入 I/O；不承载颜色顺序、归一化、resize 或输出格式。运行时不会把 `.onnx` 临时转成 `.engine`。

## 检查与测试

```bash
./ssv model export
./ssv build --profile cpu
./ssv inspect
./ssv test
```

按模块测试：

```bash
meson test -C build --print-errorlogs
python -m unittest discover -s scripts/ssv_cli/tests -p 'test_*.py'
cd agent && uv run --extra dev pytest
```

真实 RTSP、GPU Provider、TensorRT engine、显示设备和模型效果仍需要在对应环境中单独验证。
