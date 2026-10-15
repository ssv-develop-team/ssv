# RTSP 输出 Sink 选择实施计划

## 步骤 1：配置与 plan

- 修改 `gst/ssv-common/include/ssv_config.hpp` 和 `gst/ssv-common/config/ssv_config.cpp`。
- 新增 `SsvDisplayBackend::RtspClientSink`、`SsvRtspOutputConfig` 和 `display.rtsp.location`。
- 更新 `--display-backend` 的解析和错误信息。
- 修改 `runner/pipeline/ssv_pipeline_plan.hpp/.cpp`，解析 RTSP sink、编码器能力和 I420 输出合同。
- 更新配置单元测试与 plan 单元测试。

## 步骤 2：GStreamer topology 与 builder

- 修改 `runner/pipeline/ssv_pipeline_topology.hpp` 和 `runner/pipeline/ssv_pipeline_builder.cpp`。
- 为 RTSP backend 增加 `videoconvert -> I420 -> H.264 encoder -> h264parse -> rtph264pay -> rtspclientsink` 阶段。
- 配置 `location`、RTP payload 和输出分支节流；保持证据缓存与分析分支独立。
- 更新 pipeline topology/builder 测试。

## 步骤 3：运行时生命周期与观测

- 修改 `runner/runtime/ssv_run_attempt_factory.cpp` 和 `runner/runtime/ssv_run_attempt.cpp`。
- 只有 GTK backend 创建和校验 `SsvDisplayWindow`；RTSP backend 直接运行 pipeline。
- 修改 `runner/pipeline/ssv_pipeline_instance.cpp` 和运行时事件适配，使 RTSP 输出能被识别为 display backend。
- 更新运行时测试，覆盖 RTSP 不创建窗口和 GTK 既有约束。

## 步骤 4：文档与验证

- 更新 `config/ssv.example.yaml`、`docs/检测前端配置.md` 和必要的 README/依赖说明。
- 运行配置、plan、builder、runtime 的定向测试，再运行完整 Meson 测试、Python 测试、Ruff、mypy 和 `git diff --check`。
- 使用 `gst-inspect-1.0` 记录本机 `rtspclientsink`、编码器和 `rtph264pay` 能力；没有 RTSP server 时不宣称端到端推流已验证。

## 交付边界

本变更不提交、不推送、不暂存。现有工作树中的其他修改保持原样。
