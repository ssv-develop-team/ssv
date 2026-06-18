# T4 Agent 上下文工程设计 Spec

## 目标

将当前分散在各模块的上下文组装逻辑收敛为统一的"上下文工程"子系统。五大要素——系统提示、工具定义、历史记录、检索上下文、用户输入——纳入统一的生命周期管理。整个子系统在 T1/T2/T3 未接通时独立可开发、独立可测试。

---

## 设计决策记录（ADR）

以下决策贯穿全规范，在此集中说明理由，避免正文中反复论证。

| ID | 决策 | 理由 |
|:--|:--|:--|
| D1 | `prompts/system.py` 为系统提示词的 canonical source，`templates.py` 删重复内容 | 两处维护必然漂移。`templates.py` 的模板负责策略级差异化，`system.py` 负责全局不变部分 |
| D2 | `BudgetEngine` 分配配额，`PromptAssembler` 执行截断 | 分配是跨要素的结构性决策（策略相关），截断是单要素的内容裁剪（token 相关）。分离后各自可独立测试 |
| D3 | `ReviewContext` 废弃，`ContextPack` 成为唯一容器 | 两套容器并存会让 StateMachine / Provider / Assembler 三处出现分支判断。M4 保留 `to_review_context()` 兼容方法，M5 移除 |
| D4 | Provider 双轨过渡：M4 保留 `analyze(context)`，M5 切换为 `analyze_from_messages(messages)` | MockProvider 当前不消费 messages 列表；提前定义目标接口避免 M5 大改 |
| D5 | few-shot 选择保留在 `PromptAssembler`，不进入 `ContextEngine` | few-shot 选择与目标 LLM 上下文窗口强相关，属于组装层而非收集层 |
| D6 | `UserInputBuilder` 输出完整的 user message 文本（含模板渲染） | 让 `PromptAssembler` 的职责收缩为"拼接 + 截断"两个操作，模板渲染内聚到输入构建器 |
| D7 | `prompts/manager.py` 废弃，合并到 `SystemPromptManager` | manager.py 功能单薄且从未被调用，单独保留增加维护成本 |

---

## 一、现状与约束

### 1.1 数据流现状

```
C++ 插件链 (T1/T2)                 Redis Streams (T3)              Python Agent (T4)
┌──────────────────┐              ┌──────────────────┐           ┌──────────────────────┐
│ rtspsrc           │              │                  │           │                      │
│   → ssvinfer      │── detections─→  ssv:events      │──消费──→  │ EventConsumer        │
│   → ssvtrack      │              │  (字段契约已冻结)  │           │   → StateMachine     │
│   → ssvpub        │              │                  │           │   → MockProvider     │
└──────────────────┘              └──────────────────┘           │   → ResultWriter     │
                                                                  └──────────────────────┘
```

**当前 T4 稳定依赖的数据**：`DetectionEvent`（source、frame_id、timestamp_ms、detections 列表）。证据路径、跟踪状态、场景元数据和事件序列均通过可为空的集成接口接入；T1/T2/T3 未提供时由 mock 实现返回空值或 `unknown`，不影响 T4 单元测试。

### 1.2 五大要素的就绪状态

| 要素 | 就绪 | 缺口 |
|:--|:--:|:--|
| 系统提示 | ✅ | 一段平铺文本，无分层版本管理；`system.py` 与 `templates.py` 存在内容重复 |
| 工具定义 | ⚠️ | `ToolRouter` 能注册调用，但 schema 未注入 prompt，LLM 不可见 |
| 用户输入 | ⚠️ | 事件→文本渲染散落在 `models/event.py` (`prompt_context`) 和 `prompts/assembler.py` (`_build_event_context`) 两处 |
| 历史记录 | ⚠️ | `HistoryManager` 已有内存写入和窗口检索；尚未持久化 |
| 检索上下文 | ✅ | `RetrievalManager` 从 `config/rules.yaml` 加载规则，缺失时降级到内置规则 |

### 1.3 设计原则

1. **T4 自包含** — 不依赖 T1/T2/T3 提供额外数据即可完整运行
2. **Mock 可验证** — 每个模块用构造的 `DetectionEvent` 通过 `pytest`
3. **接口预留** — 与 T1/T2/T3 的集成点使用 Protocol，当前 mock，将来切换零改动
4. **渐进激活** — 数据为空时静默跳过，接通后自动生效；模块未初始化时产出空 Block，不影响其他要素

---

## 二、上下文组装管线

### 2.1 当前落地状态

```
DetectionEvent → StateMachine
                      │
                      ├─ ContextEngine.collect() → ContextPack
                      └─ MockProvider.analyze(ContextPack)
```

