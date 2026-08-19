# Site Safety Vision

安全帽佩戴视频监测分析系统。当前项目以 GStreamer C++ 插件承载实时视频分析链路，Redis Streams 作为实时链路和 Agent 复核链路之间的异步边界，Python 服务负责事件消费和后续智能复核编排。

`./ssv` 是统一开发入口。shell 负责构建、测试、依赖快照、模型准备和本地服务；长期实时链路由 C++ `ssv-runner` 负责配置校验、pipeline 构建、GTK、错误处理、结构化日志和资源释放。

## 架构概览

```text
RTSP / 后续文件输入
    |
    v
./ssv -> scripts/run.sh -> build/runner/ssv-runner
    |
    v
C++ pipeline 与运行时
    rtspsrc -> H.264 depay/parse -> resolved decoder -> tee
        |                                           |
        |                                           +-> GTK 视频层 + 独立框层
        +-> model canvas RGBA -> ssvinfer -> ssvtrack -> ssvpub
                                                   |
                                                   v
                                           Redis Streams
                                                   |
                                                   v
                                           Python Agent 服务
```

显示分支与分析分支各自使用容量为 1 的 downstream-leaky queue。GTK 显示框由透明 `GtkDrawingArea` 绘制，不修改视频像素；`ssvoverlay` 只保留给外部 SystemMemory 兼容 pipeline。完整架构见 [系统设计文档](docs/specs/2026-05-21-安全帽佩戴视频监测分析系统设计.md)，阶段计划见 [Roadmap](docs/roadmap.md)。

## 当前能力

- C++ `ssv-runner`：严格 YAML、纯值 pipeline plan、GStreamer/GTK 生命周期、memory contract probe、稳定退出码与结构化日志。
- C++ GStreamer 插件：`ssvtemplate`、`ssvinfer`、`ssvtrack`、`ssvpub`、`ssvoverlay`；正式 runner 中插件保持薄 adapter。
- 共享 C++ 模块：配置、分析帧 lease、检测/跟踪元数据、展示选择与运动预测。
- ONNX Runtime 正式推理路径：单一 runtime profile、Provider chain、session/cache key、latest-pending worker 和五秒统计窗口。
- 独立 TensorRT 实验对照：严格 engine manifest、固定 CUDA 资源复用和无 SDK unavailable stub。
- 模型尺寸 `uint8 RGBA NHWC` wrapper 输入；完整原始帧不在加速路径映射到 CPU。
- Redis Streams 发布插件和 Python Agent 消费基线。
- Docker Redis 开发环境。
- `./ssv` 统一入口脚本。
- C++ runner/插件测试、Agent 单元测试、CLI 与依赖脚本测试。

尚未完成：各 GPU profile 真机人工验收、完整事件判定、证据输出、真实安全帽专用模型、完整 Agent 状态机、工具调用、知识库和端到端报告闭环。

## 依赖

| 依赖 | 版本要求 | 用途 |
| --- | --- | --- |
| GStreamer | >= 1.20，含 base/video/good/bad/tools | 视频分析和调试 |
| Meson + Ninja | Meson >= 1.1 | C++ 构建 |
| yaml-cpp | >= 0.7 | C++ YAML 配置解析 |
| hiredis | >= 0.14 | Redis 发布插件 |
| nlohmann-json | >= 3 | 事件 JSON 序列化 |
| ONNX Runtime | >= 1.20 | YOLO ONNX 推理 |
| OpenCV | >= 4.5（managed 默认源码构建 4.10.0） | sparse optical flow GMC；不参与解码、显示或推理预处理 |
| BLAS + LAPACK | 系统开发包 | managed OpenCV 的宿主数学运行库 |
| Python | >= 3.12 | Agent 服务 |
| uv | >= 0.11 | Python 包管理 |
| Docker + Compose | Docker >= 24 | 本地 Redis |

Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential pkg-config cmake ninja-build meson curl ca-certificates \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgtk-3-dev gstreamer1.0-tools gstreamer1.0-libav \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  libyaml-cpp-dev libhiredis-dev nlohmann-json3-dev \
  libblas-dev liblapack-dev \
  python3 python3-venv docker.io docker-compose-plugin
```

`./ssv prepare-model`、`./ssv test` 和默认模型导出还需要 `uv`。请按本机发行版安装
`uv`，然后确认 `uv --version` 可执行；`./ssv build` 不会替你安装 Python 工具、GPU 驱动或
Docker 服务。

没有发行版包时，可使用官方安装脚本：

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
exec "$SHELL"
uv --version
```

Debian 12 默认源通常没有 ONNX Runtime C++ 开发包。无 GPU 的宿主上，默认 `./ssv build` 解析为 CPU profile，并准备 managed ONNX Runtime `1.25.1` 和 managed OpenCV `4.10.0` 源码构建；其他 profile 会在 Meson 前严格验证对应 ORT artifact 和 Provider 运行库。Intel profile 的 managed 来源使用 ORT 源码构建并捆绑 OpenVINO runtime。三类依赖都遵循同一条路径：`SOURCE -> provider/system -> pkg-config -> Meson`。

NVIDIA managed profile 使用官方 ONNX Runtime `1.25.1` CUDA 13 artifact。默认 provider
下载 TensorRT `10.16.1` 与 cuDNN `9.25` 包；所有 managed 来源都精确要求 TensorRT
`10.16.1`，并校验 CUDA 13 ABI 与 cuDNN SONAME 9。宿主须提供可用的 CUDA 13
toolkit/runtime，当前验证组合为 13.2；版本或 SONAME 不匹配会在 Meson 前失败，不会通过
伪造链接兼容。依赖签名变化时，构建入口会清理 Meson dependency cache，因此同一 `build`
目录可以在 CPU 与 NVIDIA profile 间安全切换。

