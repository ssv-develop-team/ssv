# T4 Agent 与知识复核主线 —— 项目进度汇报

> 汇报人：lzy | 日期：2026-06-10 | 范围：`agent/` 目录，M4 里程碑

---

## 核心成果

1. **初步打通整个 T4 链路** — 事件产生 → 证据输出 → Redis 异步边界 → Agent 事件消费 → 上下文构造 → 状态机编排 → 策略选择 → 模型调用 → 结果汇总 → 结果回写，端到端闭环
2. **完成 Agent 上下文工程设计** — 五要素（系统提示/工具定义/历史/检索/用户输入）统一管线，ContextEngine → BudgetEngine → PromptAssembler 三层组装
3. **针对 Agent 输出不可控问题，新增提示词模块加固** — 5 条核心原则（清晰明确/提供上下文/具体要求/示例参考/分步思考），从 4 行硬编码升级为 7 个模块集中管理

---

## 一、详细进度 |

---

## 二、子系统一：事件模型 + 状态机 + Provider

### 做了什么

把 `agent/` 从 M0 基线（消费 Redis → 打印 → ACK）推进到具备完整复核编排能力。

### 核心模块

| 模块 | 文件 | 职责 |
|:--|:--|:--|
| 事件领域模型 | `models/event.py` | pydantic 强类型：Detection / DetectionEvent / ReviewResult / ReviewContext / ToolResult。事件自带 infer_severity() / infer_trigger_reason() / select_strategy() 自动推断逻辑 |
| LLM 状态机 | `state_machine/machine.py`, `states.py` | 7 流转状态 + 3 终态 + 4 条复核策略路径。依赖注入 Provider/ToolRouter/ResultWriter，Protocol 协议解耦 |
| Provider 抽象 | `providers/base.py`, `mock.py` | BaseProvider（文本模型）+ BaseVLMProvider（视觉模型）双抽象基类 + MockProvider / MockVLMProvider 开发阶段 mock |
| 上下文构造 | `context/builder.py` | ContextBuilder（M4 阶段，后续升级为 ContextEngine） |
| 工具路由 | `tools/base.py`, `router.py` | BaseTool 抽象 + ToolRouter 注册调用 |
| 结果回写 | `writer/result_writer.py` | Redis Streams + 结构化日志双通道 |
| 服务整合 | `service.py` | AgentService 编排完整链路：消费 → 状态机 → 回写 → ACK |

### 四条复核策略

| 策略 | 触发条件 | 行为 |
|:--|:--|:--|
| DIRECT_CONFIRM | head-only + 高置信度 | 直接确认违规，跳过工具调用 |
| VISUAL_REVIEW | 低置信度/遮挡 | 调用 VLM provider 复核关键帧 |
| RULE_EXPLAIN | head+helmet 同时存在 | 检索规则 + LLM 解释冲突 |
| NOTIFY_REPORT | CRITICAL 严重程度 | 生成通知 + 报告 |

### 关键数据流

```
Redis Streams (ssv:events)
  → DetectionEvent (pydantic)
  → StateMachine.execute()
    → 解析 → 上下文 → 策略选择 → 工具调用 → 汇聚 → 回写
  → ReviewResult
  → Redis Streams (ssv:review-results) / 结构化日志
  → ACK
```

---

## 三、子系统二：提示词管理模块

### 做了什么

将 Agent 的提示词从 4 行 if-else 硬编码字符串收敛为集中管理的模块，遵循 5 条核心原则：清晰明确、提供必要上下文、设定具体要求、给出示例参考、分步骤思考。

### 核心模块

| 模块 | 文件 | 职责 |
|:--|:--|:--|
| 系统提示词 | `prompts/system.py` | 角色定义 + 判断原则 + 输出格式 + 行为约束。唯一 canonical source |
| 策略模板 | `prompts/templates.py` | 4 套策略模板（含占位符 + 分步子问题 + 策略专属系统提示追加） |
| Few-shot 示例 | `prompts/examples.py` | 5 条人造示例，按标签检索，ExampleBank 管理 |
| 组装器 | `prompts/assembler.py` | 模板+示例+上下文→messages 列表 + Token 预算截断 |
| 调试日志 | `prompts/logger.py` | 每次 LLM 调用记录完整 prompt+response（mock 模式下默认关闭） |
| 版本管理 | `prompts/manager.py` | validate() 健康检查 + get_versions() 版本快照 |

