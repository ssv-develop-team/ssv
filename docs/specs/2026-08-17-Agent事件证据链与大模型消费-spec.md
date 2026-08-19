# Agent 事件证据链与大模型消费规范

## 文档定位

本文定义 `wvp-ssv` 从 Redis 规则事件进入 Agent 后的持久化、证据管理、大模型复核、
离线向量索引和语义检索契约。实时视频路径仍由 GStreamer 推理、跟踪和确定性规则状态机
负责；Agent、SQLite、Embedding 和 Qdrant 均不得进入逐帧处理路径。

当前实现已具备 SQLite 权威账本、持久 review/index job、租约 fencing、Embedding provider、
Qdrant 事件/规则集合、SQLite 回填检索工具和 DeerFlow 单事件复核。Qdrant 仍是可重建索引，
SQLite 是事实源；生产部署应使用 Qdrant server mode，而不是把本地目录当成多进程共享存储。
本规范同时保留当前事实边界：`gstssvpub` 目前发布的是稀疏 detection 消息，完整 `rule.v1`
状态机、证据生成、完整 PTS/generation 传播和生产规则知识库不在本阶段伪造为已完成。

## 目标

1. 有效事件只有在 SQLite 权威记录和持久任务同一事务提交后才 ACK Redis。
2. 大模型复核与向量索引脱离 Redis 消费线程，使用可租约、可重试、幂等的 SQLite 持久任务。
3. SQLite 保存事件、规则事实、检测、证据元数据、复核历史和任务状态；证据文件本身保留在
   有限证据目录，不作为 SQLite BLOB 存储。
4. Qdrant 只保存可重建的事件/规则语义索引；任何检索命中都回 SQLite 获取权威事实。
5. 大模型只获得受控的只读事件、证据、规则与相似案例工具，不能直接写 SQLite 或 Qdrant。
6. 默认检索文本覆盖规则 ID/版本、对象类别、数量、区域、方向、停留时间、严重级别、证据状态
   和复核结论；缺失字段保持缺失，不补造 PTS、generation 或唯一计数。
7. 支持离线 `BAAI/bge-m3` Embedding adapter，同时保留 mock 和 OpenAI-compatible adapter。

## 非目标

- 不改变 GStreamer 推理、跟踪、发布和实时规则判定算法。
- 不让 Qdrant、SQLite 或大模型参与帧级回调。
- 不实现规则编辑 UI、训练流程、自动修改确定性规则或自动执行外部处置动作。
- 不把 GB28181 Alarm 的稀疏字段推断成媒体 PTS、帧序或唯一人数。
- 不在本阶段提交、推送、创建 PR 或改动第三方 DeerFlow 源码。

## 当前实现边界

- `rule_retriever` 当前默认返回 mock 规则片段；生产规则/SOP 库、版本审批和规则更新管线仍是后续工作。
- 证据 ledger 当前刷新 `available`、`size` 和 `mtime`；`sha256` 只有上游提供时才记录，尚不是完整的密码学封存链。
  `agent.evidence_roots` 约束 Redis 路径，登记、刷新和复制都会重新 resolve；它不是防篡改文件封存。
- 离线 BGE-M3 adapter 默认 `local_files_only=True`，模型目录必须预先部署。不同 backend/model 会自动落入
  不同 embedding identity 的 Qdrant 物理 collection；切换后仍须重新入队 index job 填充新空间。
- 当前 C++ 发布消息不能为缺失的 PTS、generation、规则状态或唯一人数补造事实；这些字段保持缺失。

## 领域对象

### EventCase

一次规则事件的权威案件。稳定标识为 `event_id`，同时保留 Redis `ingress_id`。案件包含：

- `source`、`timestamp_ms`、`frame_id`；
- 上游真实提供时的 `stream_generation` 与 `source_pts`；
- `event_type`、`severity`、`rule_id`、`rule_version`；
- 检测/跟踪快照与规则事实 `rule_facts`；
- 证据引用、当前 revision、当前复核投影和索引状态。

