# Site Safety Vision

安全帽佩戴视频监测分析系统。当前项目以 GStreamer C++ 插件承载实时视频分析链路，Redis Streams 作为实时链路和 Agent 复核链路之间的异步边界，Python 服务负责事件消费和后续智能复核编排。

当前实现是开发期可运行基线：`./ssv` shell CLI 负责构建、测试、运行和本地依赖启动；实时链路由 `gst-launch-1.0` 拼接 C++ 插件运行。后续 roadmap 会新增 C++ pipeline runner，把长期运行时的 pipeline 构建、错误处理和状态观测迁入 C++。

## 架构概览

```text
RTSP / 后续文件输入
    |
    v
./ssv -> scripts/pipeline.sh -> gst-launch-1.0
    |
    v
GStreamer C++ 插件链
    rtspsrc/decodebin -> videoconvert/videoscale/videorate
        -> ssvtemplate -> ssvinfer -> ssvtrack -> ssvpub
                                      |
                                      v
                              Redis Streams
                                      |
                                      v
                              Python Agent 服务
```

显示调试模式会通过 `tee` 分出显示分支，可选接入 `ssvoverlay` 绘制检测框。完整架构见 [系统设计文档](docs/specs/2026-05-21-安全帽佩戴视频监测分析系统设计.md)，阶段计划见 [Roadmap](docs/roadmap.md)。

## 当前能力

- C++ GStreamer 插件：`ssvtemplate`、`ssvinfer`、`ssvtrack`、`ssvpub`、`ssvoverlay`。
- 共享 C++ 模块：配置加载、日志、检测元数据。
- YOLO ONNX Runtime 推理插件，支持 mock 检测和异步推理开关。
- Redis Streams 发布插件和 Python Agent 消费基线。
- Docker Redis 开发环境。
- `./ssv` 统一入口脚本。
- C++ 插件测试、Agent 单元测试、CLI 脚本测试。

尚未完成：生产级 C++ pipeline runner、完整事件判定、证据输出、真实安全帽专用模型、完整 Agent 状态机、工具调用、模型 provider、知识库和端到端报告闭环。

## 依赖

| 依赖 | 版本要求 | 用途 |
| --- | --- | --- |
| GStreamer | >= 1.20，含 base/video/good/bad/tools | 视频分析和调试 |
| Meson + Ninja | Meson >= 1.1 | C++ 构建 |
| yaml-cpp | >= 0.7 | C++ YAML 配置解析 |
| hiredis | >= 0.14 | Redis 发布插件 |
| nlohmann-json | >= 3 | 事件 JSON 序列化 |
| ONNX Runtime | >= 1.20 | YOLO ONNX 推理 |
| OpenCV | >= 4.5 | 图像处理依赖 |
| BLAS + LAPACK | 系统开发包 | managed OpenCV 的宿主数学运行库 |
| Python | >= 3.12 | Agent 服务 |
| uv | >= 0.11 | Python 包管理 |
| Docker + Compose | Docker >= 24 | 本地 Redis |

Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential pkg-config cmake ninja-build meson \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  libyaml-cpp-dev libhiredis-dev nlohmann-json3-dev \
  libblas-dev liblapack-dev \
  python3 python3-venv docker.io docker-compose-plugin
```

Debian 12 默认源通常没有 ONNX Runtime C++ 开发包。默认 `./ssv build` 会准备 managed ONNX Runtime `1.25.1` 和 managed OpenCV `4.10.0`；TensorRT 使用 `auto`，没有完整 SDK 时编译明确报错的 unavailable stub。三类依赖都遵循同一条路径：`SOURCE -> provider/system -> pkg-config -> Meson`。

managed OpenCV 当前由 provider 获取预编译包，使用 `ar` 和 `tar` 解包，不依赖 `apt` 或 `dpkg-deb`。它对项目公开链接 `core`、`imgproc`、`video`、`calib3d`、`features2d`、`flann`，并额外携带 `dnn` 及其特定 SONAME 的 protobuf 运行库以闭合 `libopencv_video` 的动态依赖；这些私有运行库不进入 `opencv4.pc` 的公开 OpenCV 模块列表。包来源属于 provider 内部实现，对外只声明 OpenCV `4.10.0`。

如果你在当前宿主机自行编译 OpenCV 4.10.0，可以使用 `local` provider。它只读取你填写的头文件目录和库目录，不修改或复制 OpenCV 安装目录：

```bash
SSV_OPENCV_SOURCE=local \
SSV_OPENCV_INCLUDE_DIR=/path/to/opencv/include/opencv4 \
SSV_OPENCV_LIB_DIR=/path/to/opencv/lib \
./ssv build
```

provider 会生成 `.deps/opencv/local/lib/pkgconfig/opencv4.pc`，并执行 C++ 编译/加载探针；只有实际版本为 `4.10.0` 且所需模块和运行库完整时才会继续构建。项目内自行编译时统一使用 `.deps/opencv/local/{source,build,install}`，managed 解包结果放在 `.deps/opencv/managed`；下载文件仍与其他依赖一起保存在 `.deps/downloads/opencv`。Python `cv2` 文件不能代替 C++ OpenCV SDK。

Arch Linux:

```bash
sudo pacman -S gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad \
  yaml-cpp hiredis nlohmann-json cblas blas lapack meson python uv docker docker-compose