当前已经收敛的点：
- `ContextEngine` 已在 `StateMachine._do_context_building()` 中调用，产出 `ContextPack`
- `UserInputBuilder` 承担事件到 user message 的主要渲染，`ReviewContext.prompt_context` 仅保留兼容路径并标记 deprecated
- `RetrievalManager` 已接入 `config/rules.yaml`，替代旧 `ContextBuilder._lookup_rules()` 的硬编码路径
- `BaseTool` 已具备自描述属性和 `to_definition()`，可被 `ToolDefinitionsRenderer` 渲染给后续真实 LLM

仍保留的 M5 激活点：
- `PromptAssembler` 和 `BudgetEngine` 已在 `AgentService` 中构造，但 M4 状态机仍调用 `MockProvider.analyze(context)`；真实 provider 接入时再切换到 `analyze_from_messages(messages)`
- `ContextBuilder` 仅作为兼容旧测试和旧调用的薄包装保留

### 2.2 统一管线

```
DetectionEvent + Strategy + ToolRouter
  │
  ▼
ContextEngine.collect()
  ├─ SystemPromptManager.render(strategy)     → 分层拼接 (L0-L4)
  ├─ ToolDefinitionsRenderer.render(router)   → 条件注入
  ├─ HistoryManager.get_window(event)         → 滑动窗口 + 压缩
  ├─ RetrievalManager.search(event, strategy)  → 规则匹配 + 去重
  ├─ UserInputBuilder.build(event, strategy)  → 完整 user message 文本
  └─ 集成数据源注入（空则跳过）                 → 证据/跟踪/元数据
  │
  ▼
ContextPack (五要素统一容器)
  │
  ▼
BudgetEngine.allocate(pack)  → 按策略分配配额，附加 TokenBudget 到 pack
  │
  ▼
PromptAssembler.assemble(pack)
  ├─ 系统提示词拼接 (L0-L3)
  ├─ 工具定义注入 (L4, 条件过滤)
  ├─ Few-shot 示例选择 + 注入
  ├─ 任务 user message 拼接
  └─ Token 截断 (按 BudgetEngine 配额执行)
  │
  ▼
PromptAssembly → messages 列表
```

> **注**：M4 已实现 `ContextEngine.collect()` 到 `ContextPack`；`BudgetEngine.allocate()` 和 `PromptAssembler.assemble()` 已可单独测试，真实模型 provider 接入前不驱动生产复核结论。

### 2.3 关键收敛

| 原逻辑 | 散落位置 | 收敛到 | 说明 |
|:--|:--|:--|:--|
| `prompt_context` 字符串拼接 | `models/event.py` → `ReviewContext.prompt_context` | `UserInputBuilder` | 标记 deprecated，M5 移除 |
| `_build_event_context()` | `prompts/assembler.py` | `UserInputBuilder` | 合并入 `UserInputBuilder.build()` |
| `_render_system_prompt()` | `prompts/assembler.py` | `SystemPromptManager` | 含分层拼接 + 版本快照 |
| `_render_task_template()` | `prompts/assembler.py` | `UserInputBuilder` | 模板渲染逻辑内聚到输入构建器 |
| `_lookup_rules()` | `context/builder.py` | `RetrievalManager` | `ContextBuilder` 整体废弃 |
| `MockProvider` 直接读 `prompt_context` | `providers/mock.py` | 已兼容 `ContextPack`（M4）→ `PromptAssembly`（M5） | 见第四节 Provider 双轨过渡 |
| `SYSTEM_PROMPT_CONTENT` 重复定义 | `prompts/templates.py` | 删除，`system.py` 为 canonical source | D1 |

---

## 三、系统提示的工程化

### 3.1 Canonical Source

`prompts/system.py` 的 `DEFAULT_SYSTEM_PROMPT` 为系统提示词的**唯一 canonical source**。`prompts/templates.py` 中的 `SYSTEM_PROMPT_CONTENT` 和 `OUTPUT_FORMAT_SPEC` 删除，改为 `from ssv_agent.prompts.system import DEFAULT_SYSTEM_PROMPT` 引用。

### 3.2 五层分层

当前 `DEFAULT_SYSTEM_PROMPT.content` 是一段平铺文本。改为按稳定性和职责的五层结构，在 `SystemPrompt` 数据类内部以 `layers: dict[str, str]` 存储：

```python
@dataclass
class SystemPrompt:
    layers: dict[str, str]     # {"role": "...", "domain_knowledge": "...", ...}
    version: str = "1.0.0"
    output_spec: str = ""
    constraints: list[str] = field(default_factory=list)

    def render(self, layers: Optional[list[str]] = None) -> str:
        """按指定层列表渲染，默认全部。"""
        keys = layers or list(self.layers.keys())
        return "\n\n".join(self.layers[k] for k in keys if k in self.layers)
```