managed OpenCV 当前由 provider 下载 OpenCV `4.10.0` 源码并使用 CMake 编译，安装到 `.deps/opencv/install`；源码和构建目录分别保留在 `.deps/opencv/source`、`.deps/opencv/build`，下载归档位于 `.deps/downloads/opencv/4.10.0`。构建关闭 CUDA、GStreamer、GUI、视频解码后端和可选图像编解码器，只生成 BoT-SORT GMC 所需的 CPU 模块及 DNN 依赖，不依赖 `apt` 或 `dpkg-deb`。provider 随后生成并验证 `.deps/opencv/install/lib/pkgconfig/opencv4.pc`，并检查所有 OpenCV 动态库的运行时闭包。首次构建需要宿主提供 CMake、C++ 编译器、make 或 Ninja，以及 BLAS/LAPACK；可用 `SSV_OPENCV_BUILD_JOBS` 覆盖并行线程数。

如果你在当前宿主机自行编译 OpenCV 4.10.0，可以使用 `local` provider。它只读取你填写的头文件目录和库目录，不修改或复制 OpenCV 安装目录：

```bash
SSV_OPENCV_SOURCE=local \
SSV_OPENCV_INCLUDE_DIR=/path/to/opencv/include/opencv4 \
SSV_OPENCV_LIB_DIR=/path/to/opencv/lib \
./ssv build
```

provider 会生成 `.deps/opencv/local/lib/pkgconfig/opencv4.pc`，并执行 C++ 编译/加载探针；只有实际版本为 `4.10.0` 且所需模块和运行库完整时才会继续构建。local provider 不复制或修改用户提供的 SDK，项目内仅在 `.deps/opencv/local` 保存私有 pkg-config 文件。managed provider 的源码、构建和安装结果分别位于 `.deps/opencv/{source,build,install}`，下载归档位于 `.deps/downloads/opencv`。Python `cv2` 文件不能代替 C++ OpenCV SDK。

Arch Linux:

```bash
sudo pacman -S gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav gtk3 \
  gst-plugin-gtk yaml-cpp hiredis nlohmann-json cblas blas lapack meson python uv \
  docker docker-compose curl ca-certificates
```

Arch 上按硬件能力补充安装：

```bash
# GTK 显示插件：gtksink/gtkglsink（独立包，不在 gst-plugins-bad 中）
sudo pacman -S gst-plugin-gtk

# Intel VAAPI 硬解驱动与诊断工具
sudo pacman -S intel-media-driver libva libva-utils

# Intel OpenVINO GPU 推理运行库（可选；不装则 OpenVINO 回退 CPU）
sudo pacman -S intel-compute-runtime
```

注意：本仓库验证时，Arch 官方仓库没有 `gstreamer-vaapi` 包（AUR 只有旧的 1.24 git
版本），`gst-plugins-bad` 的新 GstVA 插件也尚未打包，因此 `vah264dec`/`vapostproc`
不可用，VAAPI 硬解路径暂不可用；`decode.mode: auto` 会记录原因并回退软件解码。
`gtkglsink` 需要 VA 解码导出 DMABuf，软件解码下会回退 `gtksink`；软件解码路径的
协商修复与验证结果见 `docs/specs/2026-08-07-T5-软件解码-NV12-协商修复-spec.md`。

## 快速开始

下面按“硬件 profile → 输入和模型 → 编译 → Redis → 运行”的顺序执行。`./ssv build` 是
构建入口，不是系统安装器；它不会安装驱动、CUDA、TensorRT、GTK、Redis 或 MediaMTX，也
不会自动创建 YAML、下载任意模型或猜测 RTSP 地址。

### 1. 先按本机硬件选择 runtime profile

| 本机情况 | 首选命令 | 构建前必须确认 |
| --- | --- | --- |
| 没有可用 GPU，或只想用 CPU | `./ssv build --profile cpu` | 使用 managed ONNX Runtime CPU 和 managed OpenCV |
| NVIDIA GPU，宿主可见 CUDA 13 | `./ssv build --profile nvidia` | NVIDIA 驱动、CUDA 13 toolkit/runtime；默认还会准备 TensorRT 10.16.1 与 cuDNN 9 |
| Intel GPU | `./ssv build --profile intel` | managed 源码构建 ONNX Runtime `1.25.1` + OpenVINO `2025.4.1`；首次构建需要 CMake、C++ 编译器、Python 3 与 ninja/make |
| AMD GPU | `./ssv build --profile amd` | 系统 `onnxruntime.pc`，且包含 MIGraphX Provider |
| 不确定，或混合 GPU | `./ssv build --profile auto` | `/sys/class/drm` 能看到设备；选择顺序为 `nvidia → intel → amd → cpu` |

`--profile` 只选择 ONNX Runtime 的推理 Provider，不决定视频解码。`auto` 只负责检测并选择
profile，不会在依赖缺失时自动退回 CPU；例如检测到 NVIDIA 但 CUDA/TensorRT 不完整时，命令
会在 Meson 前失败。新克隆且没有 `.env` 时，CPU/NVIDIA 默认使用 managed 预编译，
Intel 默认使用 managed 源码构建，AMD 默认使用 system provider。

