# RTSP 烧录 Overlay 实施计划

## 步骤 1：配置与文档

- 修改 `gst/ssv-common/include/ssv_config.hpp`，为
  `SsvRtspOutputConfig` 增加 `burn_in_overlay`，默认关闭。
- 修改 `gst/ssv-common/config/ssv_config.cpp`，解析、校验并保留
  `display.rtsp.burn_in_overlay`。
- 更新 `config/ssv.example.yaml`、`README.md` 和 `docs/检测前端配置.md`，说明
  RTSP 烧录是显式选择且会进入视频像素。
- 更新 `gst/ssv-common/tests/test_ssv_config.cpp`，覆盖默认值、true 值和未知键。

## 步骤 2：Pipeline plan 与 caps contract

- 修改 `runner/pipeline/ssv_pipeline_plan.cpp/.hpp`，当 RTSP 烧录开启时检查
  `ssvoverlay` 能力，并生成 `DisplayOverlayInput = BGRx/SystemMemory`。
- 修改 `runner/pipeline/ssv_pipeline_contract.hpp/.cpp`，注册新的
  `DisplayOverlayInput` boundary，保持边界编号解析和错误文本稳定。
- 修改 `runner/runtime/ssv_run_attempt.cpp`，把新的 expected caps 纳入待完成
  contract 集合。
- 更新 plan 和 contract 测试，覆盖 RTSP 烧录开启、关闭和能力缺失。

## 步骤 3：RTSP topology 与 builder

- 修改 `runner/pipeline/ssv_pipeline_topology.hpp`，增加 overlay caps 文本字段。
- 修改 `runner/pipeline/ssv_pipeline_builder.cpp`，在 RTSP 分支中按配置插入
  `videoconvert -> capsfilter(BGRx) -> ssvoverlay`，注入 source context、字体、
  motion prediction 和 horizon；再继续 I420 编码链路。
- 为 overlay 输入和最终编码输入安装 contract probe。
- 更新 topology/builder 测试，验证 stage 顺序、属性和 source context 所有权。

## 步骤 4：回归验证

- 运行配置、plan、contract、builder、runtime 的定向 Meson 测试。
- 运行完整 `./ssv test` 和 `git diff --check`。
- 使用 `gst-inspect-1.0 ssvoverlay`、`rtspclientsink`、H.264 encoder 记录本机
  能力；缺少 `rtspclientsink` 或 RTSP server 时，区分能力验证与端到端验证。

## 交付边界

本变更不提交、不推送、不暂存。工作树中的 Agent、证据缓存和其他既有修改保持原样。
