# T4 Agent 基线（DeerFlow 接入）Spec

- 日期：2026-08-12
- 状态：待评审

## 背景

当前 `agent/` 只完成 Redis Streams 事件消费基线：读取 `ssv:events`、解析 JSON、
日志输出、XACK。本阶段基于 DeerFlow Harness 建立“事件 → 复核 → 结果落盘”的最小
闭环，作为后续 M6 工具路由、M7 规则知识、M8 端到端验收的接缝。

参考 `video-search-and-summarization`（VSS）与 `mydemo/deerflow-mydemo` 时，只借鉴
依赖引入形式（editable path → git submodule），不采用 demo 的 daemon、作业队列、
SSE、HTTP 架构。

## 目标

1. 在现有 `agent` 服务进程内嵌入 DeerFlow Harness，逐事件调用视觉复核。
2. 按当前 `ssvpub` 输出设计 `ReviewContext`，派生字段全部占位。
3. 注册 `evidence_reader` 与 `view_image` 两个工具，验证工具调用链路与缺证据降级。
4. 复核结果按固定模板落盘 `outputs/<event_id>/result.md`；失败不落盘，只写结构化日志。
5. 提供独立脚本注入样例事件，便于无真实链路的手工验收。

## 非目标（非本阶段范围）

- DeerFlow App、daemon、作业队列、HTTP/SSE/tail。
- 崩溃恢复（XAUTOCLAIM 捞回）。
- 事件判定与证据输出插件（对应后续 M4）。
- RAG 规则检索、事件/证据结果索引（对应后续 M7/M8）。
- 修改 `./ssv` 与 `scripts/agent.sh`。
- 真实安全帽模型与准确率验收。

## 技术栈

| 项 | 决策 |
| --- | --- |
| 语言/包管理 | Python >= 3.12，uv |
| 框架 | deerflow-harness 2.1.0（git submodule，固定 commit `8659fca8`） |
| 接入方式 | `DeerFlowClient` + `agent/config.yaml`，进程内调用 |
| 模型 | 配置驱动（config.yaml `models`），API key 环境变量 |
| 工具 | `@tool` 自定义 `evidence_reader`；`view_image` 复用 harness 内置 |
| 存储 | Redis Streams 仅 XACK；复核结果本地文件 |

## 目录结构

```text
agent/
├── pyproject.toml
├── config.yaml                 # DeerFlow harness 配置（models / tools / sandbox / checkpointer）
├── third_party/
│   └── deer-flow/              # git submodule，固定 commit
├── scripts/
│   └── enqueue_event.py        # 独立样例事件注入脚本
├── src/ssv_agent/
│   ├── config.py               # 现有 ssv.yaml 配置（扩展 agent 段）
│   ├── cli.py                  # 现有 CLI
│   ├── service.py              # 入口，启动 consumer
│   ├── event_consumer.py       # 消费循环（改为调用复核）
│   ├── review_context.py       # Detection / ReviewContext + from_event()
│   ├── prompt.py               # build_review_prompt()
│   ├── runner.py               # DeerFlowClient.stream 单事件执行
│   ├── result.py               # parse_result_markdown() / write_result_markdown()
│   ├── logging.py              # 现有结构化日志
│   └── tools/
│       ├── __init__.py
│       └── evidence_reader.py  # @tool 占位
└── tests/
    ├── test_review_context.py
    ├── test_prompt.py
    ├── test_result.py
    ├── test_runner.py          # mock 模型，不依赖真实 API
    ├── test_event_consumer.py
    └── test_service.py
```

## 关键类设计

### Detection / ReviewContext

`Detection` 字段与 `ssvpub` 输出逐一对应：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `class_name` | str | 类别名 |
| `class_id` | int | 类别 id |
| `confidence` | float | 置信度 |
| `bbox` | list[float] | `x1, y1, x2, y2` |
| `track_id` | int | 默认 -1 |
| `track_state` | str \| None | 可空 |
| `occluded` | bool | 是否遮挡 |

`ReviewContext` 只承载当前 publish 已有字段；派生字段占位为空：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `event_id` | str | Redis Stream entry id |
| `source` | str | 视频源 |
| `timestamp_ms` | int | 事件时间戳 |
| `frame_id` | int | 帧号 |
| `detections` | list[Detection] | 检测列表 |
| `event_type` | str \| None | 占位，默认 None |
| `severity` | str \| None | 占位，默认 None |
| `frame_path` | str \| None | 占位，默认 None |
| `clip_path` | str \| None | 占位，默认 None |
| `question` | str \| None | 占位，默认 None |

