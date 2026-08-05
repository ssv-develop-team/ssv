# SSV 实时视频运行上下文

本上下文描述 SSV 单路实时视频运行从配置解析到最终退出之间的核心语言，用于区分顶层运行、
单次执行尝试和已经解析的管线选择。

## Language

**Run（运行）**:
从一份已验证配置开始，经过零次或多次回退尝试，最终产生唯一运行结果的完整执行。
_Avoid_: Pipeline session、runner session

**Run Attempt（运行尝试，`SsvRunAttempt`）**:
一次使用固定有效配置和固定 Pipeline Plan 的单次执行；无论成功、受控停止或失败，结束后都不能
重新使用。发生回退时必须创建新的 Run Attempt。
_Avoid_: Pipeline session、一次 Run

**Pipeline Plan（管线计划，`SsvPipelinePlan`）**:
为一个 Run Attempt 解析出的解码、显示、推理选择及预期数据契约；它描述选择和约束，不代表
已经创建的 GStreamer 资源。
_Avoid_: Pipeline、Pipeline Instance

**Inference Runtime Snapshot（推理运行时快照，`SsvInferenceRuntimeSnapshot`）**:
Inference Service 创建成功后形成的一次性 owning 值，记录已经解析的 Provider、设备、精度、模型
身份、输入契约、cache 状态和 Provider fallback；Run Attempt 持有该值，不借用 service、backend、
session 或 cache 生命周期。
_Avoid_: BackendInfo、Inference Service 状态、Runtime Resolved Event
