# 检测事件 Episode 与精确证据窗口规格

## 背景

SSV 已经由 GStreamer 持续写入带 `source_pts` 和
`stream_generation` 的短时缓存。当前 Agent 仍把每次 Redis 事件当作独立案件，
并在固定的单点事件窗口上创建取证任务。检测事件通常由同一条 track 的开始、持续、
丢失和结束组成；按单条消息取证会产生重复片段，也可能在目标尚未离开时过早截断。

本规格把 Redis 事件的去重结果与检测生命周期关联起来：同一 source、规则和流代际
内的连续观测聚合为一个 episode，episode 结束后只生成一个精确的 PTS 证据窗口。

## 目标

- 保留现有 Redis ingress 的幂等、ACK、lease 和 SQLite 事务语义。
- 不因 Redis 冷却去重而丢失 episode 的 `last_seen_pts`、结束状态和流代际信息。
- 将同一生命周期内的事件合并为一个 episode，避免每条重复消息创建取证任务。
- 以 `[start_pts - pre_roll, end_pts + post_roll)` 作为唯一取证窗口，并禁止跨
  `stream_generation` 拼接。
- 让 `LOST` 进入宽限期；收到 `MATCHED/NEW` 时恢复，收到 `DEAD` 或超时后关闭。
- 保留一个 episode 对应一个 canonical event case，使既有 `get_event`、复核和索引
  工具继续可用。

## 非本阶段范围

- 不在 Agent 侧用 IoU 或外观特征重新关联人员；`track_id` 和 `track_state` 仍以
  GStreamer tracker 发布的结果为准。
- 不让 episode 控制 GStreamer cache 写盘；cache 始终连续运行并按自身 retention 清理。
- 不从 `timestamp_ms`、`frame_id` 或 Redis entry ID 推导媒体 PTS。
- 不恢复 MediaMTX provider，也不实现长期录像、回放或跨设备归档。
- 不改变复核模型可访问的证据登记规则；模型仍只能读取账本已登记的派生文件。

## 术语与生命周期

一个 episode 的 canonical event 是首次打开该 episode 的有效 Redis 事件。后续事件只
作为 episode observation 保存，不再创建独立的 review/index/evidence_extract 工作。
生产 `EventConsumer` 在取证开启时通过 Episode 聚合入口处理事件；`EventLedger.record()`
仍保留为历史调用方和测试使用的兼容入口，直接调用它不会替代 Episode 聚合，也不应被
当作生产 Redis 消费路径。

```text
OPEN -> LOST_GRACE -> OPEN
  |        |
  |        `-> CLOSED (lost grace expired)
  `-> CLOSED (DEAD, explicit END, generation change, max duration)
```

聚合键为：

```text
(source, event_type, rule_id, rule_version, stream_generation)
```

当新观测与同键 episode 的 `last_seen_pts` 间隔不超过 `merge_gap_ms` 时合并；
间隔过大时先关闭旧 episode，再打开新 episode。没有完整 `source_pts` 或
`stream_generation` 的事件不能进入精确 episode，只保留现有事件记录语义并不创建精确
取证窗口。

状态判断使用 tracker 发布的字段：

- `NEW` 或 `MATCHED`：打开或续接 episode，并更新 `last_seen_pts`。
- `LOST`：更新最后观测，进入 `LOST_GRACE`，不立即关闭。
- `DEAD` 或显式 `event_phase=end`：关闭 episode。
- Redis 去重返回 `SKIP`：只跳过重复的业务副作用；只要 ingress 本身未处理过，仍
  作为 episode observation 更新生命周期。

## 持久化契约

新增 `episodes` 表保存当前状态：

| 字段 | 语义 |
| --- | --- |
| `episode_id` | 稳定 ID，同时作为 canonical event 的关联 ID |
| `canonical_event_id` | 首次打开 episode 的 event ID |
| `source`、`event_type`、`rule_id`、`rule_version` | episode 聚合键的非代际部分 |
| `stream_generation` | 不允许跨代拼接的流代际 |
| `start_pts` | episode 首次观测 PTS |
| `last_seen_pts` | 最近一次有效观测 PTS |
| `end_pts` | 关闭时的最后有效观测 PTS |
| `state` | `open`、`lost_grace` 或 `closed` |
| `close_reason` | `dead`、`timeout`、`generation_change`、`max_duration` 等 |
| `evidence_window_start`、`evidence_window_end` | 关闭后冻结的精确窗口 |
| `event_ids_json` | 已接收事件 ID 的有序去重列表 |

