"""提示词模板 —— 按复核策略维度组织，每种策略持有独立的模板变体。

设计原则:
  1. 模板与策略一一对应，便于单策略独立迭代
  2. 使用 {placeholder} 占位符语法，与 PromptAssembler 协作填充
  3. 每个模板都有 version 字段，记录变更历史
  4. 模板内容使用中文（业务领域语言），技术术语保留英文
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum


class PromptRole(str, Enum):
    """提示词角色类型。"""
    SYSTEM = "system"       # 系统提示词
    USER = "user"           # 用户消息（上下文 + 任务）
    ASSISTANT = "assistant" # 助手示例回复（few-shot）


@dataclass
class PromptTemplate:
    """单条提示词模板。

    示例:
        template = PromptTemplate(
            name="direct_confirm_v1",
            strategy="direct_confirm",
            role=PromptRole.USER,
            version="1.0.0",
            content="事件摘要: {event_summary}\n请确认以下检测结果并给出结论。",
        )
    """

    name: str                          # 模板唯一标识
    strategy: str                      # 所属复核策略
    role: PromptRole                   # 提示词角色
    version: str = "1.0.0"            # 语义版本号
    content: str = ""                  # 模板正文，含 {placeholder}
    description: str = ""             # 模板用途说明
    changelog: list[str] = field(default_factory=list)  # 版本变更记录


# ── 策略模板集 ─────────────────────────────────────────────────────────────


@dataclass
class StrategyTemplateSet:
    """一条复核策略的完整提示词模板集。

    包含: 系统提示词 + 任务模板 + 可选 few-shot 模板
    """

    strategy: str                                    # 策略标识
    system_template: PromptTemplate                  # 系统级提示词
    task_template: PromptTemplate                    # 任务提示词
    output_format_hint: str = ""                     # 输出格式约束（追加到 system）
    few_shot_examples: list[dict[str, str]] = field(default_factory=list)
    # few_shot_examples: [{"input": "...", "output": "..."}, ...]
    metadata: dict = field(default_factory=dict)     # 扩展元数据


# ── 默认模板内容 ──────────────────────────────────────────────────────────
#
# 注 (D1): SYSTEM_PROMPT_CONTENT 和 OUTPUT_FORMAT_SPEC 已从本文件移除。
# prompts/system.py 的 DEFAULT_SYSTEM_PROMPT 为 canonical source。
# 如需引用，使用: from ssv_agent.prompts.system import DEFAULT_SYSTEM_PROMPT

# ── 策略专属系统提示词（追加到全局系统提示词之后） ──────────────────────────
#
# 不同策略对 LLM 的引导侧重点不同。策略专属提示词是**追加**而非替换，
# 全局系统提示词中的角色定义、输出格式、行为约束保持不变。

STRATEGY_SYSTEM_TEMPLATES: dict[str, str] = {
    "direct_confirm": (
        "## 当前策略指引\n"
        "当前事件已被管线判定为高置信度违规，你的默认立场是**确认**，"
        "除非发现明确的矛盾信号（如检测框不稳定、多帧间类别反复跳变等）。"
    ),
    "visual_review": (
        "## 当前策略指引\n"
        "当前事件置信度较低或存在遮挡。重点关注：光照条件、目标遮挡程度、"
        "检测框稳定性。低置信度不必然意味着误检——也可能是目标部分可见、"
        "光照不足或运动模糊导致。请综合判断而非直接否定。"
    ),
    "rule_explain": (
        "## 当前策略指引\n"
        "当前事件存在冲突信号（head+helmet 同时出现），你需要判断是误分类"
        "还是正常佩戴。bbox 重叠程度是关键依据：若 head 和 helmet 的 bbox "
        "高度重叠且属于同一 track_id，倾向于判定为已佩戴安全帽。"
    ),
    "notify_report": (
        "## 当前策略指引\n"
        "当前为严重事件。除复核判断外，你还需要生成面向现场安全员的通知文本，"
        "包含：时间、位置、违规描述、建议处置措施。通知文本放在 conclusion 字段中。"
    ),
}

# 四条策略的任务模板
STRATEGY_TASK_TEMPLATES = {
    "direct_confirm": PromptTemplate(
        name="direct_confirm_task",
        strategy="direct_confirm",
        role=PromptRole.USER,
        version="1.0.0",
        content="""## 复核任务: 直接确认

以下检测事件已被管线判定为高置信度违规，请直接确认或质疑:

{event_context}

## 请回答
1. 检测结果是否合理？
2. 是否需要降级或升级严重程度？
3. 给出最终结论（JSON 格式）。""",
        description="高置信度违规的直接确认模板",
    ),
    "visual_review": PromptTemplate(
        name="visual_review_task",
        strategy="visual_review",
        role=PromptRole.USER,
        version="1.0.0",
        content="""## 复核任务: 视觉复核

以下检测事件置信度较低或存在遮挡，需要你结合关键帧图像进行复核判断:

{event_context}

## 关键帧信息
{evidence_summary}

## 请回答
1. 基于检测数据和关键帧描述，判断是否真的存在违规？
2. 低置信度可能的原因是什么（遮挡/光照/模型局限）？
3. 给出最终结论（JSON 格式）。""",
        description="低置信度/遮挡场景的视觉复核模板",
    ),
    "rule_explain": PromptTemplate(
        name="rule_explain_task",
        strategy="rule_explain",
        role=PromptRole.USER,
        version="1.0.0",
        content="""## 复核任务: 规则解释

以下检测事件中存在矛盾信号（如 head 和 helmet 同时出现），需要结合安全规则进行解释性复核:

{event_context}

## 适用规则
{rule_snippets}

## 请回答
1. 当前检测信号是否存在冲突？如何解释这种冲突？
2. 根据安全规则，应该判定为违规还是合规？
3. 给出最终结论（JSON 格式）。""",
        description="检测信号冲突时的规则解释模板",
    ),
    "notify_report": PromptTemplate(
        name="notify_report_task",
        strategy="notify_report",
        role=PromptRole.USER,
        version="1.0.0",
        content="""## 复核任务: 严重事件通知报告

以下检测事件为严重安全违规，除复核判断外，还需要生成通知报告内容:

{event_context}

## 请回答
1. 确认事件严重程度是否合理？
2. 生成一段面向现场安全员的通知文本（包含: 时间、位置、违规描述、建议处置）
3. 给出最终结论（JSON 格式），并在 conclusion 字段中附上通知文本。""",
        description="严重安全事件的通知报告生成模板",
    ),
}