```

## 快速开始

```bash
# 1. 进入项目
cd site-safety-vision

# 2. 准备本机 YAML 配置
cp config/ssv.example.yaml config/ssv.yaml

# 3. 编辑 config/ssv.yaml，至少设置 sources[0].uri

# 4. 下载默认 YOLOv8n ONNX 模型，或在 YAML 设置 inference.model_path
./ssv download-model

# 5. 编译 C++ 插件
./ssv build

# 6. 启动本地 Redis
./ssv redis

# 7. 运行测试套件
./ssv test

# 8. 打开显示窗口观察实时链路
./ssv run --display
```

`./ssv test` 先跑代码测试，再在 YAML `sources[0].uri` 或 `SSV_RTSP_URL`、模型文件等条件满足时做一次有界链路冒烟测试，完成后退出。`pipeline.check_timeout` 只影响这一步 smoke。`./ssv run` 是长期运行模式，按 `Ctrl+C` 退出；`./ssv run --display` 关闭视频窗口或中断进程后退出。

## 命令

| 命令 | 说明 |
| --- | --- |
| `./ssv build` | 编译 C++ GStreamer 插件和测试 |
| `./ssv clean` | 删除 Meson 构建目录 `build` |
| `./ssv redis` | 启动 Docker Redis 开发环境 |
| `./ssv test` | 运行代码测试和链路冒烟测试后退出 |
| `./ssv run` | 运行无头实时链路 |
| `./ssv run --display` | 运行实时链路并打开视频窗口 |
| `./ssv run --display --overlay` | 在显示窗口绘制检测框，当前用于调试 |
| `./ssv run --display --sink waylandsink` | 指定显示 sink |
| `./ssv agent` | 启动 Python Agent 服务 |
| `./ssv inspect` | 查看插件注册和属性信息 |
| `./ssv stop` | 停止后台服务 |
| `./ssv download-model` | 下载默认 YOLOv8n ONNX 模型 |

## 配置

YAML 示例配置只保留 `config/ssv.example.yaml`。本地运行配置优先读取项目根目录 `ssv.yaml`，不存在时读取 `config/ssv.yaml`，两者都已加入 `.gitignore`，适合保存 RTSP 地址、模型路径和显示偏好。首次运行前复制模板：

```bash
cp config/ssv.example.yaml config/ssv.yaml
```

常用配置优先写入 `config/ssv.yaml`：`sources[0].uri`、模型路径、推理 runtime/device、分析分辨率、Redis 地址和显示 sink 都由 YAML 提供默认值。`.env` 只保留配置文件选择、少量临时调试覆盖和构建配置：

| 变量 | 作用 | 默认值 |
| --- | --- | --- |
| `SSV_CONFIG_PATH` | YAML 配置文件路径 | `ssv.yaml`、`config/ssv.yaml`、`config/ssv.example.yaml` |
| `SSV_RTSP_URL` | 临时覆盖 RTSP 视频源地址 | YAML `sources[0].uri` |
| `GST_DEBUG` | 临时覆盖 GStreamer 调试级别 | YAML `logging.cpp_debug_level` |
| `REDIS_HOST` / `REDIS_PORT` | 部署环境临时覆盖 Redis 地址 | YAML `redis.host` / `redis.port` |

模型、推理 runtime/device、RTSP transport/latency、显示帧率和 overlay 等运行参数统一写入 YAML 配置，不再提供同名环境变量覆盖。

`pipeline.analysis_fps` 控制推理/跟踪/事件发布分支的抽帧频率，默认示例为 `5`，用于降低 GPU 和事件吞吐压力。需要测试 TensorRT 或让分析分支按源视频可用帧率运行时，将它设为 `0` 表示不限流；显示窗口帧率仍由 `display.fps` 控制。

三类 SDK 使用一致的配置维度：`SOURCE` 选择来源，managed 使用稳定 `ROOT`，可选能力再使用 `MODE`。ONNX Runtime 和 TensorRT 支持 `managed|system`；OpenCV 额外支持 `local`，通过 `SSV_OPENCV_INCLUDE_DIR` 和 `SSV_OPENCV_LIB_DIR` 引用本机编译安装。当前 shell 优先于根 `.env`，`.env` 优先于项目默认版本。

ONNX Runtime 是必需依赖，默认 managed CPU：

```bash
# 默认 managed CPU
SSV_ONNXRUNTIME_SOURCE=managed \
SSV_ONNXRUNTIME_VERSION=1.25.1 \
SSV_ONNXRUNTIME_ROOT=.deps/onnxruntime \
./ssv build

