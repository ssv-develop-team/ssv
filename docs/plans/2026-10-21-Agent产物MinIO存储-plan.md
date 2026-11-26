# Agent 产物 MinIO 存储实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将新生成的复核 JSON、Markdown 报告和证据图片异步存入 MinIO，并保持历史本地读取、现有 Agent 工具和只读 Web 图片 API 可用。

**Architecture:** SQLite 新增不可变产物登记和独立上传任务表；业务 worker 在原有 fenced 提交事务中登记产物、业务关联和上传任务，独立上传 worker 在事务外写入并校验 MinIO，完成后按产物所有权清理本地暂存。`ArtifactReader` 统一承接历史文件、待上传暂存与已上传对象的读取，Web 仅只读查询账本并通过同一读取边界访问图片。

**Tech Stack:** Python 3.12、Pydantic、SQLite、MinIO Python SDK、FastAPI、Docker Compose、MinIO Container、`mc`。

**Spec:** [docs/specs/2026-10-21-Agent产物MinIO存储-spec.md](../specs/2026-10-21-Agent产物MinIO存储-spec.md)

## Global Constraints

- MinIO 是新复核 JSON、Markdown 报告和证据图片的长期权威存储；SQLite 继续是事件、复核事实和产物关联的事实源。
- 视频不上传 MinIO；现有视频取证、模型消费和本地视频生命周期继续遵循既有契约。
- 现有历史产物不迁移、不自动创建上传任务，保留原有本地读取能力。
- 复核完成与上传完成是不同状态；上传失败不能回滚已提交的复核，也不能触发模型重新复核。
- SQLite 中登记新产物、关联业务记录和创建上传任务必须在同一事务完成。
- 上传、下载和远端存在性检查均在 SQLite 写事务外执行。
- 本地暂存只在上传已校验、完成状态已提交后允许删除。
- 文件大小、内容类型和 SHA-256 都必须验证；S3 ETag 不替代应用层 SHA-256。
- 已登记的存储位置由产物记录决定，不随当前写入配置切换而改变。
- bucket 禁止匿名访问；Agent 应用账号不授予 bucket 管理或对象删除权限。
- Web 继续使用 SQLite `mode=ro` 与 `query_only`，不运行迁移、不启动上传 worker、不写状态。
- 新增内容优先沿用仓库已有文件位置、配置模型、账本事务、worker 生命周期、Compose 编排、CLI 入口和测试写法；只有已有边界无法承载清晰职责时才新增文件。
- 每个任务完成验证后保留未暂存变更供人工审阅；未经明确要求不提交、不推送、不创建 PR。

---

## 文件结构与接口

已有 `event_store/ledger.py` 集中放置 `DurableJob`、`ReviewRecord`、`ReportRecord` 等账本值对象，新增 `ArtifactRecord`、`StagedArtifact`、`ArtifactUploadJob` 也先放在该文件；现有 `test_event_ledger.py` 和 `test_web_api.py` 承载兼容测试。仅为真实的新边界新增以下文件：

- `agent/src/ssv_agent/artifact_storage.py`：封装 `ArtifactReader`、存储 Protocol 及可读性、完整性、远端不可用错误类型；现有路径读取逻辑适配到这里。
- `agent/src/ssv_agent/minio_store.py`：隔离 MinIO SDK 客户端，实现对象键、上传、读取和真实内容校验。
- `agent/src/ssv_agent/artifact_worker.py`：现有 `workers.py` 已负责四类业务 worker，独立文件放置上传任务的领取、心跳、完成、失败和本地清理流程，仍遵循 `_PollingWorker` 的生命周期语义。
- `docker/compose.yaml`：延续 Redis 和 Qdrant 的单文件部署模式，在 `minio-init` 一次性容器中执行初始化并生成容器内临时策略文件，不增加宿主辅助脚本或策略文件。

稳定接口：

```python
class ArtifactObjectStore(Protocol):
    def ensure_uploaded(self, artifact: ArtifactRecord, source: BinaryIO) -> None: ...
    def open(self, artifact: ArtifactRecord) -> ContextManager[BinaryIO]: ...


class ArtifactReader:
    def open(self, artifact_id: str | None, *, legacy_path: str | None = None,
             expected_sha256: str | None = None,
             expected_size: int | None = None) -> ContextManager[BinaryIO]: ...


class ArtifactUploadWorker:
    def run_once(self) -> bool: ...
```