构造统一走 `ReviewContext.from_event(entry_id, payload)`，消费路径与测试路径共用。

### prompt.py

`build_review_prompt(context) -> str` 参数化事件上下文，并固定以下约束：

1. 必须先调用 `evidence_reader`。
2. `evidence_reader` 返回 `available=false` 时只能输出 `结论: uncertain`。
3. 最终回答必须符合 result.md 模板，不得输出模板之外的自由格式。

### runner.py

`run_review(client, context) -> str`：

- `thread_id = f"event-{context.event_id}"`；
- 调用 `client.stream(thread_id, message=build_review_prompt(context))`；
- 从 `values` 事件提取最终 AI 消息并返回；
- 异常向上抛出，由 `EventConsumer` 统一按失败处理。

### result.py

`parse_result_markdown(text) -> ReviewResult` 校验模板字段：

- `结论` ∈ `compliant / violation / uncertain`；
- `置信度` ∈ `[0, 1]`；
- `证据状态` ∈ `available / missing`；
- `证据状态=missing` 时 `结论` 必须为 `uncertain`。

`write_result_markdown(event_id, text) -> Path` 以临时文件 + `os.replace` 原子写入
`outputs/<event_id>/result.md`。

### tools/evidence_reader.py

`@tool` 自定义工具，输入 `event_id / frame_path / clip_path`，输出：

```json
{"available": true|false, "reason": "...", "frame_path": "...", "clip_path": "..."}
```

只做路径存在性校验，不做视觉内容读取；看图由 `view_image` 负责。

### scripts/enqueue_event.py

独立脚本，不进入 `ssv_agent` 包，也不修改 `./ssv`。参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--source` | `camera-1` | 视频源 |
| `--frame-id` | `1` | 帧号 |
| `--frame-path` | 空 | 可选关键帧路径 |
| `--clip-path` | 空 | 可选片段路径 |
| `--detection` | `person:0.92` | 可重复，格式 `class:confidence` |

脚本构造当前 `ssvpub` schema 的检测事件并 `XADD` 到 `ssv:events`。

## Agent 架构与流程

```text
ssv:events
    │ XREADGROUP
    ▼
EventConsumer
    │ ReviewContext.from_event()
    ▼
build_review_prompt()
    │
    ▼
DeerFlowClient.stream(thread_id=event-<event_id>)
    │  evidence_reader / view_image
    ▼
parse_result_markdown()
    ├─ 通过 → write_result_markdown() → XACK
    └─ 失败 → 结构化日志 → XACK（不落盘、不重试）
```

每次复核独立 thread；进程崩溃遗留的未确认消息不捞回。

## 接口契约

### 输入事件

当前 `ssvpub` 发布的 JSON：`type / source / timestamp_ms / frame_id / detections`。

### result.md

成功复核的落盘模板：

```markdown
结论: violation
置信度: 0.87
证据状态: missing
依据: <说明>
```

### 失败语义

- 模型/工具异常：日志输出 `event_id + 错误原因`，XACK，不落盘。
- 模板解析失败或编造结论（缺证据却非 uncertain）：日志输出，XACK，不落盘。
- 缺证据：正常落盘 `结论: uncertain` + `证据状态: missing`。

## 配置契约

- `agent/config.yaml`：DeerFlow harness 的 `models / tools / sandbox / checkpointer`。
- `ssv.yaml` 的 `agent` 段扩展：
  - `output_dir`：结果目录，默认 `outputs`；
  - `model_name`：可选，覆盖 config.yaml 默认模型。

## 验证方式

1. 单元测试覆盖 `review_context`、`prompt`、`result` 解析与原子落盘。
2. `runner` 测试使用 fake DeerFlowClient，不依赖真实 API。
3. 手工验收：
   - `uv run --isolated --script agent/scripts/enqueue_event.py` 注入样例事件；
   - 启动 `agent` 服务消费；
   - 无证据样例预期 `outputs/<event_id>/result.md` 为 `uncertain / missing`；
   - 故障样例预期只有日志、无 result.md。
