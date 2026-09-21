# 文档索引

本文档目录按“使用说明”和“设计资料”分开。README 只保留项目定位和最短启动路径，具体配置以本目录的使用文档和仓库中的示例配置为准。

## 使用文档

- [Agent 架构与实现](Agent架构与实现.md)：给 Codex/Agent 阅读的系统边界、启动流程、硬件选择、fallback、实现状态和验证矩阵。
- [依赖与构建](依赖与构建.md)：系统包、Python 环境、`uv`、runtime profile、ONNX Runtime/OpenCV/TensorRT provider 和构建缓存。
- [检测前端配置](检测前端配置.md)：实时输入、解码、GTK 显示、overlay、推理、模型、跟踪和调试。
- [Agent 配置](Agent配置.md)：Redis 消费、SQLite 账本、复核 worker、索引 worker、DeerFlow 模型、证据和 Qdrant。
- [YOLO 推理链路说明](yolo推理链路说明.md)：模型输入、输出、推理设备和常见失败。
- [安全帽模型验证说明](安全帽模型验证说明.md)：安全帽模型验证、label map 和模型接入。

## 设计与状态

- [系统设计](specs/2026-05-21-安全帽佩戴视频监测分析系统设计.md)：系统分层、实时链路与 Agent 边界。
- [Roadmap](roadmap.md)：里程碑、主线和当前工程基线。

## 需求与实施计划

- `specs/`：中文需求、接口契约和范围约束。
- `plans/`：按步骤拆解的实施计划、文件边界和验证方式。

新增跨模块功能前先补充对应 spec 和 plan；专题文档记录当前实现与可复现命令，不替代配置模板。
