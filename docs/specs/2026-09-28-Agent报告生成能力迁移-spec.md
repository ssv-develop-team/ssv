# Agent 报告生成能力迁移规格

## 1. 背景

VSS 的报告链路把结构化分析结果整理成可展示的 Markdown，并返回报告内容和可访问资源。SSV 已经由 `ReportWorker` 按指定 `ReviewRecord` 生成确定性 Markdown artifact，但当前 Agent 工具只能查询事件和复核 JSON，不能安全读取已生成的报告，也不能把报告交给 DeerFlow 输出目录展示。

本阶段把 VSS 报告链路中与 SSV 事实边界相容的部分迁入：保留 SSV 的 SQLite 账本、历史 revision、内容寻址和 lease 语义，增加报告的只读查询与受控展示入口。

## 2. 目标

- 继续使用固定模板从 `EventCase` 和指定历史 `ReviewRecord` 生成报告，不在展示阶段调用模型或重新判断。
- 增加 `get_report` 只读工具，按 `event_id` 和可选 `review_id` 读取已经登记的报告；省略 `review_id` 时选择最新报告。
- 在 `get_event` 返回已登记报告的安全元数据，方便 Agent 判断报告是否已经完成。
- 校验报告 artifact 位于 `SSV_OUTPUTS_DIR`，是普通文件，大小和 SHA-256 与 SQLite `ReportRecord` 一致后才可读取。
- 在 DeerFlow review thread 的 outputs 目录生成受控 Markdown 副本，返回虚拟路径和报告正文；虚拟路径只能指向当前线程输出。
- 返回报告关联的 `event_id`、`review_id`、`evidence_id` 资源引用，证据继续通过 `evidence_reader` 获取。

## 3. 接口契约

### 3.1 `get_report`

工具参数：

- `runtime`：DeerFlow 自动注入的运行时。
- `event_id`：SQLite 账本中的事件 ID。
- `review_id`：可选的历史复核 ID；省略时使用该事件最新的已登记报告。
- `include_content`：是否返回 Markdown 正文，默认 `true`。

成功响应为 JSON 对象，包含：

- `found: true`、`event_id`、`review_id`、`review_revision`；
- `template_version`、`sha256`、`size_bytes`、`created_ms`；
- `content`（`include_content=true` 时）;
- `virtual_path`（thread outputs 可用时）；
- `evidence_refs`，每项包含 `event_id`、`evidence_id`、`reader: "evidence_reader"`。

报告不存在、事件不存在、artifact 被删除或校验失败时返回 `found: false` 和稳定的 `reason`，不泄露宿主机路径。

### 3.2 `get_event`

事件详情增加 `reports` 数组。每项只包含 `report_id`、`review_id`、`review_revision`、`template_version`、`sha256`、`size_bytes` 和 `created_ms`，不包含 `artifact_path`。

## 4. 安全与所有权

- `ReportWorker` 是报告生成和 `ReportRecord` 登记的唯一持久入口；`get_report` 不修改事件、复核、报告登记或 durable job 状态。
- artifact 路径必须解析到 `SSV_OUTPUTS_DIR` 内，拒绝 symlink、目录、越界路径、大小不一致和 SHA-256 不一致的文件。
- thread 输出文件名由事件、复核和内容摘要生成，工具不接受宿主机路径、目标目录或复制参数。
- 报告正文中的事件字段、复核解释和规则来源继续按现有 renderer 规则转义，并隐藏本地路径。
- 证据不复制进报告 artifact；报告只提供稳定的 `event_id/evidence_id` 引用，模型需要图片或视频时继续调用 `evidence_reader` 和 `sample_video`。

## 5. 非本阶段范围

- 不引入 VSS/NAT object store、PDF 转换、HTTP 报告服务或公开下载 URL。
- 不把 `context.mp4` 或其他宿主机路径写入报告或直接交给模型。
- 不增加跨事件/时间范围聚合报告，不自动回填历史 review，也不改变报告 worker 的重试、lease 和 fencing 语义。
- 不新增第二次 LLM 调用，不让报告内容成为事件或复核事实源。

## 6. 验收标准

1. 已登记报告可以按最新报告或指定 `review_id` 读取，报告正文与 artifact SHA-256 一致。
2. 删除、替换、越界或 symlink artifact 时工具 fail closed，响应不含宿主机路径。
3. `get_event` 只返回报告元数据，保留现有事件字段和证据路径隐藏行为。
4. thread outputs 可用时返回 `/mnt/user-data/outputs/...` 虚拟路径，副本不越界且重复读取不破坏已有内容。
5. review 工具配置和 fail-closed RBAC 同时包含 `get_report`，既有只读工具仍保持可用。
6. 报告 worker、账本和历史 revision 行为保持不变；自动化测试覆盖成功、未找到、完整性失败和路径安全边界。