AMD profile 不是“下载即用”的 managed 路径。若本机没有带 MIGraphX 的 `onnxruntime.pc`，
请先准备对应 system/local artifact，或选择 `cpu` profile。Intel profile 默认由 managed
provider 下载 ORT `1.25.1` 源码与 OpenVINO `2025.4.1` runtime 并本机编译，工作区位于
`.deps/onnxruntime-openvino`；构建还会下载 protobuf、onnx、re2 等 ORT 外部依赖，
首次构建耗时较长，可用 `SSV_ONNXRUNTIME_BUILD_JOBS` 控制并行度。实际 GPU 推理仍需
宿主提供 Intel GPU 驱动/OpenCL runtime（Arch 上为 `intel-compute-runtime`；VAAPI 解码
另需 `intel-media-driver` 与 `libva`）。容器中使用 `auto` 时还必须透传 GPU 和 `/sys`；
否则它会按 CPU 处理。

NVIDIA 机器可先检查 `nvidia-smi`、CUDA 头文件和 `libcudart.so.13` 是否可见。managed NVIDIA
依赖下载和解包还需要 `curl`、`ca-certificates` 与 `dpkg-deb`；非 Debian 主机可改用已安装的
系统 TensorRT：

```bash
SSV_TENSORRT_SOURCE=system ./ssv build --profile nvidia
```

如果希望 NVIDIA 解码也走 NVDEC，在 YAML 中显式设置 `sources[0].decode.mode: nvdec` 和
`sources[0].decode.device: cuda:0`。默认 `decode.mode: auto` 会按可用 GStreamer 元素选择
VAAPI、NVDEC 或 software；显式硬件模式不可用时不会静默回退。

### 2. 克隆仓库并准备本机配置

```bash
cd site-safety-vision
cp config/ssv.example.yaml config/ssv.yaml
```

编辑 `config/ssv.yaml`，至少修改 `sources[0].uri`。输入必须是可访问的 H.264 8-bit RTSP
源；仓库不会启动 MediaMTX。默认 Redis 地址是 `localhost:6379`，如果使用外部 Redis，请
同步修改 `redis.host`、`redis.port` 和 `redis.stream_key`。

配置文件按 `ssv.yaml`、`config/ssv.yaml`、`/etc/ssv/ssv.yaml` 搜索；`config/ssv.example.yaml`
只是模板，不会被自动运行。也可以用 `./ssv run --config PATH` 或 `SSV_CONFIG_PATH` 指定
文件。仓库已有 `.env` 时，构建脚本会加载其中的 provider/OpenCV 设置；先检查它是否指向
本机真实 SDK，不要把无效路径带入新机器。

### 3. 准备模型

默认示例使用 YOLOv8n。`download-model` 会尝试用 `uv`/`pip` 和 `ultralytics` 导出原始
`models/yolov8n.onnx`，它不是任意模型下载器；已有自己的 ONNX 文件时可以跳过该命令。

```bash
./ssv download-model
./ssv prepare-model \
  --input models/yolov8n.onnx \
  --output models/yolov8n-preproc.onnx \
  --family yolo \
  --output-format yolov8
```

然后把 `config/ssv.yaml` 中的 `inference.model.path` 改为
`models/yolov8n-preproc.onnx`。原始 float32 NCHW ONNX 不能直接运行；如果输出已经是
`[1,N,6]` 的 `x1,y1,x2,y2,score,class_id`，使用 `--output-format yolo_nx6`。输出格式应按
实际图结构选择，不能只看模型文件名。

如果手上的模型已经带有 `rgba_u8_nhwc_v1` wrapper metadata，可跳过 `download-model` 和
`prepare-model`，直接把 `inference.model.path` 指向该 wrapper；仍需确保 `family`、
`output_format` 和 label map 与模型实际输出一致。

### 4. 编译并检查插件

```bash
./ssv build --profile cpu    # 将 cpu 替换为第 1 步选定的 profile
./ssv inspect
```

首次 managed 构建会下载 ONNX Runtime，并从源码编译 OpenCV 4.10.0；OpenCV 的源码、构建和
安装目录位于 `.deps/opencv/{source,build,install}`。构建较慢时可用
`SSV_OPENCV_BUILD_JOBS=4 ./ssv build --profile cpu` 限制并行度。

### 5. 启动 Redis，并先做一次验证

```bash
./ssv redis
./ssv test
```

`./ssv redis` 只启动 Docker Redis，不启动 RTSP 服务。`./ssv test` 会运行契约、C++、CLI 和
Agent 测试；存在 `config/ssv.yaml` 时还会尝试 30 秒无头 runner smoke，RTSP 不可达时该项
会以警告结束。要直接运行而不打开窗口，可使用 `./ssv run --headless`。

注意：当前 `./ssv test` 内部会用默认 `auto` profile 重新调用构建。若你在 NVIDIA 主机上
刻意选择了 `--profile cpu`，可跳过该编排命令，改用 `./ssv inspect`、相关单项测试和
`./ssv run`，避免它重新选择 NVIDIA 依赖。

### 6. 打开视频观测窗口

在有本地图形桌面的机器上运行：

```bash
./ssv run --display --overlay
```

`--display` 打开 GTK 视频窗口，`--overlay` 在独立 GTK 框层绘制检测框；两个参数会覆盖
YAML 中相应的显示开关。`--display` 需要可用的 `DISPLAY` 或 Wayland 会话。若 GL/DMABuf
路径不可用，先尝试兼容的 SystemMemory sink：

```bash
./ssv run --display --display-backend gtksink
```

按 `Ctrl+C` 退出长期运行的 runner。无桌面或 SSH 会话使用 `--headless`；不要把 GTK 弹窗
问题误判为 ONNX Runtime 或 Redis 问题。

## 命令

