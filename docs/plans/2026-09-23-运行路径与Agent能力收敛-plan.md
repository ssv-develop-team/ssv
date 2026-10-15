# 运行路径与 Agent 能力收敛计划

## 实施基线

本计划归纳当前工作区相对本地 main（e8382bd）的完整代码和文档差异。相关实现已存在于当前分支，本计划用于统一记录范围、接口和验证口径，不表示本次文档整理会重做代码。工作区未跟踪的 docs/ssv-product-architecture.html 保持不动。

## 步骤 1：规则检索与引用验证收敛

涉及文件：

- agent/src/ssv_agent/knowledge/backends/local_markdown.py：改为调用共享规则分块器。
- agent/src/ssv_agent/knowledge/backends/qdrant.py：复用分块器并归一化规则过滤键。
- agent/src/ssv_agent/knowledge/catalog.py：保留稳定规则文档类型与身份契约。
- agent/src/ssv_agent/knowledge/rule_chunks.py：新增 RulePassage、split_rule_clauses 和 rule_chunk_id。
- agent/src/ssv_agent/result.py：校验规则引用；无有效依据时降级为 uncertain；抽出通用内容寻址 artifact writer，保持复核 JSON/历史 Markdown 行为。
- agent/tests/test_evidence_rag_review_integration.py
- agent/tests/test_local_markdown.py
- agent/tests/test_qdrant_rule_retriever.py
- agent/tests/test_qdrant_store.py
- agent/tests/test_result.py

标识符与职责：

- RulePassage / split_rule_clauses / rule_chunk_id：统一条款切分、规则元数据和稳定 chunk 身份。
- _normalize_rule_filters：将 event_type 查询字段映射到 event_types 存储字段。
- validate_rule_citations / _downgrade_rule_result：拒绝不可验证的确定性规则结论，保守转换为 uncertain。
- write_content_addressed_artifact：在 outputs root 内安全原子写入并防止不同内容覆盖。

验证：运行上述规则检索、复核结果和 artifact writer 测试；确认本地/Qdrant 使用相同 chunk identity，过滤与失败降级语义有覆盖。

## 步骤 2：硬件快照、编解码 plan 与 RTSP 旁路

涉及文件：

- gst/ssv-common/include/ssv_config.hpp
- gst/ssv-common/config/ssv_config.cpp
- gst/ssv-common/tests/test_ssv_config.cpp
- gst/ssv-infer/backends/onnxruntime/ssv_onnxruntime_backend.cpp
- gst/ssv-infer/backends/onnxruntime/ssv_onnxruntime_backend.hpp
- gst/ssv-infer/core/ssv_inference_config.cpp
- gst/ssv-infer/core/ssv_inference_config.hpp
- gst/ssv-infer/core/ssv_inference_service.cpp
- gst/ssv-infer/public/ssv_inference_service.hpp
- gst/ssv-infer/tests/test_ssv_provider_runtime.cpp
- runner/pipeline/ssv_evidence_cache.hpp：澄清缓存资源所有权的中文注释；不改变运行行为。
- runner/pipeline/ssv_hardware_capabilities.cpp
- runner/pipeline/ssv_hardware_capabilities.hpp
- runner/pipeline/ssv_pipeline_builder.cpp
- runner/pipeline/ssv_pipeline_builder.hpp
- runner/pipeline/ssv_pipeline_contract.cpp
- runner/pipeline/ssv_pipeline_contract.hpp
- runner/pipeline/ssv_pipeline_plan.cpp
- runner/pipeline/ssv_pipeline_plan.hpp
- runner/pipeline/ssv_pipeline_topology.hpp
- runner/pipeline/tests/test_ssv_pipeline_builder.cpp
- runner/pipeline/tests/test_ssv_pipeline_contract.cpp
- runner/pipeline/tests/test_ssv_pipeline_plan.cpp
- runner/runtime/ssv_run_attempt.cpp
- runner/runtime/ssv_run_attempt_factory.cpp
- runner/runtime/ssv_run_attempt_factory.hpp
- runner/runtime/ssv_run_fallback_state.cpp
- runner/runtime/ssv_run_fallback_state.hpp
- runner/runtime/ssv_runner.cpp
- runner/runtime/ssv_runtime_event_adapter.cpp
- runner/runtime/tests/test_ssv_run_attempt.cpp
- runner/runtime/tests/test_ssv_runner.cpp
- runner/runtime/tests/test_ssv_runtime_event_adapter.cpp

标识符与职责：

- SsvHardwareCapabilities / SsvCapabilitySnapshot：区分原始 probe 结果与单次运行只读能力视图。
- SsvSystemHardwareCapabilitiesProbe::detect：采集 GStreamer factory、编译能力和 ONNX Runtime provider。
- SsvInferencePlan / SsvEncodePlan / SsvCodecPlan / SsvPipelinePlan：分别表达推理、输出编码、完整编解码路径与本次运行计划。
- SsvPipelinePlan::resolve / SsvPipelinePlan::decoded_path_required：从配置与快照解析 plan，并决定是否创建解码分支。
- ssv_inference_detect_available_providers / ssv_inference_service_create：provider 列表从启动快照进入 runner-owned inference service。
- display.rtsp.encoded_passthrough：默认关闭的 H.264 旁路选项，与 burn_in_overlay 互斥。
- SsvRunFallbackState::try_mixed_codec_fallback / force_mixed_codec：硬件 pair 运行失败后先尝试硬件解码加软件编码；符合条件时再进入软件解码 fallback。
- SsvEvidenceCache：只观察 parser 与 splitmuxsink 消息，不拥有 pipeline。

