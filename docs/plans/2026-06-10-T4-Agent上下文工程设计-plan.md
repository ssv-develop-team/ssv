# T4 Agent 上下文工程设计 Implementation Plan

> **Goal:** 将分散在各模块的上下文组装逻辑收敛为统一的"上下文工程"子系统——五大要素（系统提示、工具定义、历史记录、检索上下文、用户输入）纳入 ContextEngine 统一收集，经 BudgetEngine 配额分配后由 PromptAssembler 组装为 LLM-ready messages。

**Architecture:** `ContextEngine.collect()` 顺序调用各个 Manager/Renderer 收集上下文要素，产出 `ContextPack` 统一容器；`BudgetEngine.allocate()` 与 `PromptAssembler.assemble()` 已实现并可测试，真实 LLM provider 接入前暂不驱动生产复核结论。整个链路用 Protocol 解耦，各模块可独立替换和独立测试。

**Tech Stack:** Python 3.12, dataclasses, PyYAML, structlog。不引入新第三方依赖。

---

## File Structure

### 新增

```
agent/src/ssv_agent/context/
├── __init__.py              包入口
├── pack.py                  ContextPack + 五要素 Block + HistoryMessage + ToolDefinition
├── engine.py                ContextEngine (collect 主入口)
├── system_prompt.py         SystemPromptManager (分层渲染 + 版本快照)
├── tool_definitions.py      ToolDefinitionsRenderer (工具描述→prompt 文本)
├── history_manager.py       HistoryManager (三级压缩 + 优先级驱逐)
├── retrieval_manager.py     RetrievalManager (YAML 加载 + 精确匹配 + 去重 + 降级)
├── user_input.py            UserInputBuilder (事件→完整 user message 文本)
├── budget.py                BudgetEngine + TokenBudget (配额分配)
└── builder.py               [废弃] → 功能收敛到 ContextEngine + RetrievalManager

agent/src/ssv_agent/integrations/
├── __init__.py
├── protocols.py             EvidenceProvider / TrackContextProvider / SourceMetadataProvider / EventSequenceProvider
└── mocks.py                 全部 Mock 实现

config/rules.yaml            安全规则知识库（YAML 格式）
```

### 修改

```
agent/src/ssv_agent/prompts/system.py      → SystemPrompt 新增 layers 字段
agent/src/ssv_agent/prompts/templates.py   → 删 SYSTEM_PROMPT_CONTENT / OUTPUT_FORMAT_SPEC 重复定义
agent/src/ssv_agent/prompts/assembler.py   → assemble() 接收 ContextPack
agent/src/ssv_agent/prompts/logger.py      → CallRecord 新增 5 字段
agent/src/ssv_agent/prompts/manager.py     → 保留轻量 validate/get_versions，SystemPromptManager 承担分层渲染
agent/src/ssv_agent/tools/base.py          → +4自描述属性 + to_definition()
agent/src/ssv_agent/service.py             → 构造 ContextEngine + BudgetEngine，传入 StateMachine
agent/src/ssv_agent/state_machine/machine.py → _do_context_building 调用 ContextEngine.collect()
agent/src/ssv_agent/providers/mock.py      → 新增 analyze_from_messages() + ContextPack 兼容
agent/src/ssv_agent/config.py              → 新增上下文工程配置项
agent/src/ssv_agent/models/event.py        → ReviewContext.prompt_context 标记 deprecated
```

---

## Task 1: 数据容器 `context/pack.py`

**Files:** `agent/src/ssv_agent/context/pack.py`

- [x] **Step 1: 定义五要素 Block**

```python
@dataclass
class SystemPromptBlock:
    content: str
    versions: dict[str, str]

@dataclass
class ToolDefinitionsBlock:
    tools: list[ToolDefinition]
    format: str  # "openai" / "anthropic" / "text"

@dataclass
class HistoryBlock:
    messages: list[HistoryMessage]
    summary: str
    compression_level: str
    compression_records: list[CompressionRecord]

@dataclass
class RetrievalBlock:
    items: list[RetrievalItem]

@dataclass
class UserInputBlock:
    event_context: str
    evidence_summary: str
    track_context: list[TrackContext]
    source_metadata: Optional[SourceMetadata] = None
```

- [x] **Step 2: 定义 HistoryMessage 和 ToolDefinition**

```python
@dataclass
class HistoryMessage:
    event_id: str
    track_ids: list[int]        # -1 表示未跟踪
    timestamp_ms: int
    severity: str
    trigger_reason: str
    strategy: str
    conclusion_summary: str

@dataclass
class ToolDefinition:
    name: str
    description: str
    parameters_schema: dict
    returns_description: str
    side_effects: str
```

- [x] **Step 3: 定义 ContextPack 统一容器**

