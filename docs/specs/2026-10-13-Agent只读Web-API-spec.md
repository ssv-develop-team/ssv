# Agent 只读 Web API 规格

## 目标

在 Python Agent 包中提供供同源转发前端读取的 HTTP API。服务单独通过 `ssv-agent web` 启动，加载现有 `SsvConfig`，只读取配置指定的 SQLite EventLedger 和已登记的图片证据。

## 接口

- `GET /api/v1/overview` 返回 SQLite 中事件总数、pending、manual_review、completed、violation 事件数、账本登记为不可用的证据数，以及按 `timestamp_ms` 倒序的最多 8 条最近事件。事件统计以 `events` 中每个 `event_id` 为单位，不读取 Redis Stream。
- `GET /api/v1/events` 支持 `query`、`status`、`page`、`page_size`。`query` 以大小写无关的 event_id、source、event_type 子串匹配。筛选和分页使用参数化 SQL。status 仅接受 pending、manual_review、completed；页码从 1 起，页大小为 1 至 100。分页顺序为 `timestamp_ms` 倒序，再按 `event_id` 升序，返回 `{items,total,page,page_size}`。
- `GET /api/v1/events/{event_id}` 返回事件摘要、检测、追加式复核历史和已登记证据。当前 revision 取同一事件复核记录的最大 revision；结论与置信度从该记录读取，置信度保持数值。历史按 revision 升序，复核证据引用和证据记录只按 `(event_id,evidence_id)` 关联。检测不返回 `bbox_json`，因为目前没有可确认的坐标空间与图片尺寸。
- `GET /api/v1/events/{event_id}/evidence/{evidence_id}/image` 仅返回该事件已登记且通过校验的 frame 图片。先验证事件和证据 ID 关联，再使用服务端允许的 evidence roots 安全打开文件；检查普通文件、图片内容和账本元信息。响应给出实际图片 `Content-Type`、`X-Content-Type-Options: nosniff` 和 `Cache-Control: private, no-store`。

事件摘要字段为 `event_id`、原始 `source`、`timestamp_ms`、`event_type`、`severity`、工作流 `status`、`analysis_task`、`current_revision`、`verdict`、`confidence`。检测字段至少为 `class_name`、`class_id`、`confidence`、`track_id`、`track_state`、`occluded`。复核历史字段为 `event_id`、`review_id`、`revision`、`created_ms`、`verdict`、`confidence`、`evidence_status`、`explanation`、`evidence_ids`。证据字段为 `event_id`、稳定 `evidence_id`、`kind`、`status`、`mime_type`、`size`，以及账本已有的 `coverage`。

## Source 标识边界

事件中的 `source` 是 SSV 配置的 `sources[].id`，只作为 SSV 输入源标识，不隐含或推导 WVP 通道编码。
共享配置、EventLedger 和 Agent Web API 不维护 WVP 通道映射，也不提供按 WVP 通道编码的筛选字段。
需要在 WVP 页面关联事件时，应由独立 WVP 适配层或 BFF 管理映射；WVP 通道目录与设备管理不属于本 API。

## 工作流状态与 Agent 任务状态

`status` 是事件工作流状态，不是 worker 线程状态：新事件入账时为 `pending`；若录像上下文取证在重试耗尽后失败，或只保存了锚点图片而无法自动复核，会置为 `manual_review`；Agent 复核结果写入账本后置为 `completed`。`completed` 只表示复核结果已持久化，不代表违规判定为否，也不代表人工已处理。

摘要的 `analysis_task` 来自 SQLite `durable_jobs`，只返回 `{kind,state}`，不返回 worker 名称、租约持有者或服务器路径。`kind` 为 `review` 或 `evidence_extract`；`state` 为 `not_scheduled`、`waiting`、`queued`、`processing`、`lease_expired`、`completed` 或 `failed`。`waiting` 对应 `available_at_ms` 尚未到的 pending job，可能是首轮取证等待 post-roll，也可能是失败后的退避等待；`queued` 对应已到可领取时间的 pending job；`processing` 表示持久任务记录有未过期租约，不承诺操作系统线程此刻仍在执行；过期的 processing 租约返回 `lease_expired`；`dead` 映射为 `failed`。若无匹配 job 但事件已有复核 revision 或工作流状态为 `completed`，返回 `review/completed`；其他无匹配任务的事件返回 `kind = null, state = not_scheduled`。因此事件 `status = pending` 不能解释成 Agent 正在处理。

这里的“分析任务”指 Agent 的持久化复核/取证任务。当前事件账本与只读 API 不保存 SSV 实时推理帧、运行阶段或逐帧检测状态；若 Web 要显示实时推理状态，需要 runner 侧另行提供带 source/channel 关联的运行状态接口或遥测数据源，不能从历史 EventLedger 事件推断。

## 数据与安全约束

- SQLite 连接必须使用 URI `mode=ro`，并启用 `PRAGMA query_only=ON`。不通过 `EventLedger` 或 `SsvEventStore` 的写入构造路径打开数据库；服务启动和请求均不迁移 schema、不写账本、不连接 Redis。
- 复用 `agent.event_db_path`、`agent.evidence_roots` 和现有 EventLedger 数据口径。Web 监听配置由服务端 `agent.web` 提供，默认 `127.0.0.1:8765`；凭据不进入 Web 配置参数或响应。
- 不泄漏主机路径、凭据、SQL 或异常详情。统一错误体为 `{ "error": { "code": "...", "message": "..." } }`：事件不存在 404 `event_not_found`；证据不存在 404 `evidence_not_found`；clip 或非图片证据 415 `evidence_not_image`；已登记图片不可用或校验失败 410 `evidence_unavailable`；参数错误 422 `invalid_query`；SQLite 不可读 503 `read_store_unavailable`；未预期错误 500 `internal_error`。
- 缺少稳定 `evidence_id` 的旧记录不合成 ID；缺失的复核引用不合成证据记录。图片路径不得由客户端提交。

## 非本阶段范围

账号和权限、写入或人工复核、事件控制、视频和 Range 读取、播放会话、WVP 通道目录、设备管理及通道映射、多通道调度、检测框绘制、Redis Stream 读取、SSV 实时推理状态接口。
