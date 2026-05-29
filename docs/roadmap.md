# 安全帽佩戴视频监测分析系统 Roadmap

本路线图采用“里程碑优先，任务包领取，主线作为领域标签”的组织方式。团队按阶段目标推进，每个阶段拆成互不冲突的并行任务包，由 4 名成员领取；`T1-T5` 只用于标记领域边界和接口影响，不绑定固定成员。

当前仓库已经具备单机工程基线：`./ssv` 和 `scripts/` 提供开发入口，`gst/` 中的 GStreamer C++ 插件承载实时视频分析节点，`Redis Streams` 是实时链路和 Agent 链路的异步边界，`agent/` 提供 Python Agent 服务基线。后续目标是把“视频输入 -> YOLO 检测 -> 跟踪 -> 事件 -> 证据 -> Agent 复核 -> 结果输出”逐步打通成可演示、可验证、可交接的单机原型。

## 协作原则

1. 按里程碑推进，每个里程碑进入实现前必须有中文 spec 和 plan。
2. 每个里程碑拆成 4 个左右任务包，成员按任务包领取，尽量避免多人同时修改同一文件。
3. 跨领域标签的接口变更必须先更新中文 spec，再进入实现。
4. 脚本继续承担构建、清理、依赖检查、本地 Redis、调试入口和测试编排；长期运行时错误处理后续由 C++ pipeline runner 承担。
5. Python Agent 不进入每帧同步检测链路，只消费 Redis 中的事件和证据。

## 文档规则

每个里程碑进入实现前必须补齐：

1. `docs/specs/YYYY-MM-DD-Mx-名称-spec.md`。
2. `docs/plans/YYYY-MM-DD-Mx-名称-plan.md`。
3. 如果里程碑对应集成节点，也可以使用 `Ix` 命名，例如 `docs/specs/YYYY-MM-DD-I1-本地可运行链路稳定-spec.md`。
4. spec 必须说明目标、范围、接口契约、数据结构、错误处理和验证方式。
5. plan 必须说明实施步骤、文件改动、测试命令、兼容性和回滚方式。

spec 和 plan 使用中文；代码标识、命令、路径、配置键保持英文原文。文档中不要保留未完成占位内容；暂不展开的内容写入“非本阶段范围”。

## 领域标签

| 标签 | 领域边界 | 主要目录 | 关键接口 |
| --- | --- | --- | --- |
| T1 | 实时视频链路与运行时：输入、解码、显示、pipeline runner、运行状态 | `scripts/`、`config/`、后续 runner | YAML 配置、GStreamer pipeline、退出码、日志字段 |
| T2 | 感知算法与元数据：YOLO 推理、后处理、检测元数据、跟踪、overlay | `gst/ssv-infer`、`gst/ssv-track`、`gst/ssv-overlay`、`gst/ssv-common`、`gst/tests` | `ssv_meta`、插件属性、测试素材 |
| T3 | 事件与异步边界：事件判定、证据输出、Redis 消息、事件状态 | `gst/ssv-pub`、后续事件/证据模块、`config/` | 事件 schema、证据路径、Redis Streams |
| T4 | Agent 与知识复核：事件消费、上下文构造、状态机、工具路由、模型 provider | `agent/`、后续知识库/工具模块 | Agent 输入输出、工具协议、provider 抽象 |
| T5 | 工程集成与质量：测试矩阵、文档模板、CI、本地验证、demo 和交付检查 | `tests/`、`docs/`、构建脚本、CI 配置 | 测试命令、集成验收、文档和发布检查清单 |

领域标签只说明任务影响范围，不绑定具体成员。一个成员可以领取跨多个领域的任务包；同一领域也可以被多人并行处理，只要文件边界、接口契约和合流顺序清楚。

## 任务领取规则

1. 每个里程碑默认拆成 `A-D` 四个任务包，4 名成员各领取一个任务包。
2. 任务包按文件边界和接口边界拆分，避免多人同时改同一核心文件。
3. 每个任务包必须写清楚输出和验收，不用“负责某领域”代替具体工作。
4. 涉及接口冻结的任务包先完成文档，再改代码。
5. T5 类质量工作按任务包轮值承担，不固定压给某一个人。

