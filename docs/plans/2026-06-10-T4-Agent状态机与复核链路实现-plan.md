# T4 Agent 状态机与复核链路实现 Plan

## 背景

当前 `agent/` 只有 M0 基线：消费 Redis Streams → 解析 JSON → 打印日志 → ACK。需要补齐 Agent 核心能力：事件领域模型、LLM 状态机、provider 抽象、上下文构造、工具路由、结果回写。

实现范围严格限定在 `agent/` 目录内，不跨主线。所有 provider 使用 mock 实现，不依赖外部模型 API。

## 实现步骤

### Phase 1: 事件领域模型

**文件**: `agent/src/ssv_agent/models/event.py`

**做了什么**: 创建 pydantic 强类型数据类，替换 event_consumer 中散落的 JSON dict 操作。

**具体做法**:
1. `Detection` — 单条检测结果。字段对齐 ssvpub C++ 插件输出的 JSON schema（class/class_id/confidence/bbox/track_id）。使用 `Field(alias="class")` 让 pydantic 能同时接受 Python 字段名 `class_name` 和 JSON key `class`。加了 `populate_by_name=True` 让两边的名字都能用。
2. `DetectionEvent` — 从 Redis 消费的完整事件消息。除 ssvpub 原始字段外，扩展了 `severity`、`trigger_reason`、`state`、`evidence_paths` 等 Agent 需要的字段。
3. 事件自带推断逻辑：`infer_severity()` 根据 head/helmet 的检测结果和置信度自动判断严重程度（LOW/MEDIUM/HIGH/CRITICAL），`infer_trigger_reason()` 判断触发原因，`select_strategy()` 根据严重程度和检测组合自动选择复核策略。
4. `ReviewResult` — 复核最终结果，包含 event_id、final_state、strategy、conclusion、summary、tool_results。
5. `ToolResult` — 单次工具调用结果（tool_name/success/output/error）。
6. `ReviewContext` — 传给 Provider 的完整上下文（事件+证据摘要+规则片段+历史经验）。
7. 枚举类：`EventState`（8个流转状态+3个终态）、`ReviewStrategy`（4条复核路径）、`Severity`、`TriggerReason`。

**测试覆盖**: 28 个用例。包括 Redis JSON 解析、字段默认值、坐标面积计算、置信度边界校验、严重程度推断、触发原因推断、策略选择、终态判断、上下文 prompt 文本生成。

---

### Phase 2: LLM 状态机

**文件**: `agent/src/ssv_agent/state_machine/states.py`, `machine.py`

**做了什么**: 实现自研轻量状态机，驱动事件走完 8 个状态 → 3 个终态的完整复核流程。

**具体做法**:
1. `StateContext`（states.py）— dataclass，携带全生命周期上下文：event、当前 state、strategy、context、tool_results 列表、error、超时信息。提供 `transition()`、`record_tool_result()`、`set_error()`、`build_result()` 方法。`build_result()` 根据当前状态和策略合成结论文本和事件摘要。
2. `StateMachine`（machine.py）— 主类，通过依赖注入接收 Provider、ToolRouter、ResultWriter 三个协议（只用 Protocol 定义接口，不依赖具体实现类）。
3. 状态流转顺序：PENDING → PARSING → CONTEXT_BUILDING → STRATEGY_SELECTING → TOOL_CALLING → RESULT_AGGREGATING → RESULT_WRITING → COMPLETED。FAILED/NEEDS_HUMAN 可提前退出（跳过回写）。
4. 四条策略路径的处理：
   - `_handle_direct_confirm` — 跳过工具调用，直接进入结果汇聚
   - `_handle_visual_review` — 调 provider.analyze()，记录 visual_review 工具结果
   - `_handle_rule_explain` — 先调 tool_router 检索规则，再调 provider 解释
   - `_handle_notify_report` — 调 tool_router 生成通知 + provider 生成报告
5. 超时保护：`_do_result_aggregation` 检查 `ctx.has_timed_out`，超时 → NEEDS_HUMAN
6. 全工具失败降级：所有 tool_result 都 failed → NEEDS_HUMAN
7. `execute()` 入口用 try/except 兜底，未捕获异常 → FAILED