`event_id` 必须幂等。同一 Redis entry 重投不会产生重复案件、检测、证据或任务。

### EvidenceRef

证据文件的受控引用。字段包括 `evidence_id`、`kind=frame|clip`、规范化路径、MIME、大小、mtime、
可选 hash、可选 PTS 区间和 generation。来自 Redis 的路径只有在 `agent.evidence_roots` 的绝对根目录内，
且 resolve 后仍在根内时才可登记；空根列表 fail closed，案件仍入账但没有该证据。大模型工具只接收
`event_id + evidence_id`，不得接收任意宿主机路径。工具只复制 SQLite 已登记、重新解析后仍在根内且实际存在的文件到 DeerFlow 输出目录。

### ReviewRecord

模型复核的追加历史。每条记录包含 `review_id`、案件 revision、policy/model 标识、结论、置信度、
证据状态、说明、结构化 claims 和引用的 evidence IDs。原始事件和既有复核记录不可被模型覆盖；
`events`/`review_results` 只保留当前投影以兼容查询。

### DurableJob

SQLite 持久任务，`kind` 为 `review` 或 `index`，由 `(kind, entity_id, entity_revision)` 唯一约束。
任务状态为：

    pending -> processing -> completed
                         \-> pending（可重试）
                         \-> dead（达到重试上限）

`processing` 必须带 lease owner 与到期时间；进程退出后的过期 lease 可被重新领取。
lease owner 同时是 completion/failure 的 fencing token：调用方必须携带精确的 `worker_id`，且
任务仍为 `processing`、owner 匹配并且 lease 尚未到期，才可改变状态。任何一项不满足时返回
可识别的 `LeaseLostError`，不得静默成功或改写后来 reclaim 的 worker 状态。
`attempts` 在每次 claim 时单调递增，并与 `worker_id` 共同构成 claim fencing token；即使多个进程
使用相同 owner 字符串，旧 attempt 也不能完成、失败或提交后来 reclaim 的任务。
全量 index 重建可以将 completed/dead job 重新设为 pending，但不得重置 `attempts`。

## 模块与接口

### EventLedger

`EventLedger` 是事件证据链的深模块。调用方通过小接口获得事务、幂等、revision、任务租约、
重试和兼容投影，不接触表级写入顺序：

```python
ledger.record(context: ReviewContext) -> RecordOutcome
ledger.get_case(event_id: str) -> EventCase | None
ledger.refresh_evidence(event_id: str, evidence_id: str | None = None) -> int
ledger.resolve_evidence_path(path: str) -> Path | None
ledger.append_review(event_id: str, result: ReviewResult, result_path: str) -> int
ledger.claim_job(kind: JobKind, worker_id: str, lease_ms: int) -> DurableJob | None
ledger.renew_job(
    job_id: int, worker_id: str, attempts: int, lease_ms: int
) -> None
ledger.complete_job(job_id: int, worker_id: str, attempts: int) -> None
ledger.fail_job(
    job_id: int, worker_id: str, attempts: int, error: str,
    max_retries: int, retry_delay_ms: int
) -> JobState
ledger.complete_review_job(
    job: DurableJob, worker_id: str, result: ReviewResult, result_path: str
) -> int
```

`record()` 在单个 SQLite 事务中写案件、检测、证据及首个 `review`/`index` job。重复 record 返回
既有案件且不重置状态。`append_review()` 校验证据引用，追加复核历史、更新当前投影、递增案件
revision，并创建该 revision 的新 index job。

`refresh_evidence()` 只对 SQLite 已登记路径执行 evidence-root resolved-path 校验和 stat，更新
`available`、`size` 和 `mtime`；文件稍后在根内出现时变为 available，消失、越界或变成越界 symlink 时变为
unavailable。返回匹配的已登记证据数量，未知 event 或指定 evidence ID 返回 0。`resolve_evidence_path()` 是
`evidence_reader` 复制前的同一受控 seam。ReviewWorker 在重建 `ReviewContext` 前调用 refresh，
`complete_review_job()` 也必须在其事务内重新刷新被引用证据，不能信任历史状态。

