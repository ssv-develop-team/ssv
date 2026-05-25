# AGENTS.md

本文件给自动化编码 Agent 和协作者使用。项目按并行主线开发，任何改动都要先确认所属主线和跨主线接口影响。

## 并行主线

- `T1` 实时视频链路与运行时：`scripts/`、`config/`、pipeline runner、输入/显示/运行状态。
- `T2` 感知算法与元数据：`gst/ssv-infer`、`gst/ssv-track`、`gst/ssv-overlay`、`gst/ssv-common`、`gst/tests`。
- `T3` 事件与异步边界：事件判定、证据输出、`gst/ssv-pub`、Redis 消息和事件状态。
- `T4` Agent 与知识复核：`agent/`、事件消费、上下文构造、状态机、工具路由、模型 provider。
- `T5` 工程集成与质量：测试矩阵、文档模板、CI/本地验证、demo 和集成验收。

集成节点使用 `I1-I4`，只做合流验收、接口适配、测试补齐和小范围修复，不承载大功能开发。

## 开发规则

- 开始实现前，先确认改动属于哪条 `T` 主线；跨主线改动必须先更新中文 spec。
- 每条主线或集成节点进入实现前必须有中文文档：`docs/specs/YYYY-MM-DD-Tx-名称-spec.md` 和 `docs/plans/YYYY-MM-DD-Tx-名称-plan.md`，集成节点使用 `Ix`。
- spec 和 plan 使用中文；代码标识、命令、路径、配置键保持英文原文。
- 面向人的文档统一使用中文，Markdown 文件名优先使用中文主题；正式设计文档放在 `docs/specs/` 下。
- 文档中不要保留 `TODO`、`TBD`、`待补充` 等占位内容；暂不展开的内容写入“非本阶段范围”。
- Mermaid 图中的节点文本使用中文，技术名保留原文；架构图优先使用 `flowchart`，事件链路优先使用 `sequenceDiagram`。
- 修改跨主线接口时必须同步文档和测试。重点接口包括 `ssv_meta`、YAML 配置、插件属性、Redis 事件 schema、证据路径、Agent 输入输出、runner 退出码和日志字段。
- 不要把构建、清理、下载模型、启动 Docker Redis 迁入 C++；这些保持脚本职责。
- 不要把长期运行时错误处理继续堆进 shell；T1 后续由 C++ pipeline runner 负责 pipeline 构建、bus 监听、错误码和状态观测。
- 不要让 Python Agent 进入每帧同步检测链路；Agent 只消费 Redis 中的事件和证据。
- 不要回退他人或既有未提交改动；不要使用 `git reset --hard` 或 `git checkout --` 清理文件。

## 常用验证

根据改动范围运行对应命令：

```bash
# C++ 插件、元数据、Meson
./ssv build
meson test -C build

# Python Agent
cd agent && uv run --extra dev pytest

# CLI/脚本
bash tests/ssv_cli_test.sh

# 本地链路，依赖 RTSP、模型和 Redis
./ssv test
./ssv run --display
```

如果某个验证因本地环境缺失无法运行，在最终说明中明确原因。

## 参考文档

- Roadmap：`docs/roadmap.md`
- 总体设计：`docs/specs/2026-05-21-安全帽佩戴视频监测分析系统设计.md`
- 运行说明：`README.md`
