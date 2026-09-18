# Agent 可读性重构规格

## 需求

整理 Agent 内部实现，使运行时配置、Worker 调度和 DeerFlow review 配置各自拥有清晰的模块接口，降低重复实现和跨文件追踪成本。

## 约束

- Redis ingress、SQLite `EventLedger`、durable job 的 lease/fence、post-commit ACK 和 Qdrant 可重建 projection 的行为保持不变。
- 相对路径继续相对于 Agent 项目目录解析；配置文件、CLI 参数和环境变量的优先级保持不变。
- review 继续使用临时 DeerFlow 配置、fail-closed RBAC 和 canonical builtin `view_image` 校验。
- 录像取证 Worker 继续使用当前默认值：poll 1 秒、lease 30 秒、最多 3 次重试、重试延迟 2 秒。
- Worker 的 `run_once()` 仍是单次处理接口；停止事件仍阻止领取新任务，异常仍通过轮询循环恢复。
- `config/ssv.yaml` 同时经过 C++ runner 校验；新增的录像 Worker 字段必须在 C++ 与 Python
  两侧接受相同的字段名、默认值和范围。
- 依赖声明只在 Agent 存在直接 import 且运行时需要时保留，不因为 DeerFlow 的传递依赖而删除直接依赖。

## 目标结构

- `runtime.py`：提供 Agent 根目录、配置发现、路径解析、embedding/Qdrant 目标和运行环境值的共同实现。
- `review_runtime.py`：提供 review 临时配置生成和 builtin 校验，隐藏 DeerFlow 配置细节。
- `workers.py`：用一个内部轮询模块承载通用调度骨架，各 Worker 聚焦自己的账本处理流程。
- `service.py`：只负责 Agent 生命周期、资源所有权和 Worker 启动编排。

## 非本阶段范围

- 不合并 Python Agent、C++ runner 配置解析器和根目录 CLI 配置解析器。
- 不迁移或删除 SQLite、Qdrant、结果文件、录像证据或 DeerFlow 配置文件。
- 不升级、替换或删除 Agent 的运行时依赖。
- 不修改 DeerFlow 第三方源码。
