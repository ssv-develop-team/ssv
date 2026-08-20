# 安全帽佩戴视频监测分析系统 Roadmap

本路线图采用“里程碑优先，任务包领取，主线作为领域标签”的组织方式。团队按阶段目标推进，每个阶段拆成互不冲突的并行任务包，由 4 名成员领取；`T1-T5` 只用于标记领域边界和接口影响，不绑定固定成员。

当前仓库已经具备单机工程基线：`./ssv` 和 `scripts/` 提供开发入口，`gst/` 中的 GStreamer C++ 插件承载实时视频分析节点，`Redis Streams` 是实时链路和 Agent 链路的异步边界，`agent/` 提供 Python Agent 服务基线。后续目标是把“视频输入 -> YOLO 检测 -> 跟踪 -> 事件 -> 证据 -> Agent 复核 -> 结果输出”逐步打通成可演示、可验证、可交接的单机原型。

## 协作原则

1. 按里程碑推进，每个里程碑进入实现前必须有中文 spec 和 plan。
2. 每个里程碑拆成 4 个左右任务包，成员按任务包领取，尽量避免多人同时修改同一文件。
3. 跨领域标签的接口变更必须先更新中文 spec，再进入实现。
4. Python `./ssv` 入口统一承担构建、清理、依赖检查、本地 Redis、调试入口和测试编排；长期运行时由 C++ pipeline runner 承担。
5. Python Agent 不进入每帧同步检测链路，只消费 Redis 中的事件和证据。

## 文档规则

每个里程碑进入实现前必须补齐：

1. `docs/specs/YYYY-MM-DD-Mx-名称-spec.md`。
2. `docs/plans/YYYY-MM-DD-Mx-名称-plan.md`。
3. spec 必须说明目标、范围、接口契约、数据结构、错误处理和验证方式。
4. plan 必须说明实施步骤、文件改动、测试命令、兼容性和回滚方式。

spec 和 plan 使用中文；代码标识、命令、路径、配置键保持英文原文。文档中不要保留未完成占位内容；暂不展开的内容写入“非本阶段范围”。

## 领域标签

| 标签 | 领域边界 | 主要目录 | 关键接口 |
| --- | --- | --- | --- |
| T1 | 实时视频链路与运行时：输入、解码、显示、pipeline runner、运行状态 | `scripts/`、`config/`、`runner/` | YAML 配置、GStreamer pipeline、退出码、日志字段 |
| T2 | 感知算法与元数据：YOLO 推理、后处理、检测元数据、跟踪、overlay | `gst/ssv-infer`、`gst/ssv-track`、`gst/ssv-overlay`、`gst/ssv-common`、`gst/tests` | `ssv_meta`、插件属性、测试素材 |
| T3 | 事件与异步边界：事件判定、证据输出、Redis 消息、事件状态 | `gst/ssv-pub`、后续事件/证据模块、`config/` | 事件 schema、证据路径、Redis Streams |
| T4 | Agent 与知识复核：事件消费、上下文构造、状态机、工具路由、模型 provider | `agent/`、后续知识库/工具模块 | Agent 输入输出、工具协议、provider 抽象 |
| T5 | 工程集成与质量：测试矩阵、文档模板、CI、本地验证、demo 和交付检查 | `scripts/ssv_cli/`、`docs/`、CI 配置 | 测试命令、集成验收、文档和发布检查清单 |

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

- `./ssv build` / `./ssv clean` / `./ssv test` / `./ssv run` / `./ssv run --display` / `./ssv agent` / `./ssv redis start` / `./ssv redis stop` / `./ssv inspect` / `./ssv model ...`。
- Meson 构建输出目录固定为 `build`。
- GStreamer C++ 插件构建基线：`ssv-template`、`ssv-infer`、`ssv-track`、`ssv-pub`、`ssv-overlay`、`ssv-common`。
- Python Agent 服务、配置加载、Redis Streams 消费基线。
- Docker Redis 开发环境。
- C++ 插件单元测试、Agent 单元测试以及 CLI、依赖和模型服务测试基线。
- GitHub Actions CI 基线：PR 到 `main` 和 push 到 `main` 时运行 Python CLI 检查与 `./ssv test`；CI 不依赖 RTSP、模型 smoke 或显示环境。

已识别缺口：

