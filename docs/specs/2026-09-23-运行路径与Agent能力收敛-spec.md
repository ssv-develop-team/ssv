# 运行路径与 Agent 能力收敛规格

## 基线与范围

本规格归纳当前工作区相对本地 main（e8382bd）的完整变更，覆盖 GStreamer 能力规划与编解码路径、Agent 规则检索一致性和分析报告生成。它记录的是一组已经进入当前分支实现的关联变更，不代表这些能力已在任意部署环境完成端到端验证。

工作区未跟踪的 docs/ssv-product-architecture.html 不属于该 Git 差异，本规格不纳入或修改它。

## 目标

- 一次运行只探测一次 GStreamer、推理运行时和 ONNX Runtime provider 能力；plan、builder、推理服务与 fallback 共享同一能力快照。
- 把推理 backend 与视频 codec backend 分开规划；RTSP 重编码优先使用同设备编解码器，无法配对时明确回退到混合路径。
- 为不需要像素级 overlay 的 RTSP 输出提供 H.264 编码旁路，避免无关的输出解码和重编码。
- 统一规则文档分块和 chunk 身份，让本地检索与 Qdrant 使用相同契约；无法验证的确定性规则结论保守降级。
- 对每个新提交的复核版本生成可追溯、可重试的确定性 Markdown 报告，不让报告改变复核事实或依赖第二次模型判断。

## GStreamer 能力与路径契约

### 能力快照与 plan

- SsvHardwareCapabilities 表示启动探测到的原始事实，包括 GStreamer factory、编译能力和 ONNX Runtime provider。
- SsvCapabilitySnapshot 对这些事实提供只读查询；本次 run 的 SsvPipelinePlan 持有该快照。
- SsvInferencePlan 独立描述推理 backend/provider；SsvCodecPlan 描述解码器、编码器、memory contract、路径类别及 fallback。SsvEncodePlan 描述输出编码器和设备约束。
- SsvPipelinePlan::resolve、builder、topology contract 和 inference service 不得各自重新探测。ORT provider 在运行准备阶段解析一次后传入 inference service；模型/session/cache 的状态仍由 inference service 和 attempt 管理。
- 选择推理 backend 不得根据 decoder 类型推断，选择 encoder 则必须考虑所选 decoder 的设备与 memory contract。

### 编解码选择与 fallback

- 不需要重新编码时不创建 encoder plan，并明确标记 Passthrough 或 DecodeOnly，不伪装成 software codec pair。
- RTSP 重编码优先使用同一硬件设备的 decoder/encoder pair：VAAPI 使用同一 DRM render node，NVDEC/NVENC 使用同一 CUDA device。Auto selector 选择同 backend 的通用 factory；显式 selector 必须保留并校验。
- 硬件 pair 不存在或 memory contract 不兼容时，保留硬件 decoder、切换到 SystemMemory 软件 encoder，路径标记为 Mixed。
- Mixed 路径仍失败时，只有 decode.mode 为 auto 才能继续回退到软件 decoder；显式硬件模式严格失败。
- NVDEC 到 CPU 推理、gtksink 或软件 encoder 的边界必须显式下载到 SystemMemory；硬件 NVDEC/NVENC pair 保留 CUDA memory。
- codec pair fallback 只处理 display 编码创建或 contract 失败，不吞并推理/分析 contract 错误。所有 fallback 记录原路径、目标路径、阶段和原因，并复用原能力快照。

### RTSP 编码旁路

- 配置键 display.rtsp.encoded_passthrough 默认为 false，仅适用于 display.backend: rtsp，且不能与 burn_in_overlay 同时开启。
- 启用时，从 depay/parser 后的 H.264 分支进入有界 queue、rtph264pay 和 rtspclientsink；保留源编码参数、帧率和时间戳，不执行输出分支解码、videorate、像素转换或编码。
- 若推理/分析仍启用，单独创建解码分析分支；若没有分析消费者，则不创建 decoder 和 decoded tee。
- passthrough 分支不受 display.fps 限流。若需要 overlay、改帧率或转码，使用默认解码后输出路径。
- encoded tee 可并行提供 RTSP 输出与 splitmuxsink 短时证据缓存；网络输出分支不得阻塞分析分支。旁路 queue 有界且不任意丢弃 H.264 buffer。
- 纯 passthrough 不要求 H.264 encoder；没有分析分支时也不要求 H.264 decoder。rtspclientsink 是发布客户端，不是本项目提供的 RTSP server。

