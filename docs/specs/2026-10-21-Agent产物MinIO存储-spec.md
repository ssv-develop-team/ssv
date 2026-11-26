# Agent 产物 MinIO 存储规格

## 1. 背景与已确认决策

当前复核 JSON 和 Markdown 报告通过本地内容寻址文件写入 `agent.output_dir`，默认目录为 `agent/outputs`；证据提取器也在 outputs 下生成图片、视频和 manifest。SQLite EventLedger 保存事件、追加式复核历史、报告登记和证据引用。现有 `get_report`、`evidence_reader` 及只读 Web 图片接口均依赖本地文件。

本规格记录 2026-10-21 确认的存储调整：

- MinIO 是新复核 JSON、Markdown 报告和证据图片的长期权威存储；SQLite 继续是事件、复核事实和产物关联的事实源。
- 本地文件仅作为上传暂存或读取期间的工作副本，不长期保留第二份已上传产物。
- 视频不上传 MinIO；现有视频取证、模型消费和本地视频生命周期继续遵循既有契约。
- MinIO 在项目现有 Docker Compose 中部署。
- 上传采用独立持久任务。MinIO 暂时不可用时继续复核，文件保留在暂存区，恢复后自动重试。
- 现有历史产物不迁移、不自动创建上传任务，保留原有本地读取能力。

“历史产物”以启用 MinIO 前已登记的文件为准，不按事件发生时间划分。历史事件此后新生成的复核版本或报告属于新产物，按当前写入配置处理。

当前工作区已经包含只读 Web API。本阶段适配已有图片读取接口，避免清理本地图片后破坏现有调用；不新增页面或报告下载接口。本规格对存储相关行为的修订适用于本阶段新增产物，既有历史规格保持其历史描述。

## 2. 范围与不变量

### 2.1 纳入范围

- Compose 中的 MinIO 服务、持久数据卷、私有 bucket 和应用账号初始化。
- 存储配置、SQLite 产物登记、按产物领取的上传任务和进程生命周期管理。
- 新复核 JSON、新报告、新登记 frame 图片的暂存、上传、校验、读取和本地清理。
- 完整上下文取证图片与锚点降级图片；原有 `coverage`、时间窗口和 `stream_generation` 语义继续保留。
- Agent 工具、复核上下文及现有 Web 图片读取的存储适配。
- 上传失败的可观察状态、有限重试和人工重试入口。

### 2.2 核心不变量

1. 复核完成与上传完成是不同状态；上传失败不能回滚已提交的复核，也不能触发模型重新复核。
2. SQLite 中登记新产物、关联业务记录和创建上传任务必须在同一事务完成。
3. 上传、下载和远端存在性检查均在 SQLite 写事务外执行。
4. 本地暂存只在上传已校验、完成状态已提交后允许删除。
5. 文件大小、内容类型和 SHA-256 都必须验证；S3 ETag 不替代应用层 SHA-256。
6. `event_id`、`review_id`、`evidence_id` 继续作为业务关联，不由对象 URL 或本地路径替代。
7. 已登记的存储位置由产物记录决定，不随当前写入配置切换而改变。

## 3. 部署与配置

### 3.1 Compose

在 `docker/compose.yaml` 增加 `minio` 和一次性 `minio-init` 服务，继续使用 `ssv-network`：