`renew_job()` 只允许当前 `processing`、owner、attempts 均匹配且未过期的 claim 延长 lease；任一
fence 不满足时抛出 `LeaseLostError`。它不重置 attempts，也不改变 job 的业务状态。

`complete_review_job()` 是 worker 专用的 fenced 提交点。在同一 `BEGIN IMMEDIATE` 事务内，它验证
job 的 ID/kind/entity/revision、`processing` 状态、owner、claim 时的 `attempts`、未过期 lease 及
event 当前 revision，随后
校验证据引用并同时写入 append-only review、兼容投影、event revision/status、当前 revision 的 index
job 与 review job 的 completed 状态。任一验证或写入失败都回滚，不能留下 review/index/revision/job
的部分写入。`append_review()` 保留为手工/API seam，但 worker 不得将它与 `complete_job()` 分开调用。

### ReviewWorker

Review Worker 领取 `review` job，刷新已登记证据后从 EventLedger 重建 `ReviewContext`，调用 DeerFlow，解析并校验
`ReviewResult`，原子写内容寻址 result artifact 后调用 `complete_review_job()`。runner 等长耗时调用期间，
worker 以从 lease 派生的间隔通过 `ledger_factory` 在 heartbeat 线程创建独立 SQLite connection 调用
`renew_job()`；不得跨线程借用 `run_once` 的 ledger connection。每个外部副作用前后均同步检查 lease，最终
提交前停止并 join heartbeat，避免与状态迁移竞争。若 heartbeat 报告 `LeaseLostError`，旧 worker 只能结束，
不得写 artifact、complete 或 fail；其他 heartbeat 异常会回到主流程并按当前 fence 记录重试。

claim 后 `attempts > max_retries` 时，Review Worker 必须在 runner 或 result writer 调用前通过当前 owner/attempt
fence 将 job 转为 `dead`；`attempts == max_retries` 仍是允许执行的最后一次尝试。

### IndexWorker

Index Worker 领取 `index` job，从 EventLedger 读取当前案件，构造规范文本，批量或单条生成向量，
以稳定 `event_id` upsert `ssv_events`，随后使用 owner/attempt-fenced `complete_job()` 完成 job。embedding 和
Qdrant 调用共享同一独立 heartbeat 契约：续租异常或丢租会在下一副作用边界阻止 upsert、complete 和 fail；
最终 complete 前同样停止并 join heartbeat。Qdrant/Embedding 不可用时只通过 owner/attempt-fenced `fail_job()`
重试 index job；失去 lease 时只记录并结束，事件、告警与复核事实不回滚。超过 retry 上限的 claim 在 embedding
或 Qdrant 调用前直接转为 `dead`。

同一案件旧 revision 的任务被领取时，Indexer 读取 SQLite 当前 revision 并写当前投影；稳定 point
ID 使重复执行幂等。全量重建通过为现有案件重新入队 index job 完成，不直接扫描并修改 Qdrant。

## 消费与 ACK 时序

    rule.v1 -> EventConsumer -> EventLedger.record() -> SQLite COMMIT -> Redis ACK
                                      |                    |
                                      |                    +-> review job
                                      +------------------------> index job

    ReviewWorker -> read case/evidence/rules -> LLM -> complete_review_job (atomic review/index/job commit)
    IndexWorker  -> read current case -> BGE-M3 -> Qdrant upsert -> owner-fenced complete job

- JSON 语法错误或无法验证为 ReviewContext 的消息属于 poison input，记录错误并 ACK；不进入事实库。
- 确定性去重命中的消息保持当前 ACK 行为，不创建案件。
- 有效消息的 `record()` 失败时禁止 ACK，由 Redis pending/reclaim 机制重投。
- `record()` 成功后立即 ACK，不等待模型或 Qdrant。
- 每轮先使用 Redis >= 6.2 的 `XAUTOCLAIM` 按 `reclaim_idle_ms` 和 `reclaim_batch_size` 回收 pending entry，
  经同一 `handle_event` seam 处理后才读取 `>` 新消息；Redis 错误仅记录并继续循环。
