# 安全帽佩戴视频监测分析系统 - Roadmap

本路线图按团队并行协作重规划。当前仓库已经完成项目骨架和实时运行基线，后续不再按单人串行的 M2-M7 推进，而是拆成 4 条长期并行技术主线，加 1 条工程集成主线，并通过阶段性集成节点合流。

当前实现形态：`./ssv` 和 `scripts/` 提供开发入口，`gst/` 中的 GStreamer C++ 插件承载实时视频分析节点，`Redis Streams` 是实时链路和 Agent 链路的异步边界，`agent/` 提供 Python Agent 服务基线。`./ssv test` 作为测试编排入口，负责代码测试和链路冒烟验证，不承担常驻运行职责。

## 协作原则

1. 按主线并行开发，按接口契约集成。
2. 主线之间通过配置、元数据、事件消息、证据路径和 Agent 输入输出解耦。
3. 每条主线可以独立开分支、写 spec、写 plan、开发和测试。
4. 涉及跨主线接口的改动必须先更新中文 spec，并在集成节点统一验收。
5. 脚本继续承担构建、依赖检查、本地 Redis、调试入口和测试编排等开发运维职责；长期运行时的 pipeline 构建、节点参数、错误处理和状态观测逐步迁入 C++ pipeline runner。

## 文档约束

每条技术主线和每个集成节点在进入实现前都必须落中文文档：

1. `docs/specs/YYYY-MM-DD-Tx-主线名称-spec.md` 或 `docs/specs/YYYY-MM-DD-Ix-集成节点-spec.md`。
2. `docs/plans/YYYY-MM-DD-Tx-主线名称-plan.md` 或 `docs/plans/YYYY-MM-DD-Ix-集成节点-plan.md`。
3. spec 必须说明目标、范围、接口契约、数据结构、错误处理、验证方式。
4. plan 必须说明实施步骤、文件改动、测试命令、兼容性和回滚方式。
5. 阶段完成后同步更新 `README.md`、本 roadmap 和相关 spec。

spec 和 plan 必须使用中文；代码标识、命令、路径、配置键保持英文原文。

## 当前基线 P0

**状态**：已完成。

P0 是所有并行主线的共同起点，已经具备：

- `./ssv build` / `./ssv clean` / `./ssv test` / `./ssv run` / `./ssv run --display` / `./ssv agent` / `./ssv redis` / `./ssv inspect` / `./ssv stop`。
- Meson 构建输出目录固定为 `build`。
- GStreamer C++ 插件目录和构建基线：`ssv-template`、`ssv-infer`、`ssv-track`、`ssv-pub`、`ssv-overlay`、`ssv-common`。
- Python Agent 服务、配置加载、Redis Streams 消费基线。
- Docker Redis 开发环境。
- C++ 插件单元测试、Agent 单元测试和 CLI 脚本测试基线。

P0 尚未完成但已识别的边界：

- `scripts/pipeline.sh` 仍使用 `gst-launch-1.0` 拼接运行链路，`scripts/test.sh` 负责测试编排。
- YAML 配置已存在，但部分运行参数仍由 `.env` 和脚本环境变量覆盖。
- 事件判定、证据输出、完整 Agent 状态机尚未完成。

## 并行主线总览

| 主线 | 名称 | 责任边界 | 主要目录 | 主要接口 |
| --- | --- | --- | --- | --- |
| T1 | 实时视频链路与运行时 | 输入、解码、显示、pipeline runner、运行时错误处理 | `scripts/`、`config/`、后续 runner、部分 `gst/` | YAML 配置、GStreamer pipeline、运行状态 |
| T2 | 感知算法与元数据 | YOLO 推理、后处理、检测元数据、跟踪、overlay | `gst/ssv-infer`、`gst/ssv-track`、`gst/ssv-overlay`、`gst/ssv-common`、`gst/tests` | `ssv_meta`、插件属性、测试素材 |
| T3 | 事件与异步边界 | 事件判定、证据输出、Redis 消息、事件状态 | `gst/ssv-pub`、后续事件/证据模块、`config/` | 事件 schema、证据路径、Redis Streams |
| T4 | Agent 与知识复核 | 事件消费、上下文构造、状态机、工具路由、模型 provider、知识库 | `agent/`、后续知识库/工具模块 | Agent 输入输出、工具协议、provider 抽象 |
| T5 | 工程集成与质量 | CI、测试矩阵、文档规范、demo、发布检查 | `tests/`、`docs/`、构建脚本、CI 配置 | 测试命令、集成验收、文档模板 |