`ArtifactReader.open` 返回的流只在上下文内有效；调用方需要给 DeerFlow 或既有工具提供路径时，由调用方拥有临时副本，并在操作完成后清理。上传 worker 只拥有其 `ArtifactRecord.staging_path` 指向的文件，不接管事件目录、clip、manifest、thread outputs 或外部输入文件。

## 实施任务

### Task 1：增加 MinIO 配置和 SDK 依赖

**Files:**
- Modify: `agent/src/ssv_agent/config.py`
- Modify: `agent/pyproject.toml`
- Modify: `agent/uv.lock`
- Modify: `config/ssv.example.yaml`
- Test: `agent/tests/test_config.py`

**Interfaces:**
- 产出严格校验的 `ArtifactStorageConfig`、`MinioStorageConfig` 和继承 `PollingWorkerConfig` 字段的上传 worker 配置。
- `agent.storage.backend` 取值 `local|minio`，缺省 `local`；`agent.storage.minio` 持有 endpoint、bucket、prefix、凭据环境变量名与超时。
- 仅保存凭据环境变量名，不把访问键或密钥写入配置模型、日志或 YAML 示例。

- [ ] **Step 1：先为默认值、严格校验和旧配置兼容写测试**

```python
def test_artifact_storage_defaults_to_local_and_old_config_still_loads():
    config = SsvConfig.model_validate({"version": "2.0", "agent": {"output_dir": "outputs"}})
    assert config.agent.storage.backend == "local"
    assert config.agent.storage.upload.max_retries == 3


def test_minio_storage_rejects_unsafe_prefix_and_nonpositive_timeout():
    with pytest.raises(ValidationError):
        MinioStorageConfig(prefix="ssv/../other")
```

- [ ] **Step 2：运行配置测试确认新行为尚未实现**

Run: `cd agent && uv run pytest tests/test_config.py -q`
Expected: 新增测试因 storage 配置类型或验证缺失而失败。

- [ ] **Step 3：实现配置模型、环境覆盖和依赖**

在 `config.py` 新增模型及 endpoint/prefix/timeout 验证；bucket 缺省从 `SSV_MINIO_BUCKET` 读取，未设置时使用 `ssv-artifacts`。访问键和密钥由模型提供的 env 名在创建客户端时解析。`agent.storage.backend=minio` 时校验凭据存在；后台仍有 MinIO 待上传或待清理责任时，即使写入 backend 已切回 local，也保留处理配置要求。将 MinIO SDK 加入运行依赖，更新 `config/ssv.example.yaml` 显式配置 MinIO 后端与非凭据项；通过 `uv lock` 更新锁文件并检查既有改动。

- [ ] **Step 4：验证配置与锁文件**

Run: `cd agent && uv run pytest tests/test_config.py -q && uv run ruff check src/ssv_agent/config.py tests/test_config.py`
Expected: PASS；`uv lock --check` PASS；`agent/uv.lock` 中保留开始任务前已存在的改动。

### Task 2：增加产物账本结构与历史 schema 兼容

**Files:**
- Modify: `agent/src/ssv_agent/event_store/ledger.py`
- Modify: `agent/src/ssv_agent/event_store/schema.py`
- Modify: `agent/src/ssv_agent/event_store/__init__.py`
- Modify: `agent/tests/test_event_ledger.py`

**Interfaces:**
- `ArtifactRecord` 至少包含 `artifact_id`、`event_id`、`kind`、`storage_backend`、`bucket`、`object_key`、`sha256`、`size_bytes`、`mime_type`、`staging_path`、`created_ms`、`uploaded_ms`、`local_cleaned_ms`。
- 新增 `artifacts`、`artifact_upload_jobs` 表；`reviews`、`reports`、`evidence` 增加 nullable `artifact_id` 外键。
- 非空 `staging_path` 建唯一索引；每个业务产物独占一个清理责任。

- [ ] **Step 1：先写新建与历史迁移测试**

```python
def test_schema_migration_preserves_legacy_review_and_index_jobs(tmp_path):
    # 沿用本文件现有测试创建 legacy.db、review 行和 durable_jobs 的 SQL。
    db_path = tmp_path / "legacy.db"
    # 在现有断言之后增加：
    with EventLedger(db_path) as ledger:
        assert ledger.list_artifacts() == ()
        assert ledger.get_review_record("legacy", 1).artifact_id is None
```

- [ ] **Step 2：运行迁移测试确认 schema 尚未支持**

