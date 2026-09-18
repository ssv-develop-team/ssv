# RTSP 烧录 Overlay

## 目标

当 SSV 使用 `rtsp` display backend 时，可选择把检测框、类别、置信度和
track ID 绘制到视频像素中，再通过 `rtspclientsink` 发布。这样接收端只需要
播放普通 RTSP 视频，不需要理解 SSV 的检测元数据协议。

## 配置契约

```yaml
display:
  enabled: true
  backend: "rtsp"
  rtsp:
    location: "rtsp://127.0.0.1:8554/ssv"
    burn_in_overlay: true
  overlay:
    font:
      face: "regular"
      size: 12
    motion_prediction:
      enabled: true
      max_horizon_ms: 300
```

- `display.rtsp.burn_in_overlay` 只对 `display.backend: rtsp` 生效，默认值为
  `false`。
- `display.overlay.font` 和 `display.overlay.motion_prediction` 复用现有
  overlay 样式与展示预测配置。
- `display.overlay.enabled` 仍控制 GTK 独立 overlay；RTSP 烧录由
  `display.rtsp.burn_in_overlay` 显式控制，避免打开 RTSP 后意外增加编码前绘制开销。
- 非 RTSP backend 配置 `burn_in_overlay: true` 不改变 GTK 行为；配置键仍被解析，
  但 plan 不创建 `ssvoverlay` 阶段。
- 开启烧录时必须存在 `ssvoverlay` GStreamer element，否则 plan 以
  `CapabilityUnavailable` 失败。

## Pipeline

不开启烧录时保持现有 RTSP 分支：

```text
decoded-tee
  -> queue -> videorate
  -> [VAAPI postproc/download]
  -> videoconvert -> capsfilter(I420)
  -> H.264 encoder -> h264parse -> rtph264pay
  -> rtspclientsink
```

开启烧录时只扩展 RTSP display 分支：

```text
decoded-tee
  -> queue -> videorate
  -> [VAAPI postproc/download]
  -> videoconvert -> capsfilter(BGRx)
  -> ssvoverlay
  -> videoconvert -> capsfilter(I420)
  -> H.264 encoder -> h264parse -> rtph264pay
  -> rtspclientsink
```

`ssvoverlay` 使用 builder 注入的 `source-id` 和 `source-context`，从同一条
分析链的 `SsvSourceMeta` 查询已发布的 immutable tracked snapshot。它按输出帧
PTS 选择不晚于当前 PTS 的 snapshot，并沿用现有的 generation 校验和可选位置预测。
overlay 阶段只修改 RTSP 分支中的 SystemMemory 视频 buffer，不修改分析、Redis
发布或证据缓存分支。

## Caps 合同

增加 `DisplayOverlayInput` 边界：

- format：`BGRx`
- memory：`SystemMemory`

RTSP 烧录分支仍保留 `DisplayEncodeInput` 合同：

- format：`I420`
- memory：`SystemMemory`

`ssvoverlay` 的输入和编码器输入分别安装 contract probe，任何协商不匹配都归类
为 display pipeline contract failure。

## 时间与延迟

分析分支和 RTSP display 分支从 `decoded-tee` 并行运行。overlay 不等待未来的
tracking 结果，只使用不晚于当前输出 PTS 的历史 snapshot；因此在推理延迟较高时
可能显示上一份框或暂时没有框。现有 motion prediction 只在 snapshot 已存在时
补偿短时位置变化，不伪造未来检测结果。

本阶段不引入隐藏的阻塞等待或无界缓存。若后续需要严格让每一帧都等待对应 tracking
结果，应另行增加有界 playout delay 和明确的延迟配置。

## 非本阶段范围

- 不改变 GTK overlay 的独立窗口绘制模型。
- 不通过 RTSP 发送可供接收端独立绘制的 bbox 元数据。
- 不实现 WVP/WebSocket/浏览器端的透明 overlay。
- 不改变 encoded tee、证据缓存、事件去重或 Redis payload。
- 不实现 RTSP server、认证或服务发现。

## 验收标准

- 默认配置生成的 RTSP topology 不包含 `ssvoverlay`。
- `burn_in_overlay: true` 且 `ssvoverlay` 可用时，topology 包含
  `capsfilter(BGRx) -> ssvoverlay -> videoconvert -> capsfilter(I420)`。
- 缺少 `ssvoverlay` 时，配置解析成功但 pipeline plan 明确返回能力不可用。
- RTSP 烧录分支正确注入 `source-id`、`source-context`、字体和预测配置。
- GTK、headless、证据缓存和分析分支行为保持不变。
- 配置、plan、topology、builder、contract 和运行时测试通过；真实 RTSP server
  发布及独立客户端播放仍作为环境验证项。