## Agent 规则检索契约

- 规则条款切分、RulePassage 元数据和稳定 chunk ID 由共享的 rule_chunks 模块定义，本地 Markdown 和 Qdrant backend 复用同一实现。
- 规则查询对外的 event_type 过滤键映射为向量 payload 中的 event_types，避免公开查询契约与存储字段名不一致。
- ReviewResult 的确定性结论只有在规则上下文可用、引用 chunk 存在且来源、规则身份、版本和条款元数据一致时才保留。
- 规则上下文缺失、确定性结论无引用或引用无法验证时，将结果降为 uncertain、置信度置零并清除无效 citations；不得因无法验证而保留肯定/否定结论。
- 规则目录本身的版本化身份和发现约定沿用已有规则契约；本次增量聚焦共享分块实现、过滤键归一化及 citation 失败语义。

## 分析报告契约

- ReviewWorker 是唯一复核判断者。报告只呈现账本已经提交的案件事实和精确 ReviewRecord，不调用 DeerFlow/LLM，不修改 verdict、confidence、claims、证据引用或复核状态。
- complete_review_job 在同一 SQLite 事务中追加 review，并创建对应的 index 与 report durable jobs。Report job 指向产生它的 review revision。
- ReviewRecord 读取 append-only reviews 历史；rule citations 持久化到 reviews.rule_citations_json。reports 表追加报告元数据，durable_jobs.kind 的迁移须保留旧 job 的 ID、状态、lease、重试次数与错误信息。
- ReportWorker 通过历史版本读取接口取得指定 ReviewRecord；不能读取可能已被后续 review 更新的当前投影。
- 报告使用固定模板及 REPORT_TEMPLATE_VERSION，按 event、review、revision 和模板版本确定来源；稳定输入产生稳定正文和 SHA-256。
- 报告正文只使用账本字段、规则引用和 evidence IDs；不输出宿主机证据路径，不推算缺失的媒体元数据。外部和模型文本按不可信 Markdown 转义。
- Markdown artifact 原子写入受控 outputs root，以内容寻址避免重试覆盖不同内容。SQLite 保存 append-only ReportRecord 元数据和 report job 状态；SQLite review 历史仍是事实源，Markdown 是派生产物。
- 报告 job 使用现有 lease、heartbeat、retry、dead 和 fencing 语义。报告失败或暂停不回滚 review/index，也不改变 Redis ACK。
- agent.reporting 默认启用；禁用只暂停领取，既有 pending jobs 保留并可恢复。历史 review 不自动回填报告。

## 非目标

- 不实现硬件热插拔、运行中能力刷新或跨设备负载均衡。
- 不让每个 GStreamer element 或 Agent 重新探测硬件/provider；不把全量硬件快照注入 plugin ABI。
- 不实现 GPU overlay、RTSP server、认证服务或本机长期录像系统。
- 不使用 subagent 或第二次 LLM 调用撰写报告，不增加报告 API/UI、跨事件报告、历史回填或 artifact GC。
- 不把 Qdrant、报告 Markdown 或当前案件投影提升为事件/复核事实源。

## 验收标准

1. 同一 run 的 plan、builder、inference service 和 fallback 复用同一 capability snapshot；provider availability 不因 attempt 重建而重复探测。
2. plan 能区分 Passthrough、DecodeOnly、HardwarePair、Mixed 和 Software；编码与推理选择相互独立。
3. RTSP passthrough 配置默认关闭、与烧录互斥；纯旁路不创建无消费者的 decoder，分析和证据分支可与 encoded branch 共存。
4. 硬件 pair 失败时先进入硬件解码加软件编码的 Mixed 路径；后续软件解码 fallback 遵循 auto/显式模式边界。
5. 本地 Markdown 与 Qdrant 分块得到一致的条款身份；event_type 过滤正确作用于 event_types；无效规则引用不能保留确定性结论。
6. 新 review、index job 与 report job 原子提交；报告绑定 job 指定 revision，fence 失败不得登记成功。
7. 报告重试稳定、不同内容不互相覆盖，且报告失败不改变 review、index 或 Redis ingress 的成功状态。
8. Python 与 C++ 对共享 YAML 中 display.rtsp.encoded_passthrough 和 agent.reporting 的字段类型、默认值及约束保持兼容。
9. 自动化测试只证明代码和契约；硬件 factory、真实 RTSP server、模型、Redis 和部署环境需另行验证后才能声称端到端可用。