Run: `cd agent && uv run pytest tests/test_event_ledger.py::test_schema_migration_preserves_legacy_review_and_index_jobs -q`
Expected: FAIL，缺少 artifact 表或查询接口。

- [ ] **Step 3：添加仅增量迁移和只读模型**

通过现有 `migrate_schema` 扩展新表及业务外键列，在 `ledger.py` 中按已有 dataclass 模式定义产物值对象；不扫描 outputs、不创建历史 artifact、不改历史 review 值。迁移对现有数据库可重复执行，foreign key 与唯一索引覆盖事件归属和 staging 独占。旧 schema 的 Web 只读兼容测试留在现有 `test_web_api.py`，由 Task 8 落地。

- [ ] **Step 4：验证新旧 schema**

Run: `cd agent && uv run pytest tests/test_event_ledger.py -q`
Expected: PASS；检查迁移前后的历史业务行数、路径、review revision 和文件均不改变。

### Task 3：实现产物登记、上传任务租约与 fencing

**Files:**
- Modify: `agent/src/ssv_agent/event_store/ledger.py`
- Modify: `agent/src/ssv_agent/event_store/__init__.py`
- Modify: `agent/tests/test_event_ledger.py`

**Interfaces:**
- `ArtifactUploadJob` 表示按 `artifact_id` 唯一关联的持久上传任务，状态为 `pending|processing|completed|dead`。
- `EventLedger` 新增 `list_artifacts`、`get_artifact`、`get_review_artifact`、`get_artifact_for_evidence`、`get_artifact_upload_job`、`claim_artifact_upload_job`、`renew_artifact_upload_job`、`complete_artifact_upload_job`、`fail_artifact_upload_job`、`list_artifacts_pending_cleanup`、`mark_artifact_cleaned`、`retry_dead_artifact_upload`。
- 所有 claim 操作以 `job_id`、`lease_owner`、`attempts`、`lease_expires_ms` fencing；完成任务与写 `uploaded_ms` 同一事务。

- [ ] **Step 1：先写 fencing、原子完成和 dead-only retry 测试**

```python
def test_old_artifact_upload_claim_cannot_complete_after_reclaim(ledger, monkeypatch):
    clock = [1_000]
    monkeypatch.setattr(ledger_module, "_now_ms", lambda: clock[0])
    first = ledger.claim_artifact_upload_job("worker-a", lease_ms=10)
    assert first is not None
    clock[0] += 11
    second = ledger.claim_artifact_upload_job("worker-b", lease_ms=10_000)
    assert second is not None and second.attempts > first.attempts
    with pytest.raises(LeaseLostError):
        ledger.complete_artifact_upload_job(first, "worker-a")
```

- [ ] **Step 2：运行账本测试确认上传任务 API 未实现**

Run: `cd agent && uv run pytest tests/test_event_ledger.py -q`
Expected: 新增上传任务测试失败，既有 durable job 测试保持通过。

- [ ] **Step 3：实现任务事务接口**

登记业务产物及上传任务使用同一个已打开 SQLite 事务；claim 只领取到期的 `pending` 或租约过期的 `processing` 任务，服从 `available_at_ms` 和重试预算；renew/complete/fail 只能更新匹配 claim。`retry_dead_artifact_upload` 只接收 `dead` 任务，原子重置错误、lease、attempts 与 available time，不改变 artifact ID、摘要和对象键。清理恢复查询只返回已完成上传且暂存未登记清理完成的行。

- [ ] **Step 4：验证状态与事务边界**

Run: `cd agent && uv run pytest tests/test_event_ledger.py -q`
Expected: PASS；增加提交回滚断言，证明产物关联与上传任务不会出现单边提交。

### Task 4：实现 MinIO 对象适配与统一读取边界

**Files:**
- Create: `agent/src/ssv_agent/artifact_storage.py`
- Create: `agent/src/ssv_agent/minio_store.py`
- Create: `agent/tests/test_artifact_storage.py`

**Interfaces:**
- `ArtifactObjectStore.ensure_uploaded(artifact, source)` 在已存在时实际读取对象并核对字节数、SHA-256 和 Content-Type；同 key 不同内容拒绝覆盖。
- `ArtifactReader.open(artifact_id, legacy_path, expected_sha256, expected_size)` 根据 artifact 和上传任务状态读取 verified staging 或 MinIO；无 artifact ID 时校验历史本地文件；完成上传后始终以 MinIO 为读来源。
- 使用上下文管理的二进制流；公开错误只区分远端暂不可用、对象不存在和内容完整性失败。