- MinIO 使用独立 named volume `minio-data`；正常 `down`、重建容器和服务升级均保留数据卷。
- S3 API 默认映射宿主机 `127.0.0.1:9000`，管理控制台默认映射 `127.0.0.1:9001`；分别通过 `MINIO_API_PORT`、`MINIO_CONSOLE_PORT` 调整端口。
- 使用固定发布版本的服务与初始化工具镜像，实施时核验并固定具体版本，不使用 `latest`。
- `minio-init` 等待服务就绪，幂等创建 bucket、应用账号和仅作用于该 bucket 的访问策略。初始化失败必须使启动命令报错，不能只报告容器已运行。
- bucket 默认 `ssv-artifacts`，通过 `SSV_MINIO_BUCKET` 配置；禁止匿名访问。
- `MINIO_ROOT_USER`、`MINIO_ROOT_PASSWORD` 只用于部署初始化；Agent 使用 `SSV_MINIO_ACCESS_KEY`、`SSV_MINIO_SECRET_KEY` 对应的应用账号。凭据不提供内置默认值，不写入受版本管理的 YAML。
- 应用账号仅具备目标 bucket 内正常上传、校验和读取所需权限，不授予管理账号、修改 bucket 策略或删除对象的权限。
- 根目录 `./ssv redis start` 目前会启动整个 Compose。本阶段保留入口，补充 MinIO 健康状态和初始化成功检查，成功消息准确列出 Redis、Qdrant、MinIO；停止入口同步说明其作用于这些本地依赖，且不删除数据卷。
- Compose 与 Agent 使用同一组环境变量约定；文档写明显式传入 `docker/.env` 的方式，不依赖不同工作目录下隐式加载同一文件。

### 3.2 Agent 配置

配置放在现有 `agent` section 下，接受严格校验：

| 配置键 | 约定 |
| --- | --- |
| `agent.storage.backend` | `local` 或 `minio`；缺省为 `local` 以兼容既有配置，新示例显式选择 `minio` |
| `agent.storage.minio.endpoint` | 服务端 S3 HTTP(S) 地址；本机部署示例为 `http://127.0.0.1:9000` |
| `agent.storage.minio.bucket` | bucket 名称；缺省读取 `SSV_MINIO_BUCKET`，未设置时为 `ssv-artifacts` |
| `agent.storage.minio.prefix` | 对象前缀，默认 `ssv/v1`；禁止绝对路径、空路径组件和 `.`、`..` 组件 |
| `agent.storage.minio.access_key_env` | 访问键所在环境变量名，默认 `SSV_MINIO_ACCESS_KEY` |
| `agent.storage.minio.secret_key_env` | 密钥所在环境变量名，默认 `SSV_MINIO_SECRET_KEY` |
| `agent.storage.minio.connect_timeout_seconds` | 连接超时，默认 5 秒，必须大于零 |
| `agent.storage.minio.read_timeout_seconds` | 网络读取超时，默认 15 秒，必须大于零 |
| `agent.storage.upload` | 沿用 `PollingWorkerConfig` 的 `poll_interval_ms`、`lease_ms`、`max_retries`、`retry_delay_ms` 及默认值 |

`endpoint` 不接受嵌入凭据、query 或 fragment。配置错误在启动时明确失败；MinIO 网络故障由上传任务处理，不在 Agent 启动时强制探测远端。远端上传的重试预算由持久任务统一控制，SDK 的内部重试也必须有明确上限。

`backend=local` 保留既有写入行为；`backend=minio` 对新产物启用异步上传。只要账本仍有已登记的 MinIO 待上传任务或已上传但未清理的暂存，Agent 都应继续处理它们；切换写入 backend 不取消历史队列或清理责任。缺少必需的 MinIO 配置或凭据时明确报错，不把待上传记录降级为本地长期存储。

`agent.output_dir` 继续作为 Agent 管理的输出根目录，相对路径仍按 Agent 根解析。写入时的 bucket、对象键和内容摘要登记在账本中；后续修改 bucket 或 prefix 只影响新产物。单实例服务地址可以调整，但不支持在一次配置切换中把已登记对象静默改指到另一个存储集群。

## 4. 数据模型与稳定标识

### 4.1 产物记录

新增 `artifacts` 表和对应 `ArtifactRecord`，每条记录表示一个不可变业务产物，至少包含：

- `artifact_id`、`event_id`、`kind`（`review_json`、`report_markdown`、`evidence_frame`）。
- `storage_backend`（本阶段新登记为 `minio`）、`bucket`、`object_key`。
- `sha256`、`size_bytes`、`mime_type`。
- `staging_path`、`created_ms`、`uploaded_ms`、`local_cleaned_ms`；清理完成后 `staging_path` 置空。

非空 `staging_path` 必须唯一，一份暂存文件只由一个产物负责清理。内容相同但属于不同业务产物时，生产者使用独立暂存位置，不能把同一个内容寻址文件登记给多个独立上传任务。`artifact_id` 在本次生成开始时分配，并保持到业务提交或失败退出。