## 当前基线 M0

**状态**：已完成。

已经具备：

- `./ssv build` / `./ssv clean` / `./ssv test` / `./ssv run` / `./ssv run --display` / `./ssv agent` / `./ssv redis` / `./ssv inspect` / `./ssv stop`。
- Meson 构建输出目录固定为 `build`。
- GStreamer C++ 插件构建基线：`ssv-template`、`ssv-infer`、`ssv-track`、`ssv-pub`、`ssv-overlay`、`ssv-common`。
- Python Agent 服务、配置加载、Redis Streams 消费基线。
- Docker Redis 开发环境。
- C++ 插件单元测试、Agent 单元测试和 CLI 脚本测试基线。
- GitHub Actions CI 基线：PR 到 `main` 和 push 到 `main` 时运行 shell 语法检查与 `./ssv test`；CI 不依赖 RTSP、模型 smoke 或显示环境。

已识别缺口：

- `scripts/pipeline.sh` 仍使用 `gst-launch-1.0` 拼接运行链路。
- YAML 配置已存在，但部分运行参数仍由 `.env` 和脚本环境变量覆盖。
- 当前模型 `models/yolov8n.onnx` 是 COCO 模型，只能验证 `person` 检测链路，不能直接判断安全帽。
- 事件判定、证据输出、完整 Agent 状态机尚未完成。
- 生产级 C++ pipeline runner 尚未完成。

## 里程碑总览

| 里程碑 | 阶段目标 | 集成节点 | 主要输出 |
| --- | --- | --- | --- |
| M0 | 工程基线确认 | P0 | 已完成的构建、测试、插件、Redis、Agent 和 CLI 基线 |
| M1 | YOLO 工程化实践与安全帽模型训练预研 | I1 前置 | YOLO 推理链路说明、`ssv_meta` 检测契约、mock/真实模型 smoke、安全帽训练最小闭环、事件输入草案 |
| M2 | 本地视频链路稳定 | I1 | RTSP/文件输入、显示/无显示运行、overlay 调试、链路 smoke |
| M3 | 检测、跟踪、事件消息打通 | I2 | track ID、事件 schema、Redis Streams 消息、Agent 消费样例 |
| M4 | 证据输出与 Agent 复核打通 | I3 | 证据路径、Agent 上下文、状态机、provider、复核结果回写 |
| M5 | 端到端 demo 和交付收口 | I4 | 可重复演示、运行手册、验收脚本、故障排查、发布检查清单 |

推进顺序：

```text
M0 工程基线
  |
  v
M1 YOLO 工程化实践与安全帽模型训练预研
  |
  v
M2 本地视频链路稳定 --> I1
  |
  v
M3 检测、跟踪、事件消息打通 --> I2
  |
  v
M4 证据输出与 Agent 复核打通 --> I3
  |
  v
M5 端到端 demo 和交付收口 --> I4
```

## M1: YOLO 工程化实践与安全帽模型训练预研

**目标**：让团队掌握当前 YOLO ONNX 推理链路，冻结检测元数据契约，形成可复现的 mock 和真实模型验证方式；同时启动安全帽模型自主训练的最小闭环预研。本阶段允许用小规模数据集完成训练、导出和工程接入验证，但不承诺安全帽识别准确率；`models/yolov8n.onnx` 仍只用于验证 COCO `person` 检测链路。

建议文档：