T1-T4 是至少 4 个团队分支可并行推进的主线。T5 可以由专人负责，也可以由技术负责人或轮值维护。

## 集成节点总览

| 集成节点 | 目标 | 依赖主线 | 验收重点 |
| --- | --- | --- | --- |
| I1 | 本地可运行链路稳定 | T1、T2、T5 | RTSP/文件输入、显示、mock/真实推理、基础测试稳定 |
| I2 | 检测/跟踪/Redis 消息打通 | T2、T3、T5 | 检测元数据、track ID、Redis 消息格式、消费样例一致 |
| I3 | 事件/证据/Agent 消费打通 | T3、T4、T5 | 事件 schema、证据路径、Agent 上下文和状态流转 |
| I4 | 端到端 demo 和运行手册 | T1、T2、T3、T4、T5 | 视频输入到复核输出完整闭环，可重复演示和调试 |

集成节点不是长期分支，而是合流检查点。每个集成节点必须有中文 spec 和 plan，明确冻结哪些接口、跑哪些测试、接受哪些降级行为。

## T1: 实时视频链路与运行时

**目标**：稳定视频输入、显示分支和长期运行时，使实时链路能从开发调试入口演进到 C++ pipeline runner。

责任范围：

- RTSP 输入稳定性。
- 本地视频文件输入。
- 解码、格式转换、分析帧率和显示帧率控制。
- 显示 sink 自动选择和显式 `--sink` 覆盖。
- 显示分支与分析分支隔离。
- `--overlay` 调试路径接入。
- C++ pipeline runner：YAML 驱动 pipeline、GStreamer bus 监听、错误码、退出码、运行状态。
- `./ssv run` 从 `gst-launch` wrapper 平滑迁移到 runner，保留 fallback 调试路径。

不负责：

- YOLO 后处理算法细节。
- 事件判定规则。
- Agent 状态机。

关键接口：

- `config/ssv.default.yaml` 中的 source、display、pipeline、inference、tracking、redis 配置字段。
- 插件属性设置约定。
- runner 的退出码和日志格式。

建议分支：`feature/t1-runtime-pipeline`。

## T2: 感知算法与元数据

**目标**：把检测、后处理、跟踪和 overlay 输出稳定为统一 C++ 元数据模型。

责任范围：

- `ssvinfer` 的 YOLO ONNX Runtime 推理。
- 输入尺寸、颜色格式、letterbox、置信度过滤、类别过滤、NMS。
- 检测框、类别、置信度、frame ID、source ID 等 `ssv_meta` 定义。
- `ssvtrack` 的 track ID、轨迹状态、mock/测试模式。
- `ssvoverlay` 的检测框和调试显示。
- C++ 单元测试、插件测试、测试帧和 mock 数据。

不负责：

- 视频源连接和 pipeline runner。
- Redis 消息状态语义。
- Agent 复核流程。

关键接口：

- `ssv_meta` 的 ABI/API。
- `ssvinfer`、`ssvtrack`、`ssvoverlay` 插件属性。
- 传给 T3 的检测/跟踪元数据字段。

建议分支：`feature/t2-perception-meta`。

## T3: 事件与异步边界

**目标**：把检测/跟踪结果转换为结构化事件，保存证据，并通过 Redis Streams 可靠交给 Agent。

责任范围：

- 事件判定节点或插件。
- 未佩戴安全帽、低置信度、检测冲突、连续命中等事件规则。
- 事件类型、严重程度、触发原因、source ID、frame ID、detections、tracks 的结构化定义。
- 关键帧、检测框渲染图、可选短片段的证据输出。
- Redis Streams 消息 schema。
- Redis 发布失败、重试、降级和状态记录边界。

不负责：

- YOLO 模型内部推理。
- Agent 工具调用和模型 provider。
- C++ runner 的 bus 监听实现。

关键接口：

