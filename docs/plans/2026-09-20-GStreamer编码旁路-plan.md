# GStreamer RTSP 编码旁路实施计划

## 步骤 1：配置与 plan

- 修改 `gst/ssv-common/include/ssv_config.hpp`，增加
  `SsvRtspOutputConfig::encoded_passthrough`。
- 修改 `gst/ssv-common/config/ssv_config.cpp`，解析配置键并校验它与
  `burn_in_overlay` 的互斥关系。
- 修改 `runner/pipeline/ssv_pipeline_plan.hpp/.cpp`，输出旁路解析结果；旁路
  时跳过 H.264 encoder 能力检查；纯旁路且未启用推理时跳过 decoder factory
  能力解析，并保留 `not-applicable` 计划标记。
- 更新 `gst/ssv-common/tests/test_ssv_config.cpp` 与
  `runner/pipeline/tests/test_ssv_pipeline_plan.cpp`。

## 步骤 2：topology 与 builder

- 修改 `runner/pipeline/ssv_pipeline_topology.hpp`，表达编码 tee 和 display
  branch 的输入来源。
- 修改 `runner/pipeline/ssv_pipeline_builder.cpp`，让编码旁路从
  `encoded-tee` 连接 `queue -> rtph264pay -> rtspclientsink`；需要时继续保留
  decoded discard branch，避免 decoder tee 无下游消费者。
- 更新 `runner/pipeline/tests/test_ssv_pipeline_builder.cpp`，验证阶段顺序、
  输入来源和 encoder 缺失时的旁路行为。

## 步骤 3：文档与验证

- 更新 `config/ssv.example.yaml`、`docs/检测前端配置.md` 和 RTSP 设计文档，
  说明旁路的帧率语义与 overlay 互斥关系。
- 运行配置、plan、builder 定向测试，再运行完整 `./ssv test`、Agent 测试和
  `git diff --check`。
- 追加验证旁路、推理与证据缓存同时启用时，encoded tee、decoded tee、分析和
  splitmuxsink 分支可以共存。
- 使用 `gst-inspect-1.0` 记录实际 `rtph264pay` 与 `rtspclientsink` 能力；没有
  外部 RTSP server 时不宣称端到端推流验证完成。

## 交付边界

- 本阶段不修改 GPU overlay，也不提交或推送远端分支。
- 工作区已有的 `docs/ssv-product-architecture.html` 保持未跟踪状态，不纳入本次
  改动。
