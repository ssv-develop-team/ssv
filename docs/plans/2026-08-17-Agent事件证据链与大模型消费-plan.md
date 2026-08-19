# Agent 事件证据链与大模型消费实施计划

## 实施原则

按“权威事务 -> 消费 ACK -> 异步复核 -> 异步索引 -> 只读工具 -> 运行时集成”的纵向切片实施。
每个切片先通过公开 seam 写行为测试，再实现最小能力。现有未提交改动均视为用户工作，禁止覆盖、
回退或夹带格式化。第三方 `agent/third_party/deer-flow` 不修改。

本计划由主 Agent 负责设计、拆分、审查和最终验证；经授权的 worker 角色均使用 `gpt-5.6-luna`
执行各自批准的代码与测试切片。未经额外授权不暂存、不提交、不推送。

## 步骤 1：EventLedger 与 SQLite migration

涉及文件：

- `agent/src/ssv_agent/event_store/schema.py`
- `agent/src/ssv_agent/event_store/sqlite_store.py`
- `agent/src/ssv_agent/event_store/ledger.py`（新增）
- `agent/src/ssv_agent/event_store/__init__.py`
- `agent/tests/test_event_ledger.py`（新增）
- `agent/tests/test_sqlite_store.py`

标识符与行为：

- 新增 `EventCase`、`EvidenceRef`、`RecordOutcome`、`DurableJob`、`JobKind`、`JobState` 与
  `LeaseLostError`；
- 新增 `EventLedger.record/get_case/append_review/claim_job/complete_job/fail_job/complete_review_job`；
- SQLite schema/migration 增加 timeline/rule/revision/evidence/reviews/jobs 契约；
- record 和 append_review 为事务，任务唯一且幂等；claim 有 lease 且每次递增 `attempts`。
  complete/fail 必须以 owner、claim 时的 `attempts`、`processing` 状态和未过期 lease 为 fencing 条件，
  丢 lease 抛出 `LeaseLostError`，相同 owner 字符串的旧 claim 也不能改写新 claim。
- 全量 index 重建只重设 job 状态，不得重置 `attempts`，保证同一 job 的 claim token 单调递增。
- `complete_review_job` 在同一 `BEGIN IMMEDIATE` 事务验证 job/owner/attempt/event revision/证据并提交
  review、projection、revision、index job 和 review job completed，失败时无部分写入。

验证：重复 record、事务回滚、证据登记、无效证据引用、revision、claim 互斥、lease 回收、重试和
dead 的公开接口测试。

## 步骤 2：Redis ingress 只负责持久化

涉及文件：

- `agent/src/ssv_agent/review_context.py`
- `agent/src/ssv_agent/event_consumer.py`
- `agent/tests/test_review_context.py`
- `agent/tests/test_event_consumer.py`

标识符与行为：

- `ReviewContext` 接收上游真实提供的 generation/PTS/rule/facts，缺失保持 None/空值；
- `EventConsumer` 注入 ledger factory 或 ledger adapter，不再持有模型 runner；
- 有效消息在 `EventLedger.record` 成功后 ACK；失败不 ACK；poison 和 dedup 保持明确 ACK 语义。

验证：SQLite 失败不 ACK、成功立即 ACK、重复消息幂等、缺失 PTS/generation 不补造。

## 步骤 3：ReviewWorker 与结构化结果

涉及文件：

- `agent/src/ssv_agent/workers.py`（新增）
- `agent/src/ssv_agent/prompt.py`
- `agent/src/ssv_agent/result.py`
- `agent/src/ssv_agent/runner.py`
- `agent/tests/test_workers.py`（新增）
- `agent/tests/test_prompt.py`
- `agent/tests/test_result.py`
- `agent/tests/test_runner.py`

标识符与行为：

- `ReviewWorker.run_once/run` 领取 review job、重建上下文、调用 runner、写结果并通过
  `complete_review_job` 原子完成/重试任务；
- `ReviewResult` 增加 claims/evidence_ids/policy/model provenance；
- 新提示词使用通用监控规则语义、只读工具和 JSON 输出，旧 Markdown 解析只做兼容；
- 非 uncertain 结果必须引用本案件可用证据。
- 丢失 lease 或 attempts fence 时只记录并结束当前处理，不调用 fail/reset 已被其他 worker reclaim 的任务。