- 当前模型 `models/yolov8n.onnx` 是 COCO 模型，只能验证 `person` 检测链路，不能直接判断安全帽。
- 事件判定、证据输出、完整 Agent 状态机尚未完成。

2026-07-31 增量状态：长期实时链路已迁移到 C++ runner，生产入口通过 `./ssv run` 调用
runner；严格配置、VA/DMABuf/GL memory contract、ONNX Runtime Provider、
模型尺寸 host boundary、GTK 独立框层、结构化日志和退出码已完成自动回归。
原生 Linux 各 GPU profile 的真机强验收仍待分机记录；WSL2/WSLg 只完成了 GTK sink
与 GL context 的兼容性诊断，不作为 VA 强验收。

## 里程碑总览

| 里程碑 | 阶段目标 | 主要输出 |
| --- | --- | --- |
| M0 | 工程基线确认 | 已完成的构建、测试、插件、Redis、Agent 和 CLI 基线 |
| M1 | YOLO 工程化实践与安全帽模型训练预研 | YOLO 推理链路说明、`ssv_meta` 检测契约、mock/真实模型 smoke、安全帽训练最小闭环、事件输入草案 |
| M2 | 跟踪算法工程化实践 | 跟踪算法说明、`track_id` 契约、mock/真实跟踪 smoke、事件输入扩展 |
| M3 | 检测跟踪结果事件消息打通 | 事件 schema、Redis Streams 消息、Agent 消费样例、字段一致性验收 |
| M4 | 事件证据与状态层 | 证据路径、事件状态、失败降级、Agent 输入字段 |
| M5 | Agent 输入与状态机最小闭环 | Agent 上下文、状态机、状态存储、结果回写 |
| M6 | 工具路由与 OpenAI SDK 复核 | 工具路由、证据读取、OpenAI SDK 薄封装、视觉复核和文本解释 |
| M7 | 规则知识与报告输出 | 规则知识输入、检索边界、规则解释、报告输出 |
| M8 | 端到端 demo 和交付收口 | 可重复演示、运行手册、验收脚本、故障排查、发布检查清单 |

推进顺序：

```text
M0 工程基线
  |
  v
M1 YOLO 工程化实践与安全帽模型训练预研
  |
  v
M2 跟踪算法工程化实践
  |
  v
M3 检测跟踪结果事件消息打通
  |
  v
M4 事件证据与状态层
  |
  v
M5 Agent 输入与状态机最小闭环
  |
  v
M6 工具路由与 OpenAI SDK 复核
  |
  v
M7 规则知识与报告输出
  |
  v
M8 端到端 demo 和交付收口
```

## M1: YOLO 工程化实践与安全帽模型训练预研

**目标**：在现有 `ssvinfer` 基础上完成 YOLO 工程化梳理和契约冻结，形成可复现的 mock 和真实模型验证方式；同时启动安全帽模型自主训练的最小闭环预研。本阶段允许用小规模数据集完成训练、导出和工程接入验证，但不承诺安全帽识别准确率；`models/yolov8n.onnx` 仍只用于验证 COCO `person` 检测链路。

现有实现基线：

- `ssvinfer` 已具备 ONNX Runtime 加载、BGR 输入、letterbox、CHW、置信度过滤、类别过滤、NMS、`mock-detect` 和异步推理能力。
- `SsvDetection`、`SsvFrameDetections` 和检测结果写入 `SsvDetectionStore` 的基础结构已存在。
- `./ssv run` 和 `./ssv test` 已具备真实模型 smoke 的环境依赖分支，运行参数统一来自 YAML 配置；本阶段不重复实现这些基础能力，只补齐说明、契约和验收。

建议文档：