- [ ] **Step 1：先写内存对象存储与 reader 行为测试**

```python
def test_uploaded_artifact_reads_remote_even_while_staging_file_remains(reader, fake_store, staged):
    with reader.open(staged.artifact_id) as stream:
        assert stream.read() == fake_store.objects[staged.object_key]
    assert fake_store.opened_keys == [staged.object_key]


def test_existing_object_with_wrong_sha_is_never_replaced(store, wrong_object, staged):
    with pytest.raises(ArtifactIntegrityError):
        store.ensure_uploaded(staged.record, io.BytesIO(staged.bytes))
    assert store.object_bytes(staged.object_key) == wrong_object
```

- [ ] **Step 2：运行存储测试确认 adapter 尚未实现**

Run: `cd agent && uv run pytest tests/test_artifact_storage.py -q`
Expected: FAIL，缺少存储边界或 MinIO 实现。

- [ ] **Step 3：实现对象键和校验**

新增 MinIO SDK adapter，以 `<prefix>/events/<sha256(event_id)>/<kind>/<artifact_id>/<sha256>.<ext>` 构建不可变 key；上传、head/get 和内容读取均在 SQLite 写事务外。对远端已有对象通过 GET 核对实际 bytes、SHA-256、size 与 Content-Type，不信任 ETag 或自定义 metadata。`ArtifactReader.open` 对 local staging 使用安全的普通文件打开与允许根验证；远端读取完验证后才交给调用方。

- [ ] **Step 4：运行单元和类型检查**

Run: `cd agent && uv run pytest tests/test_artifact_storage.py -q && uv run ruff check src/ssv_agent/artifact_storage.py src/ssv_agent/minio_store.py tests/test_artifact_storage.py`
Expected: PASS；fake MinIO 测试覆盖连接失败、对象缺失、同 key 摘要冲突和响应流关闭。

### Task 5：在复核、报告和图片生产事务中登记产物

**Files:**
- Modify: `agent/src/ssv_agent/result.py`
- Modify: `agent/src/ssv_agent/report.py`
- Modify: `agent/src/ssv_agent/evidence_provider.py`
- Modify: `agent/src/ssv_agent/ssv_cache_evidence.py`
- Modify: `agent/src/ssv_agent/event_consumer.py`
- Modify: `agent/src/ssv_agent/workers.py`
- Modify: `agent/src/ssv_agent/event_store/ledger.py`
- Test: `agent/tests/test_workers.py`
- Modify: `agent/tests/test_event_consumer.py`
- Test: `agent/tests/test_report.py`
- Test: `agent/tests/test_ssv_cache_evidence.py`

**Interfaces:**
- 新产物由生成开始前的 `artifact_id` 标识，文件先原子写入 Agent 管理的独立暂存路径，再计算并提交摘要。
- `ReviewWorker`、`ReportWorker` 在对应原业务 lease 的 fenced 事务中同时登记 artifact、业务外键和上传任务。
- `EvidenceArtifact` 对 frame 携带 staging 所有权和 `artifact_id`；clip 和 manifest 不登记到 artifact 上传队列。
- Redis ingress 自带的 frame 先复制到 Agent 管理的独立 staging 文件，再与事件及证据关联一起提交；数据库事务外执行复制，失败不会登记指向源端的异步上传任务。

- [ ] **Step 1：先写提交原子性和图片排除测试**

```python
def test_review_commit_registers_json_and_upload_job_atomically(tmp_path, review_worker):
    review_worker.run_once()
    with EventLedger(tmp_path / "events.db") as ledger:
        artifact = ledger.get_review_artifact("event-1", revision=1)
        job = ledger.get_artifact_upload_job(artifact.artifact_id)
    assert artifact.kind == "review_json"
    assert job.state == JobState.PENDING


def test_evidence_commit_queues_frames_but_never_video_or_manifest(ledger, evidence_job, artifacts):
    ledger.complete_evidence_extract_job(evidence_job, "extractor", artifacts)
    assert [item.kind for item in ledger.list_artifacts()] == ["evidence_frame"] * 3
```

- [ ] **Step 2：运行生产者测试确认未登记产物**

Run: `cd agent && uv run pytest tests/test_workers.py tests/test_event_consumer.py tests/test_report.py tests/test_ssv_cache_evidence.py -q`
Expected: 新增 artifact 登记断言失败；原有复核、报告及取证用例通过。

