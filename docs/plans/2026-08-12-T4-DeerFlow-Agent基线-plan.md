# T4 第一阶段 Agent（DeerFlow 接入）Plan

- 日期：2026-08-12
- 前置：`docs/specs/2026-08-12-T4-DeerFlow-Agent基线-spec.md`

实施范围仅限 `agent/`，不修改 `./ssv`、`scripts/agent.sh`、`gst/`、`runner/`。
每个步骤完成后保持变更未暂存，供人工审阅；收到进入下一步的明确指示后再暂存。

## 步骤 1：引入依赖

涉及文件
- `agent/third_party/deer-flow`：新增 git submodule，固定 commit `8659fca8`
- `agent/pyproject.toml`：修改，新增依赖与 `[tool.uv.sources]`

涉及标识符
- `deerflow-harness`：editable path 指向 `third_party/deer-flow/backend/packages/harness`
- `qdrant-client`：向量存储客户端
- `redis`：保持 `>=5.0.0,<8`

接口与所有权影响
- 仅 `agent` 包新增依赖

验证
- `cd agent && uv sync`
- `uv run python -c "import deerflow.client, qdrant_client"`

## 步骤 2：agent/config.yaml

涉及文件
- `agent/config.yaml`：新增

涉及标识符
- `models` / `tools` / `sandbox` / `checkpointer` 配置段
- `sandbox`：LocalSandboxProvider，`allow_host_bash: false`

接口与所有权影响
- `DeerFlowClient` 启动时读取；工具与模型配置化

验证
- 配置可加载；`DeerFlowClient` 可构造

## 步骤 3：event_store（SQLite）

涉及文件
- `agent/src/ssv_agent/event_store/schema.py`：新增
- `agent/src/ssv_agent/event_store/sqlite_store.py`：新增
- `agent/tests/test_sqlite_store.py`：新增

涉及标识符
- `SsvEventStore`：事件/检测/证据/结果条目读写
- `insert_event` / `insert_detections` / `insert_evidence` / `insert_result`
- `query_events` / `get_event` / `get_result`

接口与所有权影响
- SQLite 连接由 `SsvEventStore` 持有并在关闭时释放；事件消费与查询共用同一实例

验证
- `cd agent && uv run --extra dev pytest tests/test_sqlite_store.py`

## 步骤 4：Qdrant 客户端与集合初始化

涉及文件
- `agent/src/ssv_agent/event_store/qdrant_store.py`：新增
- `agent/tests/test_qdrant_store.py`：新增

涉及标识符
- `QdrantStore`：`ensure_collections` / `upsert_event_vector` / `upsert_rule_vector` /
  `search_events` / `search_rules`
- 集合：`ssv_events`、`ssv_rules`

接口与所有权影响
- 本地嵌入模式 `QdrantClient(path=...)`；配置可切换服务模式

验证
- 单元测试使用临时目录；集合初始化幂等

## 步骤 5：RAG 上层抽象

涉及文件
- `agent/src/ssv_agent/knowledge/schema.py`：新增
- `agent/src/ssv_agent/knowledge/retriever.py`：新增
- `agent/src/ssv_agent/knowledge/ingester.py`：新增
- `agent/src/ssv_agent/knowledge/registry.py`：新增
- `agent/src/ssv_agent/knowledge/backends/mock.py`：新增
- `agent/tests/test_knowledge.py`：新增

涉及标识符
- `Chunk` / `RetrievalResult` / `IngestResult`
- `Retriever` / `Ingester`：抽象接口
- `register_backend` / `get_retriever` / `get_ingester`
- `MockRetriever` / `MockIngester`：占位后端

接口与所有权影响
- 工具层只依赖 `Retriever` 接口；后端通过 registry 按配置切换

验证
- `cd agent && uv run --extra dev pytest tests/test_knowledge.py`

## 步骤 6：媒体工具

涉及文件
- `agent/src/ssv_agent/media/ffmpeg_utils.py`：新增
- `agent/src/ssv_agent/media/stream_probe.py`：新增
- `agent/src/ssv_agent/media/analyze_video.py`：新增
- `agent/src/ssv_agent/media/sample_video.py`：新增
- `agent/src/ssv_agent/media/extract_snapshot.py`：新增
- `agent/src/ssv_agent/media/extract_clip.py`：新增
- `agent/tests/test_media_tools.py`：新增