- `docs/specs/YYYY-MM-DD-M1-YOLO工程化与安全帽训练预研-spec.md`
- `docs/plans/YYYY-MM-DD-M1-YOLO工程化与安全帽训练预研-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T2 | YOLO 推理链路梳理：基于现有 `ssvinfer` 分析 ONNX Runtime 加载、BGR 输入、letterbox、CHW、置信度过滤、类别过滤和 NMS | YOLO 推理链路说明，明确当前支持的输出格式和限制 | `./ssv build`、`meson test -C build` |
| B | T2 | 检测元数据契约：基于现有结构固化 `SsvDetection`、`SsvFrameDetections` 字段语义和坐标规则，补齐边界测试 | `ssv_meta` 检测字段契约，坐标统一为归一化坐标 | `gst/tests` 覆盖元数据基本行为和异常输入边界 |
| C | T2 | 安全帽训练最小闭环：定义类别和 label map，用小规模数据集跑通 YOLO baseline 训练、ONNX 导出和单图推理 | 类别表、class_id 顺序、数据目录约定、训练命令、导出命令、ONNX 产物说明 | 不以准确率验收，至少完成数据到 ONNX 的闭环；如时间允许接入 `ssvinfer` smoke |
| D | T1/T3/T5 | 工程验证与下游输入：整理 `./ssv run` 与 YAML 配置中的 mock/真实模型 smoke 参数，定义事件输入草案，形成 M1 验收清单 | mock/真实模型验证命令、YAML 运行参数清单、事件输入字段草案、测试矩阵增量 | mock smoke 和真实模型 smoke 的环境边界清楚；文档评审通过 |

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

## M2: 跟踪算法工程化实践

**目标**：在现有 `ssvtrack` 基础上完成跟踪算法工程化梳理和契约冻结，形成可复现的 mock 和真实跟踪验证方式。本阶段参考 M1 的 YOLO 工程化实践组织方式，重点不是引入复杂跟踪框架或重写跟踪插件，而是把现有 `ssvtrack` 的 IoU 匹配逻辑、参数、状态流转、元数据写回和下游消费边界讲清楚、测清楚。

现有实现基线：

- `ssvtrack` 已具备 IoU 匹配、类别约束、简单速度预测、轨迹保留、ID 分配和 `mock-track` 能力。
- `track_id` 字段、`SsvDetectionStore` 的 `HAS_DETECTIONS -> HAS_TRACKS` 状态流转、overlay 读取最新跟踪结果的路径已存在。
- 本阶段不重复开发跟踪插件，重点是契约冻结、测试补强、验证命令和 T3 输入边界。

建议文档：

- `docs/specs/YYYY-MM-DD-M2-跟踪算法工程化实践-spec.md`
- `docs/plans/YYYY-MM-DD-M2-跟踪算法工程化实践-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T2 | 跟踪算法链路梳理：基于现有 `ssvtrack` 分析输入元数据、IoU 匹配、类别约束、速度预测、轨迹保留和 ID 分配策略 | 跟踪算法链路说明，明确当前支持的跟踪能力和限制 | `./ssv build`、`meson test -C build` |
| B | T2 | 跟踪元数据契约：基于现有 `track_id` 和 `SsvDetectionStore` 流转固化语义，补齐边界测试 | 跟踪字段契约，明确检测、跟踪、overlay、发布之间的数据交接 | `gst/tests` 覆盖 track ID 写回、未跟踪默认值和元数据状态流转 |
| C | T2/T5 | mock/真实跟踪验证：补齐 `mock-track` 和 IoU 跟踪的最小 smoke，用人工构造检测序列验证 ID 递增、匹配、丢失保留和重建 | mock/真实跟踪验证命令、测试素材或构造方式、验收清单 | mock 跟踪可在无模型环境跑通；IoU 跟踪行为有自动测试或可复现手工命令 |
| D | T3/T5 | 下游事件输入扩展：梳理 T3 消费跟踪结果所需字段，定义事件消息中 `detections` 和 `tracks` 的最小输入草案 | 事件输入字段扩展、测试矩阵增量、M2 验收清单 | 文档评审通过；C++、CLI 基础测试通过 |

冻结接口：

- `track_id` 字段语义：`-1` 表示未跟踪，非负值表示 `ssvtrack` 分配的轨迹 ID。
- `ssvtrack` 基础插件属性：`frame-rate`、`track-thresh`、`track-buffer`、`match-thresh`、`mock-track`。
- `SsvDetectionStore` 中检测结果进入跟踪、跟踪结果进入发布和 overlay 的状态流转。
- T3 消费检测和跟踪结果所需的最小字段。

退出标准：

- 团队能说明当前 `ssvtrack` 的 IoU 匹配、ID 分配、轨迹保留和限制。
- `track_id` 字段契约和元数据状态流转被文档化，并有基础测试覆盖。
- mock 跟踪链路稳定通过自动测试。
- 真实跟踪链路有可复现命令，并明确依赖模型文件、视频源和显示环境。
- T3 事件消息能够基于 M1 检测字段和 M2 跟踪字段继续设计。

非本阶段范围：