```python
@dataclass
class ContextPack:
    system_prompt: SystemPromptBlock
    tool_definitions: ToolDefinitionsBlock
    history: HistoryBlock
    retrieval_context: RetrievalBlock
    user_input: UserInputBlock
    context_id: str
    token_budget: Optional[TokenBudget] = None
    metadata: dict = field(default_factory=dict)

    def to_review_context(self) -> ReviewContext:
        """M4 向后兼容。M5 移除。"""
```

- [x] **Step 4: 定义 TokenBudget**

```python
@dataclass
class TokenBudget:
    allocations: dict[str, int]  # 要素 → token 上限
    total: int
    strategy: str
```

**Verification:**
```python
context_id = ContextPack.generate_context_id("cam-1", 42)
pack = ContextPack.create_empty(context_id, strategy="direct_confirm")
assert pack.context_id == context_id
assert pack.system_prompt.content == ""
review_ctx = pack.to_review_context()
assert review_ctx is not None
```

---

## Task 2: 系统提示词管理 `context/system_prompt.py`

**Files:** `agent/src/ssv_agent/context/system_prompt.py`

- [x] **Step 1: 实现五层拆分**

从 `DEFAULT_SYSTEM_PROMPT.content` 按 Markdown 标题 (`## 你的职责`, `## 判断原则` 等) 自然切分为 layers dict：

| 层 | key | 来源 |
|:--|:--|:--|
| L0 角色定义 | `role` | `## 你的职责` 段 |
| L1 领域知识 | `domain_knowledge` | `## 判断原则` 段 |
| L2 策略指引 | `strategy_guidance` | `templates.py` → `STRATEGY_SYSTEM_TEMPLATES` |
| L3 输出约束 | `output_constraints` | `output_spec` + `constraints` |

- [x] **Step 2: 实现 SystemPromptManager**

`render(strategy)` — 按策略拼接 L0-L3，返回完整系统提示词文本。
`render_layers(layers)` — 按指定层列表渲染。
`snapshot_versions()` — 返回各层版本快照 dict。
`validate_constraints()` — 硬约束检查（总字符≤2000、每层≤500、原则≤8条、每条≤80字）。

- [x] **Step 3: 同步更新 prompts/system.py**

`SystemPrompt` 新增 `layers: dict[str, str]` 字段。
`templates.py` 删除 `SYSTEM_PROMPT_CONTENT` 和 `OUTPUT_FORMAT_SPEC`，改为 `from ssv_agent.prompts.system import DEFAULT_SYSTEM_PROMPT` 引用（D1）。

- [x] **Step 4: 废弃 prompts/manager.py**

`validate()` 和 `get_versions()` 功能合并到 `SystemPromptManager`。

**Verification:**
```python
from ssv_agent.context.system_prompt import SystemPromptManager
mgr = SystemPromptManager()
rendered = mgr.render("direct_confirm")
assert "工地安全巡检" in rendered
snapshot = mgr.snapshot_versions()
assert "role" in snapshot
```

---

## Task 3: 工具定义渲染 `context/tool_definitions.py`

**Files:** `agent/src/ssv_agent/context/tool_definitions.py`, `agent/src/ssv_agent/tools/base.py`

- [x] **Step 1: BaseTool 扩展 4 个自描述属性**

```python
class BaseTool(ABC):
    @property
    @abstractmethod
    def description(self) -> str: ...       # LLM 判断"该不该调用"
    @property
    @abstractmethod
    def parameters_schema(self) -> dict: ... # LLM 构造调用参数
    @property
    @abstractmethod
    def returns_description(self) -> str: ...# LLM 理解返回值
    @property
    @abstractmethod
    def side_effects(self) -> str: ...       # LLM 理解副作用

    def to_definition(self) -> ToolDefinition:
        """导出为纯数据，解耦渲染与执行。"""
```

- [x] **Step 2: 实现 ToolDefinitionsRenderer**

`render(router, strategy)` — 按策略过滤已注册工具，渲染为 prompt 文本。
`render_openai_format(router, strategy)` — 渲染为 OpenAI API `tools` 参数格式。
`render_compact(router, strategy)` — 渲染为紧凑纯文本（回退方案）。

条件注入：`direct_confirm` 只看到 1 个工具，`notify_report` 看到 2-3 个。

**Verification:**
```python
from ssv_agent.context.tool_definitions import ToolDefinitionsRenderer
renderer = ToolDefinitionsRenderer()
text = renderer.render(router, "notify_report")
assert "call_tool" in text or len(text) > 0
```

---

## Task 4: 历史管理 `context/history_manager.py`

**Files:** `agent/src/ssv_agent/context/history_manager.py`

- [x] **Step 1: 实现 HistoryManager**

`record(event, result)` — 复核完成后写入内存（当前阶段不持久化）。
`get_window(current_event)` — 按关联度排序返回相关历史记录。
`compress(messages, budget)` — 三级压缩：