验证：

- 配置测试覆盖 passthrough 默认值、类型和 overlay 互斥。
- plan/builder 测试覆盖独立 inference 与 codec 选择、device selector、SystemMemory/CUDAMemory 边界、decode-only、纯旁路与分析/证据分支共存。
- contract/runtime 测试覆盖硬件 pair 到 Mixed、Mixed 到 software decode 的顺序、错误阶段隔离和 snapshot/provider 复用。
- 运行定向 Meson 测试、meson test -C build --print-errorlogs、./ssv test 和 git diff --check。
- 如无可用 RTSP server、目标 factory 或设备，只记录 plan/builder 契约结果，不报告真实推流或硬件编解码成功。

## 步骤 3：确定性分析报告及持久任务

涉及文件：

- agent/src/ssv_agent/config.py
- agent/src/ssv_agent/event_store/__init__.py
- agent/src/ssv_agent/event_store/ledger.py
- agent/src/ssv_agent/event_store/schema.py
- agent/src/ssv_agent/event_store/sqlite_store.py
- agent/src/ssv_agent/report.py
- agent/src/ssv_agent/result.py：与步骤 1 共享 artifact writer。
- agent/src/ssv_agent/service.py
- agent/src/ssv_agent/workers.py
- agent/tests/test_config.py
- agent/tests/test_event_ledger.py
- agent/tests/test_report.py
- agent/tests/test_service.py
- agent/tests/test_workers.py
- config/ssv.example.yaml
- gst/ssv-common/config/ssv_config.cpp：同时校验 Python Agent 使用的 reporting 扩展字段。
- gst/ssv-common/tests/test_ssv_config.cpp：覆盖 reporting 默认值、类型与范围。

标识符与职责：

- ReportWorkerConfig / agent.reporting：报告 worker 的启停、轮询、lease 和重试设置；禁用只暂停领取。
- JobKind.REPORT / ReviewRecord / ReportRecord：分别表示持久报告任务、指定复核历史版本及已登记 artifact。
- EventLedger._append_review_in_transaction：同事务追加 review 并创建 index/report jobs。
- EventLedger.get_review_record / EventLedger.complete_report_job：按历史 revision 读取并以 lease fence 原子提交报告记录和 job 完成状态。
- REPORT_TEMPLATE_VERSION / render_analysis_report / write_analysis_report：固定模板、纯渲染及受控内容寻址写入。
- ReportWorker / AgentService：复用 durable worker 生命周期；报告阶段独立于 DeerFlow 与 Redis ACK。

验证：

- EventLedger 测试覆盖 rule citations 保存、report job 原子创建、fence、幂等和旧 durable_jobs schema 升级保留。
- renderer/writer 测试覆盖历史版本、特殊字符转义、无宿主机路径、稳定 hash 和不同内容不覆盖。
- worker/service 测试覆盖成功、重试、dead、lease 丢失、启停和启动失败清理；报告失败不得改变 review/index 状态。
- Python 配置与 C++ shared-config 测试接受相同 reporting 字段名、默认值和范围。

## 步骤 4：使用文档与计划收敛

涉及文件：

- README.md：增加 Agent 架构文档入口。
- docs/README.md：登记架构说明。
- docs/Agent架构与实现.md：新增系统所有权、启动、硬件选择、fallback、Agent workers 与验证现状说明。
- docs/Agent配置.md：说明 ReportWorker、reporting 配置和报告产物边界。
- docs/检测前端配置.md：说明 encoded passthrough 与 overlay、display.fps 的关系。
- docs/specs/2026-09-23-运行路径与Agent能力收敛-spec.md：本次统一规格。
- docs/plans/2026-09-23-运行路径与Agent能力收敛-plan.md：本次统一计划。

删除并合并：

- docs/specs/2026-09-20-GStreamer编码旁路-spec.md
- docs/plans/2026-09-20-GStreamer编码旁路-plan.md
- docs/specs/2026-09-20-硬件能力快照与建链解耦-spec.md
- docs/plans/2026-09-20-硬件能力快照与建链解耦-plan.md
- 原 docs/specs/2026-09-23-Agent分析报告生成-spec.md 与 docs/plans/2026-09-23-Agent分析报告生成-plan.md 改名为上面的统一文件。

验证：检查文档链接和路径引用，确认 docs/specs 与 docs/plans 的本次新增内容只保留这一对规格/计划，并运行 git diff --check。

## 范围约束

- 保留与本次代码对应的 Agent 架构/配置/检测配置手册；它们是使用文档，不再额外拆分每个功能的 spec/plan。
- 不修改既有历史规格，不触碰未跟踪的 docs/ssv-product-architecture.html。
- 不新增 Python 依赖、subagent、报告 API、报告历史回填或 artifact GC。
- 不提交、不推送、不创建 PR；代码差异仅作范围归纳，不在本次文档整理中重写。

## 实施与验证记录

- 当前分支已有规则检索、能力快照/编解码路径和分析报告实现及其测试。本计划覆盖相对 main 的代码差异；没有把代码测试文件存在误述为生产环境验证。
- 原报告实施记录为：Agent 全量 pytest 289 passed、Ruff 通过、Meson 33/33、旧 SQLite schema migration fixture 1 passed、git diff --check 通过。这些是先前记录的结果，本次文档整理未重跑代码测试。
- 本次只需验证文档改动：文档差异清单、引用路径和 git diff --check。真实 GStreamer factory、硬件设备、RTSP server、Redis、模型服务和部署输出目录仍需按目标环境单独验证。