| 命令 | 说明 |
| --- | --- |
| `./ssv build [--profile auto|cpu|nvidia|intel|amd]` | 准备依赖并按单一 runtime profile 编译 C++ runner、插件和测试 |
| `./ssv clean` | 删除 Meson 构建目录 `build` |
| `./ssv redis` | 启动 Docker Redis 开发环境 |
| `./ssv test` | 运行代码测试和链路冒烟测试后退出 |
| `./ssv run` | 运行无头实时链路 |
| `./ssv run --config PATH` | 使用指定的 YAML 配置 |
| `./ssv run --headless` | 强制关闭 YAML 中启用的显示 |
| `./ssv run --display` | 运行实时链路并打开视频窗口 |
| `./ssv run --display --overlay` | 在独立 GTK 框层绘制检测框 |
| `./ssv run --display --display-backend gtkglsink` | 严格使用 GTK GL 显示；也可显式选择 `gtksink` 兼容路径 |
| `./ssv agent` | 启动 Python Agent 服务 |
| `./ssv inspect` | 查看插件注册和属性信息 |
| `./ssv stop` | 停止后台服务 |
| `./ssv download-model` | 用 `ultralytics` 导出默认 YOLOv8n ONNX 模型 |
| `./ssv prepare-model ...` | 生成经过校验的 RGBA uint8 wrapper ONNX 模型 |

## 配置

YAML 示例配置只保留 `config/ssv.example.yaml`，它不参与默认运行搜索。本地配置依次搜索项目根目录 `ssv.yaml`、`config/ssv.yaml` 和 `/etc/ssv/ssv.yaml`；前两个本地路径已加入 `.gitignore`，适合保存 RTSP 地址、模型路径和显示偏好。首次运行前复制模板：

```bash
cp config/ssv.example.yaml config/ssv.yaml
```

常用配置优先写入 `config/ssv.yaml`：`sources[0].uri`、`inference.model.path`、推理 runtime/device、分析帧率、Redis 地址和显示 backend 都由 YAML 提供默认值。`.env` 只保留配置文件选择、少量临时调试覆盖和构建配置：

| 变量 | 作用 | 默认值 |
| --- | --- | --- |
| `SSV_CONFIG_PATH` | 显式 YAML 配置文件路径 | 默认搜索 `ssv.yaml`、`config/ssv.yaml`、`/etc/ssv/ssv.yaml` |
| `SSV_RTSP_URL` | 临时覆盖 RTSP 视频源地址 | YAML `sources[0].uri` |
| `GST_DEBUG` | 临时覆盖 GStreamer 调试级别 | YAML `logging.cpp_debug_level` |
| `REDIS_HOST` / `REDIS_PORT` | 部署环境临时覆盖 Redis 地址 | YAML `redis.host` / `redis.port` |

模型、推理 runtime/device、RTSP transport/latency、显示帧率和 overlay 等运行参数统一写入 YAML 配置，不再提供同名环境变量覆盖。

`inference.analysis_fps` 控制推理/跟踪/事件发布分支的抽帧上限，默认示例为 `15`。将它设为 `0` 表示不限流，但分析分支仍只保留最新等待帧；显示窗口帧率仍由 `display.fps` 控制。

三类 SDK 使用一致的配置维度：`SOURCE` 选择来源，managed 使用稳定 `ROOT`，可选能力再使用 `MODE`。ONNX Runtime 支持 `managed|local|system`；TensorRT 支持 `managed|system`；OpenCV 通过 `local` 的 include/lib 路径引用本机编译安装。当前 shell 优先于根 `.env`，`.env` 优先于项目默认版本。

ONNX Runtime 是必需依赖。每次构建只解析并记录一个 runtime profile；`auto` 从 DRM 设备厂商解析，hybrid 主机按 `nvidia -> intel -> amd -> cpu` 选择一个结果。显式 profile 不做硬件改写：

```bash
# 无 GPU 的默认结果；也可显式固定 CPU
./ssv build --profile auto
./ssv build --profile cpu

# NVIDIA 默认准备受控 managed ORT/TensorRT/cuDNN，并使用宿主 CUDA 13
./ssv build --profile nvidia
# 也可复用通过 nvinfer.pc 暴露的 system TensorRT/CUDA
SSV_TENSORRT_SOURCE=system ./ssv build --profile nvidia

# Intel 默认 managed 源码构建；AMD 默认验证系统 onnxruntime.pc，也可引用只读 local artifact
./ssv build --profile intel
SSV_ONNXRUNTIME_SOURCE=local \
SSV_ONNXRUNTIME_ROOT=/opt/onnxruntime-migraphx \
./ssv build --profile amd
```

CPU/NVIDIA 默认使用 managed 预编译 artifact；Intel 默认使用 managed 源码构建；
AMD 默认使用 system artifact。system/local 的 ORT 版本必须与项目固定版本精确一致，
并提供当前 profile 所需的 Provider 动态库。local artifact 约定包含 `VERSION_NUMBER`、
`include/` 和 `lib/`，构建只在 build 目录生成 pkg-config adapter，不修改 artifact。

OpenCV 默认 enabled；不需要 GMC 时完全跳过准备和发现：

```bash
SSV_OPENCV_MODE=disabled ./ssv build
SSV_OPENCV_SOURCE=system ./ssv build
# 本地编译的 OpenCV 4.10.0
SSV_OPENCV_SOURCE=local \
SSV_OPENCV_INCLUDE_DIR=/path/to/opencv/include/opencv4 \
SSV_OPENCV_LIB_DIR=/path/to/opencv/lib \
./ssv build
```