### 5 条核心原则与模块映射

| | 清晰明确 | 提供上下文 | 具体要求 | 给出示例 | 分步思考 |
|:--|:--:|:--:|:--:|:--:|:--:|
| `system.py` | ★ | ★ | ★ | | ★ |
| `templates.py` | ★ | ★ | ★ | | ★ |
| `examples.py` | | | | ★ | |
| `assembler.py` | ★ | ★ | | | ★ |

### 集成方式

M4 阶段 `PromptAssembler` 和 `PromptLogger` 已在 `service.py` 中实例化，暂未接入 `StateMachine`。M5 接入真实 LLM Provider 时生效，改动范围仅限 `providers/llm.py`（新文件）和 `service.py`（小改）。

---

## 四、子系统三：Agent 上下文工程

### 做了什么

将分散在 `models/event.py`、`prompts/assembler.py`、`context/builder.py` 三处的上下文组装逻辑收敛为统一的"上下文工程"子系统。五大要素——系统提示、工具定义、历史记录、检索上下文、用户输入——纳入 `ContextEngine` 统一生命周期管理。

### 核心模块（8 文件 + 2 集成文件）

| 模块 | 文件 | 职责 |
|:--|:--|:--|
| 数据容器 | `context/pack.py` | ContextPack + 五要素 Block + HistoryMessage + ToolDefinition + TokenBudget |
| 上下文引擎 | `context/engine.py` | ContextEngine.collect()：并发调用 6 个 Manager/Renderer，产出 ContextPack |
| 系统提示管理 | `context/system_prompt.py` | SystemPromptManager：L0-L4 五层分层渲染 + 版本快照 + 硬约束检查 |
| 工具定义渲染 | `context/tool_definitions.py` | ToolDefinitionsRenderer：按策略条件注入工具描述到 prompt |
| 历史管理 | `context/history_manager.py` | HistoryManager：三级压缩（全量→轻量→摘要）+ 优先级驱逐算法 |
| 检索管理 | `context/retrieval_manager.py` | RetrievalManager：YAML 规则加载 + 精确匹配 + MD5 去重 + 降级 |
| 用户输入构建 | `context/user_input.py` | UserInputBuilder：事件→完整 user message 文本 + 模板渲染 |
| 预算引擎 | `context/budget.py` | BudgetEngine：按策略为五要素分配 token 配额 |
| 集成接口 | `integrations/protocols.py`, `mocks.py` | 4 套跨主线 Protocol（Evidence/Track/SourceMeta/EventSequence）+ Mock 实现 |

### 统一管线

```
DetectionEvent + Strategy + ToolRouter
  → ContextEngine.collect()
    ├─ SystemPromptManager.render(strategy)     → 五层拼接
    ├─ ToolDefinitionsRenderer.render(router)   → 条件注入
    ├─ HistoryManager.get_window(event)         → 滑动窗口 + 压缩
    ├─ RetrievalManager.search(event, strategy)  → 规则匹配 + 去重
    ├─ UserInputBuilder.build(event, strategy)  → 完整 user message
    └─ 集成数据源注入（空则跳过）
  → ContextPack
  → BudgetEngine.allocate(pack)
  → PromptAssembler.assemble(pack)
  → PromptAssembly (messages 列表)
```

### 关键设计决策

| ID | 决策 |
|:--|:--|
| D1 | `prompts/system.py` 为系统提示词 canonical source，`templates.py` 删重复内容 |
| D2 | `BudgetEngine` 分配配额，`PromptAssembler` 执行截断 |
| D3 | `ReviewContext` 废弃，`ContextPack` 成为唯一容器（M4 保留兼容方法） |
| D4 | Provider 双轨过渡：M4 保留 `analyze(context)`，M5 切换 `analyze_from_messages(messages)` |
| D5 | few-shot 选择保留在 `PromptAssembler`，不进入 `ContextEngine` |
| D6 | `UserInputBuilder` 输出完整 user message 文本（含模板渲染） |
| D7 | `prompts/manager.py` 废弃，合并到 `SystemPromptManager` |

