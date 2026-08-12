# T4 Agent 基线（DeerFlow 接入）Plan

- 日期：2026-08-12
- 前置：`docs/specs/2026-08-12-T4-DeerFlow-Agent基线-spec.md`

实施范围仅限 `agent/`，不修改 `./ssv`、`scripts/agent.sh`、`gst/`、`runner/`。
每个步骤完成后保持变更未暂存，供人工审阅；收到进入下一步的明确指示后再暂存。

## 步骤 1：引入 deerflow-harness 依赖

涉及文件
- `agent/third_party/deer-flow`：新增 git submodule，固定 commit `8659fca8`
- `agent/pyproject.toml`：修改，新增依赖与 `[tool.uv.sources]`

涉及标识符
- `deerflow-harness`：依赖名，editable path 指向 `third_party/deer-flow/backend/packages/harness`
- `redis`：保持 `>=5.0.0,<8`，避免阻塞读超时

接口与所有权影响
- 仅 `agent` 包新增依赖；其它模块不受影响

验证
- `cd agent && uv sync`
- `uv run python -c "import deerflow.client"`

## 步骤 2：新增 agent/config.yaml

涉及文件
- `agent/config.yaml`：新增

涉及标识符
- `models`：视觉模型配置，API key 引用环境变量
- `tools`：注册 `evidence_reader` 与 `view_image`
- `sandbox`：LocalSandboxProvider，`allow_host_bash: false`
- `checkpointer`：sqlite，`checkpoints.db`

接口与所有权影响
- `DeerFlowClient` 启动时读取；模型与工具列表配置化

验证
- `cd agent && uv run python -c "from deerflow.config import ..."` 配置可加载

## 步骤 3：ReviewContext 与提示词

涉及文件
- `agent/src/ssv_agent/review_context.py`：新增
- `agent/src/ssv_agent/prompt.py`：新增
- `agent/tests/test_review_context.py`：新增
- `agent/tests/test_prompt.py`：新增

涉及标识符
- `Detection` / `ReviewContext` / `ReviewContext.from_event`：事件输入模型
- `build_review_prompt`：复核提示词构造

接口与所有权影响
- `from_event` 同时被消费路径与测试使用；坏 JSON 抛异常，由调用方按失败处理

验证
- `cd agent && uv run --extra dev pytest tests/test_review_context.py tests/test_prompt.py`

## 步骤 4：结果模板解析与落盘

涉及文件
- `agent/src/ssv_agent/result.py`：新增
- `agent/tests/test_result.py`：新增

涉及标识符
- `ReviewResult`：解析结果模型
- `parse_result_markdown`：模板校验
- `write_result_markdown`：原子落盘 `outputs/<event_id>/result.md`

接口与所有权影响
- 解析失败返回明确错误，不抛半成品；落盘失败抛异常，由调用方按失败处理

验证
- `cd agent && uv run --extra dev pytest tests/test_result.py`

## 步骤 5：evidence_reader 工具

涉及文件
- `agent/src/ssv_agent/tools/__init__.py`：新增
- `agent/src/ssv_agent/tools/evidence_reader.py`：新增
- `agent/config.yaml`：修改，注册工具
- `agent/tests/test_evidence_reader.py`：新增

涉及标识符
- `evidence_reader`：@tool，路径存在性校验
- `view_image`：复用 harness 内置，仅在 config.yaml 注册

接口与所有权影响
- 工具只读证据路径，不产生新文件

验证
- `cd agent && uv run --extra dev pytest tests/test_evidence_reader.py`

## 步骤 6：runner 封装

涉及文件
- `agent/src/ssv_agent/runner.py`：新增
- `agent/tests/test_runner.py`：新增

涉及标识符
- `run_review`：`DeerFlowClient.stream` 薄封装，`thread_id=event-<event_id>`
- `FakeDeerFlowClient`：测试替身，返回固定 values 事件

接口与所有权影响
- 输入 `DeerFlowClient + ReviewContext`，输出最终文本；异常上抛不吞

验证
- `cd agent && uv run --extra dev pytest tests/test_runner.py`

## 步骤 7：改造事件消费与入口

涉及文件
- `agent/src/ssv_agent/event_consumer.py`：修改，`_handle_event` 调用复核
- `agent/src/ssv_agent/service.py`：修改，装配 `DeerFlowClient` 与配置
- `agent/tests/test_event_consumer.py`：更新
- `agent/tests/test_service.py`：更新

涉及标识符
- `EventConsumer`：消费循环，失败只记日志并 XACK
- `run_consumer` / `run`：入口装配
- `ReviewContext.from_event`：消费路径接入

接口与所有权影响
- XACK 语义不变：成败都 XACK，不重试
- 成功写 `outputs/<event_id>/result.md`；失败不落盘

验证
- `cd agent && uv run --extra dev pytest tests/test_event_consumer.py tests/test_service.py`

## 步骤 8：独立样例事件注入脚本

涉及文件
- `agent/scripts/enqueue_event.py`：新增
- `agent/tests/test_enqueue_event.py`：新增

涉及标识符
- `build_sample_event`：构造当前 ssvpub schema 的事件 JSON
- `main`：argparse + `XADD` 到 `ssv:events`

接口与所有权影响
- 独立脚本，不进入 `ssv_agent` 包，不修改 `./ssv`

验证
- `uv run --isolated --script agent/scripts/enqueue_event.py --help`
- `cd agent && uv run --extra dev pytest tests/test_enqueue_event.py`

## 步骤 9：全量验证与交付

验证命令
- `cd agent && uv run --extra dev pytest`
- 手工冒烟：
  1. 启动本地 Redis；
  2. `uv run --isolated --script agent/scripts/enqueue_event.py --source camera-1 --frame-path /path/to/frame.png`；
  3. 启动 `agent` 服务消费；
  4. 检查 `outputs/<event_id>/result.md`；
  5. 无证据样例预期 `uncertain / missing`；故障样例预期只有日志、无 result.md。

交付动作
- 检查 `git status` 与 `git diff`，确认只包含本计划涉及文件；
- 暂存、提交并创建 PR，PR 描述指向本 spec 与 plan。
