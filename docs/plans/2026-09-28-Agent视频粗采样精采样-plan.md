# Agent 视频粗采样与精采样实施计划

## 步骤 1：规格和工具边界

涉及文件：

- `docs/specs/2026-09-28-Agent视频粗采样精采样-spec.md`
- `docs/plans/2026-09-28-Agent视频粗采样精采样-plan.md`

内容：

- 固定 `event_id + evidence_id` 的输入边界。
- 定义 coarse/fine 的窗口、帧数、时间戳和输出生命周期。

## 步骤 2：实现受控视频采样工具

涉及文件：

- `agent/src/ssv_agent/tools/video_sampler.py`：新增

标识符：

- `sample_video_tool`：LangChain/DeerFlow 工具入口。
- `_select_clip`：从账本中选择唯一可用 clip evidence。
- `_sampling_window`：校验 coarse/fine 窗口和 clip PTS 范围。
- `_pin_clip`：在 ffmpeg 前用硬链接或复制固定输入。
- `_extract_frame`：按固定时间点生成 JPEG 并原子发布。

接口与所有权影响：

- 工具持有本次调用的临时输入和输出文件；调用结束后释放输入。
- SQLite evidence 仍由 `EventLedger` 所有；工具只读取，不登记采样 JPEG。
- ffmpeg 失败只返回 `available=false`，不产生部分可见结果。

## 步骤 3：接入 review runtime 和 prompt

涉及文件：

- `agent/config.example.yaml`：新增 `sample_video` 配置项。
- `agent/src/ssv_agent/review_runtime.py`：将 `sample_video` 纳入允许工具集合。
- `agent/src/ssv_agent/prompt.py`：增加“粗采样→查看→按需精采样”的执行步骤。

标识符：

- `_REVIEW_TOOL_NAMES` / `_REVIEW_CONFIGURED_TOOL_NAMES`：更新 RBAC 工具集合。
- `build_review_prompt`：更新工具约束、时间窗口和 evidence 引用说明。

## 步骤 4：同步 Agent 配置文档

涉及文件：

- `docs/Agent配置.md`：新增采样工具参数、调用顺序和限制。

内容：

- 说明 `context.mp4` 仍由后台生成，模型通过 `sample_video` 获取 JPEG。
- 说明粗采样与精采样不改变四个长期 evidence 文件。
- 说明采样失败、缺少 PTS、窗口越界时的 fail-closed 行为。

## 验证

- 运行 Ruff 对新增和修改的 Python 文件进行静态检查。
- 运行 `compileall` 检查语法和导入级别错误。
- 本阶段不运行测试套件；测试执行需要单独授权。

## 非目标

- 不改变 runner、SQLite schema、Episode 聚合和 cache 清理。
- 不新增任意路径视频读取权限。
- 不直接向模型上传 MP4。