| 层 | key | 内容 | 变更频率 | 来源 |
|:--|:--|:--|:--|:--|
| L0 角色定义 | `role` | "你是工地安全巡检智能 Agent..." | 几乎不变 | `system.py` `## 你的职责` 段 |
| L1 领域知识 | `domain_knowledge` | 判断原则（置信度阈值、bbox 重叠规则） | 月级 | `system.py` `## 判断原则` 段 |
| L2 策略指引 | `strategy_guidance` | "当前事件已被管线判定为高置信度违规..." | 周级 | `templates.py` `STRATEGY_SYSTEM_TEMPLATES` |
| L3 输出约束 | `output_constraints` | JSON Schema + 行为约束 + 字数限制 | 周级 | `system.py` `output_spec` + `constraints` |
| L4 工具使用说明 | `tool_usage` | 可用工具列表 + 调用时机 | 天级 | `ToolDefinitionsRenderer` 动态生成 |

**L0/L1 拆分方式**：`DEFAULT_SYSTEM_PROMPT.content` 按 `## 你的职责` 和 `## 判断原则` 等 Markdown 标题自然切分，无需人工重写。

### 3.3 版本管理

每层独立语义版本号（`MAJOR.MINOR.PATCH`），措辞调整 +PATCH，增删原则 +MINOR，改角色/格式 +MAJOR。每次组装时记录版本快照到 `PromptAssembly.metadata`：

```python
"system_prompt_versions": {
    "role": "1.0.0", "domain_knowledge": "1.2.0",
    "strategy_guidance": "1.1.0", "output_constraints": "1.0.1",
    "tool_usage": "auto"
}
```

### 3.4 硬约束

| 约束 | 阈值 |
|:--|:--|
| 总字符数（L0-L3，不含工具层） | ≤ 2000 字符 |
| 每层字符数 | ≤ 500 字符 |
| 判断原则条数 | ≤ 8 条 |
| 每条原则字符数 | ≤ 80 字符 |

超限 log warning，debug 模式 raise。A/B 变体和回滚机制预留接口（`SystemPromptManager.select_version(variant)`），M5 接入真实 LLM 后激活。

### 3.5 激活阶段

| 功能 | M4 | M5+ |
|:--|:--:|:--:|
| 分层渲染 + 版本快照 | ✅ 实现 | 激活 |
| 硬约束检查 | ✅ 实现 | 激活 |
| A/B 变体接口 | 接口预留 | ✅ |

---

## 四、工具描述与 LLM 可见性

### 4.1 缺口

`ToolRouter` 能按名调用工具，但工具没有自描述能力——LLM 不知道有哪些工具、每个工具做什么、需要什么参数。

### 4.2 BaseTool 扩展

当前只有 `name` + `execute`。新增四个自描述属性 + 一个导出方法：

| 属性/方法 | 作用 | 示例 |
|:--|:--|:--|
| `description` | LLM 判断"该不该调用" | "输入触发原因，返回匹配的安全规范条文。当检测信号冲突时使用。" |
| `parameters_schema` | LLM 构造调用参数 | `{"type":"object","properties":{"event_type":{"enum":["no_helmet",...]}}}` |
| `returns_description` | LLM 理解返回值 | "返回规则文本列表。无匹配时返回空列表（不是错误）。" |
| `side_effects` | LLM 理解副作用 | `""`（无副作用）/ `"发送通知到现场安全员"` |
| `to_definition()` | 导出为可序列化的 `ToolDefinition` | 供 `ToolDefinitionsRenderer` 使用 |

### 4.3 ToolDefinition 数据类

```python
@dataclass
class ToolDefinition:
    name: str
    description: str
    parameters_schema: dict
    returns_description: str
    side_effects: str
```

`BaseTool.to_definition() -> ToolDefinition` 将工具实例的自描述信息导出为纯数据对象，`ToolDefinitionsRenderer` 只依赖 `ToolDefinition` 而非 `BaseTool` 实例，解耦渲染与执行。

### 4.4 注入策略

| LLM 平台 | 注入方式 | Token 消耗 |
|:--|:--|:--|
| OpenAI / Anthropic (native) | API `tools` 参数 | 不计入 prompt |
| 其他模型 | 嵌入 system prompt（紧凑纯文本） | 计入 prompt |

**条件注入**：按策略过滤可见工具。`direct_confirm` 只看到 1 个工具，`notify_report` 看到 2-3 个，避免 LLM 困惑。过滤规则：`ToolDefinitionsRenderer.render(router, strategy)` 按策略名过滤已注册工具。

**工具描述编写规范**（六条）：名称动词化、参数枚举完整、失败语义明确、给出使用时机、一句话说清输入→输出映射。

---

## 五、历史记录的压缩策略

### 5.1 当前状态

`ReviewContext.history_summary` 始终为空。无写入路径、无存储、无压缩——Agent 每次复核都是"失忆"状态。

**这不是集成问题**——历史数据完全由 T4 自己的复核结果产生，当前即可构建完整机制。

### 5.2 三级压缩