---

## 五、文件清单汇总

### 新增文件（32 个）

```
agent/src/ssv_agent/
├── models/
│   └── event.py                    # 事件领域模型
├── state_machine/
│   ├── states.py                   # 状态上下文
│   └── machine.py                  # LLM 状态机
├── providers/
│   ├── base.py                     # Provider 抽象
│   └── mock.py                     # Mock 实现
├── prompts/
│   ├── system.py                   # 系统提示词
│   ├── templates.py                # 策略模板
│   ├── examples.py                 # Few-shot 示例
│   ├── assembler.py                # 组装器
│   ├── logger.py                   # 调试日志
│   └── manager.py                  # 轻量管理
├── context/
│   ├── pack.py                     # ContextPack 容器
│   ├── engine.py                   # ContextEngine
│   ├── system_prompt.py            # 系统提示管理
│   ├── tool_definitions.py         # 工具定义渲染
│   ├── history_manager.py          # 历史管理
│   ├── retrieval_manager.py        # 检索管理
│   ├── user_input.py               # 用户输入构建
│   └── budget.py                   # 预算引擎
├── tools/
│   ├── base.py                     # 工具抽象（含自描述）
│   └── router.py                   # 工具路由
├── writer/
│   └── result_writer.py            # 结果回写
└── integrations/
    ├── protocols.py                # 跨主线接口
    └── mocks.py                    # Mock 实现

config/rules.yaml                    # 安全规则知识库

docs/specs/
├── 2026-06-10-T4-Agent状态机与复核链路-spec.md
├── 2026-06-10-T4-提示词管理模块-设计构想.md
└── 2026-06-10-T4-Agent上下文工程设计-spec.md

docs/plans/
├── 2026-06-10-T4-Agent状态机与复核链路实现-plan.md
├── 2026-06-10-T4-提示词管理模块-plan.md
└── 2026-06-10-T4-Agent上下文工程设计-plan.md
```

### 测试文件（10 个）

```
agent/tests/
├── test_models.py                  # 28 tests — 领域模型
├── test_state_machine.py           # 17 tests — 状态机 4 路径
├── test_providers.py               # 13 tests — Mock Provider
├── test_context_tools_writer.py    # 13 tests — 上下文/工具/回写
├── test_event_consumer.py          # 4 tests — Redis I/O
├── test_service.py                 # 4 tests — 端到端
├── test_prompts/
│   ├── test_assembler.py           # 组装器测试
│   ├── test_logger.py              # 日志测试
│   └── test_manager.py             # 管理测试
└── test_config.py                  # 3 tests — 配置加载
```

**总计：134 tests，ruff lint zero errors**

### 修改文件（6 个）

- `agent/src/ssv_agent/service.py` — 重构为 AgentService + 集成全链路
- `agent/src/ssv_agent/event_consumer.py` — 剥离 I/O
- `agent/src/ssv_agent/config.py` — 新增 Agent 配置项
- `agent/src/ssv_agent/cli.py` — 新增 --no-mock 标志
- `agent/src/ssv_agent/state_machine/machine.py` — 集成 ContextEngine
- `agent/src/ssv_agent/providers/mock.py` — 新增 analyze_from_messages() + ContextPack 兼容

---

## 六、当前不做的（非本阶段范围）

| 项目 | 说明 |
|:--|:--|
| 真实 LLM/VLM API 调用 | 全部 mock，M5 接入 |
| 向量数据库检索 | 当前 YAML 精确匹配，M5 语义检索 |
| 通知/工单外部系统 | 接口预留，M5 对接 |
| Agent 状态持久化 | 当前内存，M5 数据库 |
| VLM 提示词组装 | `assemble_vlm()` 方法预留，M5 实现 |
| 历史 Level 1-3 压缩 | 接口预留，M5 激活 |

---

## 七、验证命令

```bash
cd agent
uv run --extra dev pytest         # 134 passed
uv run --extra dev ruff check src/ # All checks passed
```

## 八、Git 提交记录

```
aa9c94d docs(t4): 补充提示词管理模块与上下文工程设计 plan
30094c1 feat(t4): 提示词管理模块
327dd76 feat(t4): Agent 状态机与复核链路实现
```