TensorRT 与 CUDA Runtime 作为一个依赖单元。NVIDIA profile 默认使用 managed `enabled`，准备固定的 TensorRT/cuDNN 包并要求宿主 CUDA 13；其他 profile 默认使用 managed `auto`，只复用已有完整 ROOT，没有 SDK 时不下载并使用 stub。`enabled` 要求 provider 成功；版本从 `NvInferVersion.h` 自动读取：

```bash
# 已解包 SDK，或用户明确提供归档
SSV_TENSORRT_MODE=enabled SSV_TENSORRT_ROOT=.deps/tensorrt ./ssv build
SSV_TENSORRT_MODE=enabled SSV_TENSORRT_ARCHIVE=/path/to/TensorRT.tar.zst ./ssv build

# URL 必须可直接下载；需要 NVIDIA 认证时先下载，再使用 ARCHIVE
SSV_TENSORRT_MODE=enabled SSV_TENSORRT_URL=https://example.invalid/TensorRT.tar.zst ./ssv build

# 使用系统 nvinfer.pc，或明确关闭
SSV_TENSORRT_SOURCE=system SSV_TENSORRT_MODE=auto ./ssv build
SSV_TENSORRT_MODE=disabled ./ssv build
```

公开构建变量：

| 变量 | 作用 | 默认值 |
| --- | --- | --- |
| `SSV_ONNXRUNTIME_SOURCE` | ONNX Runtime 来源：`managed`、`local`、`system` | CPU/NVIDIA: `managed` 预编译；Intel: `managed` 源码构建；AMD: `system` |
| `SSV_ONNXRUNTIME_ROOT` | managed 工作区/安装目录或只读 local artifact | CPU/NVIDIA: `.deps/onnxruntime`；Intel: `.deps/onnxruntime-openvino` |
| `SSV_ONNXRUNTIME_BUILD_JOBS` | Intel managed 源码构建并行线程数 | 逻辑 CPU 数 |
| `SSV_OPENCV_SOURCE` | OpenCV 来源：`managed`、`local`、`system` | `managed` |
| `SSV_OPENCV_MODE` | OpenCV/GMC：`enabled`、`disabled` | `enabled` |
| `SSV_OPENCV_ROOT` | managed/local 共用的 OpenCV 工作根目录 | `.deps/opencv` |
| `SSV_OPENCV_INCLUDE_DIR` | local OpenCV 头文件目录 | 无 |
| `SSV_OPENCV_LIB_DIR` | local OpenCV 动态库目录 | 无 |
| `SSV_TENSORRT_SOURCE` | TensorRT 来源：`managed`、`system` | `managed` |
| `SSV_TENSORRT_MODE` | TensorRT：`auto`、`enabled`、`disabled` | NVIDIA profile: `enabled`；其他: `auto` |
| `SSV_TENSORRT_ROOT` | managed SDK 目录 | `.deps/tensorrt` |
| `SSV_TENSORRT_ARCHIVE` | 本地 TensorRT SDK 归档路径，支持 `.tar.*` 和 `.zip` | 无 |
| `SSV_TENSORRT_URL` | 可直接下载的 SDK 归档 URL | 无 |
| `CUDA_HOME` | managed TensorRT 的 CUDA Toolkit 补充路径 | 自动发现 |
| `SSV_EXTRA_PKG_CONFIG_PATH` | 额外 pkg-config 搜索路径 | 无 |

ONNX Runtime 版本由 `--profile` 固定派生，不再接受 `SSV_ONNXRUNTIME_VERSION` 覆盖。成功依赖快照为 ONNX Runtime、OpenCV 和 TensorRT 统一保留各自的 `PCDIR` 与 `RUNTIME_DIRS`，供 Meson 和运行入口精确定位实际库。

### 模型准备

正式推理路径不直接接受原始 float NCHW ONNX。先用离线工具生成一个输入为 `uint8 [1,H,W,4]`、layout 为 NHWC 的 wrapper：

```bash
./ssv prepare-model \
  --input models/yolov8n.onnx \
  --output models/yolov8n-preproc.onnx \
  --family yolo \
  --output-format yolov8
```

原模型必须恰好有一个 float32、batch 1、静态 `[1,3,H,W]` 输入。工具只在图首加入去 alpha、Cast、除以 255 和 NHWC 到 NCHW 的 Transpose；resize 与 letterbox 仍由视频管线完成。产物写入 `rgba_u8_nhwc_v1` 契约、宽高、layout、dtype、归一化、通道规则、原模型 SHA-256、模型族、输出格式和工具版本 metadata，并在发布前执行 ONNX checker、shape inference 与 ORT CPU smoke。该隔离环境不安装或导入 OpenCV。

`--output-format` 支持 `yolov8` 和 `yolo_nx6`。原模型输出为 `[1,N,6]`、每行已经是
`x1,y1,x2,y2,score,class_id` 的端到端检测结果时，必须使用 `yolo_nx6`；不能仅按模型名称
把它标记为 `yolov8`。

同一输入和参数重复执行会幂等成功。已有目标内容不同则默认拒绝；`--force` 仅允许替换能通过完整契约和 ORT smoke 的本工具产物，不会覆盖任意用户文件。输入文件始终只读，输出通过同目录临时文件原子发布。

TensorRT 后端只加载已构建好的 `.engine` 文件，不在插件内把 `.onnx` 转成 `.engine`。独立
TensorRT 路径是实验对照，正式主路径仍是 ONNX Runtime。先在最终运行机器上从同一个 wrapper
构建 engine，再为现有产物写 manifest：