**测试覆盖**: 17 个用例。StateContext 的状态迁移/构建结果/超时检测 + StateMachine 四条路径端到端 + provider 失败降级 + 全部工具失败降级 + 无 provider 的 DIRECT_CONFIRM 仍正常工作 + 空检测列表 + 异常捕获。

---

### Phase 3: Provider 抽象

**文件**: `agent/src/ssv_agent/providers/base.py`, `mock.py`

**做了什么**: 定义模型 provider 抽象层，屏蔽具体模型厂商和部署形态。开发阶段全部用 mock。

**具体做法**:
1. `base.py`:
   - `BaseProvider(ABC)` — 文本模型抽象，`analyze(context) -> str`，接收 ReviewContext 返回结论文本。
   - `BaseVLMProvider(ABC)` — 多模态视觉模型抽象，`review_keyframe(image_path, context) -> ReviewConclusion`。
   - `ReviewConclusion` dataclass — is_helmet_worn/explanation/confidence/key_observations。
   - `ProviderResult` dataclass — conclusion/confidence/model_used/latency_ms。
2. `mock.py`:
   - `MockProvider(BaseProvider)` — 根据 strategy 返回不同的中文结论文本。支持 `fixed_response` 预设回复和 `simulate_latency_ms` 模拟延迟。记录 `call_count` 供测试验证。
   - `MockVLMProvider(BaseVLMProvider)` — 返回默认复核结论（"未佩戴安全帽"），支持 `fixed_conclusion` 自定义。

**测试覆盖**: 13 个用例。MockProvider 四种策略的返回内容验证 + 固定回复 + 调用计数 + 延迟模拟 + MockVLMProvider 默认/自定义结论 + key_observations。

---

### Phase 4: 上下文构造 + 工具路由 + 结果回写

**文件**: `agent/src/ssv_agent/context/builder.py`, `tools/base.py`, `tools/router.py`, `writer/result_writer.py`

**做了什么**: 实现复核上下文的构造、工具的注册调用路由、结果的 Redis+日志双通道回写。

**具体做法**:
1. `ContextBuilder` — `build(event, strategy) -> ReviewContext`。当前 M3 未完成，证据路径为空占位。规则片段根据 trigger_reason 返回中文安全规范文本（"安全帽佩戴规范"、"连续违规判定"、"冲突判定规则"）。后续 M4 接入向量数据库后替换为真实检索。
2. `BaseTool(ABC)` — 工具抽象，只有 `name` 和 `execute(params) -> ToolResult` 两个接口。
3. `ToolRouter` — 管理工具注册表（dict），`register(tool)` 注册、`call_tool(name, params)` 调用。调用失败（抛异常或工具返回 failure）时返回带 error 的 ToolResult。已注册工具覆盖时打 warning。支持 `call_count` 统计。
4. `ResultWriter` — `write(result)` 双通道输出：
   - 始终写结构化日志（`logger.info("review result", ...)`）
   - 如果传了 redis_client，同时 XADD 到 `ssv:review-results` Stream
   - 非 COMPLETED 终态额外打 warning 日志

**测试覆盖**: 13 个用例。ContextBuilder 基本构造/证据路径/规则检索/计数 + ToolRouter 注册调用/未注册工具/工具失败/计数/注销/覆盖 + ResultWriter 无 Redis 写日志/写计数。

---

### Phase 5: 整合 service.py

**文件**: `agent/src/ssv_agent/service.py`, `event_consumer.py`, `config.py`, `cli.py`

**做了什么**: 把 Phase 1-4 的模块串成完整的处理管线，重构 event_consumer 剥离 I/O，扩展配置和 CLI。

