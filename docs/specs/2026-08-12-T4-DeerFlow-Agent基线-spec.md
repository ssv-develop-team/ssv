# T4 第一阶段 Agent（DeerFlow 接入）Spec

- 日期：2026-08-12
- 状态：待评审

## 背景

当前 `agent/` 只有 Redis Streams 事件消费基线：读取 `ssv:events`、解析 JSON、
日志输出、XACK。本阶段基于 DeerFlow Harness 建立可用的视频分析 Agent，重点放在
三块能力：

1. **融合检索**：事件/检测/复核结果的结构化检索与语义检索融合；
2. **RAG 层**：安全规则与知识的上层抽象、检索与解释；
3. **RTSP 可用的帧采集/视频处理工具**：直接对实时流探测、抽帧、截图、片段录制。

参考 `video-search-and-summarization`（VSS）与 `mydemo/deerflow-mydemo` 时，只借鉴
依赖引入形式与工具实现模式，不采用 demo 的 daemon、作业队列、SSE、HTTP 架构。

## 目标

1. 在现有 `agent` 服务进程内嵌入 DeerFlow Harness，逐事件调用复核。
2. 建立 `event_store`：SQLite 存结构化条目，Qdrant 存向量，支撑融合检索。
3. 实现 `search_events`：自然语言查询分解 + 结构化属性检索 + 语义检索 + 融合排序。
4. 建立 RAG 上层抽象（`Retriever` / `Ingester` + registry + mock 后端），提供
   `rule_retriever` / `rule_explainer` 工具。
5. 实现 RTSP 可用的媒体工具：`stream_probe`、`analyze_video`、`sample_video`、
   `extract_snapshot`、`extract_clip`、`view_image`。
6. 保留复核闭环：事件消费 → DeerFlow 复核 → `result.md` 落盘（成功）或结构化
   日志（失败）。
7. 提供独立脚本注入样例事件，便于无真实链路的手工验收。

## 非目标（非本阶段范围）

- WVP / ZLMediaKit 历史回放接入；`extract_clip` 的历史时间戳模式仅占位。
- 真实外部通知发送；`notify` 仅 mock。
- 规则来源格式与分块策略冻结；`Ingester` 接口先占位，具体文档格式在接入
  Qdrant 知识集合前定义。
- 崩溃恢复（XAUTOCLAIM 捞回）。
- 修改 `./ssv` 与 `scripts/agent.sh`。
- `python_executor` 等不安全工具；`chart_generator`、`geolocation`、
  `rtvi_vlm_alert` 等无关工具。
- 大规模并发、多机部署与高可用。

## 技术栈

| 项 | 决策 |
| --- | --- |
| 语言/包管理 | Python >= 3.12，uv |
| 框架 | deerflow-harness 2.1.0（git submodule，固定 commit `8659fca8`） |
| 接入方式 | `DeerFlowClient` + `agent/config.yaml`，进程内调用 |
| 模型 | 配置驱动（config.yaml `models`），API key 环境变量 |
| 结构化存储 | SQLite（stdlib `sqlite3`），记录事件/检测/证据/复核结果条目 |
| 向量存储 | Qdrant（`qdrant-client` 本地嵌入模式起步，后续可切服务模式） |
| 媒体处理 | ffmpeg / ffprobe 子进程，统一超时与错误映射 |
| 消息存储 | Redis Streams 仅 XACK；复核结果本地文件 |

## 目录结构

```text
agent/
├── pyproject.toml
├── config.yaml                 # DeerFlow harness 配置（models / tools / sandbox / checkpointer）
├── third_party/
│   └── deer-flow/              # git submodule，固定 commit
├── scripts/
│   └── enqueue_event.py        # 独立样例事件注入脚本
├── src/ssv_agent/
│   ├── config.py               # ssv.yaml 配置（扩展 agent 段）
│   ├── cli.py                  # 现有 CLI
│   ├── service.py              # 入口，装配 consumer 与存储
│   ├── event_consumer.py       # 消费循环，写 event_store 并调用复核
│   ├── review_context.py       # Detection / ReviewContext + from_event()
│   ├── prompt.py               # build_review_prompt()
│   ├── runner.py               # DeerFlowClient.stream 单事件执行
│   ├── result.py               # parse_result_markdown() / write_result_markdown()
│   ├── logging.py              # 现有结构化日志
│   ├── event_store/
│   │   ├── schema.py           # SQLite 建表语句
│   │   └── sqlite_store.py     # SsvEventStore
│   ├── knowledge/
│   │   ├── schema.py           # Chunk / RetrievalResult / IngestResult
│   │   ├── retriever.py        # Retriever ABC
│   │   ├── ingester.py         # Ingester ABC
│   │   ├── registry.py         # 注册 + 懒加载工厂
│   │   └── backends/
│   │       └── mock.py         # MockRetriever / MockIngester
│   ├── search/
│   │   ├── decompose.py        # 自然语言查询分解
│   │   ├── attribute_search.py # SQLite 结构化检索
│   │   ├── embed_search.py     # Qdrant 语义检索
│   │   └── fusion.py           # 融合排序
│   ├── media/
│   │   ├── ffmpeg_utils.py     # ffmpeg/ffprobe 子进程封装
│   │   ├── stream_probe.py     # RTSP 可达性与码流参数
│   │   ├── analyze_video.py    # 视频元数据
│   │   ├── sample_video.py     # 抽帧
│   │   ├── extract_snapshot.py # 抓关键帧
│   │   └── extract_clip.py     # 截取片段
│   └── tools/
│       ├── __init__.py
│       ├── evidence_reader.py  # 证据路径校验
│       ├── view_image.py       # 看图（harness 内置包装）
│       ├── rule_retriever.py   # 规则检索
│       ├── rule_explainer.py   # 规则解释
│       └── search_events.py    # 融合检索入口
└── tests/
    ├── test_review_context.py
    ├── test_prompt.py
    ├── test_result.py
    ├── test_runner.py
    ├── test_event_consumer.py
    ├── test_sqlite_store.py
    ├── test_knowledge.py
    ├── test_search.py
    ├── test_media_tools.py
    └── test_enqueue_event.py
```