| Level | 条件 | 动作 |
|:--|:--|:--|
| 0 | token < budget×0.8 | 全量保留 |
| 1 | token ≥ budget×0.8 | 去 bbox/track_id，仅保留 class+confidence |
| 2 | 轻量后仍超 | 保留最近5条，其余按 track_id 分组各一句摘要 |
| 3 | 摘要后仍超 | 所有历史合并为一段统计文本 |

- [x] **Step 2: 实现优先级驱逐算法**

关联度评分（0~1）：track_id 重叠(0.50) + 时间邻近度(0.25) + 严重程度(0.15) + 同触发原因(0.10)。按 score 升序逐条移除。

- [x] **Step 3: 空历史静默降级**

`get_window()` 返回空 `HistoryBlock`，组装管线静默跳过。

**Verification:**
```python
from ssv_agent.context.history_manager import HistoryManager
mgr = HistoryManager()
block = mgr.get_window(event)  # 空历史
assert block.summary == ""
assert len(block.messages) == 0
```

---

## Task 5: 检索管理 `context/retrieval_manager.py`

**Files:** `agent/src/ssv_agent/context/retrieval_manager.py`, `config/rules.yaml`

- [x] **Step 1: 创建 config/rules.yaml**

3 条初始规则：安全帽佩戴规范（`safety-helmet-mandatory`）、连续违规判定（`consecutive-violation`）、冲突判定（`conflict-resolution`）。每条含 `id`/`trigger`/`content`/`source`/`source_type`/`priority`。

- [x] **Step 2: 实现 RetrievalManager**

查找顺序：`config/rules.yaml` → `$SSV_RULES_PATH` → 硬编码默认规则（降级）。
`search(event, strategy)` — 精确匹配 trigger_reason → MD5 去重 → source_type 标注 → priority 排序截断。
空结果时返回空 `RetrievalBlock`，由 `UserInputBuilder` 在 prompt 文本中表达“未找到匹配规则”的降级语义。

- [x] **Step 3: Prompt 呈现分级**

法规依据（`regulation`）→ 专家经验（`expert`）→ 统计规律（`statistics`），帮助 LLM 判断采信权重。

- [x] **Step 4: 废弃 context/builder.py**

`_lookup_rules()` 功能已收敛到 `RetrievalManager`。`ContextBuilder` 类标记 deprecated。

**Verification:**
```python
from ssv_agent.context.retrieval_manager import RetrievalManager
mgr = RetrievalManager()
items = mgr.search(event, "rule_explain")
assert len(items) >= 1
```

---

## Task 6: 用户输入构建 `context/user_input.py`

**Files:** `agent/src/ssv_agent/context/user_input.py`

- [x] **Step 1: 实现 UserInputBuilder**

`build(event, strategy, evidence_summary="") -> UserInputBlock` — 将 DetectionEvent 渲染为完整 user message 文本。

渲染模式：`full`（完整 bbox 坐标+track_id）、`compact`（仅保留 class_name + confidence）。

- [x] **Step 2: 收敛散落逻辑**

| 原逻辑 | 原位置 | 收敛后 |
|:--|:--|:--|
| `prompt_context` 属性 | `models/event.py` | `UserInputBuilder.build()` |
| `_build_event_context()` | `prompts/assembler.py` | `UserInputBuilder.build()` |
| 模板渲染 | `prompts/assembler.py._render_task_template()` | `UserInputBuilder`（D6） |

- [x] **Step 3: ReviewContext.prompt_context 标记 deprecated**

**Verification:**
```python
from ssv_agent.context.user_input import UserInputBuilder
builder = UserInputBuilder()
block = builder.build(event, "direct_confirm")
assert "cam-1" in block.event_context
```

---

## Task 7: 预算引擎 `context/budget.py`

**Files:** `agent/src/ssv_agent/context/budget.py`

- [x] **Step 1: 实现 BudgetEngine**

`allocate(pack, strategy) -> TokenBudget` — 按策略为五要素分配 token 上限（D2）。

| 要素 (tokens) | direct_confirm | visual_review | rule_explain | notify_report |
|:--|:--:|:--:|:--:|:--:|
| system_prompt | 800 | 800 | 800 | 800 |
| tool_definitions | 200 | 400 | 400 | 400 |
| user_input_base | 400 | 400 | 400 | 400 |
| user_input_details | 600 | 600 | 400 | 600 |
| retrieval_context | 200 | 400 | **800** | 400 |
| history | 300 | 400 | 400 | 300 |
| few_shot | 300 | 300 | 200 | 300 |

- [x] **Step 2: 逐级截断链（PromptAssembler 侧）**

`PromptAssembler` 从 `ContextPack.token_budget` 读取配额，超出时按优先级：few_shot → 历史 → 检索 → 检测详情，逐级裁剪。