- [ ] **Step 3：改造文件暂存与 fenced 提交**

给 JSON、Markdown writer 增加每 artifact 独立暂存目录，保留确定性序列化、报告模板和内容 SHA。review/report complete 方法在原 fenced 事务内登记 metadata、foreign key 和 pending upload job；重复提交复用已有关联。Evidence extract 的完整、锚点降级图片和 ingress 自带 frame 都登记 Agent 持有的图片副本。Ingress frame 复制发生在 SQLite 写事务前，随后在事务中与证据关联登记；事务失败留下的文件不得被上传 worker 扫描领取。写入复核/报告失败仍按原业务 job 处理；上传失败不得让 worker 重新跑模型。提取器恢复时通过账本已提交证据识别上传清理后的图片，不将它们当作取证丢失。

- [ ] **Step 4：运行生产者回归测试**

Run: `cd agent && uv run pytest tests/test_workers.py tests/test_event_consumer.py tests/test_report.py tests/test_ssv_cache_evidence.py tests/test_event_ledger.py -q`
Expected: PASS；断言重复业务步骤不重复登记任务，同事件多张 frame 各自有唯一 staging path。

### Task 6：新增独立上传与清理恢复 worker

**Files:**
- Create: `agent/src/ssv_agent/artifact_worker.py`
- Modify: `agent/src/ssv_agent/service.py`
- Modify: `agent/src/ssv_agent/workers.py`
- Modify: `agent/tests/test_service.py`
- Create: `agent/tests/test_artifact_worker.py`

**Interfaces:**
- `ArtifactUploadWorker` 使用 `ArtifactLedgerFactory`、`ArtifactObjectStore` 和 storage upload policy，公开 `run_once() -> bool` 与 `run(stopping: Event)`。
- `AgentService` 创建并持有 worker thread 与 MinIO client；停止领取后等待在途调用结束再关闭 client。
- worker 启动时也处理先前登记的 MinIO 队列和 uploaded-but-not-cleaned 文件，即使当前新写入 backend 为 `local`。

- [ ] **Step 1：先写 MinIO 故障、租约重领和清理恢复测试**

```python
def test_minio_outage_retries_upload_without_calling_review_runner(worker, ledger, runner):
    assert worker.run_once()
    assert ledger.get_artifact_upload_job("artifact-1").state == JobState.PENDING
    assert runner.call_count == 0


def test_uploaded_staging_cleanup_recovers_after_restart(worker, ledger, completed_job, staged_file):
    ledger.complete_artifact_upload_job(completed_job, "uploader")
    staged_file.unlink()
    assert worker.run_once()
    assert ledger.get_artifact("artifact-1").local_cleaned_ms is not None
```

- [ ] **Step 2：运行 worker/service 测试确认新 worker 缺失**

Run: `cd agent && uv run pytest tests/test_artifact_worker.py tests/test_service.py -q`
Expected: 新 worker 测试因实现缺失失败，既有生命周期测试通过。

- [ ] **Step 3：实现上传、heartbeat 和幂等清理**

上传前安全打开并核对 staging；对象写入与校验成功后经 lease fence 原子更新 `completed`/`uploaded_ms`；然后仅 unlink staging 内登记且 SHA 仍匹配的普通文件。若删除失败，记录受控清理错误并保留维护可见状态；文件已删但标记未提交时下轮幂等补记。退避及耗尽 dead 沿用配置的有限重试。`backend=local` 时不领取新 local 工作，但继续履行账本内既有 MinIO 上传与清理责任。所有网络操作不在事务内。

- [ ] **Step 4：验证停机和并发状态**

Run: `cd agent && uv run pytest tests/test_artifact_worker.py tests/test_service.py tests/test_workers.py -q`
Expected: PASS；包含 lease 失效后旧 worker 不可提交、服务停机不提前关闭在用客户端和重复上传复用对象。

### Task 7：适配 Agent 工具和复核上下文读取

**Files:**
- Modify: `agent/src/ssv_agent/event_store/ledger.py`
- Modify: `agent/src/ssv_agent/tools/get_report.py`
- Modify: `agent/src/ssv_agent/tools/evidence_reader.py`
- Modify: `agent/src/ssv_agent/workers.py`
- Modify: `agent/src/ssv_agent/service.py`
- Test: `agent/tests/test_get_report.py`
- Test: `agent/tests/test_evidence_reader.py`
- Test: `agent/tests/test_review_context.py`
- Test: `agent/tests/test_evidence_rag_review_integration.py`