在 `reviews`、`reports`、`evidence` 增加可空 `artifact_id` 外键；各业务记录只关联自己的产物，不改变历史 review 的结论、revision 或证据 ID。`reports` 中既有大小与哈希必须与关联产物一致。

`artifact_id` 为空表示沿用历史本地存储契约，数据库升级不扫描目录、不自动补产物记录。新记录优先通过 `artifact_id` 读取；一旦存在该关联，就不能因远端故障退回读取旧路径列。

旧 `result_path`、`artifact_path`、`evidence.path` 保留本地路径含义。为兼容既有非空约束，新记录可保留初始本地生成路径，但该路径在清理后可能不再存在，只作为生成来源信息；MinIO 对象键不得填入这些字段。关联新产物的调用方必须使用产物读取接口。

### 4.2 上传任务

新增 `artifact_upload_jobs`，一条产物唯一对应一条上传任务，以 `artifact_id` 外键关联。任务保存 `job_id`、`state`、`attempts`、`lease_owner`、`lease_expires_ms`、`available_at_ms`、`last_error`、`error_code`、`error_message`、`failed_at_ms`、`created_ms`、`updated_ms`。

状态采用现有语义：`pending`、`processing`、`completed`、`dead`。`artifacts` 不重复保存一份可独立变更的上传状态。登记上传完成时，同一事务将任务置为 `completed` 并设置 `uploaded_ms`。

现有 `durable_jobs` 的 `entity_id` 引用事件，唯一键为 `(kind, entity_id, entity_revision)`，无法表达同事件多张图片。本阶段保持其业务含义，上传采用独立任务表，沿用租约、heartbeat、attempt fencing 和重试上限约定，避免借用事件 revision 表示图片序号。

### 4.3 对象键

对象键格式为 `<prefix>/events/<event-token>/<kind>/<artifact-id>/<sha256>.<ext>`。`event-token` 是原始 `event_id` 的 UTF-8 SHA-256；`artifact-id` 使用服务端生成的安全标识；扩展名仅为固定 `json`、`md`、`jpg` 或受支持的图片扩展名。

产物记录一旦提交，对象键不再改变。JSON、Markdown 保留现有确定性序列化和内容寻址规则，不为对象存储重新生成内容。所有位置从账本读取，客户端只提交业务 ID；不从用户输入拼接 bucket、对象键或文件路径。不保存签名 URL。

## 5. 生成、登记和上传流程

### 5.1 生成与登记

1. 生产者在 Agent 管理的目录原子生成文件并计算摘要，不调用 MinIO。JSON、Markdown 使用按 `artifact_id` 隔离的暂存位置；图片复用唯一对应其证据记录的生成位置。重复执行已提交的业务步骤复用已有产物关联，不创建第二条任务。
2. `ReviewWorker` 在完成复核的 fenced 事务中登记 JSON 产物与上传任务，同时保留现有复核追加、当前投影和后续任务创建行为。
3. `ReportWorker` 在登记报告的 fenced 事务中登记 Markdown 产物与上传任务；报告始终绑定指定历史 review，报告失败继续独立于复核。
4. 完整取证和锚点降级提交在登记 frame 证据的同一 fenced 事务中登记图片产物与上传任务。clip 和 manifest 不创建上传任务。新 ingress 自带的受信 frame 也需要在登记时形成 Agent 拥有的稳定暂存副本；不得异步依赖可能被源端回收的图片路径。
5. 未发生业务提交的文件不允许被上传 worker 领取。业务事务回滚或失租仍可能留下未登记的本地文件；本阶段不通过全目录扫描将其视为有效产物。

本地文件写入失败仍属于生产者自身失败。上传独立重试的保证适用于已完成文件生成和账本登记的产物，不承诺在模型调用与本地业务提交之间发生崩溃时也绝不重复模型调用。

### 5.2 上传执行

`ArtifactUploadWorker` 的生命周期由 `AgentService` 持有，独立于 review/report worker 是否启用。领取任务后：