# GPU 归档通过版本后缀选择；CUDA/cuDNN 仍由本机提供
SSV_ONNXRUNTIME_VERSION=1.25.1-gpu ./ssv build

# 使用系统 onnxruntime.pc
SSV_ONNXRUNTIME_SOURCE=system ./ssv build
```

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

TensorRT 与 CUDA Runtime 作为一个依赖单元。默认 managed `auto` 只复用已有完整 ROOT，没有 SDK 时不下载并使用 stub。`enabled` 要求 provider 成功；版本从 `NvInferVersion.h` 自动读取：

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
| `SSV_ONNXRUNTIME_SOURCE` | ONNX Runtime 来源：`managed`、`system` | `managed` |
| `SSV_ONNXRUNTIME_VERSION` | managed 版本：`x.y.z` 或 `x.y.z-gpu` | `1.25.1` |
| `SSV_ONNXRUNTIME_ROOT` | managed 安装目录 | `.deps/onnxruntime` |
| `SSV_OPENCV_SOURCE` | OpenCV 来源：`managed`、`local`、`system` | `managed` |
| `SSV_OPENCV_MODE` | OpenCV/GMC：`enabled`、`disabled` | `enabled` |
| `SSV_OPENCV_ROOT` | managed/local 共用的 OpenCV 工作根目录 | `.deps/opencv` |
| `SSV_OPENCV_INCLUDE_DIR` | local OpenCV 头文件目录 | 无 |
| `SSV_OPENCV_LIB_DIR` | local OpenCV 动态库目录 | 无 |
| `SSV_TENSORRT_SOURCE` | TensorRT 来源：`managed`、`system` | `managed` |
| `SSV_TENSORRT_MODE` | TensorRT：`auto`、`enabled`、`disabled` | `auto` |
| `SSV_TENSORRT_ROOT` | managed SDK 目录 | `.deps/tensorrt` |
| `SSV_TENSORRT_ARCHIVE` | 本地 TensorRT SDK 归档路径，支持 `.tar.*` 和 `.zip` | 无 |
| `SSV_TENSORRT_URL` | 可直接下载的 SDK 归档 URL | 无 |
| `CUDA_HOME` | managed TensorRT 的 CUDA Toolkit 补充路径 | 自动发现 |
| `SSV_EXTRA_PKG_CONFIG_PATH` | 额外 pkg-config 搜索路径 | 无 |

TensorRT 后端只加载已构建好的 `.engine` 文件，不在插件内把 `.onnx` 转成 `.engine`。

## 运行和调试

### 构建与插件检查

```bash
./ssv build
./ssv inspect
```

如果插件没有被发现，先确认 `./ssv build` 成功。脚本会自动导出 `GST_PLUGIN_PATH` 和 `LD_LIBRARY_PATH`；手动运行 `gst-launch-1.0` 时需要自己设置这些路径，推荐优先通过 `./ssv run` 调试。

### 测试套件

```bash
./ssv test
```

该命令会先跑构建、Meson 测试、CLI 测试和 Python Agent 测试；当 YAML `sources[0].uri` 或 `SSV_RTSP_URL`、模型文件都可用时，再跑一次短时链路冒烟测试。退出后返回统一结果码。smoke 超时时间由 `ssv.yaml` 的 `pipeline.check_timeout` 控制。

### 显示调试

```bash
./ssv run --display
./ssv run --display --sink waylandsink
./ssv run --display --sink glimagesink
./ssv run --display --overlay
```

`--overlay` 是检测框绘制调试路径。如果窗口异常，先去掉 `--overlay` 验证基础显示分支。

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

Agent 当前用于消费 Redis Streams 并验证事件消费基线。完整上下文构造、状态机、工具路由和模型 provider 在后续 roadmap 阶段实现。

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
├── gst/                        # GStreamer C++ 插件
│   ├── ssv-common/             # 配置、日志、元数据共享库
│   ├── ssv-template/           # pass-through 模板插件
│   ├── ssv-infer/              # YOLO ONNX 推理插件
│   ├── ssv-track/              # 跟踪插件
│   ├── ssv-pub/                # Redis Streams 发布插件
│   ├── ssv-overlay/            # 调试 overlay 插件
│   └── tests/                  # C++/GStreamer 测试
├── agent/                      # Python Agent 服务
├── config/                     # YAML 配置
├── docker/                     # Docker Compose 开发依赖
├── scripts/                    # build/run/test/redis/agent 等脚本
├── tests/                      # CLI 脚本测试
└── docs/                       # 中文设计文档、roadmap、后续 spec/plan
```

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
