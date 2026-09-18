# 检测事件 Episode 与精确证据窗口实施计划

## 步骤 1：建立 episode 数据模型与配置

涉及文件：

- `agent/src/ssv_agent/config.py`
- `agent/src/ssv_agent/review_context.py`
- `agent/src/ssv_agent/event_store/schema.py`
- `agent/src/ssv_agent/event_store/ledger.py`
- `agent/src/ssv_agent/event_store/sqlite_store.py`
- `agent/src/ssv_agent/event_store/__init__.py`

工作内容：

1. 为 `RecordingEvidenceConfig` 增加 episode 合并、丢失宽限和最大时长配置及范围校验。
2. 为 `events`、`episodes`、`episode_observations` 增加可重复迁移；保留历史事件、
   durable job、lease 和 review/index 数据。
3. 增加 `EventEpisode`、`EpisodeObservation`、`EventCase.episode_id` 和显式证据窗口。
4. 在 `EventLedger` 内提供 observation 幂等、episode 打开/更新/关闭、job 查询和
   canonical case 快照接口。

验收：新旧 SQLite 都能启动；历史 schema 不丢数据；配置默认值和错误边界有测试。

## 步骤 2：实现 EventEpisodeAggregator

涉及文件：

- `agent/src/ssv_agent/event_episode.py`：新增模块
- `agent/src/ssv_agent/event_store/ledger.py`
- `agent/src/ssv_agent/event_store/__init__.py`

工作内容：

1. 以 `(source, event_type, rule_id, rule_version, generation)` 查找可续接 episode。
2. 把首次观测转换为 canonical event；后续观测只写 observation 并更新 PTS 和状态。
3. 处理 `NEW/MATCHED/LOST/DEAD`、显式结束阶段、merge gap、lost grace、generation
   变化和 max episode 时长。
4. 以 `source_pts` 计算冻结窗口；只使用 `timestamp_ms` 做任务延迟，不作为媒体锚点。

验收：纯 SQLite/内存 Redis 测试覆盖打开、合并、重投幂等、恢复、关闭和并行事务。

## 步骤 3：接入 Consumer 与任务链

涉及文件：

- `agent/src/ssv_agent/event_consumer.py`
- `agent/src/ssv_agent/event_store/ledger.py`
- `agent/src/ssv_agent/workers.py`

工作内容：

1. 保留 malformed/invalid ingress 的 ACK 行为和账本失败 pending 行为。
2. 将 dedup 结果交给 episode aggregator；`SKIP` 只跳过重复的 canonical 业务副作用，
   不跳过新 ingress 的 episode lifecycle update。
3. Consumer 空闲轮询时 flush 到期的 `LOST_GRACE`/静默 episode；停止时只 flush 到期
   状态，不强制关闭仍开放的 Episode，以便进程重启后继续聚合。
4. 关闭 episode 时只创建一个 extract job；extract 成功后继续创建一个 review job。

验收：旧 Consumer 测试保持通过，并新增“dedup skip 仍更新时间”和“一个 episode 一个
extract job”的集成测试。

## 步骤 4：让 extractor 使用冻结窗口

涉及文件：

- `agent/src/ssv_agent/ssv_cache_evidence.py`
- `agent/tests/test_ssv_cache_evidence.py`

工作内容：

1. 让 extractor 读取 `EventCase.evidence_window_start/end`。
2. 校验窗口 generation 与 case generation 一致，manifest 记录 episode 窗口和实际输入。
3. 保留没有 episode 窗口时的旧单事件兼容路径，避免破坏已有测试和历史任务。

验收：episode 窗口不是固定 `source_pts +/- clip_*`；跨代际、未 finalized 和不完整
窗口仍拒绝或重试。

## 步骤 5：更新样例、文档与验证

涉及文件：

- `config/ssv.example.yaml`
- `docs/Agent配置.md`
- `docs/roadmap.md`
- `agent/tests/test_config.py`
- `agent/tests/test_event_consumer.py`
- `agent/tests/test_event_ledger.py`
- 新增 `agent/tests/test_event_episode.py`

工作内容：

- 解释 episode、dedup、tracker lifecycle、source PTS 和 cache 的所有权。
- 明确一个 episode 只生成一个视频段落，MediaMTX 不再作为 Agent provider。
- 完成 Python focused tests、Agent 全量 tests、Ruff、必要的 Meson tests 和
  `git diff --check`。

## 兼容、回滚与交付边界

- 默认开启 episode 配置只在 `recording_evidence.enabled=true` 的链路生效；关闭取证
  时保留原有每事件 review/index 语义。
- 不修改 GStreamer cache 的连续写盘实现，不跨 `stream_generation` 拼接。
- 不暂存、不提交、不推送、不创建 PR；所有改动保持工作树未暂存供人工审阅。