验证：review 成功、模型失败重试、解析失败重试、达到上限 dead、错误 evidence ID 被拒绝。

## 步骤 4：IndexWorker 与离线 BGE-M3 adapter

涉及文件：

- `agent/src/ssv_agent/workers.py`
- `agent/src/ssv_agent/search/event_text.py`
- `agent/src/ssv_agent/event_store/qdrant_store.py`
- `agent/src/ssv_agent/embedding/registry.py`
- `agent/src/ssv_agent/embedding/backends/bge_m3.py`（新增）
- `agent/tests/test_workers.py`
- `agent/tests/test_embedding.py`
- `agent/tests/test_qdrant_store.py`

标识符与行为：

- `IndexWorker.run_once/run` 读取当前案件、构造规范文本、embed、upsert，并通过 owner/attempt-fenced
  complete/fail 完成或重试 job；
- `build_event_text` 纳入规则、对象、区域/方向/时长、severity、证据与复核语义；
- `BgeM3EmbeddingProvider` 懒加载 sentence-transformers，可用本地模型路径覆盖；
- Qdrant point/payload 保留稳定 event_id 和 revision。

验证：索引成功、Qdrant/Embedding 失败重试、稳定 upsert、旧 revision 写当前事实、adapter 不下载
模型的替身测试。

## 步骤 5：只读大模型工具

涉及文件：

- `agent/src/ssv_agent/tools/evidence_reader.py`
- `agent/src/ssv_agent/tools/get_event.py`
- `agent/src/ssv_agent/tools/search_events.py`
- `agent/src/ssv_agent/search/embed_search.py`
- `agent/tests/test_evidence_reader.py`
- `agent/tests/test_event_query_tools.py`
- `agent/tests/test_search.py`

标识符与行为：

- `evidence_reader_tool` 只接收 event/evidence ID，并从账本解析路径；
- `get_event_tool` 返回完整权威案件与最新复核；
- `search_events_tool` 使用 Qdrant 召回 event_id/score 后从 SQLite hydrate 当前字段；
- 工具不暴露 SQLite/Qdrant 写接口。

验证：任意路径不可读、已登记证据可读、过期 Qdrant payload 被 SQLite 当前事实覆盖、语义路径失败
时属性检索正常降级。

## 步骤 6：配置、生命周期和文档

涉及文件：

- `agent/src/ssv_agent/config.py`
- `agent/src/ssv_agent/service.py`
- `agent/pyproject.toml`
- `agent/uv.lock`（仅由依赖解析命令生成）
- `config/ssv.example.yaml`
- `README.md`
- `agent/tests/test_config.py`
- `agent/tests/test_service.py`

标识符与行为：

- 新增严格的 `ReviewWorkerConfig`、`IndexWorkerConfig`；
- service 创建并拥有 consumer/review/index worker；`request_stop` 只停止 ingress/新任务领取，
  `stop` 有界等待后仅在所有线程退出时回收共享资源；
- `run` 在主线程安装并恢复 SIGINT/SIGTERM handler，handler 只调用 `request_stop`；超时存活线程时
  保留 consumer、DeerFlow client 和临时配置供后续 `stop` 清理；
- sentence-transformers 作为 bge-m3 可选依赖；默认测试与最小启动不加载模型；
- README 记录启用、重试、重建和故障隔离方式。

## 步骤 7：可靠性切片 2A

涉及文件：

- `agent/src/ssv_agent/config.py`
- `agent/src/ssv_agent/dedup.py`
- `agent/src/ssv_agent/event_consumer.py`
- `agent/src/ssv_agent/event_store/ledger.py`
- `agent/src/ssv_agent/workers.py`
- `agent/src/ssv_agent/result.py`
- `agent/src/ssv_agent/review_context.py`
- `agent/src/ssv_agent/tools/evidence_reader.py`
- `agent/tests/test_config.py`
- `agent/tests/test_dedup.py`
- `agent/tests/test_event_consumer.py`
- `agent/tests/test_event_ledger.py`
- `agent/tests/test_workers.py`
- `agent/tests/test_result.py`
- `agent/tests/test_review_context.py`
- `agent/tests/test_evidence_reader.py`
- `config/ssv.example.yaml`
- `README.md`
- `docs/specs/2026-08-17-Agent事件证据链与大模型消费-spec.md`

