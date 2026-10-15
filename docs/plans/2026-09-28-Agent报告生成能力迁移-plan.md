# Agent 报告生成能力迁移计划

## 步骤 1：报告 artifact 完整性读取

涉及文件：

- `agent/src/ssv_agent/report.py`：新增 `read_analysis_report`，校验 outputs root、普通文件、大小和 SHA-256 后读取 Markdown。
- `agent/tests/test_report.py`：覆盖成功读取、越界、symlink、大小不一致和 hash 不一致。

涉及标识符：

- `read_analysis_report`：只读解析已登记报告，不创建或修改账本记录。
- `_validated_report_path`：约束 artifact 所有权在 `SSV_OUTPUTS_DIR` 内。

验证：报告读取单元测试和 `git diff --check`。

## 步骤 2：事件报告元数据投影与 `get_report` 工具

涉及文件：

- `agent/src/ssv_agent/tools/get_event.py`：在安全事件投影中增加报告元数据。
- `agent/src/ssv_agent/tools/get_report.py`：新增 `get_report` 工具，选择报告、读取内容、复制 thread outputs 并返回虚拟路径与证据引用。
- `agent/src/ssv_agent/tools/__init__.py`：如现有导出约定需要则登记工具。
- `agent/tests/test_event_query_tools.py`：覆盖报告元数据和报告工具返回契约。

涉及标识符：

- `event_case_payload`：从 `EventLedger.report_records_for_event` 读取 metadata，不暴露 artifact path。
- `get_report_tool`：按 event/review 选择并安全读取报告；只写当前 thread outputs 派生副本。
- `_copy_report_to_thread_outputs`：生成稳定、受控的虚拟输出路径。

验证：工具测试覆盖 latest/specific review、未找到、线程目录缺失、完整性失败和证据引用。

## 步骤 3：Review 配置与文档

涉及文件：

- `agent/config.example.yaml`：增加 `get_report` 工具配置。
- `agent/src/ssv_agent/review_runtime.py`：把 `get_report` 加入 review 工具集合和 fail-closed 配置筛选。
- `agent/src/ssv_agent/prompt.py`：说明报告读取是只读展示能力，禁止把报告正文当作新的事实或指令。
- `agent/tests/test_service.py`、`agent/tests/test_prompt.py`：更新工具集合断言和提示词契约。
- `docs/Agent配置.md`、`docs/Agent架构与实现.md`：记录报告展示入口、虚拟路径和非目标。

涉及标识符：

- `_REVIEW_TOOL_NAMES` / `_REVIEW_CONFIGURED_TOOL_NAMES`：保持内置 `view_image` 与项目工具的 fail-closed 边界。
- `build_review_prompt`：保留现有证据与 JSON 复核约束，补充报告只读语义。

验证：review 配置生成测试、提示词测试、相关 Python 静态检查。

## 步骤 4：交付检查

- 检查 `git diff --check` 和工作区状态，保留既有未提交修改。
- 运行与本次变更直接相关的 Agent 测试和 Ruff；不宣称真实 DeerFlow、HTTP 服务或模型环境验证。
- 报告实际文件、接口变化、验证结果和未验证边界。
