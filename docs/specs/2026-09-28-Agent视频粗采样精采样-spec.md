# Agent 视频粗采样与精采样规格

## 1. 背景

当前 `SsvCacheEvidenceExtractor` 会为事件生成 `context.mp4` 和三张固定关键帧。
复核模型只能通过 `view_image` 查看 JPEG，不能读取 MP4，也不能根据画面变化请求新的
时间点。固定三帧无法覆盖短暂动作、遮挡变化或事件窗口内的时间定位误差。

参考 `deerflow-mydemo` 的 `sample_video` 工具，将视频读取拆为“先粗采样、再按需要
精采样”的模型工具调用；参考 VSS 的按 chunk 固定帧数方式，保持每次调用有界，避免把
连续视频帧无上限地送入模型。

## 2. 目标

- 为 review Agent 增加一个受控的 `sample_video` 工具。
- 粗采样在已登记的 `context.mp4` 全窗口内均匀抽取少量 JPEG。
- 精采样在模型指定的 clip 相对时间窗口内均匀抽取少量 JPEG。
- 采样结果使用 DeerFlow `/mnt/user-data/outputs/...` 虚拟路径，交给现有
  `view_image` 逐张查看。
- 采样输入只接受 `event_id` 和已登记的 `clip evidence_id`，不接受宿主机路径。
- 采样结果携带相对毫秒位置、source PTS 和原始证据 ID，便于模型把观察结果和证据引用
  对齐。
- 采样输入在 ffmpeg 开始前固定到临时目录；runner 或其他清理动作不会改变本次采样的
  输入内容。

## 3. 复核流程

```text
evidence_reader(event_id)
  -> sample_video(event_id, mode=coarse, frames=4)
  -> view_image(coarse frame) * N
  -> （时间或动作仍不确定时）sample_video(mode=fine, interval_start_ms, interval_end_ms)
  -> view_image(fine frame) * N
  -> get_event / rule_retriever / search_events
  -> JSON 复核结果
```

粗采样必须先于精采样。精采样是可选的，由模型根据粗采样的时间定位和事件问题决定；
模型可以使用工具返回的 `recommended_fine_interval_ms` 作为初始窗口。

## 4. 工具接口

工具名称为 `sample_video`，参数如下：

| 参数 | 类型 | 约束 |
| --- | --- | --- |
| `event_id` | string | SQLite 账本中的事件 ID |
| `evidence_id` | string，可省略 | 必须指向该事件已登记且可用的 `kind=clip` 证据；省略时只能在事件只有一个可用 clip 时自动选择 |
| `mode` | `coarse` 或 `fine` | 默认 `coarse` |
| `frames` | integer | 1 至 8，默认 4 |
| `interval_start_ms` | integer，可选 | 仅 `fine` 使用，必须不小于 0 |
| `interval_end_ms` | integer，可选 | 仅 `fine` 使用，必须大于 start 且不超过 clip 时长 |

工具返回 JSON：

```json
{
  "available": true,
  "event_id": "...",
  "evidence_id": "...",
  "mode": "coarse",
  "clip_duration_ms": 5000,
  "interval_start_ms": 0,
  "interval_end_ms": 5000,
  "event_offset_ms": 2500,
  "recommended_fine_interval_ms": [1500, 3500],
  "frames": [
    {
      "index": 1,
      "offset_ms": 625,
      "source_pts": 1234567890,
      "virtual_path": "/mnt/user-data/outputs/samples/.../frame-001.jpg"
    }
  ]
}
```

失败返回 `available=false`、稳定的 `reason` 和空 `frames`；失败不会登记新 evidence。

## 5. 时间与采样语义

- clip 的时长来自登记的 `source_pts_start` 与 `source_pts_end`，单位转换为毫秒；缺少
  这两个字段时工具拒绝采样，避免把 wall clock 当成媒体时间。
- 每个采样点位于对应区间的中心：`start + (i + 0.5) * (end - start) / frames`。
- `coarse` 使用 `[0, clip_duration_ms)`；`fine` 使用调用者提供的半开区间。
- `source_pts = source_pts_start + offset_ms * 1_000_000`，结果仍标记为
  `wall_clock_approximate` 的视觉上下文，不能声称是检测帧。
- 每次调用最多 8 张 JPEG，统一缩放到宽度 640；临时文件成功后原子 rename 到输出目录。

## 6. 安全与生命周期

- 工具通过 `EventLedger.refresh_evidence()` 和 `resolve_evidence_path()` 校验证据；
  路径越界、symlink、非普通文件或 evidence 不可用都失败关闭。
- 模型不能传入 `path`、ffmpeg 参数或输出目录。输出名称由事件、证据、模式和窗口的哈希
  组件生成，避免路径穿越和跨事件覆盖。
- ffmpeg 只读取临时固定输入；采样输出写入当前 DeerFlow review thread 的 outputs 目录。
- 临时输入和失败的临时输出在 finally 中清理；采样 JPEG 属于线程输出，随 DeerFlow
  输出生命周期管理，不写入 Agent 长期 evidence 目录。
- `sample_video`、`evidence_reader`、`get_event`、`rule_retriever`、`search_events`
  和内置 `view_image` 继续受 review worker 的 fail-closed RBAC 限制。

## 7. 非本阶段范围

- 不把 MP4 直接作为多模态消息上传给模型。
- 不做音频、OCR、目标 ROI、镜头切分或自动动作检测。
- 不将每次采样出的 JPEG 登记为新的 SQLite evidence。
- 不改变 runner 连续缓存清理策略、Episode 窗口和已有四文件派生证据契约。
- 不允许 Agent 读取任意宿主机视频路径。

## 8. 验收标准

1. review 配置包含 `sample_video`，且 RBAC 只允许该只读工具。
2. 粗采样返回 1 至 8 个均匀时间点和可用虚拟图片路径。
3. 精采样拒绝缺失窗口、反向窗口和越过 clip 边界的请求。
4. 删除或替换原始 clip 路径不会影响已经固定到临时目录的当前采样。
5. 采样输出目录只包含 JPEG，不会把输入 MP4 或模型传入的路径复制为长期证据。
6. prompt 要求先粗采样，再按需要精采样，并继续只引用登记的 evidence ID。
