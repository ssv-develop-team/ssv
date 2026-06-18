# T4 Agent 状态机与复核链路 Spec

## 目标

把 `agent/` 从 M0 基线（消费+打印+ACK）推进到可独立验证的复核编排能力：事件模型、轻量状态机、provider 抽象、上下文构造、工具路由、结果回写。本阶段复核链路默认使用 mock provider，也支持可选 `local_yolo` provider 对事件证据关键帧做异步复核；Agent 不进入每帧同步检测链路。

## 范围

本阶段包含：
- pydantic 事件领域模型（Detection / DetectionEvent / ReviewResult）
- 自研轻量状态机（7 个流转状态 + 3 个终态）
- 4 条复核策略路径（直接确认 / 视觉复核 / 规则解释 / 通知报告）
- 模型 Provider 抽象（BaseProvider / BaseVLMProvider / MockProvider / LocalYoloProvider）
- 上下文构造器（ContextEngine，ContextBuilder 仅保留兼容）
- 工具调用路由（ToolRouter，内置规则检索和通知草稿工具）
- 结果回写器（ResultWriter，Redis + 日志双通道，Stream key 来自 `AgentConfig.review_result_stream`）

本阶段不包含：
- 真实 LLM/VLM API 调用
- 向量数据库语义检索
- 外部通知/工单系统对接
- Agent 状态持久化

## 接口契约

### 输入（从 Redis Streams 消费）

字段对齐 `ssvpub` C++ 插件输出：

```json
{
  "type": "detection",
  "source": "pipeline-0",
  "timestamp_ms": 1700000000000,
  "frame_id": 42,
  "detections": [
    {
      "class": "helmet",
      "class_id": 0,
      "confidence": 0.95,
      "bbox": [0.1, 0.2, 0.5, 0.8],
      "track_id": 5
    }
  ]
}
```

### 输出（回写到 Redis Stream）

默认回写到 `AgentConfig.review_result_stream = "ssv:review-results"`，可在 `config/ssv.default.yaml` 或环境加载后的配置中覆盖。

```json
{
  "event_id": "pipeline-0-42",
  "source": "pipeline-0",
  "frame_id": 42,
  "final_state": "completed",
  "strategy": "direct_confirm",
  "conclusion": "直接确认: 检测到未佩戴安全帽，高置信度违规",
  "summary": "[high] 来源=pipeline-0 帧=42 策略=direct_confirm 检测={head(conf=0.95)}",
  "severity": "high",
  "detections_count": 1,
  "tool_results": [],
  "provider_used": "mock",
  "created_at": 1700000001.0
}
```

### 状态机流转

```
PENDING → PARSING → CONTEXT_BUILDING → STRATEGY_SELECTING
  → TOOL_CALLING → RESULT_AGGREGATING → RESULT_WRITING
  → COMPLETED / FAILED / NEEDS_HUMAN
```

- FAILED / NEEDS_HUMAN 提前退出，跳过结果回写
- COMPLETED 正常完成回写
- 非 `direct_confirm` 策略如果缺少可用 provider，状态机不得伪造模型结论，应记录失败工具结果并收敛到 `needs_human`

### Provider 协议

```python
class ProviderProtocol(Protocol):
    def analyze(self, context: ReviewContext) -> str: ...
```

StateMachine 通过 Protocol 依赖注入，不绑定具体实现类。当前使用 MockProvider 返回预定义中文结论文本。
当配置 `mock_provider=false` 且 `provider_type=local_yolo` 时，StateMachine 使用本地 YOLO provider 读取 `DetectionEvent.evidence_paths` 中的关键帧，并基于 `helmet/head` 检测结果生成复核结论。模型依赖 `ultralytics/torch`，缺失时按 provider 调用失败处理。

## 错误处理

| 场景 | 行为 |
|------|------|
| Redis 消费失败 | 打 warning 日志，continue 等待下次拉取 |
| JSON 解析失败 | 打 warning 日志，不 ACK（等下次重试） |
| Provider 调用抛异常 | 记录失败 ToolResult，不中断状态机 |
| Provider 未配置 | 非直接确认策略进入待人工复核，不伪造模型结论 |
| 关键帧证据缺失 | 本地 YOLO provider 返回失败，事件进入待人工复核 |
| 全部工具调用失败 | 汇聚阶段判定 → NEEDS_HUMAN |
| 状态机超时 | NEEDS_HUMAN（超时阈值来自 AgentConfig.state_machine_timeout，默认300s） |
| 未捕获异常 | execute() 顶层 try/except → FAILED |
| 结果回写失败 | 打 error 日志，不影响 ACK |

## 验证方式

```bash
cd agent
uv run --extra dev pytest    # 模型校验、状态机4路径、provider mock、工具路由、端到端
uv run --extra dev ruff check src/  # 代码风格检查
```

## 依赖

- pydantic >= 2.0（已有）
- redis >= 5.0（已有）
- structlog >= 24.0（已有）
- 本地 YOLO provider 需要可选依赖 `ultralytics`（通过 `agent` 的 `vision` extra 安装）
