# GStreamer RTSP 编码旁路

## 目标

当 SSV 只需要把输入 H.264 视频重新发布到 RTSP，且不需要把检测框烧录到
视频像素时，避免执行解码、颜色转换和再次编码。这样可以降低 CPU/GPU 占用、
内存带宽和输出延迟，并保留独立的检测、跟踪和事件分支。

## 配置契约

```yaml
display:
  enabled: true
  backend: "rtsp"
  rtsp:
    location: "rtsp://127.0.0.1:8554/ssv"
    burn_in_overlay: false
    encoded_passthrough: true
```

- `display.rtsp.encoded_passthrough` 默认为 `false`，关闭时保持现有 RTSP
  解码、转换、编码路径。
- 该配置只对 `display.backend: rtsp` 有效；其他 display backend 不创建编码
  旁路。
- `encoded_passthrough: true` 与 `burn_in_overlay: true` 互斥。编码旁路不能
  修改已经编码的 H.264 图像像素。
- 旁路模式直接保留输入码流的时间戳、帧率、分辨率和编码参数，`display.fps`
  不参与旁路分支的抽帧。需要改变帧率、绘制 overlay 或改变编码参数时，继续
  使用默认的解码后输出路径。
- 当前 source codec 契约固定为 H.264，因此旁路不负责转码或编解码格式转换。

## Pipeline

编码旁路开启且存在分析分支时：

```text
rtspsrc -> rtph264depay -> h264parse -> encoded-tee
                                      |-> queue -> rtph264pay -> rtspclientsink
                                      |-> decoder -> decoded-tee -> analysis
```

没有分析分支时，`decoder`、`decoded-tee` 和 decoded discard branch 都不创建；
`encoded-tee` 仍按需用于连接 RTSP 输出和证据缓存。这样旁路模式可以完全跳过
输出无关的解码，也不要求解析无关的 decoder factory 能力。此时 plan 中的
`decode.decoder_factory` 仅保留为 `not-applicable` 兼容标记，不参与建图。
plan 的 `decoded_path_required` 是 builder 判断是否创建 decoder 和 decoded tee
的唯一来源。

如果同时启用证据缓存，`encoded-tee` 继续向 `splitmuxsink` 提供独立的编码
分支。RTSP sink 的网络阻塞不能直接占用分析分支的 queue；旁路 queue 使用有界
容量，且不丢弃编码 buffer，避免任意丢弃 H.264 访问单元破坏码流。

编码旁路不开启，或开启了 `burn_in_overlay` 时，保持现有路径：

```text
decoded-tee -> queue -> videorate -> [overlay] -> H.264 encoder
```

## 错误语义

- `encoded_passthrough` 与 `burn_in_overlay` 同时为 `true` 时，配置或 pipeline
  plan 返回 `InvalidConfiguration`。
- 旁路模式只要求 `rtph264pay` 和 `rtspclientsink`，不要求 H.264 encoder。
- 纯旁路且未启用推理时不要求任何 H.264 decoder；启用推理时只为分析分支创建
  decoder 和 decoded tee。
- 旁路模式中的 RTSP sink 启动、网络和服务端错误仍归属于 display 输出分支。

## 非本阶段范围

- 不实现 GPU OSD、硬件 H.264 编码或平台专用零拷贝转换。
- 不实现 RTSP server、认证、鉴权或服务发现。
- 不修改事件、跟踪、Redis 发布、证据缓存和 Agent 数据契约。
- 不让旁路模式支持 `display.fps` 限流；需要限流时使用解码后路径。

## 验收标准

- 默认 `encoded_passthrough: false` 的 topology 与现有测试保持一致。
- 旁路 plan 不选择 H.264 encoder，且只要求 `rtph264pay`、
  `rtspclientsink`。
- 旁路 topology 从 `encoded-tee` 连接到 `queue -> rtph264pay ->
  rtspclientsink`，不包含 `videorate`、`videoconvert`、`ssvoverlay` 或 encoder。
- 旁路与分析分支、证据缓存分支可以同时存在。
- 互斥配置得到明确错误，配置、plan、builder 测试通过。