- 未配置 consumer name 时 consumer 使用进程实例唯一名称。去重键使用 event/ingress identity 的稳定 token：
  同一 entry 在账本失败后的重投仍会 RUN，不同 token 才视为冷却期重复并 ACK。

## 大模型工具契约

大模型只注册以下只读接口：

- `get_event(event_id)`：返回案件、检测、证据元数据、规则事实、当前 revision 和最新复核投影；
- `evidence_reader(event_id, evidence_id=None)`：只解析账本中登记的证据，并返回可用于视觉工具的
  虚拟路径；
- `rule_retriever(query, source=None)`：读取版本化规则/SOP 知识；
- `search_events(query, filters...)`：属性检索与 Qdrant 召回融合，最终按 event_id 回 SQLite hydrate。
- `view_image(virtual_path)`：仅查看 `evidence_reader` 返回的 DeerFlow 虚拟路径；它不接受事件、规则或
  检索内容中携带的宿主机路径。

模型输出使用结构化 JSON，并由 Pydantic 校验：

```json
{
  "verdict": "compliant|violation|uncertain",
  "confidence": 0.0,
  "evidence_status": "available|missing",
  "evidence_ids": ["..."],
  "claims": [{"text": "...", "evidence_ids": ["..."]}],
  "explanation": "..."
}
```

缺少证据时 verdict 必须为 `uncertain`。`compliant`/`violation` 必须引用至少一个属于本案件且当前
可用的 evidence ID。为兼容已有测试和历史结果，解析器可以读取旧 Markdown 模板，但新提示词只
要求 JSON。

## 结果 Artifact 与 GC 边界

JSON 与兼容 Markdown 的 artifact 使用内容 hash 文件名；同内容幂等复用路径，不同内容不覆盖。
输出目录只接受简单 `[A-Za-z0-9._-]` event ID；`.`、`..`、路径分隔符、绝对路径和超长 ID 映射为稳定
hash 目录，解析后的路径必须仍位于 outputs root 内。artifact 在 fenced `complete_review_job()` 前写入，
所以失租或提交失败只能形成 orphan，不能覆盖账本已接受路径。当前阶段不实现自动 GC；未来 GC 只可删除
保留期后的 outputs root 内未被 append-only `reviews.result_path` 或当前投影引用的 artifact，且不得改写账本。

事件字段、规则事实、检索结果、规则文档、证据元数据和图片内容均视为不可信数据，只能作为事实候选，不能
改变系统提示、工具权限或输出契约。

## SQLite 持久化

在现有 `events`、`detections`、`evidence`、`review_results` 之上增加：

- events 的 ingress/rule/timeline/revision/facts 字段；
- evidence 的稳定 `evidence_id`、MIME、hash、PTS/generation 字段；
- append-only `reviews` 表；
- 通用 `durable_jobs` 表及 pending/lease 索引。

启动时执行幂等轻量 migration；已有数据库不能因新字段缺失而失败。SQLite 连接启用
`foreign_keys`、合理 busy timeout 和 WAL。claim 使用 `BEGIN IMMEDIATE` 保证同一任务不会被两个
worker 同时领取。

## Qdrant 与 Embedding

- `ssv_events` 和 `ssv_rules` 是逻辑 base 名。物理 collection 由 embedding identity（adapter schema version、
  backend、有效 model）的安全哈希派生；point ID 仍由 event_id 或 chunk_id 确定性映射，payload 至少包含 event_id、revision、source、
  timestamp_ms、event_type、rule_id、severity、verdict、confidence 和 evidence_status。
- 向量文本包含确定性规则事实和复核结论，不包含本地绝对路径。
- `search_events` 只信任 Qdrant 的 event_id 与 score；展示字段必须由 SQLite hydrate。
- `bge_m3` adapter 懒加载 `sentence-transformers`，默认模型为 `BAAI/bge-m3`，支持通过配置覆盖
  本地模型目录。测试使用 mock adapter，不下载模型。