```bash
trtexec \
  --onnx=models/yolov8n-preproc.onnx \
  --saveEngine=models/yolov8n-preproc.engine \
  --fp16

# 示例值；替换为构建 engine 的实际软件栈与 GPU
TENSORRT_VERSION=10.16.1.11
CUDA_RUNTIME_VERSION=13020
COMPUTE_CAPABILITY=8.9

uv run --isolated --script scripts/model/write_tensorrt_manifest.py \
  --wrapper models/yolov8n-preproc.onnx \
  --engine models/yolov8n-preproc.engine \
  --output models/yolov8n-preproc.engine.json \
  --precision fp16 \
  --tensorrt-version "$TENSORRT_VERSION" \
  --cuda-runtime-version "$CUDA_RUNTIME_VERSION" \
  --compute-capability "$COMPUTE_CAPABILITY"
```

`TENSORRT_VERSION` 使用 `major.minor.patch.build`，`CUDA_RUNTIME_VERSION` 使用
`cudaRuntimeGetVersion` 的整数值，`COMPUTE_CAPABILITY` 使用 `major.minor`。这三个值必须来自
构建并运行 engine 的同一软件栈和 GPU。工具不会调用 `trtexec`、探测设备或改写 wrapper/engine；
相同内容幂等成功，不同 manifest 默认拒绝覆盖，确认替换时显式传 `--force`。

manifest 的 `schema` 固定为 `ssv.tensorrt-engine-manifest`，`schema_version` 为 `1`。`engine`
记录 engine SHA-256、精度、TensorRT/CUDA 版本和 compute capability；`wrapper` 记录 wrapper
及原模型 SHA-256、`rgba_u8_nhwc_v1` 契约、工具/模型族/输出格式和静态
`uint8 RGBA NHWC` 输入。加载时严格拒绝未知键、类型错误、hash/版本/设备不匹配、非法精度
以及实际 engine 输入不一致。配置使用：

```yaml
inference:
  model:
    path: models/yolov8n-preproc.engine
    manifest: models/yolov8n-preproc.engine.json
    family: yolo
    output_format: yolov8
    label_map: config/model-labels/coco80.txt
  runtime:
    type: tensorrt-engine
    device_id: 0
```

该 runtime 不接受 `providers`、`precision`、`cpu_threads` 或 `cache`；精度只取 manifest。

## 运行和调试

### 构建与插件检查

```bash
./ssv build
./ssv inspect
```

如果插件没有被发现，先确认 `./ssv build` 成功。`./ssv run` 和 `./ssv inspect` 都会加载同一份成功依赖快照，并精确导出 `GST_PLUGIN_PATH` 和 `LD_LIBRARY_PATH`；缺少快照时会提示先构建。

### 测试套件

```bash
./ssv test
```

该命令会先跑依赖、wrapper 准备和 TensorRT manifest 契约测试，再跑构建、Meson 测试、CLI 测试和 Python Agent 测试。存在本地运行配置时，最后通过 `scripts/run.sh` 启动一次 30 秒无头 runner smoke；超时向 runner 发送 `SIGINT`，不解析或改写 YAML。没有本地配置时明确跳过这项环境相关检查。

### 显示调试

```bash
./ssv run --display
./ssv run --display --overlay
./ssv run --display --display-backend gtkglsink
./ssv run --display --display-backend gtksink
```

`--overlay` 启用独立 GTK 框层；它复用 latest-frame、PTS 因果选择和展示运动预测，不修改视频像素。`--display-backend` 是严格选择：`gtkglsink` 不可用时直接失败，`gtksink` 是显式 SystemMemory 兼容路径。YAML 的 `display.backend: auto` 才允许记录原因后回退。

WSLg 只用于兼容性调试，不替代原生 Linux VA/DMABuf/GL 强验收。正式
runner 的 `gtkglsink` 契约要求 VA 解码导出 DMABuf；保留的 NVDEC 路径不会被标记为
该 VA/GL 加速路径。在 WSLg 上运行完整 runner 时使用 `display.backend: auto` 或显式
`gtksink`；仅检查 `gtkglsink` 元素能否创建 GL context，不能证明生产 runner 的内存
契约或显示质量已通过。检测框始终由 GTK 框层绘制。

如果画面出现宏块或花屏，先用独立客户端检查同一 RTSP。`ffmpeg -stream_loop -1 -c copy`
发布循环文件可能产生缺失参考帧或异常时间戳；这类码流损坏会同时影响软件和硬件 decoder，
不能通过更换 GTK backend 修复。测试时可让发布端重新编码并周期性生成关键帧，例如：

```bash
ffmpeg -re -stream_loop -1 -i test.mp4 -an \
  -c:v h264_nvenc -preset p3 -tune ll \
  -rc cbr -b:v 6M -maxrate 6M -bufsize 6M \
  -g 50 -forced-idr 1 -zerolatency 1 -bf 0 \
  -rtsp_transport tcp \
  -f rtsp rtsp://localhost:8554/test
```

### Redis 调试

```bash
./ssv redis
docker exec ssv-redis redis-cli XLEN ssv:events
docker exec ssv-redis redis-cli XRANGE ssv:events - + COUNT 5
```

如果 YAML 修改了 `redis.stream_key`，把命令中的 `ssv:events` 换成对应 key。

### Agent 调试

```bash
./ssv agent
```

Agent 将 Redis 规则事件写入 SQLite `EventLedger` 后立即 ACK。案件、检测、证据元数据和首个 `review`/`index` job 在同一事务提交；SQLite 写入失败时 Redis entry 保持 pending，模型和 Qdrant 不会阻塞 ACK。每轮消费会先用 Redis `XAUTOCLAIM` 回收超过 `redis.reclaim_idle_ms` 的 pending entry，再读取新消息；未配置 `redis.consumer_name` 时每个进程实例都会生成不同名称。JSON poison input 与确定性冷却去重保持 ACK 行为。

