# Site Safety Vision

安全帽佩戴视频监测分析系统。当前项目以 GStreamer C++ 插件承载实时视频分析链路，Redis Streams 作为实时链路和 Agent 复核链路之间的异步边界，Python 服务负责事件消费和后续智能复核编排。

当前实现是开发期可运行基线：`./ssv` shell CLI 负责构建、运行、测试和本地依赖启动；实时链路由 `gst-launch-1.0` 拼接 C++ 插件运行。后续 roadmap 会新增 C++ pipeline runner，把长期运行时的 pipeline 构建、错误处理和状态观测迁入 C++。

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
  python3 python3-venv docker.io docker-compose-plugin
```

Debian 12 默认源通常没有 ONNX Runtime C++ 开发包。`./ssv build` 在找不到 `onnxruntime.pc` 时会自动下载官方 CPU release 到 `.deps/onnxruntime/`，并生成 Meson 可识别的 `pkg-config` 文件。

Arch Linux:

```bash
sudo pacman -S gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad \
  yaml-cpp hiredis nlohmann-json onnxruntime-cpu meson python uv docker docker-compose opencv
```

## 快速开始

```bash
# 1. 进入项目
cd site-safety-vision

# 2. 准备本地环境变量
cp .env.example .env

# 3. 编辑 .env，至少设置 RTSP 地址
# SSV_RTSP_URL=rtsp://user:pass@host:554/stream

# 4. 下载默认 YOLOv8n ONNX 模型，或在 .env 设置 SSV_MODEL_PATH
./ssv download-model

# 5. 编译 C++ 插件
./ssv build

# 6. 启动本地 Redis
./ssv redis

# 7. 运行无头短时链路检查
./ssv test

# 8. 打开显示窗口观察实时链路
./ssv run --display
```

`./ssv test` 默认受 `SSV_CHECK_TIMEOUT` 控制，适合检查链路是否能启动并向 Redis 写入消息。`./ssv run` 是长期运行模式，按 `Ctrl+C` 退出；`./ssv run --display` 关闭视频窗口或中断进程后退出。

## 命令

| 命令 | 说明 |
| --- | --- |
| `./ssv build` | 编译 C++ GStreamer 插件和测试 |
| `./ssv clean` | 删除 Meson 构建目录 `build` |
| `./ssv redis` | 启动 Docker Redis 开发环境 |
| `./ssv test` | 运行无头短时 GStreamer 链路检查 |
| `./ssv run` | 运行无头实时链路 |
| `./ssv run --display` | 运行实时链路并打开视频窗口 |
| `./ssv run --display --overlay` | 在显示窗口绘制检测框，当前用于调试 |
| `./ssv run --display --sink waylandsink` | 指定显示 sink |
| `./ssv agent` | 启动 Python Agent 服务 |
| `./ssv inspect` | 查看插件注册和属性信息 |
| `./ssv stop` | 停止后台服务 |
| `./ssv download-model` | 下载默认 YOLOv8n ONNX 模型 |

## 配置

长期配置入口是 `config/ssv.default.yaml`。本地开发覆盖使用 `.env`，由 shell 脚本和 Python Agent 加载。首次运行前复制模板：

```bash
cp .env.example .env
```

常用环境变量：

| 变量 | 作用 | 默认值 |
| --- | --- | --- |
| `SSV_CONFIG_PATH` | YAML 配置文件路径 | `config/ssv.default.yaml` |
| `SSV_RTSP_URL` | RTSP 视频源地址 | 无，必须设置 |
| `SSV_MODEL_PATH` | YOLO ONNX 模型路径 | `models/yolov8n.onnx` |
| `SSV_CHECK_TIMEOUT` | `./ssv test` 超时时间 | `30s` |
| `GST_DEBUG` | GStreamer 调试级别 | `ssv*:4` |
| `SSV_DISPLAY_SINK` | 显示 sink | 自动选择 |
| `SSV_DISPLAY_OVERLAY` | 是否默认开启 overlay | `false` |
| `SSV_FRAME_WIDTH` | 分析帧宽度 | `640` |
| `SSV_FRAME_HEIGHT` | 分析帧高度 | `480` |
| `SSV_ANALYSIS_FPS` | 分析帧率 | `5` |
| `SSV_DISPLAY_FPS` | 显示帧率 | `30` |
| `REDIS_HOST` | Redis 地址 | `localhost` |
| `REDIS_PORT` | Redis 端口 | `6379` |
| `SSV_REDIS_STREAM_KEY` | Redis Stream key | `ssv:events` |

ONNX Runtime 下载和路径覆盖：

```bash
# 指定自动下载版本
SSV_ONNXRUNTIME_VERSION=1.25.1 ./ssv build

# 使用已有安装
export PKG_CONFIG_PATH="/path/to/onnxruntime/lib/pkgconfig:$PKG_CONFIG_PATH"
export LD_LIBRARY_PATH="/path/to/onnxruntime/lib:$LD_LIBRARY_PATH"
./ssv build

# 修改自动下载目录
SSV_ONNXRUNTIME_ROOT=/path/to/onnxruntime ./ssv build
```

## 运行和调试

### 构建与插件检查

```bash
./ssv build
./ssv inspect
```

如果插件没有被发现，先确认 `./ssv build` 成功。脚本会自动导出 `GST_PLUGIN_PATH` 和 `LD_LIBRARY_PATH`；手动运行 `gst-launch-1.0` 时需要自己设置这些路径，推荐优先通过 `./ssv run` 调试。

### 无头链路检查

```bash
SSV_CHECK_TIMEOUT=15s ./ssv test
```

该命令会构建插件、检查模型、确保 Redis 运行，然后启动 RTSP 到分析分支的短时链路。退出码 `0` 表示链路按预期启动并在超时后结束。

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

如果 `.env` 修改了 `SSV_REDIS_STREAM_KEY`，把命令中的 `ssv:events` 换成对应 key。

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

常见排查顺序：先跑 `./ssv inspect` 确认插件注册，再跑 `./ssv test` 确认无头链路，再跑 `./ssv run --display` 排查显示分支。

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