- 引入 DeepSORT、ByteTrack、OC-SORT 等外部跟踪框架。
- ReID 特征提取和跨摄像头跟踪。
- 完整事件判定和 Agent 复核。
- 多路视频调度。

## M3: 检测跟踪结果事件消息打通

**目标**：把 M1/M2 已冻结的检测和跟踪元数据规范化为 T3 的结构化事件，并通过 Redis Streams 给 T4 提供稳定消费契约。

现有实现基线：

- `ssvpub` 已能向 Redis Streams 发布包含 `type`、`source`、`timestamp_ms`、`frame_id`、`detections`、`bbox` 和 `track_id` 的检测消息。
- Python Agent 已有 Redis Streams 消费、JSON 解析、日志输出和 ACK 的最小样例。
- 本阶段不重复实现基础发布/消费链路，重点是正式冻结 schema、补字段一致性测试和增加安全帽事件规则。

建议文档：

- `docs/specs/YYYY-MM-DD-M3-检测跟踪结果事件消息打通-spec.md`
- `docs/plans/YYYY-MM-DD-M3-检测跟踪结果事件消息打通-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T2/T3 | 字段交接：确认 M1 检测字段和 M2 跟踪字段进入事件层的映射关系 | 检测/跟踪到事件字段映射表 | C++ 发布字段和文档字段一致 |
| B | T3 | 事件消息：规范化现有 Redis 消息为最小事件 schema，补齐字段类型、兼容性和错误语义 | 事件类型、严重级别、触发原因、检测列表、轨迹列表 | Redis Stream 中消息字段可被样例消费，并有字段一致性测试 |
| C | T3 | 安全帽事件规则：定义连续命中、低置信度、检测冲突等初版规则 | 规则说明和测试入口 | 支持 mock/no-op 降级 |
| D | T4/T5 | 消费与验收：对齐现有 Agent 消费样例和 M3 schema，增加 M3 集成验收 | 事件消费样例、解析测试、C++ 发布和 Python 消费字段一致性检查 | C++、CLI、Agent 基础测试通过 |

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

## M4: 事件证据与状态层

**目标**：把 M3 的结构化事件扩展成 Agent 可消费的“事件 + 证据 + 状态”。本阶段只处理事件与证据层边界，不实现 Agent 状态机、工具调用或模型复核。

建议文档：

- `docs/specs/YYYY-MM-DD-M4-事件证据与状态层-spec.md`
- `docs/plans/YYYY-MM-DD-M4-事件证据与状态层-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T3 | 事件状态契约：定义 `pending`、`processing`、`completed`、`failed`、`manual_review` 和失败原因字段 | 事件状态字段、迁移规则、失败原因字段 | Redis 消息或状态记录中可表达状态 |
| B | T3 | 证据输出：保存关键帧、检测框渲染图和可选短片段 | 证据目录、命名规则、路径字段 | 构造事件后能生成可访问证据路径 |
| C | T3/T5 | 证据失败降级：处理关键帧缺失、写入失败、路径不可读 | 降级语义、错误字段、测试用例 | 证据缺失时事件仍可进入 Agent，并标记低置信度或待人工复核 |
| D | T5 | M4 验收：构造未佩戴、低置信度、检测冲突事件样例 | 样例事件、证据样例、验收脚本 | C++/CLI 基础测试通过，证据字段可验证 |

冻结接口：

- 证据路径和文件命名规则。
- `event_id`、`event_type`、`trigger_reason`、`severity`、`frame_path`、`clip_path`、`agent_state` 和失败原因字段。
- 证据保存失败、证据缺失和 Redis 发布失败的降级语义。

退出标准：

- 事件消息包含 Agent 可消费的最小证据和状态字段。
- 证据路径可访问或明确标记缺失原因。
- 证据缺失、写入失败、Redis 发布失败都有明确状态和错误字段。

非本阶段范围：

- Agent 状态机。
- OpenAI SDK、视觉复核和文本解释。
- 向量检索、报告生成和通知。
- 完整数据库表结构。

## M5: Agent 输入与状态机最小闭环

**目标**：Agent 能消费 M4 事件，构造固定输入上下文，按自研轻量状态机完成一次 mock 复核并回写结果。本阶段不接真实模型，不实现工具路由平台。

建议文档：