**Interfaces:**
- 新 artifact 引用优先于旧本地 path；存在 artifact 关联时，MinIO 故障不能回退到旧 path。
- `get_report` 继续返回正文和 thread 虚拟路径；结构化复核结果继续由 SQLite 读取；`evidence_reader` 继续复制经校验图片到 thread outputs。
- `EventCase.to_review_context` 对 frame 解析接受受控 resolver；复核拥有临时图片工作副本并在模型调用结束后释放，clip 保持本地现有读取契约。

- [ ] **Step 1：先写上传清理后的工具读取测试**

```python
def test_get_report_reads_uploaded_artifact_after_staging_cleanup(tool, artifact_reader):
    payload = json.loads(tool.invoke({"event_id": "event-1"}))
    assert payload["found"] is True
    assert payload["content"].startswith("# 视频事件分析报告")


def test_evidence_reader_uses_registered_image_artifact_after_cleanup(tool, remote_reader):
    payload = json.loads(tool.invoke({"event_id": "event-1"}))
    assert payload["available"] is True
    assert payload["found"][0]["evidence_id"] == "evidence-1"
```

- [ ] **Step 2：运行工具测试确认其仍要求本地文件**

Run: `cd agent && uv run pytest tests/test_get_report.py tests/test_evidence_reader.py tests/test_review_context.py -q`
Expected: 新增上传后清理场景失败，历史本地场景仍通过。

- [ ] **Step 3：将消费者切换到 reader 边界**

用账本记录读取 report/evidence 的 artifact ID；tool 使用同一个 `ArtifactReader` 注入点，不传入 URL、object key 或宿主 MinIO path。结构化复核字段仍读取 SQLite，不依赖或回退读取 `result_path`。需要线程虚拟文件时，在目标 outputs 内安全创建、完整性复核并归调用方清理；复核 context 在调用模型前 materialize frame，finally 释放工作副本。工作副本创建失败作为证据读取受控错误，不修改已提交复核历史。

- [ ] **Step 4：运行 Agent consumer 回归**

Run: `cd agent && uv run pytest tests/test_get_report.py tests/test_evidence_reader.py tests/test_review_context.py tests/test_evidence_rag_review_integration.py -q`
Expected: PASS；再运行 `tests/test_workers.py` 确认 model runner 得到有效图片工作路径。

### Task 8：适配只读 Web API 和新旧账本

**Files:**
- Modify: `agent/src/ssv_agent/event_store/readonly.py`
- Modify: `agent/src/ssv_agent/web_api.py`
- Modify: `agent/src/ssv_agent/web_server.py`
- Modify: `agent/tests/test_web_api.py`
- Modify: `agent/tests/test_web_server.py`

**Interfaces:**
- `EvidenceResponse.storage_status` 取值 `local|pending|uploading|uploaded|failed`，与 `status=available|unavailable` 独立。
- Web 通过只读账本和 `ArtifactReader` 读取新 frame，不执行迁移、任务状态更新、bucket 初始化或 worker 启动。
- 远端临时不可用映射为 503 `artifact_store_unavailable`；远端对象缺失或完整性失败映射为 410 `evidence_unavailable`。

- [ ] **Step 1：先写 read-only API 的新旧 schema 与远端故障测试**

```python
def test_web_lists_uploaded_status_but_remains_read_only(client, db_path):
    response = client.get("/api/v1/events/event-1")
    assert response.json()["evidence"][0]["storage_status"] == "uploaded"
    with sqlite3.connect(f"file:{db_path}?mode=ro", uri=True) as connection:
        assert connection.execute("SELECT state FROM artifact_upload_jobs").fetchone()[0] == "completed"


def test_web_maps_minio_timeout_to_controlled_503(client):
    response = client.get("/api/v1/events/event-1/evidence/evidence-1/image")
    assert response.status_code == 503
    assert response.json()["error"]["code"] == "artifact_store_unavailable"
```

- [ ] **Step 2：运行 Web 测试确认旧 API 未提供存储状态**

Run: `cd agent && uv run pytest tests/test_web_api.py tests/test_web_server.py -q`
Expected: 新 schema 状态断言失败；历史 endpoint 现有断言通过。

- [ ] **Step 3：保持只读兼容并注入读取器**