## 关键类设计

### Detection / ReviewContext

`Detection` 字段与 `ssvpub` 输出逐一对应：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `class_name` | str | 类别名 |
| `class_id` | int | 类别 id |
| `confidence` | float | 置信度 |
| `bbox` | list[float] | `x1, y1, x2, y2` |
| `track_id` | int | 默认 -1 |
| `track_state` | str \| None | 可空 |
| `occluded` | bool | 是否遮挡 |

`ReviewContext` 承载当前 publish 已有字段，派生字段占位：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `event_id` | str | Redis Stream entry id |
| `source` | str | 视频源 |
| `timestamp_ms` | int | 事件时间戳 |
| `frame_id` | int | 帧号 |
| `detections` | list[Detection] | 检测列表 |
| `event_type` | str \| None | 占位，默认 None |
| `severity` | str \| None | 占位，默认 None |
| `frame_path` | str \| None | 占位，默认 None |
| `clip_path` | str \| None | 占位，默认 None |
| `question` | str \| None | 占位，默认 None |

构造统一走 `ReviewContext.from_event(entry_id, payload)`。

### RAG 上层抽象

```python
class Chunk(BaseModel):
    chunk_id: str
    content: str
    score: float
    metadata: dict[str, Any]      # 约定必须含 source / rule_id / section

class RetrievalResult(BaseModel):
    chunks: list[Chunk]
    query: str
    backend: str
    success: bool
    error_message: str | None

class IngestResult(BaseModel):
    document_id: str
    chunks_count: int
    success: bool
    error_message: str | None
```

```python
class Retriever(ABC):
    backend_name: ClassVar[str]

    @abstractmethod
    async def retrieve(
        self, query: str, *, top_k: int = 5,
        filters: dict[str, Any] | None = None,
    ) -> RetrievalResult: ...

    async def health_check(self) -> bool:
        return True

class Ingester(ABC):
    backend_name: ClassVar[str]

    @abstractmethod
    async def ingest(self, document: Any) -> IngestResult: ...
```

- `registry.py`：`register_backend(name, retriever_cls, ingester_cls, config_type)` +
  `get_retriever(name, config)` / `get_ingester(name, config)`，懒加载
  `mock`（后续加 `qdrant`、`sqlite_fts`）。
- `backends/mock.py`：`MockRetriever` 返回固定样例 chunk；`MockIngester` 返回
  `success=False + "not implemented"`。
- `rule_retriever` / `rule_explainer` 只依赖 `Retriever` 接口；`summarize` 放
  `rule_explainer`，不进 Retriever。

### event_store（SQLite）

表结构：

| 表 | 关键字段 |
| --- | --- |
| `events` | `event_id` PK、`source`、`timestamp_ms`、`frame_id`、`event_type`、`severity`、`status`、`verdict`、`confidence`、`result_path`、`created_ms` |
| `detections` | `id` PK、`event_id` FK、`class_name`、`class_id`、`confidence`、`bbox_json`、`track_id`、`track_state`、`occluded` |
| `evidence` | `id` PK、`event_id` FK、`kind`（frame/clip）、`path`、`available`、`size`、`mtime` |
| `review_results` | `event_id` PK、`verdict`、`confidence`、`evidence_status`、`explanation`、`parsed_at_ms` |

`SsvEventStore` 提供：`insert_event`、`insert_detections`、`insert_evidence`、
`insert_result`、`query_events(filters)`、`get_event`、`get_result`。
证据文件本体留在 `outputs/`，SQLite 只记录路径与元信息。

### Qdrant

- 集合 `ssv_events`：payload `event_id / source / timestamp_ms / event_type / verdict`；
  向量为事件文本 embedding。
- 集合 `ssv_rules`：payload `chunk_id / rule_id / source / section`；向量为规则
  chunk embedding。
- 本地嵌入模式 `QdrantClient(path=...)` 起步；配置支持切换 `url` 服务模式。