复核与索引在独立 worker 中执行。`review` job 通过 lease 领取，模型仅能使用 `get_event`、`evidence_reader`、`rule_retriever`、`search_events` 与 `view_image` 五个只读工具；每次 review client 启动都使用独立的临时空 `extensions_config.json`，并由 DeerFlow RBAC 再次限制该 allowlist，关闭 skills、subagent 和 plan mode。`frame_path`/`clip_path` 来自 Redis 时不可信：只有 `agent.evidence_roots` 中绝对根目录内、resolve 后仍未越界的普通文件才会登记；空列表 fail closed，事件仍可入账但不带证据。worker 和 `evidence_reader` 都会在各自副作用边界重新解析根目录和刷新 `available/size/mtime`，且 fenced `complete_review_job()` 会在同一事务再次校验引用文件。结构化 JSON 结果会先原子写入输出目录，再追加到账本。`index` job 从 SQLite 当前 revision 生成向量并用稳定 `event_id` upsert 该 embedding identity 对应的事件 collection；Embedding 或 Qdrant 故障只会重试 index job，不会回滚案件或复核事实。

结果文件使用内容 hash 名称，例如 `outputs/case-1/result-<sha256>.json`；简单安全的 event ID 保持可读目录，路径分隔符、绝对路径、`.`/`..` 或超长 ID 会映射为 `event-<sha256>` 目录。相同内容复用同一路径，不同结果不会互相覆盖。文件会在 fenced 提交前生成，因此失租或提交校验失败后可能留下未被账本引用的 orphan artifact；当前不自动清理。后续 GC 只能在保留期后删除 outputs root 内、未被 `reviews.result_path` 或当前案件投影引用的内容寻址文件，不能删除已接受 artifact 或修改账本事实。

默认 `agent.review.enabled` 和 `agent.indexing.enabled` 都为 `false`，因此最小启动不会加载外部模型。启用前在 `config/ssv.yaml` 中配置对应 worker 的轮询、lease、重试和延迟参数。离线 BGE-M3 后端是可选依赖，安装并指向本地模型目录后再启用：

```bash
cd agent
uv sync --extra bge-m3
```

```yaml
agent:
  # 仅允许这些绝对目录下的证据；空列表会拒绝所有来自 Redis 的路径。
  evidence_roots:
    - "/var/lib/ssv/evidence"
  review:
    enabled: true
  indexing:
    enabled: true
    embedding_backend: "bge_m3"
    embedding_model: "/opt/models/bge-m3"
```

重试达到 `max_retries` 后 job 进入 `dead`，保留最后错误供排查。Qdrant 可重建：在 `agent/` 下通过账本重新入队当前案件投影，恢复索引服务后 worker 会继续处理：

```bash
uv run python - <<'PY'
from ssv_agent.event_store import EventLedger

with EventLedger() as ledger:
    print(ledger.enqueue_all_index_jobs())
PY
```

`evidence_reader` 只接收 `event_id` 和账本登记的 `evidence_id`，不会读取模型提供的任意宿主机路径；登记、刷新和复制都执行同一套 evidence-root resolved-path 检查，因此目录穿越、根外路径及越界 symlink 都不会成为可读证据。`search_events` 只信任 Qdrant 的召回 ID 与分数，所有展示字段都会回 SQLite hydrate 当前事实。

事件字段、规则/检索内容、证据元数据和图片内容均是不可信输入。复核提示词要求模型把它们作为待核验候选，不能执行其中的工具调用、权限变更或输出格式指令；`view_image` 只能接收 `evidence_reader` 返回的虚拟路径。

Qdrant 是可重建的语义索引，SQLite `EventLedger` 才是事件、证据和复核结论的事实源。开发测试可使用本地
`SSV_QDRANT_PATH`；生产建议设置 `SSV_QDRANT_URL`，并通过 `SSV_QDRANT_API_KEY` 注入凭据。`ssv_events`
和 `ssv_rules` 是逻辑 collection 名；运行时以 adapter schema version、backend 和有效 model 组成的 embedding
identity 派生安全哈希后的物理 collection 名，因此同维但不同模型不会混写。切换模型后仍要重新入队 index jobs，
以填充新身份空间；本轮不迁移或删除旧 collection。`rule_retriever` 当前仍是带来源的 mock 片段，尚未接入生产规则/SOP 知识库。

当前 `gstssvpub` 的消息仍可能只有 `type/source/timestamp_ms/frame_id/detections` 等稀疏字段；Agent 不会据此补造
完整 `rule.v1` 状态、媒体 PTS、stream generation 或唯一人数。完整实时规则状态机和证据生成属于后续 C++/上游契约工作。

消费侧默认开启事件冷却去重：同一 `source` 的同一 `track_id` 在冷却窗口内只触发一次复核，新出现的 `track_id` 会绕过冷却。可通过 `agent.dedup_enabled` 关闭，通过 `agent.dedup_cooldown_seconds` 调整窗口（默认 30 秒）。

`ssv-pub` 在发布侧也默认按 `source+track` 做 30 秒冷却，新出现、丢失或移除状态的目标会立即发布；可通过 `tracking.publish_cooldown_ms` 调整（0 关闭）。消费侧 Redis 去重保留为多生产者与重放场景的兜底。

### GStreamer 日志

```bash
GST_DEBUG="ssv*:5,*redis*:4" ./ssv test
GST_DEBUG="ssvinfer:6,ssvtrack:5,ssvpub:5" ./ssv run
```

