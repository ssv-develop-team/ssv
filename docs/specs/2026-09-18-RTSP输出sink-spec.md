# RTSP 输出 Sink 选择

## 需求

SSV 的视频输出分支需要支持两类互斥的最终输出：

1. 本地 GTK 窗口，继续支持 `auto`、`gtkglsink` 和 `gtksink`。
2. 通过 GStreamer `rtspclientsink` 向外部 RTSP 服务发布视频。

RTSP 发布必须保留实时检测和跟踪分支；输出分支从解码后的画面开始，经过必要的 CPU 格式转换、H.264 编码、RTP 打包后连接到 `rtspclientsink`。SSV 不在本阶段实现 RTSP server。

## 配置契约

```yaml
display:
  enabled: true
  backend: "rtsp"
  rtsp:
    location: "rtsp://127.0.0.1:8554/ssv"
```

- `display.backend` 支持 `auto`、`gtkglsink`、`gtksink` 和 `rtsp`。
- `auto` 只在 GTK 后端之间选择，不会因为本机存在 RTSP 地址而自动发布。
- 选择 `rtsp` 时，`display.rtsp.location` 必须是非空的 `rtsp://` 或 `rtsps://` 地址。
- RTSP 输出需要 `rtspclientsink`、`rtph264pay` 和至少一个 GStreamer H.264 编码器；编码器按运行时可用能力选择。
- `display.fps` 继续作为输出分支的 drop-only 上限。
- `display.gl_backend` 和 `display.overlay` 只对 GTK 窗口生效，RTSP 输出不创建 GTK 窗口，也不携带独立 GTK overlay。

## Pipeline 形态

GTK 输出保持既有解码后显示分支。RTSP 输出使用以下阶段：

```text
decoded-tee
  -> queue -> videorate
  -> [VAAPI postproc/download]
  -> videoconvert -> capsfilter(I420)
  -> H.264 encoder -> h264parse -> rtph264pay
  -> rtspclientsink(location=display.rtsp.location)
```

输出阶段继续由 pipeline plan 做能力检查和 caps 合同检查。RTSP sink 的错误归属于 display 输出分支，但不触发 GTK 后端回退。

## 非本阶段范围

- 不实现 RTSP server、用户认证、鉴权或服务发现。
- 不把 RTSP 输出加入 `auto` 的默认选择顺序。
- 不把 GTK overlay 改造成视频像素 overlay；后续如需远端带框视频，另行设计编码前 overlay 阶段。
- 不修改证据缓存的 encoded tee 和事件去重逻辑。

## 验收标准

- YAML 和 CLI 能选择 RTSP backend，并对缺少地址、非法地址和缺少 GStreamer 能力返回明确错误。
- pipeline topology 能稳定表达 GTK 与 RTSP 两种互斥输出阶段。
- RTSP backend 不初始化 GTK、不创建 `SsvDisplayWindow`，但仍能和分析分支同时存在。
- GTK 的 `auto`、`gtkglsink`、`gtksink` 既有行为和回退测试保持通过。
- 配置、pipeline plan、builder、运行时生命周期和文档测试通过；真实 RTSP server 推流留作环境验证项。