检测新列/表后构造可选查询片段；旧 schema 继续原查询，不执行 schema migration。detail response 增加 storage 状态；图片 endpoint 校验事件与 evidence 关联，再用 `ArtifactReader` 获取 bytes，保留实际 MIME、`X-Content-Type-Options: nosniff` 和 `Cache-Control: private, no-store`。MinIO client 延迟到图片访问时使用，`create_app` 不做网络探测。

- [ ] **Step 4：验证 API 与 SQLite 写保护**

Run: `cd agent && uv run pytest tests/test_web_api.py tests/test_web_server.py -q`
Expected: PASS；测试覆盖旧数据库、无关联历史行、待上传仍可读取暂存、上传状态、超时、对象损坏和 query-only 不写状态。

### Task 9：新增 Compose MinIO、least-privilege 初始化与本地服务检查

**Files:**
- Modify: `docker/compose.yaml`
- Modify: `docker/.env.example`
- Modify: `scripts/ssv_cli/services/compose.py`
- Modify: `scripts/ssv_cli/commands/redis.py`
- Modify: `scripts/ssv_cli/cli.py`
- Modify: `scripts/ssv_cli/tests/test_compose.py`
- Modify: `scripts/ssv_cli/tests/test_cli.py`

**Interfaces:**
- Compose 新增 `minio`、一次性 `minio-init` 和 `minio-data` named volume；API/console 默认 loopback 绑定 `9000`/`9001`，可分别用 `MINIO_API_PORT`/`MINIO_CONSOLE_PORT` 覆盖。
- `minio-init` 只在服务 ready 后创建私有 `ssv-artifacts` bucket、应用账号和 bucket 级策略；root 凭据只给初始化容器，Agent 专用账号不能删对象或修改 bucket policy。
- 参照现有 Redis 写法：`docker/compose.yaml` 声明服务、`container_name`、端口、named volume、healthcheck 和 restart；`scripts/ssv_cli/services/compose.py` 继续通过 `_compose(context, "up", "-d", environment=...)` 启动整组依赖。
- 现有 `./ssv redis start|stop` 名称保持兼容；在既有 `start_redis` 就绪轮询后增加 MinIO 和初始化结果检查，`stop_redis` 仍调用 `_compose(context, "down")`，不传 `-v`，不增加平行的 MinIO 启停入口。

- [ ] **Step 1：先写 Compose 与 readiness 编排测试**

```python
def test_start_waits_for_minio_initialization(tmp_path):
    context = ProjectContext(
        root=tmp_path,
        environment={"PATH": "/usr/bin"},
        config_path=None,
        build_dir=tmp_path / "build",
        compose_file=tmp_path / "docker" / "compose.yaml",
    )
    connection = MagicMock()
    connection.__enter__.return_value = connection
    with (
        patch("scripts.ssv_cli.services.compose._compose") as compose,
        patch("scripts.ssv_cli.services.compose.RedisConnection", return_value=connection),
        patch("scripts.ssv_cli.services.compose._qdrant_ready", return_value=True),
        patch("scripts.ssv_cli.services.compose._minio_ready", return_value=True),
        patch("scripts.ssv_cli.services.compose._minio_init_completed", return_value=True) as init,
    ):
        assert start_redis(context, RedisSettings()) == 0
    assert compose.call_args.args[1:] == ("up", "-d")
    init.assert_called_once_with(context)
```

- [ ] **Step 2：运行服务管理测试确认当前仅等待 Redis/Qdrant**

Run: `pytest scripts/ssv_cli/tests/test_compose.py scripts/ssv_cli/tests/test_cli.py -q`
Expected: 新增 MinIO readiness 断言失败，旧服务入口测试通过。

- [ ] **Step 3：实现 Compose 与入口检查**

沿用 Redis 已有代码落点：只在 `docker/compose.yaml` 增加 `minio`、`minio-init`、`minio-data`，保留原 `redis`、`qdrant` 和 `ssv-network` 结构；MinIO 服务使用固定发布 tag、独立 volume、loopback 端口、readiness healthcheck 和 `restart: unless-stopped`。`minio-init` 用固定的 `mc` 发布 tag 和 `depends_on: condition: service_healthy`，直接在 Compose 的多行命令中幂等创建私有 bucket、应用账号及 bucket 级策略；该容器运行时把策略 JSON 写到自身的临时路径，再交给 `mc admin policy create`，退出后不留宿主文件。初始化任一步失败必须非零退出。继续在 `start_redis` 中调用既有 `_compose(..., "up", "-d", environment=compose_environment)`，并在原 Redis PING、Qdrant `/readyz` 检查后增加宿主 MinIO `/minio/health/ready` 检查及 `minio-init` 容器 exit code 检查；失败抛 `CliError`，成功列出三项服务。在 `stop_redis` 保持 `_compose(..., "down")`，只更新提示文本。把端口、bucket、root 与应用凭据的变量名加入 `docker/.env.example`，文档明确由项目 CLI 的 `ProjectContext.environment` 或显式 `--env-file` 传入；不依赖 Compose 自动寻找 `.env`。实施前检查本机可拉取的官方镜像并固定实际 tag，若无法核验则不编造版本。

