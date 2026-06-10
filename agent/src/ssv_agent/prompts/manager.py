"""提示词轻量管理 —— 健康检查 + 版本号快照。

.. deprecated:: M4 (D7)
    功能已合并到 context/system_prompt.py (SystemPromptManager)。
    M5 移除。

设计原则:
  1. 当前阶段（M4→M5）提示词仍在快速试错，版本号通过 dataclass 字段手动管理
  2. validate() 确保所有策略模板和占位符完整
  3. get_versions() 返回当前所有组件的版本号快照
  4. 等提示词内容相对稳定后再补版本管理基础设施

后续按需实现（当前不做）:
  - 版本变更历史记录
  - 全量配置快照
  - 快照列表查询
"""

from __future__ import annotations

import structlog
import warnings

from ssv_agent.prompts.system import DEFAULT_SYSTEM_PROMPT
from ssv_agent.prompts.templates import STRATEGY_TASK_TEMPLATES, STRATEGY_SYSTEM_TEMPLATES

logger = structlog.get_logger()

# 当前阶段有效的全部策略
ALL_STRATEGIES = {"direct_confirm", "visual_review", "rule_explain", "notify_report"}


def validate() -> dict:
    """检查所有提示词组件的完整性。

    .. deprecated:: M4
        使用 SystemPromptManager 替代。M5 移除。
    """
    warnings.warn(
        "prompts.manager.validate() is deprecated, "
        "use SystemPromptManager instead",
        DeprecationWarning,
        stacklevel=2,
    )
    issues: list[str] = []

    # 检查系统提示词
    if not DEFAULT_SYSTEM_PROMPT.output_spec:
        issues.append("系统提示词 output_spec 为空")
    if "{output_format_spec}" not in DEFAULT_SYSTEM_PROMPT.content:
        issues.append("系统提示词缺少 {output_format_spec} 占位符")

    # 检查各策略
    for strategy in ALL_STRATEGIES:
        # 任务模板
        template = STRATEGY_TASK_TEMPLATES.get(strategy)
        if template is None:
            issues.append(f"策略 '{strategy}' 缺少任务模板")
        else:
            if "{event_context}" not in template.content:
                issues.append(f"策略 '{strategy}' 模板缺少 {{event_context}} 占位符")

        # 策略专属系统提示词
        if strategy not in STRATEGY_SYSTEM_TEMPLATES:
            issues.append(f"策略 '{strategy}' 缺少策略专属系统提示词")

    valid = len(issues) == 0
    logger.info("prompt validation", valid=valid, issue_count=len(issues))
    return {"valid": valid, "issues": issues}


def get_versions() -> dict[str, str]:
    """返回当前所有提示词组件的版本号快照。

    Returns:
        dict: {"system": "1.0.0", "direct_confirm": "1.0.0", ...}
    """
    versions: dict[str, str] = {
        "system": DEFAULT_SYSTEM_PROMPT.version,
    }
    for strategy in ALL_STRATEGIES:
        template = STRATEGY_TASK_TEMPLATES.get(strategy)
        versions[strategy] = template.version if template else "missing"
    return versions