```
Level 0: 全量保留    token < budget×0.8    完整 event→result 对
Level 1: 轻量压缩    token ≥ budget×0.8    去 bbox/track_id，仅保留 class+confidence
Level 2: 按目标摘要  轻量后仍超            保留最近5条，其余按 track_id 分组各一句摘要
Level 3: 全局摘要    摘要后仍超            所有历史合并为一段统计文本
```

**激活阶段**：M4 实现 Level 0（全量保留），Level 1-3 接口预留，M5 激活。

### 5.3 优先级驱逐

不是删最旧的，而是删对当前决策价值最低的。关联度评分（0~1）：

| 因子 | 权重 | 逻辑 |
|:--|:--:|:--|
| track_id 重叠 | 0.50 | 同一目标的事件最重要 |
| 时间邻近度 | 0.25 | 10s 内 0.25分，30s 内 0.15分 |
| 严重程度 | 0.15 | critical/high 0.15分 |
| 同触发原因 | 0.10 | 同类型事件的处理经验 |

按 score 升序逐条移除，达标即停。移除的消息进入 `HistoryBlock.summary`。

> **依赖说明**：`track_id 重叠` 计算依赖 `Detection.track_id` 字段（T2 提供）。当前 T2 未接通时，track_id 默认 -1，该因子退化为 0 分，不影响驱逐逻辑正常运行。

### 5.4 HistoryMessage 数据类

```python
@dataclass
class HistoryMessage:
    event_id: str
    track_ids: list[int]          # 涉及的目标 track_id 列表（-1 表示未跟踪）
    timestamp_ms: int
    severity: str
    trigger_reason: str
    strategy: str
    conclusion_summary: str       # 一条压缩后的结论摘要
```

### 5.5 生命周期

- `record(event, result)` — 复核完成后写入
- `get_window(current_event)` — 下次复核时查询，按关联度排序
- 空历史 → 返回空 `HistoryBlock`，组装管线静默跳过
- 每次压缩操作记录元数据（压缩级别、丢弃条数、压缩比）到 `PromptLogger`

---

## 六、检索上下文的质量控制

### 6.1 当前状态

`ContextBuilder._lookup_rules()` 用 if-else 返回 3 条硬编码规则。且 `StateMachine._do_context_building()` 没有调用它——`rule_snippets` 始终为空列表。

### 6.2 规则外部化

迁移到 `config/rules.yaml`（项目根，与 `config/ssv.default.yaml` 同级）。

`RetrievalManager` 查找顺序：
1. `config/rules.yaml` — 默认路径
2. `$SSV_RULES_PATH` 环境变量 — 显式覆盖
3. 硬编码默认规则 — 文件缺失/损坏时降级