**具体做法**:
1. `event_consumer.py` 重构 — `start()` 改为接收 `handler: Callable[[str, dict], None]` 回调参数。Redis 拉取循环只负责 I/O，业务逻辑交给回调。ACK 改为独立的 `ack(msg_id)` 方法，由 service 在处理完成后显式调用。新增 `redis_client` property 暴露底层连接供 ResultWriter 复用。
2. `service.py` 重构为 `AgentService` 类：
   - `__init__` 构造完整依赖链：EventConsumer → ContextBuilder → MockProvider → StateMachine → ResultWriter
   - `run()` 启动 consumer 循环，传入 `_handle_event` 回调
   - `_handle_event(msg_id, fields)` 是单条事件的处理入口：JSON 解析 → pydantic 校验 → StateMachine.execute() → ACK。解析失败不 ACK（避免丢失消息，等下次重试）。
   - 保留旧版 `run()` 函数作为 CLI 兼容入口，内部创建 AgentService 并注册 SIGINT/SIGTERM 优雅退出。
3. `config.py` — AgentConfig 新增 `mock_provider: bool = True` 和 `review_result_stream: str = "ssv:review-results"`。
4. `cli.py` — 新增 `--no-mock` 标志（默认使用 mock provider）。

**测试覆盖**: 4 个用例。AgentService 初始化 + 单事件处理（解析→状态机→ACK→结果回写） + 畸形 JSON 拒绝 + 端到端 DIRECT_CONFIRM 流程。

---

### Bug 修复记录

1. **pydantic v2 alias 行为** — `Field(alias="class")` 导致通过 Python 字段名 `class_name` 构造对象时被忽略。修复：添加 `model_config = {"populate_by_name": True}` 到 Detection 和 DetectionEvent。
2. **状态机终态覆盖** — `_do_result_writing` 执行 `ctx.transition(RESULT_WRITING)` 覆盖了 `_do_result_aggregation` 设置的终态（COMPLETED/FAILED/NEEDS_HUMAN）。修复：`_do_result_writing` 不再修改状态；`_run_states` 仅在 FAILED/NEEDS_HUMAN 时提前退出，COMPLETED 继续走到回写。
3. **触发原因优先级** — `infer_trigger_reason()` 中 `NO_HELMET` 判断排在 `LOW_CONFIDENCE` 之前，导致低置信度 head-only 被误判为明确违规。修复：低置信度判断提到最前。

### 非本阶段范围

- 真实 LLM/VLM API 调用（当前全部 mock）
- 向量数据库检索（规则知识库）
- 通知/工单外部系统对接
- Agent 状态持久化到数据库
- 多 Agent 协作或复杂图编排

## 验证

```bash
cd agent
uv run --extra dev pytest    # 82 passed
uv run --extra dev ruff check src/  # All checks passed
```

## 文件清单

```
新增文件 (16):
  agent/src/ssv_agent/models/__init__.py
  agent/src/ssv_agent/models/event.py
  agent/src/ssv_agent/state_machine/__init__.py
  agent/src/ssv_agent/state_machine/states.py
  agent/src/ssv_agent/state_machine/machine.py
  agent/src/ssv_agent/providers/__init__.py
  agent/src/ssv_agent/providers/base.py
  agent/src/ssv_agent/providers/mock.py
  agent/src/ssv_agent/context/__init__.py
  agent/src/ssv_agent/context/builder.py
  agent/src/ssv_agent/tools/__init__.py
  agent/src/ssv_agent/tools/base.py
  agent/src/ssv_agent/tools/router.py
  agent/src/ssv_agent/writer/__init__.py
  agent/src/ssv_agent/writer/result_writer.py
  agent/tests/test_models.py
  agent/tests/test_state_machine.py
  agent/tests/test_providers.py
  agent/tests/test_context_tools_writer.py

修改文件 (7):
  agent/src/ssv_agent/cli.py            # 新增 --no-mock 标志
  agent/src/ssv_agent/config.py          # AgentConfig 扩展
  agent/src/ssv_agent/event_consumer.py  # 重构为纯 I/O 层
  agent/src/ssv_agent/service.py         # 重构为 AgentService
  agent/tests/test_event_consumer.py     # 适配新接口
  agent/tests/test_service.py            # 新增端到端测试
  agent/uv.lock                          # 依赖锁文件
  .gitignore                             # 排除规则
```
