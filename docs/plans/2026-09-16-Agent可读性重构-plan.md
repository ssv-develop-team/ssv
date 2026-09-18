# Agent 可读性重构计划

## 步骤 1：收口运行时配置和 review 配置

- 新增 `ssv_agent.runtime`，统一 Agent 根路径、配置发现、相对路径解析、embedding 环境值和 Qdrant 目标选择。
- 新增 `ssv_agent.review_runtime`，从 `AgentService` 移出 DeerFlow review 临时配置生成和 builtin 校验。
- 为共同 helper 增加纯行为测试，并保留现有私有测试入口的兼容别名。

## 步骤 2：整理 Worker 调度和录像配置

- 新增内部 `_PollingWorker`，统一 `run()` 的停止、空闲等待和异常恢复骨架。
- 让 `RecordingEvidenceConfig` 继承 `WorkerConfig`，把原先的硬编码运行参数纳入主 YAML；录像默认值保持不变。
- 同步 C++ shared-config validator 和测试，避免同一份 YAML 被 C++ runner 拒绝。
- 更新服务启动传参、配置测试和 Agent 配置文档。

## 验证

- 先运行 runtime、config、knowledge ingest、service、worker 的 focused tests。
- 再运行 Agent 全量 `pytest`、`ruff check src tests`、`uv lock --check` 和仓库根测试。
- 用 `git diff --check` 检查改动；不处理 `third_party/deer-flow` 的既有第三方 Ruff 问题。