### 融合检索

- `decompose.py`：LLM 把自然语言拆成 `DecomposedQuery`（文本、source、时间范围、
  class、verdict、top_k）。
- `attribute_search`：基于 SQLite 结构化过滤。
- `embed_search`：基于 Qdrant 语义检索 + payload 过滤。
- `fusion.py`：两路结果按归一化分数加权合并（默认 attribute 0.5 / embed 0.5，
  权重可配置），按 `event_id` 去重，输出 `SearchResultItem`。

### 媒体工具

- `ffmpeg_utils.py`：`run_ffmpeg` / `run_ffprobe`，统一超时、返回码与错误文本。
- `stream_probe`：RTSP 可达性、编码、分辨率、帧率探测。
- `analyze_video`：ffprobe 元数据，支持文件与 RTSP URL。
- `sample_video`：文件按时间窗口抽帧；RTSP 周期抓取当前帧。
- `extract_snapshot`：RTSP 抓当前帧；文件按时间戳抓帧。
- `extract_clip`：RTSP 前向录制 N 秒；文件按时间范围截取；历史回放模式占位。
- `view_image`：harness 内置工具。

RTSP 是实时流，不能随机 seek：snapshot 只能是当前帧，clip 只能从调用时刻向前
录制；历史时间戳取帧/片段依赖录像回放，本阶段不实现。

### result.py

`parse_result_markdown(text) -> ReviewResult` 校验：

- `结论` ∈ `compliant / violation / uncertain`；
- `置信度` ∈ `[0, 1]`；
- `证据状态` ∈ `available / missing`；
- `证据状态=missing` 时 `结论` 必须为 `uncertain`。

`write_result_markdown(event_id, text)` 原子写入 `outputs/<event_id>/result.md`。

## Agent 架构与流程

### 复核流程

```text
ssv:events
    │ XREADGROUP
    ▼
EventConsumer
    ├─ 写入 SsvEventStore（事件 + 检测 + 证据元信息）
    ├─ ReviewContext.from_event()
    ▼
build_review_prompt()
    ▼
DeerFlowClient.stream(thread_id=event-<event_id>)
    │  evidence_reader / view_image / media tools
    ▼
parse_result_markdown()
    ├─ 通过 → write_result_markdown() + 写 review_results → XACK
    └─ 失败 → 结构化日志 → XACK（不落盘、不重试）
```

### 融合检索流程

```text
用户问题 → search_events
    → decompose（LLM 拆解）
    → attribute_search（SQLite） ‖ embed_search（Qdrant）
    → fusion 合并去重 → SearchResultItem 列表
```

### RAG 流程

```text
rule_retriever(query, filters)
    → get_retriever(knowledge.backend).retrieve()
    → RetrievalResult(chunks with source)

rule_explainer(event, chunks)
    → LLM 结合 chunk 生成解释，必须引用 source / rule_id / section
```

## 接口契约

### 输入事件

当前 `ssvpub` 发布的 JSON：`type / source / timestamp_ms / frame_id / detections`。

### result.md

```markdown
结论: violation
置信度: 0.87
证据状态: missing
依据: <说明>
```

### 失败语义

- 模型/工具异常：结构化日志输出 `event_id + 错误原因`，XACK，不落盘。
- 模板解析失败或编造结论：日志输出，XACK，不落盘。
- 缺证据：正常落盘 `结论: uncertain` + `证据状态: missing`。

### 工具契约

所有 DeerFlow 工具使用 Pydantic 输入/输出模型；占位工具返回显式
`not implemented` 或 `available: false`，不伪造结果。

## 配置契约

- `agent/config.yaml`：DeerFlow harness 的 `models / tools / sandbox / checkpointer`。
- `ssv.yaml` 的 `agent` 段扩展：
  - `output_dir`：结果目录，默认 `outputs`；
  - `model_name`：可选，覆盖 config.yaml 默认模型；
  - `event_store.path`：SQLite 文件路径，默认 `agent/data/events.db`；
  - `qdrant.path` 或 `qdrant.url`：本地嵌入模式或服务模式；
  - `qdrant.collections`：`ssv_events` / `ssv_rules`；
  - `knowledge.backend`：`mock`（后续 `qdrant` / `sqlite_fts`）；
  - `search.top_k` / `search.fusion_weights`；
  - `media.ffmpeg_timeout` / `media.snapshot_width` / `media.clip_duration`。

## 验证方式

1. 单元测试覆盖 `review_context`、`prompt`、`result`、`event_store`、
   `knowledge`、`search`、`media_tools`。
2. `runner` 测试使用 fake DeerFlowClient，不依赖真实 API。
3. RTSP 工具测试：无 RTSP 源时跳过，需真实流时用环境变量指定。
4. 手工验收：
   - `uv run --isolated --script agent/scripts/enqueue_event.py` 注入样例事件；
   - 启动 `agent` 服务消费；
   - 无证据样例预期 `uncertain / missing`；
   - 故障样例预期只有日志、无 result.md；
   - `search_events` 在注入多条事件后可按属性/语义检索。