1. 验证 lease，安全打开已登记暂存文件，验证普通文件、允许根目录、大小、SHA-256 和类型。
2. 在数据库事务外调用 `MinioObjectStore.ensure_uploaded`。对象不存在则上传；已存在则校验后复用，同键内容不匹配时失败，禁止覆盖为另一份内容。
3. 通过远端读取验证实际字节的大小和 SHA-256，并核对 Content-Type；仅 HEAD 或自定义 hash metadata 不足以判定内容一致。
4. 持续 heartbeat，在提交前检查当前 claim，以 `(job_id, lease_owner, attempts, lease_expires_ms)` 作为 fence 完成数据库提交。旧 worker 即使上传成功，也不能覆盖新 claim 的状态。
5. 上传完成提交后执行本地清理；清理失败不回滚上传完成，保留清理标记，由后续维护轮询重试。

服务停止时停止领取新任务，按现有受控停机约定等待在途任务，再释放网络资源。在途调用未退出时不提前关闭其客户端。租约到期的任务可被重新领取；重启不依赖进程内队列恢复状态。

## 6. 读取、证据可用性和本地所有权

### 6.1 统一读取边界

新增 `ArtifactReader.open`，按已登记记录提供上下文管理的可校验二进制读取资源：

- 历史记录没有 `artifact_id`：使用原有本地读取校验，包括 outputs/evidence roots、无 symlink、普通文件及已有大小和哈希约束。
- 新产物尚未上传完成：允许读取其已登记、校验通过的暂存文件，即使上传任务已失败。上传状态不等同于内容可读性。
- 新产物已上传完成：从 MinIO 读取并校验。即使清理尚未执行，也以远端为读取来源；远端失败不静默改读遗留暂存。

接口区分暂时不可访问、内容不存在和完整性失败，调用方将其映射为现有工具的受控错误。返回资源由读取上下文持有和关闭；不把网络响应、宿主路径或签名 URL 当成模型可读文件。

`get_report` 继续返回正文、历史复核关联和线程虚拟路径；`evidence_reader` 继续向当前 DeerFlow thread outputs 复制已校验图片。线程副本属于既有 thread 生命周期，上传 worker 不负责删除它。

`EventCase.to_review_context` 及其他直接消费 `frame_path` 的位置也必须适配：需要本地路径时，通过 reader 创建本次复核持有的工作副本，使用结束后释放；保留本地 clip 的现有访问方式。不能只修改工具而让复核上下文继续指向已清理图片。

### 6.2 可用性

`evidence.available` 表示账本最后确认的证据可用性，不承诺远端此刻健康。新图片上传成功后，清理本地文件不能将其改为不可用，也不能清空 size、SHA-256 或时间覆盖元数据。远端及下载副本以大小、SHA-256、类型校验，不要求其文件系统 mtime 等于原始图片 mtime；历史本地校验仍沿用原有规则。

`refresh_evidence` 对历史本地证据继续做本地检查；对关联产物的证据依据其存储状态处理。需要远端 I/O 时先在事务外读取、验证，再以产物 ID 和预期存储状态保护更新。短暂网络故障只作为本次读取失败，不永久标记证据不存在。

复核中的证据引用校验继续绑定事件及稳定证据 ID。当前读到的内容和历史结论分开处理，文件后续不可访问不能改写既有复核结论。

### 6.3 暂存与清理

- 新 JSON、Markdown 使用 outputs 下按产物隔离的暂存子目录；提取器已发布的 derived 图片可以直接作为其产物暂存。是否允许清理由产物登记和所有权决定，不依赖目录名称或文件后缀。
- 上传 worker 只删除当前产物登记、位于允许根目录且内容摘要仍匹配的具体文件，不递归删除事件目录。视频、manifest、历史文件和外部输入原文件均不属于其删除权限。
- 取证生产者的目录恢复与 manifest 校验必须区分“尚未提交的提取结果”和“已提交后图片已上传清理”，不能把后者误认作需要重新抽帧的失败结果。
- 暂存读取先安全打开并持有文件资源，再读取或复制。清理与打开竞争时，reader 重新查询状态：若上传已完成则从 MinIO 读取；否则返回受控不可用。禁止在仅返回裸路径后无保护地延后复制。
- 上传成功但清理前崩溃，由下一轮维护继续清理；文件已删除但清理标记未提交时，幂等补记清理完成。清理失败保持可观察，不能让失败文件永久无人处理。
- 未上传或上传失败的文件不按年龄自动删除。磁盘容量不足时生产者明确失败，不以丢弃待上传证据的方式维持表面成功。