系统提示词、工具定义、事件基本标识永不裁剪。

**Verification:**
```python
from ssv_agent.context.budget import BudgetEngine
engine = BudgetEngine()
budget = engine.allocate(pack, "rule_explain")
assert budget.allocations["retrieval_context"] == 800
assert budget.strategy == "rule_explain"
```

---

## Task 8: 上下文引擎 `context/engine.py`

**Files:** `agent/src/ssv_agent/context/engine.py`

- [x] **Step 1: 实现 ContextEngine**

`collect(event, strategy) -> ContextPack` — 顺序调用以下 Manager/Renderer：

1. `SystemPromptManager.render(strategy)` → SystemPromptBlock
2. `ToolDefinitionsRenderer.render(router, strategy)` → ToolDefinitionsBlock
3. `HistoryManager.get_window(event)` → HistoryBlock
4. `RetrievalManager.search(event, strategy)` → RetrievalBlock
5. `UserInputBuilder.build(event, strategy)` → UserInputBlock
6. 集成数据源注入（EvidenceProvider / TrackContextProvider 等）→ 附加到 UserInputBlock

- [x] **Step 2: 渐进激活降级**

各 Manager/Renderer 为 None 时 → 产出空 Block，不影响其他要素。
`config.py` 当前提供 `context_history_max_window`、`context_engine_debug` 和 `rules_yaml_path`；ContextEngine 默认启用，禁用开关留到真实 provider 接入后再评估。

**Verification:**
```python
from ssv_agent.context.engine import ContextEngine
engine = ContextEngine()
pack = engine.collect(event, "direct_confirm")
assert pack.system_prompt.content != ""
assert pack.user_input.event_context != ""
```

---

## Task 9: 集成 service.py 与 StateMachine

**Files:** `agent/src/ssv_agent/service.py`, `agent/src/ssv_agent/state_machine/machine.py`, `agent/src/ssv_agent/config.py`

- [x] **Step 1: config.py 新增配置项**

```python
class AgentConfig(BaseModel):
    ...
    context_history_max_window: int = 50
    context_engine_debug: bool = False
    rules_yaml_path: str = "config/rules.yaml"
```

- [x] **Step 2: service.py 构造上下文工程管线**

```python
self._context_engine = ContextEngine(...)
self._budget_engine = BudgetEngine()
self._machine = StateMachine(
    provider=provider,
    context_engine=self._context_engine,
    tool_router=tool_router,
    result_writer=self._writer,
    timeout=config.agent.state_machine_timeout,
)
```

- [x] **Step 3: StateMachine 集成 ContextEngine**

`_do_context_building()` 改为调用 `self._context_engine.collect(event, strategy)` 产出 `ContextPack`。降级：ContextEngine 为 None 或 collect() 抛异常时回退为旧 `ReviewContext`。

- [x] **Step 4: Provider 双轨过渡（M4→M5）**

`MockProvider.analyze()` 兼容 `ContextPack`（通过 `to_review_context()` 转换）。
`MockProvider.analyze_from_messages(messages)` — M5 目标接口，当前模拟实现。
`StateMachine` M4 调用 `analyze(context)`，M5 切换为 `analyze_from_messages(assembly.messages)`。

**Verification:**
```bash
uv run --extra dev pytest tests/ -v
# 所有已有测试保持通过（向后兼容）
```

---

## Task 10: 集成接口 `integrations/`

**Files:** `agent/src/ssv_agent/integrations/protocols.py`, `mocks.py`

- [x] **Step 1: 定义四套 Protocol**

```python
class EvidenceProvider(Protocol):
    def get_evidence_summary(self, event: DetectionEvent) -> str: ...
    def get_keyframe_path(self, event: DetectionEvent) -> str: ...

class TrackContextProvider(Protocol):
    def get_consecutive_count(self, event: DetectionEvent, track_id: int) -> int: ...
    def get_track_state(self, event: DetectionEvent, track_id: int) -> str: ...
    def get_track_age(self, event: DetectionEvent, track_id: int) -> float: ...

class SourceMetadataProvider(Protocol):
    def get_scene_info(self, event: DetectionEvent) -> str: ...

class EventSequenceProvider(Protocol):
    def get_pending_events(self, event: DetectionEvent) -> list[dict]: ...
```

- [x] **Step 2: 实现全部 Mock**

全部返回空字符串 / 空列表 / 0 / "unknown"。将来真实实现只需替换 factory 函数中的类名。

**Verification:** Protocol 定义清晰、Mock 实例化不抛异常。

---

## Verification

```bash
cd agent
uv run --extra dev pytest tests/ -v              # 全量测试
uv run --extra dev ruff check src/               # 代码风格
python -c "
from ssv_agent.context.engine import ContextEngine
from ssv_agent.context.budget import BudgetEngine
print('ContextEngine + BudgetEngine import OK')
"
```