- [ ] **Step 4：验证 Compose 渲染和本地管理单元测试**

Run: `pytest scripts/ssv_cli/tests/test_compose.py scripts/ssv_cli/tests/test_cli.py -q`
Expected: PASS；可用 Docker 时运行 `docker compose --env-file docker/.env -f docker/compose.yaml config`，并验证 `down` 未使用 `-v`。

### Task 10：增加人工上传重试命令、部署说明和端到端验证

**Files:**
- Modify: `agent/src/ssv_agent/cli.py`
- Modify: `agent/tests/test_artifact_worker.py`
- Modify: `agent/tests/test_event_ledger.py`
- Modify: `agent/tests/test_config.py`
- Modify: `docs/README.md`
- Modify: `README.md`
- Modify: `docker/.env.example`

**Interfaces:**
- 新增 `ssv-agent artifacts retry --artifact-id <id>`；沿用 CLI 全局 `--config`，只允许重置 `dead` 上传任务，不创建新 artifact、不触发模型、不修改业务 review。
- 文档说明启动顺序、宿主 Agent endpoint、Compose 环境文件显式传入、应用凭据注入、上传状态语义、重试命令和历史数据不迁移。

- [ ] **Step 1：先写 CLI 参数及 dead-only 行为测试**

```python
def test_artifact_retry_only_requeues_dead_job(monkeypatch, ledger, dead_artifact_job):
    monkeypatch.setattr(sys, "argv", [
        "ssv-agent", "artifacts", "retry", "--artifact-id", "artifact-dead",
    ])
    cli.main()
    assert ledger.get_artifact_upload_job("artifact-dead").state == JobState.PENDING


def test_artifact_retry_rejects_pending_job(monkeypatch, ledger, pending_artifact_job):
    monkeypatch.setattr(sys, "argv", [
        "ssv-agent", "artifacts", "retry", "--artifact-id", "artifact-pending",
    ])
    with pytest.raises(SystemExit):
        cli.main()
    assert ledger.get_artifact_upload_job("artifact-pending").state == JobState.PENDING
```

- [ ] **Step 2：运行 CLI 和数据库测试确认命令缺失**

Run: `cd agent && uv run pytest tests/test_artifact_worker.py tests/test_event_ledger.py -q`
Expected: 新 CLI 测试失败；既有 default 和 `web` 子命令行为通过。

- [ ] **Step 3：实现 dead-only 重试并更新运维说明**

把子命令连到 Task 3 的事务 API；拒绝不存在、pending、processing 或 completed 的 artifact。CLI 输出只包含 artifact ID 和新状态，不打印对象键、凭据或异常详情。文档示例使用 `docker compose --env-file docker/.env -f docker/compose.yaml`，说明 `.env` 权限与凭据不入版本库，明确正常 stop/down 保留 MinIO volume。

- [ ] **Step 4：运行全量相关验证和 Compose MinIO 集成链路**

Run: `cd agent && uv run pytest -q`
Expected: Agent 全量测试 PASS。

Run: `pytest scripts/ssv_cli/tests -q`
Expected: CLI 全量测试 PASS。

Run: `docker compose --env-file docker/.env -f docker/compose.yaml config`
Expected: Compose 配置可渲染；使用测试 bucket 启动 MinIO 后验证应用账号不能匿名访问、不能删除对象，真实上传/GET SHA 校验通过，重启容器对象仍在，MinIO 中没有视频或 manifest。

- [ ] **Step 5：检查跨组件验收项并保留未暂存变更**

Run: `cd agent && uv run ruff check src tests && uv run mypy src`
Expected: PASS。检查 `git diff --check`、`git status --short` 和 Compose diff，确认不存在历史产物自动迁移、远端对象清理、存储 URL 输出或视频上传；本任务不暂存、不提交。