## 7. 现有 Web 接口兼容

现有 `/api/v1/events/{event_id}/evidence/{evidence_id}/image` 通过相同 reader 获取图片，仍先验证事件与证据关联，再检查实际图片格式、MIME、大小和 SHA-256。返回实际图片字节，保留 `nosniff` 和 `Cache-Control: private, no-store`。

`EvidenceResponse` 增加 `storage_status`，取值为：

| 值 | 含义 |
| --- | --- |
| `local` | 无新产物关联，沿用本地读取，包括 clip |
| `pending` | 上传待领取、等待重试，或 processing 租约已过期 |
| `uploading` | 上传任务具有尚未过期的 processing 租约 |
| `uploaded` | 上传已校验并完成账本登记 |
| `failed` | 上传任务为 dead |

该字段与现有 `status=available/unavailable` 分离；例如上传失败但暂存完整时仍可读取图片。`analysis_task` 继续只描述复核/取证，不混入上传任务。

远端网络或服务暂不可用返回 503 `artifact_store_unavailable`；确认对象缺失或校验失败沿用 410 `evidence_unavailable`。既有事件/证据不存在、非图片、参数错误等响应继续保留。响应不暴露 bucket、对象键、宿主路径、凭据或异常详情。

Web 仍使用 SQLite `mode=ro` 与 `query_only`，不运行 schema 迁移、不创建 bucket、不启动上传 worker、不更新可用性或上传状态。支持在未新增产物表/列的历史账本上只读查询，按本地记录解释；新列识别只做只读 schema 检查。构造应用时不强制连接 MinIO；读取图片时才使用对象存储。

## 8. 错误处理与人工恢复

| 故障 | 处理 |
| --- | --- |
| MinIO 断连、超时或可重试服务错误 | 按持久任务策略退避重试，保留暂存，不修改已提交复核 |
| 认证或权限错误 | 记录受控错误码，进入有限重试，耗尽后 dead；修正配置后人工重试 |
| 暂存缺失或摘要不匹配 | 标记受控失败并保留诊断，不自动重跑模型或取证替代原始产物 |
| 上传已成功、完成事务失败或 lease 丢失 | 不删除暂存，下次领取校验并复用相同对象 |
| 远端同键内容不匹配 | 完整性失败，禁止覆盖或清理暂存 |
| 已上传对象后续被外部删除或损坏 | 读取受控失败，不伪造恢复成功，不重写历史结论 |
| 本地删除失败 | 上传保持 completed，后续清理轮询重试并记录清理错误 |

新增显式命令 `ssv-agent artifacts retry --artifact-id <id>`，仅重置指定 dead 上传任务为 pending，清除错误和租约、重置尝试次数，并保留原对象键及产物摘要；不存在或非 dead 的记录明确拒绝。命令不调用模型、不创建新 review、不修改业务状态。通过 CLI 全局 `--config` 指定账本和存储配置，不提供 Web 写入入口。

日志至少包含 `event_id`、`artifact_id`、`job_id`、`artifact_kind`、`state`、`attempts`、`error_code`；清理失败采用 `artifact_cleanup_failed` 受控事件并带产物 ID。密钥及带认证信息的请求不得进入日志或用户可见错误。

跨 SQLite 与 MinIO 无分布式事务。已经成功上传但业务完成登记失败时，可依靠任务和确定对象键恢复；未登记文件、账本被人工删除后的孤儿对象仍可能存在。本阶段不自动删除远端对象，避免以不完整引用视图误删证据。

## 9. 验收与验证要求