- 事件 schema。
- 证据文件路径和目录规则。
- Redis Stream key、字段格式、错误语义。
- Agent 消费端所需的最小事件输入。

建议分支：`feature/t3-events-redis`。

## T4: Agent 与知识复核

**目标**：完成 Redis 事件消费后的上下文构造、状态机编排、工具路由、模型 provider 和知识检索边界。

责任范围：

- Redis Streams 消费、确认、失败和重试语义。
- Agent 事件解析和上下文构造。
- 轻量 LLM 状态机：待消费、解析、上下文构造、策略选择、工具调用、结果汇总、结果回写、完成/失败/待人工复核。
- 工具路由：证据读取、视觉复核、规则解释、通知、报告。
- 模型 provider：多模态 provider、文本 provider。
- 规则知识库和向量检索。
- Agent 单元测试和集成测试。

不负责：

- 每帧同步检测。
- GStreamer 插件内部元数据实现。
- 证据文件生成。

关键接口：

- T3 提供的事件 schema 和证据路径。
- Agent 结果回写格式。
- 工具协议和 provider 抽象。

建议分支：`feature/t4-agent-review`。

## T5: 工程集成与质量

**目标**：让并行开发可控，保证每条主线都有独立验证和集成验收。

责任范围：

- 测试矩阵：C++、Python、CLI、集成链路。
- CI 或本地等价验证脚本。
- 文档模板：主线 spec、主线 plan、集成节点 spec、集成节点 plan。
- demo 素材和端到端运行手册。
- 版本合流检查清单。
- 目录、命名、配置字段和日志规范。

关键接口：

- 所有主线的测试命令。
- 集成节点验收脚本。
- 文档和发布检查清单。

建议分支：`feature/t5-quality-integration`。

## 接口冻结优先级

并行开发的第一优先级不是把功能一次做完，而是尽早冻结跨主线接口。

| 优先级 | 接口 | 负责主线 | 消费主线 |
| --- | --- | --- | --- |
| P0 | `ssv_meta` 检测/跟踪字段 | T2 | T1、T3 |
| P0 | Redis 事件 schema 草案 | T3 | T4、T5 |
| P0 | YAML 配置字段 | T1 | T2、T3、T4、T5 |
| P1 | 证据路径和文件命名 | T3 | T4、T5 |
| P1 | Agent 结果回写格式 | T4 | T3、T5 |
| P1 | runner 退出码和日志字段 | T1 | T5 |
| P2 | 工具协议和模型 provider 抽象 | T4 | T5 |

接口一旦被集成节点采用，后续修改必须保留兼容路径，或者在 spec 中明确迁移方式。

## 建议推进顺序

```text
P0 已完成
  |
  +--> T1 实时视频链路与运行时 -------------+
  +--> T2 感知算法与元数据 -----------------+--> I1 本地可运行链路稳定
  +--> T5 工程集成与质量 -------------------+

T2 感知元数据稳定 ----+
T3 事件与异步边界 ----+--> I2 检测/跟踪/Redis 消息打通
T5 测试矩阵 ----------+

T3 事件证据稳定 ------+
T4 Agent 与知识复核 --+--> I3 事件/证据/Agent 消费打通
T5 集成测试 ----------+

T1 + T2 + T3 + T4 + T5 --> I4 端到端 demo 和运行手册
```

## 分支和合流建议

- 每条主线使用独立长期分支，例如 `feature/t1-runtime-pipeline`。
- 跨主线接口改动先提文档 PR，再提实现 PR。
- 主线内部可以拆短分支，但合入主线前必须跑本主线测试。
- 集成节点用短生命周期分支，例如 `integration/i2-redis-message`。
- 集成分支只做接口适配、测试补齐和小范围修复，不承载大功能开发。

## 验收命令基线

各主线至少维护这些验证命令：

```bash
# C++ 插件和元数据
./ssv build
meson test -C build

# Python Agent
cd agent && uv run --extra dev pytest

# CLI 脚本
bash tests/ssv_cli_test.sh

# 测试套件；链路冒烟依赖 RTSP、模型和 Redis
./ssv test
./ssv run --display
```

每个主线 plan 必须说明本阶段需要运行哪些命令，以及哪些命令因本地环境缺失无法运行。