- `docs/specs/YYYY-MM-DD-M1-YOLO工程化与安全帽训练预研-spec.md`
- `docs/plans/YYYY-MM-DD-M1-YOLO工程化与安全帽训练预研-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T2 | YOLO 推理链路梳理：分析 `ssvinfer` 的 ONNX Runtime 加载、BGR 输入、letterbox、CHW、置信度过滤、类别过滤和 NMS | YOLO 推理链路说明，明确当前支持的输出格式和限制 | `./ssv build`、`meson test -C build` |
| B | T2 | 检测元数据契约：固化 `SsvDetection`、`SsvFrameDetections` 字段语义和坐标规则 | `ssv_meta` 检测字段契约，坐标统一为归一化坐标 | `gst/tests` 覆盖元数据基本行为 |
| C | T2 | 安全帽训练最小闭环：定义类别和 label map，用小规模数据集跑通 YOLO baseline 训练、ONNX 导出和单图推理 | 类别表、class_id 顺序、数据目录约定、训练命令、导出命令、ONNX 产物说明 | 不以准确率验收，至少完成数据到 ONNX 的闭环；如时间允许接入 `ssvinfer` smoke |
| D | T1/T3/T5 | 工程验证与下游输入：建立 mock/真实模型 smoke，梳理 `scripts/pipeline.sh` 参数，定义事件输入草案，形成 M1 验收清单 | mock/真实模型验证命令、运行参数清单、事件输入字段草案、测试矩阵增量 | mock smoke 可在无模型环境跑通；真实模型 smoke 明确环境依赖；文档评审通过 |

冻结接口：

- `ssv_meta` 检测字段和坐标语义。
- `ssvinfer` 基础插件属性：`model-path`、`conf-threshold`、`target-class`、`mock-detect`、`async`。
- 安全帽训练预研的 label map、类别顺序和 ONNX 导出约束。
- T3 消费检测结果所需的最小字段。

退出标准：

- 团队能说明当前 YOLO 模型输入、输出、后处理和限制。
- mock 推理链路稳定通过自动测试。
- 真实 `yolov8n.onnx` 链路有可复现命令，并明确依赖模型文件、视频源和显示环境。
- 安全帽训练预研至少完成类别定义、数据目录约定、训练命令和 ONNX 导出命令。
- 理想结果是自训练安全帽 ONNX 能跑一次单图推理或 `ssvinfer` smoke；如果未完成，必须记录阻塞原因和下一步。

非本阶段范围：

- 安全帽业务准确率评估。
- 大规模数据集治理和正式模型评估。
- 完整事件判定和 Agent 复核。

## M2: 本地视频链路稳定

**目标**：把本地运行链路从“能拼起来”推进到“可重复验证”。本阶段重点是输入、显示、overlay、无显示 smoke 和配置一致性。

建议文档：

- `docs/specs/YYYY-MM-DD-M2-本地视频链路稳定-spec.md`
- `docs/plans/YYYY-MM-DD-M2-本地视频链路稳定-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T1 | 输入链路：稳定 RTSP 输入和本地视频文件输入参数 | 输入配置契约，明确 `sources` 和脚本参数优先级 | 文件输入和 RTSP 输入至少一种可本地 smoke |
| B | T1 | 运行模式：稳定显示分支、无显示分支、`--overlay` 和 `--sink` 行为 | 运行模式说明 | `./ssv run`、`./ssv run --display` 按环境验证 |
| C | T2 | 可视化链路：确认 `ssvinfer -> ssvtrack -> ssvoverlay` 在 mock 和真实模型下的元数据流转 | overlay 调试路径说明 | overlay 可显示检测框，或记录无法显示原因 |
| D | T5 | I1 验收：补充集成验收脚本或手工验收清单 | I1 验收清单 | `./ssv build`、`meson test -C build`、`bash tests/ssv_cli_test.sh`、`./ssv test` |

冻结接口：

- YAML 中 `sources`、`display`、`pipeline`、`inference`、`tracking` 的字段语义。
- `./ssv run`、`./ssv run --display`、`./ssv test` 的基础行为。

退出标准：

- 本地输入到推理、跟踪、显示或无显示输出的链路可重复运行。
- mock 模式不依赖模型、视频源和显示环境即可用于基础回归。
- 真实模型和显示验证的环境依赖被明确记录。

非本阶段范围：

- 完整事件闭环。
- Agent 复核闭环。
- 多路视频调度。

## M3: 检测、跟踪、事件消息打通

**目标**：把 T2 的检测/跟踪元数据转换为 T3 的结构化事件，并通过 Redis Streams 给 T4 提供稳定消费样例。

建议文档：