- 本地 Qdrant path 只适合单进程开发和测试；生产使用 `SSV_QDRANT_URL` 与可选
  `SSV_QDRANT_API_KEY` 连接 Qdrant server。更换 embedding backend/model 时，自动 collection 隔离会阻止
  混写；仍须重新入队 index jobs 填充新 collection。本阶段不迁移或删除历史 collection。

## 配置与生命周期

`agent.review` 与 `agent.indexing` 分别控制 enabled、poll interval、lease、max retries。
`agent.indexing` 还配置 embedding backend/model；`agent.evidence_roots` 是接收 Redis 证据路径的绝对根目录
allowlist，默认空列表。默认配置保持无外部模型且不信任任何宿主机证据路径即可启动；示例配置说明如何启用 `bge_m3`。

Agent 进程拥有 EventConsumer、ReviewWorker 和 IndexWorker 的生命周期。worker 使用独立 SQLite
连接；停止时先停止领取新任务，再等待当前任务有界退出，最后关闭 DeerFlow/Qdrant 资源。`run()` 仅在
主线程安装 SIGINT/SIGTERM handler，并在退出时恢复旧 handler；handler 调用的 `request_stop()` 只能设置
停止事件并请求已存在 consumer 停止，不能重入生命周期锁。启动过程在创建 worker、consumer 与线程的边界
检查停止事件，避免 stop 与创建并发时启动新的 ingress。ReviewWorker/IndexWorker 的循环层必须捕获 SQLite
创建或 claim 等瞬时 `Exception`、记录 warning、按 poll interval 退避并继续；job 内的 lease/fence 语义不变。
若 consumer 或任一 worker 超时后仍存活，`stop()` 必须保留 consumer、
DeerFlow client 和临时 DeerFlow 配置，记录 warning，待线程退出后由后续 `stop()` 完成清理。

Review client 每次启动使用独立的临时空 `extensions_config.json`，仅在资源完全关闭后恢复
`DEER_FLOW_EXTENSIONS_CONFIG_PATH`。临时 `config.yaml` 通过 DeerFlow RBAC 只允许
`get_event`、`evidence_reader`、`rule_retriever`、`search_events`、`view_image`，并关闭 skills、subagent
和 plan mode；YAML 声明的四个项目工具和 DeerFlow builtin `view_image` 必须分开校验。

## 验收标准

1. 重复投递同一 event_id 只生成一个案件和每个 revision 一组任务。
2. SQLite record 失败时有效 Redis 消息不 ACK；record 成功后不等待模型/Qdrant 即 ACK。
3. ReviewWorker 和 IndexWorker 支持 claim、租约回收、重试、dead 和进程重启恢复。
4. Qdrant 不可用时事件写入和模型复核仍可完成；恢复后 index job 可重试成功。
5. `evidence_reader` 无法读取未登记、根外或越界 symlink 路径；非 uncertain 结论必须引用有效 evidence ID。
6. Qdrant payload 被构造为过期数据时，`search_events` 返回 SQLite 当前事实。
7. bge_m3 adapter 的配置与懒加载有单元测试，不下载真实模型。
8. 现有 Agent 测试及新增 focused tests 全部通过，Ruff 无新增错误。
9. 过期 worker 在其他 worker reclaim 后不能 complete/fail/reset job，也不能追加 review、推进 event
   revision 或产生 index job 部分写入；即使两个进程的 `worker_id` 相同，旧 `attempts` 也必须被拒绝。
10. SIGTERM 只请求 runtime 停止并恢复旧 handler；stop 与启动并发时不会死锁或启动新的 ingress，超时存活线程
    持有期间不会提前关闭共享 client 或删除临时 DeerFlow 配置。
11. ReviewWorker 和 IndexWorker 遇到 ledger 创建或 claim 的瞬时异常后保持运行并退避重试。
12. 同维但不同 embedding identity 的 event/rule 向量互不可见，且旧未分区 collection 不再被新代码写入。