新增 `episode_observations` 表，以 `ingress_id` 唯一约束 Redis 重投幂等，并记录
`event_id`、`source_pts`、`track_state` 和观测时间。Episode 写入、观测写入、
canonical event 关联和关闭时的 job 创建必须在同一个 SQLite 短事务内完成。

`events` 增加 `episode_id`。canonical event 的 `EventCase` 暴露 episode ID 和冻结
窗口；后续观测不会被当作可复核的独立案件。Episode 关闭时只创建一个
`kind=evidence_extract`、`entity_id=canonical_event_id`、`entity_revision=0` 的 job。
因此后续观测的 `event_id` 只用于 `episode_observations` 幂等和审计；需要复核、取证或
索引时使用 `canonical_event_id` 查询案件。

## 证据窗口与调度

关闭时使用：

```text
window_start = max(0, start_pts - pre_roll_ms * 1_000_000)
window_end   = end_pts + post_roll_ms * 1_000_000
```

窗口字段一旦冻结不得因重复关闭再次变化。Job 的 `available_at_ms` 只用于等待
`post_roll` 对应的墙钟近似时间；extractor 选择缓存分段和生成 manifest 时必须使用
episode 冻结的 PTS 窗口。若缓存窗口尚未 finalized，沿用现有可重试失败语义。

`SsvCacheEvidenceExtractor` 优先使用 `EventCase` 的显式窗口；没有显式窗口时才兼容
旧的单事件 `source_pts +/- clip_before/after_ms` 行为。Episode 生成的派生文件目录仍
以 canonical event ID 命名，便于既有 evidence reader 和 review 结果保持稳定。

## 配置契约

`agent.recording_evidence` 增加：

```yaml
agent:
  recording_evidence:
    merge_gap_ms: 3000
    lost_grace_ms: 3000
    max_episode_ms: 30000
```

- `merge_gap_ms` 必须大于 0，控制同一生命周期可合并的 PTS 间隔。
- `lost_grace_ms` 必须大于等于 0，控制 `LOST` 后等待恢复的墙钟近似时长。
- `max_episode_ms` 必须大于 `merge_gap_ms`，防止没有终态的事件无限延长。

`clip_before_ms` 和 `clip_after_ms` 继续作为 episode 的 pre-roll/post-roll。取证开启
仍要求 `evidence_cache.enabled=true` 且 cache 位于 `evidence_roots` 内。

## 失败语义

- Redis 重投：由 `episode_observations.ingress_id` 幂等识别，重复执行不延长 episode。
- SQLite 写入失败：Consumer 不 ACK，Redis pending 可由现有 reclaim 流程重试。
- cache 未 finalized 或窗口不完整：保留 evidence job pending/retry，不生成缩短证据。
- generation 变化：关闭旧 episode，旧窗口不跨代；新代际另开 episode。
- episode 超过最大时长：以最后观测关闭并创建一次 job，后续事件开启新 episode。
- 达到取证重试上限：沿用 `manual_review`，不创建无证据的 review job。
- Consumer 停止时只执行一次到期 Episode flush，不强制关闭仍处于 `open` 或未到期
  `lost_grace` 的 Episode；进程重启后可继续聚合，明确的 `DEAD`/结束事件或超时才会
  冻结窗口。

## 验收标准

1. 同键、短间隔事件只产生一个 episode 和一个取证 job，`last_seen_pts` 正确前移。
2. `SKIP` 事件仍能更新 episode；同一 Redis ingress 重投不会重复更新。
3. `LOST` 在宽限期内收到 `MATCHED` 会恢复，宽限期后才关闭。
4. `DEAD`、静默超时、generation 变化和最大时长都能关闭并冻结窗口。
5. 窗口以 episode 的 `start_pts/end_pts` 加 pre/post-roll 计算，extractor 使用该窗口。
6. 既有非 episode 事件、review/index worker、证据路径校验和 MediaMTX 删除后的配置
   契约不回退。