- `docs/specs/YYYY-MM-DD-M5-Agent输入与状态机最小闭环-spec.md`
- `docs/plans/YYYY-MM-DD-M5-Agent输入与状态机最小闭环-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T4 | 事件解析与上下文构造：解析事件、检测框、轨迹、证据路径和触发原因 | `ReviewContext` 数据结构 | 单测覆盖完整事件、缺证据事件和坏 JSON |
| B | T4 | 自研状态机：实现事件解析、上下文构造、策略选择、结果汇总、终态 | 状态机类、状态枚举、迁移规则 | 事件可从待消费迁移到完成、失败或待人工复核 |
| C | T4 | 状态存储：记录步骤、工具结果占位、失败原因和最终结论 | 内存或 Redis 状态存储第一版 | 失败可追踪，不停留在处理中 |
| D | T4/T5 | 结果回写：写回复核结论、处置建议、事件摘要和状态 | 结果 schema、回写逻辑、测试 | mock 事件可完整跑通一次 Agent 最小闭环 |

冻结接口：

- Agent 输入上下文。
- 状态枚举和状态迁移规则。
- 结果回写格式。
- 错误状态和待人工复核语义。

退出标准：

- Agent 能从 Redis 事件构造上下文。
- mock 状态机能完成直接确认、失败和待人工复核三类路径。
- 结果能回写到约定位置或约定消息。

非本阶段范围：

- OpenAI SDK 真实调用。
- 规则知识检索和报告生成。
- 外部通知和工单系统。

## M6: 工具路由与 OpenAI SDK 复核

**目标**：把 M5 状态机中的能力调用拆成受控工具，并用 OpenAI SDK 完成第一版视觉复核和文本解释。第一版不建设多厂商 provider 平台，只保留一个窄接口，避免 SDK 调用散落在状态机里。

建议文档：

- `docs/specs/YYYY-MM-DD-M6-工具路由与OpenAI复核-spec.md`
- `docs/plans/YYYY-MM-DD-M6-工具路由与OpenAI复核-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T4 | 工具调用路由：统一工具名、参数、调用上下文、返回结果和错误信息 | `ToolRouter` 抽象、mock 工具、错误映射 | 工具调用失败能进入失败或待人工复核 |
| B | T4 | 证据读取工具：读取关键帧、检测框渲染图和短片段路径 | evidence reader、路径校验、缺失证据结果 | 缺失证据不伪造复核结论 |
| C | T4 | OpenAI SDK 薄封装：封装视觉复核和文本解释调用，提供 mock client、配置项、超时和错误映射 | `OpenAIModelClient`、mock client、调用结果 schema | 单测不依赖真实 API；OpenAI 不可用时事件进入待人工复核或失败状态 |
| D | T4/T5 | 视觉复核和文本解释流程：状态机调用证据读取、OpenAI client 和文本解释工具并汇总结果 | mock/真实可切换复核流程、M6 验收清单 | mock 流程稳定通过；真实 OpenAI 调用有环境变量和跳过策略 |

冻结接口：

- 工具调用协议。
- OpenAI client 输入输出结构。
- 视觉复核结果、文本解释结果和错误结果 schema。
- OpenAI 超时、限流、无效响应和不可用时的状态语义。

退出标准：

- 状态机通过工具路由调用证据读取和 OpenAI client。
- 测试默认使用 mock client，不依赖真实 API。
- 真实 OpenAI SDK 调用具备明确配置、超时、失败降级和本地跳过策略。

非本阶段范围：

- 多厂商 provider 路由平台。
- 向量检索和规则知识库。
- 外部通知实际发送。

## M7: 规则知识与报告输出

**目标**：接入规则解释和报告生成能力，但仍保持单机原型边界，不做完整知识平台、前端或长期事件库。

建议文档：