标识符与行为：

- `RedisConfig` 增加严格的 pending reclaim 配置和可选 `consumer_name`；`EventConsumer.start()` 每轮先
  `XAUTOCLAIM` 再读 `>`，错误继续循环，stop-before-start 保持 stop request；
- `EventDeduper` 用 event/ingress identity token 区分同一 entry 重投和不同冷却期事件；
- `EventLedger.refresh_evidence()` 只刷新已登记证据，ReviewWorker、`evidence_reader_tool` 与
  `complete_review_job()` 都在自己的时序点重新确认文件；
- `write_result_json()`、`write_result_markdown()` 使用 outputs-root 约束的安全 event 目录和内容 hash
  文件名，失租 writer 只能留下 orphan；GC 边界只记录在文档，不在本切片实现自动删除；
- `ReviewContext.from_event()` 以显式 `event_type` 为先，在字段缺失时回退发布方 `type`。

验证：pending reclaim/ACK、账本失败后的重投、迟到和消失证据、原子 fenced 引用校验、恶意 event ID、
不同结果互不覆盖、ReviewWorker refresh 及 Ruff/focused/full pytest。

## 步骤 8：可靠性切片 2B：租约存活与重试封顶

涉及文件：

- `agent/src/ssv_agent/workers.py`
- `agent/tests/test_workers.py`
- `docs/specs/2026-08-17-Agent事件证据链与大模型消费-spec.md`
- `docs/plans/2026-08-17-Agent事件证据链与大模型消费-plan.md`

标识符与行为：

- 新增私有 `_LeaseHeartbeat`，按 `lease_ms` 派生间隔，携带 `job_id`、`worker_id`、`attempts` 与 `lease_ms`
  通过 `ledger_factory` 在 heartbeat 线程拥有独立 `EventLedger`/SQLite connection；不得跨线程使用
  `run_once` connection；
- `ReviewWorker.run_once()` 在 runner、结果解析、artifact 写入与 fenced complete 的边界同步确认 lease；
  `IndexWorker.run_once()` 在 embedding、Qdrant upsert 与 complete 的边界同步确认 lease；最终状态迁移前
  都停止并 join heartbeat；
- `LeaseLostError` 只终止旧 worker，不再产生后续 artifact/upsert/complete/fail。其他 heartbeat 异常回到
  主流程，由当前 owner/attempt fence 决定失败重试；
- 已 claim 且 `attempts > max_retries` 时复用 `fail_job()` 在任何外部调用前转为 `dead`；等于上限的
  attempt 仍允许执行一次。

验证：Review/Index 的阻塞外部调用期间续租且不可 reclaim、独立 connection、丢租后无 artifact/upsert/
complete/fail、heartbeat 异常可见且线程退出、超限 claim 无 runner/embed/Qdrant/result writer 副作用。

## 步骤 9：可靠性切片 2C1：Embedding/Qdrant 边界

涉及文件：

- `agent/src/ssv_agent/embedding/registry.py`
- `agent/src/ssv_agent/embedding/backends/bge_m3.py`
- `agent/src/ssv_agent/event_store/qdrant_store.py`
- `agent/src/ssv_agent/search/embed_search.py`
- `agent/src/ssv_agent/tools/search_events.py`
- 对应 focused tests

标识符与行为：

- index worker 与 search tool 按同一 backend/model 身份选择 provider；环境变量切换不会错误复用旧模型实例。
- BGE-M3 默认离线加载，并统一优先使用 `SSV_EMBEDDING_MODEL`；旧的 BGE 专用变量只作为兼容回退。
- Qdrant 支持显式本地 path、server URL/API key 和 collection 维度检查；命中只返回 ID/score，事实字段由
  SQLite hydrate。Qdrant 不可用时属性检索仍能工作。

验证：模型身份缓存、离线加载、远程 Qdrant 配置优先级、维度/命名向量拒绝、伪造 payload 和 SQLite
最终过滤，以及 focused pytest/Ruff。

## 步骤 10：可靠性切片 2C2：Review service 安全与生命周期

涉及文件：