常见排查顺序：先跑 `./ssv inspect` 确认插件注册，再跑 `./ssv test` 确认代码测试和链路冒烟，再跑 `./ssv run --display` 排查显示分支。

## 测试

```bash
# C++ 插件和元数据测试
./ssv build
meson test -C build

# Python Agent 单元测试
cd agent && uv run --extra dev pytest

# wrapper ONNX 准备工具
uv run --isolated --script tests/ssv_prepare_model_test.py

# TensorRT engine manifest 工具
uv run --isolated --script tests/ssv_tensorrt_manifest_test.py

# CLI 脚本测试
bash tests/ssv_cli_test.sh
```

涉及 RTSP、显示窗口、Redis 和模型文件的测试依赖本地环境。提交代码前至少运行与改动相关的测试；修改公共元数据、插件属性、配置加载或 Agent 消费逻辑时，应同时运行 C++ 和 Python 测试。

## 项目结构

```text
site-safety-vision/
├── ssv                         # 项目统一入口脚本
├── meson.build                 # 根构建文件
├── meson.options               # Meson 选项
├── runner/                     # C++ 实时运行入口与四个职责模块
│   ├── pipeline/               # plan、构建、contract 与资源 attachment
│   ├── display/                # GTK 窗口、overlay 展示与窗口生命周期
│   ├── observability/          # typed event、logger 与 stderr sink
│   ├── runtime/                # Run、Run Attempt、fallback 与事件适配
│   └── tests/                  # CLI/cross-module 集成测试
├── gst/                        # GStreamer C++ 插件
│   ├── ssv-common/             # 配置、日志、元数据共享库
│   │   ├── include/            # 对外元数据、配置和 GStreamer 日志头文件
│   │   ├── meta/               # source metadata、timeline 和 snapshot 实现
│   │   ├── config/             # YAML 解析、校验和部署覆盖实现
│   │   ├── frame/              # analysis frame 与 preprocess 实现
│   │   └── tests/              # common contract 测试
│   ├── ssv-template/           # pass-through 模板插件
│   ├── ssv-infer/              # YOLO ONNX 推理深模块
│   │   ├── public/             # 对外 service/model contract 头文件
│   │   ├── core/               # service 编排、队列、统计、buffer 与 backend factory
│   │   ├── model/              # model contract 实现与 YOLO parser
│   │   ├── backends/           # ONNX Runtime、TensorRT 与不可用后端 stub
│   │   ├── plugin/             # 薄 GStreamer 适配器（gstssvinfer）
│   │   └── tests/              # inference 模块专属测试、测试 support 与插件本地测试
│   ├── ssv-track/              # 跟踪插件
│   │   ├── plugin/             # GStreamer element adapter
│   │   ├── core/botsort/       # 不依赖 GStreamer 的 BoT-SORT 算法内核
│   │   ├── adapters/           # SSV metadata、坐标和 geometry adapter
│   │   └── tests/              # tracker/adapter contract 测试
│   ├── ssv-pub/                # Redis Streams 发布插件
│   ├── ssv-overlay/            # 调试 overlay 插件
│   └── tests/                  # pub/overlay 与跨插件 GStreamer 集成测试
├── agent/                      # Python Agent 服务
├── config/                     # YAML 配置
├── docker/                     # Docker Compose 开发依赖
├── scripts/                    # build/run/test/redis/agent 等脚本
├── tests/                      # CLI 脚本测试
└── docs/                       # 中文设计文档、roadmap、后续 spec/plan
```

`ssv-infer` 保持为一个 `ssv-inference-core` 深模块：生产调用方和 `gstssvinfer` 依赖只导出
`public/` include directory，调用方继续包含原始文件名 `ssv_inference_service.hpp` 和
`ssv_model_contract.hpp`；`core/`、`model/`、`backends/` 和 `plugin/` 的 header 是私有实现。
inference 模块专属测试在 `ssv-infer/tests/` 使用内部 seam，runner 与中央 GStreamer 测试只使用
对外接口和仅测试使用的 `ssv_inference_test_support_dep`。这样目录、Meson 依赖和测试归属
表达同一依赖方向，同时不拆分出多个浅层生产库。

`ssv-track` 的 `core/botsort` 只接收算法值类型；`adapters/` 负责 SSV metadata、坐标变换、GMC
frame 和 geometry 生命周期；`plugin/` 只负责 GStreamer 生命周期与错误映射。`ssv-common` 按
metadata、配置和 frame 的变化原因组织实现，模块测试与实现放在同一目录，中央 `gst/tests/`
只保留跨插件集成测试。

## 文档和 Roadmap

- 总体设计：[docs/specs/2026-05-21-安全帽佩戴视频监测分析系统设计.md](docs/specs/2026-05-21-安全帽佩戴视频监测分析系统设计.md)
- 实施路线：[docs/roadmap.md](docs/roadmap.md)

后续 roadmap 按并行主线推进：T1 实时视频链路与运行时、T2 感知算法与元数据、T3 事件与异步边界、T4 Agent 与知识复核、T5 工程集成与质量。集成节点使用 I1-I4 表示。

每条主线和每个集成节点在实现前都必须补齐中文 spec 和中文 plan：

- `docs/specs/YYYY-MM-DD-Tx-主线名称-spec.md`
- `docs/plans/YYYY-MM-DD-Tx-主线名称-plan.md`
- `docs/specs/YYYY-MM-DD-Ix-集成节点-spec.md`
- `docs/plans/YYYY-MM-DD-Ix-集成节点-plan.md`

阶段完成后同步更新 README、roadmap 和对应 spec，保证文档描述和当前实现一致。