涉及标识符
- `run_ffmpeg` / `run_ffprobe`：子进程封装
- `stream_probe` / `analyze_video` / `sample_video` / `extract_snapshot` /
  `extract_clip`：媒体工具函数

接口与所有权影响
- 子进程统一超时；RTSP 工具不随机 seek；产物写入 `outputs/media/`

验证
- 单元测试用本地样例视频；RTSP smoke 由环境变量控制，无源时跳过

## 步骤 7：融合检索

涉及文件
- `agent/src/ssv_agent/search/decompose.py`：新增
- `agent/src/ssv_agent/search/attribute_search.py`：新增
- `agent/src/ssv_agent/search/embed_search.py`：新增
- `agent/src/ssv_agent/search/fusion.py`：新增
- `agent/tests/test_search.py`：新增

涉及标识符
- `DecomposedQuery`：查询分解结果
- `attribute_search` / `embed_search`：双路检索
- `fusion`：加权合并与去重
- `SearchResultItem`：统一结果模型

接口与所有权影响
- `search_events` 工具组合四者；分解失败时降级为纯文本查询

验证
- `cd agent && uv run --extra dev pytest tests/test_search.py`

## 步骤 8：RAG 工具

涉及文件
- `agent/src/ssv_agent/tools/rule_retriever.py`：新增
- `agent/src/ssv_agent/tools/rule_explainer.py`：新增
- `agent/config.yaml`：修改，注册工具
- `agent/tests/test_rule_tools.py`：新增

涉及标识符
- `rule_retriever`：调用 `Retriever.retrieve`
- `rule_explainer`：结合 chunk 生成带来源解释

接口与所有权影响
- 工具只依赖 RAG 抽象；后端不可用时返回 `success=false`，不中断复核

验证
- `cd agent && uv run --extra dev pytest tests/test_rule_tools.py`

## 步骤 9：复核闭环

涉及文件
- `agent/src/ssv_agent/review_context.py`：新增
- `agent/src/ssv_agent/prompt.py`：新增
- `agent/src/ssv_agent/runner.py`：新增
- `agent/src/ssv_agent/result.py`：新增
- `agent/src/ssv_agent/event_consumer.py`：修改
- `agent/src/ssv_agent/service.py`：修改
- `agent/tests/test_review_context.py`、`test_prompt.py`、`test_runner.py`、
  `test_result.py`、`test_event_consumer.py`、`test_service.py`：新增/更新

涉及标识符
- `ReviewContext.from_event` / `build_review_prompt` / `run_review`
- `parse_result_markdown` / `write_result_markdown`
- `EventConsumer` / `run_consumer` / `run`

接口与所有权影响
- 消费事件时先写 `event_store`，再复核；成败都 XACK，不重试

验证
- `cd agent && uv run --extra dev pytest tests/test_review_context.py tests/test_prompt.py tests/test_runner.py tests/test_result.py tests/test_event_consumer.py tests/test_service.py`

## 步骤 10：独立样例事件注入脚本

涉及文件
- `agent/scripts/enqueue_event.py`：新增
- `agent/tests/test_enqueue_event.py`：新增

涉及标识符
- `build_sample_event`：构造 ssvpub schema 事件
- `main`：argparse + `XADD` 到 `ssv:events`

接口与所有权影响
- 独立脚本，不进入 `ssv_agent` 包，不修改 `./ssv`

验证
- `uv run --isolated --script agent/scripts/enqueue_event.py --help`
- `cd agent && uv run --extra dev pytest tests/test_enqueue_event.py`

## 步骤 11：全量验证与交付

验证命令
- `cd agent && uv run --extra dev pytest`
- 手工冒烟：
  1. 启动本地 Redis；
  2. 注入样例事件；
  3. 启动 `agent` 服务消费；
  4. 检查 `outputs/<event_id>/result.md`；
  5. 注入多条事件后验证 `search_events` 属性与语义检索；
  6. 无 RTSP 源时跳过媒体工具 smoke，并记录原因。

交付动作
- 检查 `git status` 与 `git diff`，确认只包含本计划涉及文件；
- 暂存、提交并更新 PR，PR 描述指向本 spec 与 plan。