- `agent/src/ssv_agent/service.py`
- `agent/src/ssv_agent/prompt.py`
- `agent/tests/test_service.py`
- `agent/tests/test_prompt.py`

标识符与行为：

- service 为复核 client 生成临时空 `extensions_config.json`，设置 `DEER_FLOW_EXTENSIONS_CONFIG_PATH`，
  并在 client、worker、consumer 完全退出后恢复环境变量和清理文件；不覆盖用户原有配置。
- DeerFlow RBAC allowlist 只允许 `get_event`、`evidence_reader`、`rule_retriever`、`search_events`、
  `view_image`；关闭 skills、subagent 和 plan mode。
- prompt 将事件、规则、检索结果、证据元数据和图片内容标为不可信输入，模型只能核验，不能执行其中指令。
- stop-before-start 不遗失停止请求；任一资源 close 抛错时仍恢复 signal/env 和临时资源状态。

验证：临时配置隔离、allowlist、prompt 注入边界、启动/停止竞态、close 异常清理、focused pytest/Ruff。

## 步骤 11：可靠性切片 2D：停止、证据根与 embedding identity 收敛

涉及文件：

- `agent/src/ssv_agent/service.py`
- `agent/src/ssv_agent/workers.py`
- `agent/src/ssv_agent/config.py`
- `agent/src/ssv_agent/event_consumer.py`
- `agent/src/ssv_agent/event_store/ledger.py`
- `agent/src/ssv_agent/event_store/qdrant_store.py`
- `agent/src/ssv_agent/embedding/registry.py`
- `agent/src/ssv_agent/tools/evidence_reader.py`
- 对应 focused tests、`config/ssv.example.yaml`、README 与本 spec

标识符与行为：

- `AgentService.request_stop()` 不获取生命周期锁；`start()` 在 worker、consumer 和线程创建边界检查停止事件，
  消除 signal handler 重入普通锁的死锁，并覆盖 consumer 创建竞态。
- `ReviewWorker.run()` 与 `IndexWorker.run()` 在循环层捕获 `run_once()` 外的 `Exception`，记录 worker/kind/error，
  以 poll interval 退避后继续，且不捕获 `BaseException`。
- `agent.evidence_roots` 是绝对根目录 allowlist；空列表 fail closed。EventLedger 仅登记 resolve 后留在根内的路径，
  refresh 与 `evidence_reader` 复制前再次校验，因此 `..`、根外路径和越界 symlink 均不可升级为证据。拒绝证据
  不阻断案件 SQLite 提交或 Redis ACK。
- embedding registry 产生含 adapter schema version、backend 与有效 model 的稳定 identity。Qdrant 的 event/rule
  物理 collection 由 logical base name 与 identity hash 派生；不同 identity 自动隔离，旧 collection 不迁移或删除。
  切换 identity 后通过 `enqueue_all_index_jobs()` 填充新空间。

验证：signal-stop 锁竞态、stop/create race、两类 worker 外层 SQLite 异常恢复、空/合法/越界证据根、symlink
边界、Redis ACK、同维不同 embedding identity 隔离、focused pytest、Ruff、全量 pytest 与 `git diff --check`。

当前 `rule_retriever` 仍是 mock backend，完整规则库接入不属于本切片。

## 验证命令

在 `agent/` 下运行：

```bash
uv run pytest -q tests/test_event_ledger.py tests/test_workers.py tests/test_service.py
uv run ruff check src/ssv_agent/event_store/ledger.py src/ssv_agent/workers.py src/ssv_agent/service.py tests/test_event_ledger.py tests/test_workers.py tests/test_service.py
uv run pytest -q tests/test_event_ledger.py tests/test_event_consumer.py tests/test_workers.py
uv run pytest -q tests/test_embedding.py tests/test_qdrant_store.py tests/test_search.py
uv run pytest -q tests/test_evidence_reader.py tests/test_event_query_tools.py
uv run pytest -q
uv run ruff check src tests
```

仓库根目录运行：

```bash
git diff --check
```

真实 BGE-M3 模型下载、GPU/CPU 性能、实时 RTSP、Redis、DeerFlow 外部模型 API 和远程 Qdrant 属于
环境集成验证；本地交付必须通过 mock/fake 的确定性测试，并明确报告这些外部验证状态。