- `docs/specs/YYYY-MM-DD-M3-检测跟踪事件消息打通-spec.md`
- `docs/plans/YYYY-MM-DD-M3-检测跟踪事件消息打通-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T2 | 跟踪字段：稳定 track ID、轨迹状态和跟踪插件属性 | 跟踪字段契约 | `meson test -C build` 覆盖基础跟踪行为 |
| B | T3 | 事件消息：定义并实现最小事件 schema | 事件类型、严重级别、触发原因、检测列表、轨迹列表 | Redis Stream 中消息字段可被样例消费 |
| C | T3 | 安全帽事件规则：定义连续命中、低置信度、检测冲突等初版规则 | 规则说明和测试入口 | 支持 mock/no-op 降级 |
| D | T4/T5 | 消费与验收：编写 Agent 侧事件解析样例，增加 I2 集成验收 | 事件消费样例、解析测试、C++ 发布和 Python 消费字段一致性检查 | C++、CLI、Agent 基础测试通过 |

冻结接口：

- Redis Stream key、字段名、字段类型和错误语义。
- `detections`、`tracks` 在事件消息中的序列化格式。
- Agent 消费事件所需的最小输入。

退出标准：

- 检测/跟踪结果能形成结构化 Redis 消息。
- Agent 侧能解析消息并完成最小消费测试。
- 后续新增证据路径和复核结果时有兼容扩展位置。

非本阶段范围：

- 证据文件生成。
- 完整 Agent 状态机。
- 外部通知和报告。

## M4: 证据输出与 Agent 复核打通

**目标**：把结构化事件扩展为“事件 + 证据 + Agent 复核结果”的异步闭环。本阶段只完成单机原型复核闭环，不做完整业务平台。

建议文档：

- `docs/specs/YYYY-MM-DD-M4-证据输出与Agent复核打通-spec.md`
- `docs/plans/YYYY-MM-DD-M4-证据输出与Agent复核打通-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T3 | 证据输出：保存关键帧、检测框渲染图和可选短片段 | 证据目录和命名契约 | 事件消息中包含可访问证据路径 |
| B | T3 | 事件状态：定义待处理、处理中、已完成、失败、待人工复核语义 | 事件状态语义和降级说明 | Redis 发布失败和证据保存失败有降级说明 |
| C | T4 | Agent 状态机：实现事件消费、上下文构造和轻量状态机 | Agent 状态流转 | `cd agent && uv run --extra dev pytest` |
| D | T4/T5 | provider 与验收：实现模型 provider、工具路由最小接口，增加 I3 验收和失败场景清单 | provider/tool 抽象、I3 验收清单 | mock provider 可跑通复核流程；事件到 Agent 复核结果可重复演示 |

冻结接口：

- 证据路径和文件命名规则。
- Agent 结果回写格式。
- Agent 状态机的外部可观测状态。

退出标准：

- Agent 能基于 Redis 事件和证据路径完成一次复核流程。
- 复核结果能回写到约定位置或约定消息。
- 证据缺失、模型 provider 不可用、Redis 异常有明确降级行为。

非本阶段范围：

- 前端页面。
- 数据库表结构。
- 多租户和权限体系。

## M5: 端到端 demo 和交付收口

**目标**：把 T1-T4 的能力合流成可演示、可排障、可交接的单机端到端原型。

建议文档：