规则内容参见[附录 C](#附录-c-rulesyaml-示例)。

### 6.3 四级处理管线

```
精确匹配 → MD5 去重 → source_type 标注 → priority 排序截断
```

### 6.4 Prompt 呈现

按来源分级，帮助 LLM 自行判断采信权重：

```
## 适用规则与知识

### 法规依据
- 《电力安全工作规程 电力线路部分》GB 26859-2011 第4.2条:
  进入电气作业区域必须正确佩戴安全帽，任何人不得在未佩戴的情况下进入带电间隔。
- 《电力安全工作规程 发电厂和变电站电气部分》GB 26860-2011 第5.4条:
  同一目标在电气作业区域内连续3帧以上未佩戴安全帽触发告警。

### 专家经验
- 电气作业安全帽检测误判分析报告 v1.0:
  bbox 高度重叠且同一 track_id → 倾向于已佩戴；无明显重叠需人工复核。
  注意"手持安全帽而未佩戴"的特殊情况。

（未找到匹配的统计规律。）
```

### 6.5 空结果处理

无匹配时不静默，显式告知 LLM：`（未找到匹配规则。请基于系统提示中的判断原则独立复核。）`——防止 LLM 编造规则，同时提示团队补充知识库。

---

## 七、上下文预算管理

### 7.1 两层预算模型

`BudgetEngine` 负责**分配**，`PromptAssembler` 负责**执行**（D2）。

```
ContextPack → BudgetEngine.allocate(pack, strategy)
              → 产出 TokenBudget（各要素 token 上限）
              → 附加到 ContextPack.token_budget
              ↓
ContextPack → PromptAssembler.assemble(pack)
              → 读取 pack.token_budget 各要素配额
              → 执行逐级截断
```

### 7.2 按策略分配（BudgetEngine）

`BudgetEngine` 按策略为五要素分配 token 上限，输出 `TokenBudget`：

| 要素 (tokens) | direct_confirm | visual_review | rule_explain | notify_report |
|:--|:--:|:--:|:--:|:--:|
| system_prompt | 800 | 800 | 800 | 800 |
| tool_definitions | 200 | 400 | 400 | 400 |
| user_input_base | 400 | 400 | 400 | 400 |
| user_input_details | 600 | 600 | 400 | 600 |
| retrieval_context | 200 | 400 | **800** | 400 |
| history | 300 | 400 | 400 | 300 |
| few_shot | 300 | 300 | 200 | 300 |

> `rule_explain` 给 retrieval_context 最多预算（需要规则条文），压缩 few_shot 和 user_input_details。

### 7.3 逐级截断链（PromptAssembler）

`PromptAssembler` 从 `ContextPack.token_budget` 读取各要素配额，超出预算时按优先级逐级裁剪，每级后重新估算，达标即停：

| 优先级 | 裁剪对象 | 动作 |
|:--:|:--|:--|
| 1 | few_shot | N→0 条递减 |
| 2 | 历史记录 | 非关联→旧事件→全部移除仅留 summary |
| 3 | 检索上下文 | relevance 从低到高逐条移除 |
| 4 | 检测详情 | 精简模式（去 bbox）→ 仅保留前 5 条 |
| — | 系统提示/工具定义/事件基本标识 | **永不裁剪** |

### 7.4 预算可观测性

每次组装后输出核算报告到 `PromptAssembly.metadata` → `PromptLogger`，按 `truncated=True` 过滤可快速定位问题。

---

## 八、Provider 接口双轨过渡

### 8.1 当前问题

`MockProvider.analyze()` 接收 `ReviewContext`（读取 `context.strategy` 做分支判断），但未来真实 LLM provider 需要接收 `messages` 列表。如果等到 M5 再改接口，`StateMachine`、`service.py`、所有 provider 实现都要同步修改。

### 8.2 过渡策略

**M4 阶段**（当前）：双接口并存

```python
class ProviderProtocol(Protocol):
    """M4 兼容：接收上下文对象，内部自行判断如何消费。"""
    def analyze(self, context) -> str: ...

    """M5 目标：接收已组装的 messages 列表，直接调用 LLM API。"""
    def analyze_from_messages(self, messages: list[PromptMessage]) -> str: ...
```

- `MockProvider` 同时实现两个方法
- `StateMachine` M4 仍调用 `analyze(context)`，传入 `ContextPack`（`MockProvider` 内部通过 `to_review_context()` 兼容）
- `PromptAssembler` 在 `service.py` 中已构造，但暂未接入 `StateMachine`

**M5 阶段**（真实 LLM 接入后）：

- `StateMachine` 改为：`ContextEngine.collect()` → `BudgetEngine.allocate()` → `PromptAssembler.assemble()` → `provider.analyze_from_messages(assembly.messages)`
- `analyze()` 方法标记 deprecated，M6 移除
- `ContextPack.to_review_context()` 移除

### 8.3 对 service.py 的影响

M4 阶段 `service.py` 变更：

```python
# 当前
self._assembler = PromptAssembler(max_tokens=config.agent.prompt_max_tokens)
self._machine = StateMachine(provider=provider, ...)

# M4 当前实现
self._context_engine = ContextEngine(...)
self._budget_engine = BudgetEngine()
self._assembler = PromptAssembler(max_tokens=config.agent.prompt_max_tokens)
self._machine = StateMachine(
    provider=provider,
    context_engine=self._context_engine,   # 新增
    ...
)
```

`StateMachine._do_context_building()` 改为调用 `self._context_engine.collect(event, strategy)` 产出 `ContextPack`。

---

## 九、跨主线集成接口

T1/T2/T3 接通后，以下四套 Protocol 将补充数据注入上下文。当前全部 mock（返空或 `unknown`），将来按配置切换。

### 9.1 四套接口总览

```
T1 (视频链路)              T2 (感知算法)            T3 (事件边界)
  │                          │                        │
  ├─ 视频源元数据             ├─ 跟踪状态               ├─ 证据文件
  │  (位置/场景/光照)         │  (state/age/           │  (关键帧/渲染图/片段)
  │                           │   consecutive_count)    │
  ▼                           ▼                        ├─ 事件序列
SourceMetadataProvider    TrackContextProvider          │  (相邻 pending 事件)
  │                           │                        ▼
  │                           │                  EvidenceProvider
  │                           │                  EventSequenceProvider
  └─────────────┬─────────────┴────────────────────┘
                ▼
       ContextEngine.collect()
```

### 9.2 接口分层

**Layer 0（本阶段实现，接通即用）**：

| 接口 | 来源 | 落点 | 影响 |
|:--|:--|:--|:--|
| `EvidenceProvider` | T3 | `UserInputBlock.evidence_summary` | 高 — visual_review 核心输入 |
| `TrackContextProvider` | T2 | `UserInputBlock.track_context` | 高 — relevance_score 精确化 |

**Layer 1（M5 激活）**：

| 接口 | 来源 | 落点 | 影响 |
|:--|:--|:--|:--|
| `SourceMetadataProvider` | T1 | `UserInputBlock.source_metadata` | 中 — 场景锦上添花 |
| `EventSequenceProvider` | T3 | `HistoryBlock` 补充 | 中 — pending 相邻事件上下文 |

### 9.3 接口定义（含 track 状态字段）

```python
# integrations/protocols.py

class EvidenceProvider(Protocol):
    """T3 证据模块 → T4 上下文。"""
    def get_evidence_summary(self, event: DetectionEvent) -> str: ...
    def get_keyframe_path(self, event: DetectionEvent) -> str: ...

class TrackContextProvider(Protocol):
    """T2 跟踪模块 → T4 上下文。"""
    def get_consecutive_count(self, event: DetectionEvent, track_id: int) -> int: ...
    def get_track_state(self, event: DetectionEvent, track_id: int) -> str: ...
    def get_track_age(self, event: DetectionEvent, track_id: int) -> float: ...

class SourceMetadataProvider(Protocol):
    """T1 视频链路 → T4 上下文。"""
    def get_scene_info(self, event: DetectionEvent) -> str: ...

class EventSequenceProvider(Protocol):
    """T3 事件边界 → T4 上下文。"""
    def get_pending_events(self, event: DetectionEvent) -> list[dict]: ...
```

> **注**：`get_consecutive_count()` 和 `get_track_state()` 为历史管理器关联度计算提供精确输入。T2 未接通时 mock 返回 0/"unknown"，不影响正常运作。

### 9.4 设计模式

每个接口遵循统一模式（以 `EvidenceProvider` 为例）：

```python
# Mock 实现 — integrations/mocks.py
class MockEvidenceProvider:
    def get_evidence_summary(self, event): return ""
    def get_keyframe_path(self, event): return ""

# 将来真实实现（骨架，本阶段不写）
class FilesystemEvidenceProvider:
    def get_evidence_summary(self, event): ...  # 读文件元数据
    def get_keyframe_path(self, event): ...     # 返回真实路径
```

**切换方式**：`ContextEngine` 依赖 Protocol，后续在 `service.py` 工厂函数中按配置字符串选择 mock 或真实实现。

**独立切换**：四个接口互不依赖，可分别接通。空值时静默跳过，不影响其他接口。

### 9.5 影响矩阵

| 上下文要素 | Evidence | Track | SourceMeta | EventSeq |
|:--|:--|:--|:--|:--|
| 用户输入 | +文件摘要 | +track状态 | +场景文本 | — |
| 历史记录 | — | relevance精确化 | — | +pending事件 |
| 检索上下文 | — | — | 场景调整搜索 | — |
| 工具定义 | +keyframe参数 | — | — | — |

---

## 十、模块结构与改动范围

### 10.1 新增模块

```
agent/src/ssv_agent/
├── context/              # 上下文工程子系统 (8 文件)
│   ├── engine.py         # ContextEngine (collect 主入口)
│   ├── pack.py           # ContextPack + 五要素 Block + HistoryMessage + ToolDefinition
│   ├── budget.py         # BudgetEngine + TokenBudget
│   ├── system_prompt.py  # SystemPromptManager (含 layers 拆分 + 版本管理)
│   ├── tool_definitions.py  # ToolDefinitionsRenderer
│   ├── history_manager.py   # HistoryManager
│   ├── retrieval_manager.py # RetrievalManager (含 YAML 加载 + 降级)
│   └── user_input.py     # UserInputBuilder (输出完整 user message 文本)
│
├── integrations/         # 跨主线集成接口 (2 文件)
│   ├── protocols.py      # 全部 Protocol + 数据类
│   └── mocks.py          # Mock 实现
│
├── prompts/              # 提示词内容层
│   ├── system.py         # 改: SystemPrompt 新增 layers 字段; 为 canonical source
│   ├── templates.py      # 改: 删 SYSTEM_PROMPT_CONTENT / OUTPUT_FORMAT_SPEC 重复定义
│   ├── assembler.py      # 重构: assemble() 接收 ContextPack; 去 _render_task_template()
│   ├── logger.py         # 小改: CallRecord 新增 5 个字段
│   ├── examples.py       # 不动
│   └── manager.py        # 废弃 → 合并到 SystemPromptManager
│
└── tools/base.py         # 小改: BaseTool 新增 4 属性 + to_definition()
```

### 10.2 改动矩阵

| 模块 | 程度 | 说明 |
|:--|:--:|:--|
| `context/` | 新包 | 8 文件 |
| `integrations/` | 新包 | 2 文件 |
| `config/rules.yaml` | 新文件 | 安全规则，查找路径见 6.2 节 |
| `prompts/system.py` | 中改 | SystemPrompt 新增 layers 拆分 |
| `prompts/templates.py` | 小改 | 删 SYSTEM_PROMPT_CONTENT / OUTPUT_FORMAT_SPEC |
| `prompts/assembler.py` | 重构 | 接收 ContextPack；委托模板渲染给 UserInputBuilder |
| `prompts/logger.py` | 小改 | CallRecord 新增字段（见 10.3 节） |
| `prompts/manager.py` | 废弃 | → SystemPromptManager |
| `tools/base.py` | 小改 | +4 属性 + to_definition() |
| `service.py` | 中改 | 构造 ContextEngine + BudgetEngine；传入 StateMachine；复核完成后记录内存历史 |
| `state_machine/machine.py` | 中改 | _do_context_building 调用 ContextEngine.collect() |
| `providers/mock.py` | 小改 | 新增 analyze_from_messages() |
| `config.py` | 中改 | 新增上下文工程配置项 |
| `context/builder.py` | 废弃 | → retrieval_manager |
| `models/event.py` | 小改 | ReviewContext.prompt_context 标记 deprecated |
| `prompts/examples.py` | **不动** | — |

### 10.3 PromptLogger 新增字段

`CallRecord` 新增以下字段：

| 字段 | 类型 | 来源 |
|:--|:--|:--|
| `system_prompt_versions` | `dict[str, str]` | `SystemPromptManager.render()` → `PromptAssembly.metadata` |
| `context_pack_id` | `str` | `ContextPack.context_id` |
| `budget_allocations` | `dict[str, int]` | `BudgetEngine.allocate()` → `TokenBudget` |
| `compression_summary` | `dict` | `HistoryManager.get_window()` — 压缩级别、丢弃条数、压缩比 |
| `truncation_details` | `dict` | `PromptAssembler._apply_token_budget()` — 哪些要素被截断、截断前后大小 |

---

## 十一、错误处理

| 场景 | 降级行为 |
|:--|:--|
| 工具定义渲染失败 | 回退纯文本紧凑格式 |
| `rules.yaml` 损坏/缺失 | 降级为硬编码默认规则（原 `_lookup_rules()` 逻辑），log error |
| 历史记录为空 | 返回空 `HistoryBlock`，静默跳过 |
| 检索结果全部低于门禁 | 返回空，显式告知 LLM |
| 截断手段用尽仍超预算 | 强制最精简版本，`truncated=True` |
| ContextEngine 未初始化 | 降级为仅 system_prompt + user_input（等效当前 `StateMachine` 行为） |
| `ContextEngine` 部分模块为 None | 已就绪模块正常收集，未就绪模块产出空 Block，日志 warning |

---

## 十二、验证

```bash
cd agent
uv run --extra dev pytest       # 全部测试（mock 数据，不依赖 Redis/RTSP/LLM）
uv run --extra dev ruff check src/
```

| 测试文件 | 覆盖 | 预计 |
|:--|:--|:--:|
| `test_budget.py` | BudgetEngine 四策略分配 + PromptAssembler 逐级截断 | 15+ |
| `test_system_prompt.py` | 分层渲染/版本快照/硬约束 | 12+ |
| `test_tool_definitions.py` | 三种格式/条件注入/紧凑模式/`to_definition()` | 10+ |
| `test_history_manager.py` | 三级压缩/驱逐算法/摘要/空历史 | 16+ |
| `test_retrieval_manager.py` | YAML加载/精确匹配/去重/空结果/降级 | 10+ |
| `test_user_input.py` | 完整/精简渲染模式 + 模板填充 | 8+ |
| `test_context_engine.py` | collect 全流程/各要素空值降级/部分模块为 None | 10+ |
| `test_context_pipeline.py` | **集成测试**：Event → ContextPack → Budget → Assembly → messages | 5+ |

---

## 十三、文件清单

```
新增 (13):
  agent/src/ssv_agent/context/__init__.py
  agent/src/ssv_agent/context/engine.py
  agent/src/ssv_agent/context/pack.py
  agent/src/ssv_agent/context/budget.py
  agent/src/ssv_agent/context/system_prompt.py
  agent/src/ssv_agent/context/tool_definitions.py
  agent/src/ssv_agent/context/history_manager.py
  agent/src/ssv_agent/context/retrieval_manager.py
  agent/src/ssv_agent/context/user_input.py
  agent/src/ssv_agent/integrations/__init__.py
  agent/src/ssv_agent/integrations/protocols.py
  agent/src/ssv_agent/integrations/mocks.py
  config/rules.yaml

修改 (9):
  agent/src/ssv_agent/prompts/system.py
  agent/src/ssv_agent/prompts/templates.py
  agent/src/ssv_agent/prompts/assembler.py
  agent/src/ssv_agent/prompts/logger.py
  agent/src/ssv_agent/tools/base.py
  agent/src/ssv_agent/models/event.py
  agent/src/ssv_agent/state_machine/machine.py
  agent/src/ssv_agent/service.py
  agent/src/ssv_agent/config.py

废弃 (2):
  agent/src/ssv_agent/context/builder.py
  agent/src/ssv_agent/prompts/manager.py

测试 (8):
  agent/tests/test_budget.py
  agent/tests/test_system_prompt.py
  agent/tests/test_tool_definitions.py
  agent/tests/test_history_manager.py
  agent/tests/test_retrieval_manager.py
  agent/tests/test_user_input.py
  agent/tests/test_context_engine.py
  agent/tests/test_context_pipeline.py
```

---

## 附录 A：关键数据类速览

```python
# ── ContextPack — 五要素统一容器 ──
@dataclass
class ContextPack:
    system_prompt: SystemPromptBlock
    tool_definitions: ToolDefinitionsBlock
    history: HistoryBlock
    retrieval_context: RetrievalBlock
    user_input: UserInputBlock
    context_id: str
    token_budget: Optional[TokenBudget] = None  # BudgetEngine 填充
    metadata: dict = field(default_factory=dict)

    def to_review_context(self) -> ReviewContext:
        """M4 向后兼容。M5 移除。"""
        ...

# ── 各要素 Block ──
@dataclass
class SystemPromptBlock:
    content: str
    versions: dict[str, str]  # 各层版本快照

@dataclass
class ToolDefinitionsBlock:
    tools: list[ToolDefinition]
    format: str  # "openai" / "anthropic" / "text"

@dataclass
class HistoryBlock:
    messages: list[HistoryMessage]
    summary: str
    compression_level: str  # "full" / "light" / "per_track" / "global"
    compression_records: list[CompressionRecord]

@dataclass
class RetrievalBlock:
    items: list[RetrievalItem]

@dataclass
class UserInputBlock:
    event_context: str          # 渲染后的事件文本
    evidence_summary: str
    track_context: list[TrackContext]
    source_metadata: Optional[SourceMetadata] = None

# ── HistoryMessage ──
@dataclass
class HistoryMessage:
    event_id: str
    track_ids: list[int]          # -1 表示未跟踪
    timestamp_ms: int
    severity: str
    trigger_reason: str
    strategy: str
    conclusion_summary: str

# ── ToolDefinition ──
@dataclass
class ToolDefinition:
    name: str
    description: str
    parameters_schema: dict
    returns_description: str
    side_effects: str

# ── TokenBudget ──
@dataclass
class TokenBudget:
    allocations: dict[str, int]  # 要素 → token 上限
    total: int
    strategy: str
```

---

## 附录 B：后续演进

| 项目 | M4 | M5+ |
|:--|:--|:--|
| 系统提示词 | 分层渲染 + 版本快照 | A/B 流量分流 + 自动评估 |
| 向量检索 | YAML 精确匹配 | embedding 语义检索 |
| Token 计数 | 字符数/2 | tiktoken / Anthropic API |
| 历史持久化 | 内存 dict (Level 0) | Level 1-3 压缩 + Redis 持久化 |
| 证据上下文 | 空占位 | T3 证据路径 → 真实文件 |
| Provider 接口 | `analyze(context)` 兼容 | 切换为 `analyze_from_messages(messages)` |
| 多模态 | 纯文本 | 关键帧图片注入 VLM |

---

## 附录 C：rules.yaml 示例

```yaml
# config/rules.yaml — 安全规则知识库
# RetrievalManager 查找顺序: 本文件 → $SSV_RULES_PATH → 硬编码默认规则

rules:
  - id: "safety-helmet-mandatory"
    trigger: ["no_helmet", "low_confidence"]
    content: >
      安全帽佩戴规范: 进入电气作业区域（变电站、配电室、电缆沟道、架空线路下方）
      必须正确佩戴安全帽。任何人不得在未佩戴安全帽的情况下进入带电间隔或作业警戒区。
    source: "《电力安全工作规程 电力线路部分》GB 26859-2011 第4.2条"
    source_type: "regulation"
    priority: 1

  - id: "consecutive-violation"
    trigger: ["consecutive_hits"]
    content: >
      连续违规判定: 同一目标在电气作业区域内连续3帧以上检测为未佩戴安全帽，
      触发告警并通知现场工作负责人。对于变电站高压区、近电作业等高风险场景，
      连续2帧即可触发。
    source: "《电力安全工作规程 发电厂和变电站电气部分》GB 26860-2011 第5.4条"
    source_type: "regulation"
    priority: 2

  - id: "conflict-resolution"
    trigger: ["rule_conflict"]
    content: >
      冲突判定规则: 当同一目标同时存在 head 和 helmet 检测时:
      (1) 若 bbox 高度重叠(IoU>0.5)且属于同一 track_id，
          倾向于判定为已佩戴安全帽(可能因安全帽颜色/材质与背景融合导致模型漏检);
      (2) 若 bbox 无明显重叠或分属不同 track_id，需人工复核，
          特别注意电气作业场景中可能出现的"手持安全帽而未佩戴"的情况。
    source: "电气作业安全帽检测误判分析报告 v1.0"
    source_type: "expert"
    priority: 2
```