- `docs/specs/YYYY-MM-DD-M7-规则知识与报告输出-spec.md`
- `docs/plans/YYYY-MM-DD-M7-规则知识与报告输出-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T4 | 规则知识输入：整理安全规则、制度片段和历史处置样例 | 规则文档目录、加载方式、规则片段格式 | 可按事件类型取到规则片段 |
| B | T4 | 检索边界：先实现 mock/retriever 抽象，保留 Qdrant、Milvus、pgvector 选型 | retriever 抽象、mock 检索结果 | 检索不可用时可降级并标明缺少知识上下文 |
| C | T4 | 规则解释工具：基于事件和规则片段生成说明 | 规则解释结果 schema、来源字段 | 输出中标明规则来源 |
| D | T4/T5 | 报告和通知内容生成：生成事件摘要、复核结论、处置建议和告警升级内容 | 报告文本或 JSON 输出、样例 | 端到端样例可生成可读报告 |

冻结接口：

- 规则片段格式。
- 检索结果格式。
- 规则解释结果格式。
- 报告输出格式。

退出标准：

- Agent 可把规则片段纳入上下文和解释。
- 检索不可用时流程可降级。
- 输出包含复核结论、规则依据、处置建议和摘要。

非本阶段范围：

- 完整知识库治理。
- 长期事件库、检索页面和统计报表。
- 真实外部通知发送。

## M8: 端到端 demo 和交付收口

**目标**：把 M1-M7 的能力合流成可演示、可排障、可交接的单机端到端原型。以 `./ssv run` 调用 C++ pipeline runner 作为演示和长期运行基线，收口 headless、display、Redis、Agent 和证据链路。

建议文档：

- `docs/specs/YYYY-MM-DD-M8-端到端Demo和交付收口-spec.md`
- `docs/plans/YYYY-MM-DD-M8-端到端Demo和交付收口-plan.md`

并行任务包：

| 领取 | 领域 | 任务包 | 输出 | 验收 |
| --- | --- | --- | --- | --- |
| A | T1/T5 | 演示运行时：收口 `./ssv run` 的 headless/display 路径和 YAML 配置，确认 C++ pipeline runner 的退出码与日志边界 | demo 运行命令、运行时边界说明 | `./ssv run` 至少有一条路径可稳定演示 |
| B | T2/T3 | 演示事件：可重复生成未佩戴、低置信度和检测冲突事件 | demo 配置、事件样例、证据样例 | Redis 中可看到结构化事件和证据路径 |
| C | T4 | 演示 Agent：跑通消费、证据读取、OpenAI/mock 复核、规则解释、结果回写 | demo Agent 流程、复核结果样例 | 单命令或清晰步骤可复现 |
| D | T5 | 交付文档：更新 README、运行手册、故障排查、测试矩阵和发布检查清单 | 交付清单和验收脚本 | 新成员可按文档复现 demo |

冻结接口：

- demo 配置文件和运行命令。
- 端到端日志字段和退出码。
- 交付验收清单。

退出标准：

- 从视频输入到检测、事件、证据、Agent 复核、规则解释、结果输出形成闭环。
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
| 跟踪字段和 track ID 语义 | M2 | T2 | T3、T5 | `track_id`、未跟踪默认值、跨帧稳定性预期 |
| `ssvtrack` 基础插件属性 | M2 | T2 | T1、T3、T5 | `frame-rate`、`track-thresh`、`track-buffer`、`match-thresh`、`mock-track` |
| Redis 事件 schema | M3 | T3 | T4、T5 | Stream key、字段名、字段类型和错误语义 |
| 证据路径、文件命名和事件状态 | M4 | T3 | T4、T5 | Agent 必须能按路径读取证据，并识别状态和降级原因 |
| Agent 输入上下文和状态机状态 | M5 | T4 | T3、T5 | 支持完成、失败、待人工复核 |
| 工具调用协议和 OpenAI client 结果 | M6 | T4 | T5 | OpenAI SDK 通过窄接口接入，测试默认使用 mock client |
| 规则解释和报告输出格式 | M7 | T4 | T5 | 包含规则来源、复核结论、处置建议和摘要 |
| 端到端运行命令和验收清单 | M8 | T5 | T1、T2、T3、T4 | README 和运行手册同步 |

接口一旦被后续里程碑采用，修改时必须保留兼容路径，或在对应 spec 中明确迁移方式。

## 验证矩阵

| 范围 | 命令 | 说明 |
| --- | --- | --- |
| C++ 插件、元数据、Meson | `./ssv build` | 构建共享库、插件和测试 |
| C++ 单元测试 | `meson test -C build` | 覆盖插件注册、元数据和 C++ 测试 |
| Python Agent | `cd agent && uv run --extra dev pytest` | 覆盖 Agent 配置、消费和服务测试 |
| CLI/依赖/模型服务 | `python3 -m unittest discover -s scripts/ssv_cli/tests -p 'test_*.py'` | 覆盖 `./ssv` CLI、依赖策略、模型服务和 Redis 管理入口 |
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