- `docs/specs/YYYY-MM-DD-M5-端到端Demo和交付收口-spec.md`
- `docs/plans/YYYY-MM-DD-M5-端到端Demo和交付收口-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T1 | 演示运行时：明确 C++ pipeline runner 第一版或 `gst-launch` fallback 演示路径 | 运行时边界说明 | runner 或 fallback 路径至少一个可稳定演示 |
| B | T2 | 演示模型：接入安全帽专用模型，或保留 mock/person 模型演示降级 | 模型接入说明 | 模型类别和阈值可配置 |
| C | T3 | 演示事件：完成事件、证据和 Redis 发布的演示路径 | 事件证据样例 | 演示事件可重复生成 |
| D | T4/T5 | 演示复核与交付：完成 Agent 复核、解释、结果输出，并更新 README、运行手册、故障排查、测试矩阵和发布检查清单 | 复核结果样例、交付文档和验收脚本 | Agent 流程可重复运行；全量可用验证命令通过，环境缺失项有明确说明 |

冻结接口：

- demo 配置文件和运行命令。
- 端到端日志字段和退出码。
- 交付验收清单。

退出标准：

- 从视频输入到检测、事件、证据、Agent 复核、结果输出形成闭环。
- 新成员可以按 README 和运行手册复现 demo。
- CI 和本地验证边界清楚，无法自动化的验收项有手工步骤。

非本阶段范围：

- WVP、ZLMediaKit 等平台级接入。
- 大规模多路视频调度、集群部署和高可用治理。
- 完整前端业务系统。

## 接口冻结表

| 接口 | 冻结里程碑 | 负责 | 消费 | 说明 |
| --- | --- | --- | --- | --- |
| `ssv_meta` 检测字段和坐标语义 | M1 | T2 | T1、T3、T5 | 检测框统一使用归一化坐标 |
| `ssvinfer` 基础插件属性 | M1 | T2 | T1、T5 | `model-path`、`conf-threshold`、`target-class`、`mock-detect`、`async` |
| 安全帽训练产物契约 | M1 | T2 | T1、T3、T5 | label map、类别顺序、ONNX 导出约束和产物说明 |
| 事件输入字段草案 | M1 | T3 | T4、T5 | 先冻结最小字段，M3 扩展为 Redis schema |
| YAML 运行配置字段 | M2 | T1 | T2、T3、T4、T5 | `sources`、`display`、`pipeline`、`inference`、`tracking` |
| Redis 事件 schema | M3 | T3 | T4、T5 | Stream key、字段名、字段类型和错误语义 |
| 跟踪字段和 track ID 语义 | M3 | T2 | T3、T5 | 进入事件消息前冻结 |
| 证据路径和文件命名 | M4 | T3 | T4、T5 | Agent 必须能按路径读取证据 |
| Agent 结果回写格式 | M4 | T4 | T3、T5 | 支持完成、失败、待人工复核 |
| 端到端运行命令和验收清单 | M5 | T5 | T1、T2、T3、T4 | README 和运行手册同步 |

接口一旦被后续里程碑采用，修改时必须保留兼容路径，或在对应 spec 中明确迁移方式。

## 验证矩阵

| 范围 | 命令 | 说明 |
| --- | --- | --- |
| C++ 插件、元数据、Meson | `./ssv build` | 构建共享库、插件和测试 |
| C++ 单元测试 | `meson test -C build` | 覆盖插件注册、元数据和 C++ 测试 |
| Python Agent | `cd agent && uv run --extra dev pytest` | 覆盖 Agent 配置、消费和服务测试 |
| CLI/脚本 | `bash tests/ssv_cli_test.sh` | 覆盖 `./ssv` CLI 和脚本入口 |
| 综合测试入口 | `./ssv test` | 代码测试和链路 smoke 编排，部分项依赖 RTSP、模型和 Redis |
| 本地显示链路 | `./ssv run --display` | 依赖视频源、模型、Redis 和显示环境 |

每个 plan 必须说明本阶段实际需要运行哪些命令，以及哪些命令因本地环境缺失无法运行。PR 合入 `main` 前至少要求 GitHub Actions CI 通过；本地链路 smoke 和显示窗口验证保留为里程碑验收项。

## 分支和合流

- 每个里程碑可以使用短生命周期分支，例如 `feature/m1-yolo-engineering` 或 `integration/m3-events-redis`。
- 成员按任务包拆短分支，例如 `feature/m1-a-infer-notes`、`feature/m1-c-helmet-training`。
- 同一里程碑内优先按文件边界拆分；确实需要多人改同一文件时，先约定合流顺序。
- 跨领域标签接口改动先提文档 PR，再提实现 PR。
- 集成分支只做接口适配、测试补齐和小范围修复，不承载大功能开发。
- 不要回退他人或既有未提交改动；不要使用 `git reset --hard` 或 `git checkout --` 清理文件。