1. Compose 首次启动创建私有 bucket 和应用账号；重复启动幂等；普通停止、重启后对象仍存在；初始化失败能被启动入口识别。
2. 严格配置校验覆盖 endpoint、凭据环境变量、bucket、prefix 和超时；旧配置继续本地模式，新示例明确启用 MinIO。
3. 从历史 schema 升级只增加登记结构，不改历史 review 内容、不扫描历史目录；已有未完成任务、租约和重试计数保留。历史本地 JSON、报告和图片继续可读。
4. 新 JSON、Markdown、完整取证图片及锚点降级图片均创建唯一产物及上传任务；同一事件多张图分别上传，视频和 manifest 不上传。
5. 产物与任务登记原子回滚；上传和下载期间不持有 SQLite 写事务。
6. MinIO 停机期间，复核与报告可完成，暂存保留、上传状态可查询；恢复服务后在重试预算内自动补传，不再次调用模型。
7. 租约过期重新领取、旧 worker 延迟返回、上传后提交失败、进程重启和重复执行均不会产生错误完成状态或改写不可变内容。
8. 上传后远端实际内容校验通过才允许清理；同键内容不匹配时失败。清理与读取竞争、清理前后崩溃均有覆盖。
9. 清理后的图片仍能用于复核、`evidence_reader` 和现有 Web 图片接口；报告仍能由 `get_report` 读取；返回虚拟路径保持线程隔离。
10. 本地 symlink/越界、远端损坏、MIME 不符、文件缺失和连接超时均受控失败，不泄漏存储位置或凭据；历史视频读取不受图片清理影响。
11. 上传耗尽后暂存保留，指定 dead 任务可人工重试；待领取和正在处理的任务不能通过该命令重置。
12. Web 在新旧 schema 上均保持只读，不触发迁移、上传或初始化；新增存储状态不改变既有事件统计及分析任务语义。

单元测试通过生产者、账本和存储接口间的真实边界注入失败；集成测试使用 Compose MinIO 验证真实上传、重启持久性、私有访问及完整性行为。实施时运行相关 Python 格式、静态检查、Agent 与 CLI 测试，并校验 Compose 配置；本规格阶段只进行文档与代码契约核对。

## 10. 非本阶段范围

- 历史产物回填、迁移命令或历史本地文件清理。
- 视频上传、视频生命周期调整、Range 播放，以及 runner 源缓存的远程访问改造。
- 新 Web 页面、复核 JSON/报告下载 HTTP 接口、签名 URL、公共 bucket 或浏览器直传。
- 账号权限产品、WVP 适配、事件人工处理流程。
- 多集群对象存储、对象版本控制、远端自动过期、远端孤儿对象自动回收及备份系统。
- 从丢失暂存或损坏对象自动重建证据、重新调用模型生成替代结果。
- 与本次存储接入无关的 worker、账本或取证算法重构。


## 11. 修订记录

- 2026-10-21：本地 MinIO 改为自建镜像 `ssv-minio:minio`（官方镜像不再提供），该镜像不包含 `mc`、`curl` 或 Python。由此取消 `minio-init` 服务与最小权限应用账号：Compose 只使用 `MINIO_ROOT_USER` / `MINIO_ROOT_PASSWORD` 启动服务，Agent 的 `SSV_MINIO_ACCESS_KEY` / `SSV_MINIO_SECRET_KEY` 与 root 凭据相同，并在首次上传时通过 MinIO SDK 幂等创建私有 bucket。第 3.1 节中关于 `minio-init`、应用账号策略和初始化失败的约定在本修订后不再适用；第 9 节第 1 条相应调整为验证服务就绪、bucket 可自动创建且对象在重启后保留。恢复最小权限账号需要重新引入基于 MinIO Admin API 或 `mc` 的初始化步骤。
- 2026-10-22：本地 MinIO 镜像改为可复现分发：仓库新增 `docker/minio/Dockerfile`（固定上游版本），CI 在 main 分支构建并推送 `ghcr.io/<owner>/ssv-minio`；`./ssv redis start` 优先拉取共享镜像，拉取失败时使用 Compose `build` 本地构建。第 11 节 2026-10-21 修订中“仅存在于本机”的分发方式不再适用，镜像内容与 root 账号约定保持不变。
