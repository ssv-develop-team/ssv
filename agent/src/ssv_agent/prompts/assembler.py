"""提示词组装器 —— 消费 ContextPack，产出 PromptAssembly（messages 列表）。

设计原则（D2/D5）:
  1. 组装逻辑与模板内容分离，修改模板不需要改代码
  2. BudgetEngine 分配配额，PromptAssembler 执行截断
  3. few-shot 选择保留在组装层（与目标 LLM 上下文窗口强相关）
  4. 模板渲染已委托给 UserInputBuilder，assembler 仅负责"拼接 + 截断"
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Optional

import structlog

from ssv_agent.context.pack import ContextPack
from ssv_agent.prompts.examples import (
    DIRECT_CONFIRM_EXAMPLES,
    VISUAL_REVIEW_EXAMPLES,
    RULE_EXPLAIN_EXAMPLES,
    NOTIFY_REPORT_EXAMPLES,
    ExampleBank,
    FewShotExample,
)

logger = structlog.get_logger()

# 策略 → 示例列表映射
_STRATEGY_EXAMPLE_MAP: dict[str, list[FewShotExample]] = {
    "direct_confirm": DIRECT_CONFIRM_EXAMPLES,
    "visual_review": VISUAL_REVIEW_EXAMPLES,
    "rule_explain": RULE_EXPLAIN_EXAMPLES,
    "notify_report": NOTIFY_REPORT_EXAMPLES,
}

# 默认 token 预算（字符数/2 估算）
DEFAULT_MAX_TOKENS = 4096


class TruncationAction(str, Enum):
    """截断动作类型，记录在 PromptAssembly.metadata 中便于回溯。"""

    NONE = "none"
    EXAMPLES_REDUCED = "examples_reduced"
    EXAMPLES_REMOVED = "examples_removed"
    RULES_TRIMMED = "rules_trimmed"
    DETECTIONS_SIMPLIFIED = "detections_simplified"
    HISTORY_TRIMMED = "history_trimmed"


@dataclass
class PromptMessage:
    """单条 prompt 消息，兼容 OpenAI / Anthropic 消息格式。

    content 类型预留为 str | list[dict]，当前用 str，
    后续扩展 list[dict] 支持多模态（VLM 图片输入）。
    """

    role: str       # system / user / assistant
    content: str    # 当前为纯文本，后续可扩展为 str | list[dict]


@dataclass
class PromptAssembly:
    """组装完成的提示词。

    包含完整的 messages 列表和元信息，可直接传给 OpenAI/Anthropic API。
    """

    messages: list[PromptMessage]
    strategy: str
    template_version: str = ""
    example_count: int = 0
    estimated_tokens: int = 0
    truncated: bool = False
    truncation_actions: list[TruncationAction] = field(default_factory=list)
    metadata: dict = field(default_factory=dict)


class PromptAssembler:
    """提示词组装器 —— 消费 ContextPack，产出 PromptAssembly。

    用法:
        assembler = PromptAssembler(max_tokens=4096)
        assembly = assembler.assemble(context_pack)
        # assembly.messages → 可直接传给 OpenAI/Anthropic API

    组装流程:
        系统提示词 → 工具定义注入 → few-shot 示例 → 任务 user message
        → Token 预算检查与逐级截断 → PromptAssembly
    """

    def __init__(
        self,
        max_examples: int = 3,
        max_tokens: int = DEFAULT_MAX_TOKENS,
    ) -> None:
        self._max_examples = max_examples
        self._max_tokens = max_tokens

    # ── 主入口 ─────────────────────────────────────────────────────────

    def assemble(
        self,
        pack,
        max_examples: Optional[int] = None,
        max_tokens: Optional[int] = None,
    ) -> PromptAssembly:
        """组装完整的提示词。

        M4 兼容: pack 可以是 ContextPack 或 ReviewContext。
        ReviewContext 会自动转换为 ContextPack（M5 移除兼容）。

        Args:
            pack: ContextPack 或 ReviewContext。
            max_examples: 最多注入几条 few-shot 示例（None 则用默认值）。
            max_tokens: token 预算上限（None 则用默认值）。

        Returns:
            PromptAssembly: 组装完成的提示词，含截断元信息。
        """
        max_n = max_examples if max_examples is not None else self._max_examples
        budget = max_tokens if max_tokens is not None else self._max_tokens

        # M4 兼容: ReviewContext → ContextPack
        if not isinstance(pack, ContextPack):
            pack = self._review_context_to_pack(pack)

        strategy = pack.metadata.get("strategy", "direct_confirm")
        truncation_actions: list[TruncationAction] = []

        # 1. 系统提示词（已由 ContextEngine/SystemPromptManager 渲染）
        system_content = pack.system_prompt.content
        system_msg = PromptMessage(role="system", content=system_content)

        # 2. Few-shot 示例
        examples = self._select_examples(strategy, pack, max_n)
        example_msgs = self._render_examples(examples)

        # 3. 用户输入（已由 ContextEngine/UserInputBuilder 渲染）
        task_content = pack.user_input.event_context
        task_msg = PromptMessage(role="user", content=task_content)

        # 4. Token 预算检查与逐级截断
        all_msgs = [system_msg] + example_msgs + [task_msg]
        estimated = self._estimate_tokens(all_msgs)

        if estimated > budget:
            all_msgs, estimated, actions = self._apply_token_budget(
                system_msg=system_msg,
                examples=examples,
                pack=pack,
                strategy=strategy,
                budget=budget,
            )
            truncation_actions.extend(actions)

        # 组装元信息
        truncated = len(truncation_actions) > 0 and any(
            a != TruncationAction.NONE for a in truncation_actions
        )

        # 记录预算核算
        truncation_details = {}
        if pack.token_budget is not None:
            from ssv_agent.context.budget import TokenBudget
            tb = pack.token_budget
            if isinstance(tb, TokenBudget):
                truncation_details = {
                    "budget_allocations": tb.allocations,
                    "budget_total": tb.total,
                    "actual_tokens": estimated,
                    "within_budget": estimated <= tb.total,
                }

        # 版本信息
        template_version = pack.system_prompt.versions.get(
            "output_constraints", "1.0.0"
        )

        logger.debug(
            "prompt assembled",
            strategy=strategy,
            messages=len(all_msgs),
            examples=example_count(all_msgs),
            estimated_tokens=estimated,
            truncated=truncated,
            context_id=pack.context_id,
        )

        return PromptAssembly(
            messages=all_msgs,
            strategy=strategy,
            template_version=template_version,
            example_count=example_count(all_msgs),
            estimated_tokens=estimated,
            truncated=truncated,
            truncation_actions=truncation_actions,
            metadata={
                "system_prompt_versions": pack.system_prompt.versions,
                "context_pack_id": pack.context_id,
                "truncation_details": truncation_details,
                "budget_allocations": (
                    pack.token_budget.allocations
                    if pack.token_budget is not None
                    else {}
                ),
            },
        )

    # ── VLM 预留接口 ────────────────────────────────────────────────────

    def assemble_vlm(
        self,
        pack: ContextPack,
        image_path: str,
    ) -> PromptAssembly:
        """组装 VLM 多模态提示词（预留接口，当前阶段不实现）。

        Args:
            pack: ContextPack。
            image_path: 关键帧图像文件路径。

        Raises:
            NotImplementedError: 当前阶段未实现，M5 接入真实 VLM 时再展开。
        """
        raise NotImplementedError(
            "VLM 多模态提示词组装尚未实现，将在 M5 接入真实 VLM 时展开。"
            "当前可复用 templates.py 中的模板内容，仅消息构造方式不同。"
        )

    # ── M4 兼容 ────────────────────────────────────────────────────────

    def _review_context_to_pack(self, context) -> ContextPack:
        """M4 兼容: 将 ReviewContext 转换为 ContextPack。

        使用旧版渲染逻辑填充 system_prompt 和 user_input。
        M5 移除。
        """
        from ssv_agent.context.pack import (
            ContextPack,
            HistoryBlock,
            RetrievalBlock,
            RetrievalItem,
            SystemPromptBlock,
            ToolDefinitionsBlock,
            UserInputBlock,
        )

        strategy = context.strategy.value if context.strategy else "direct_confirm"

        # 使用旧版渲染逻辑填充系统提示词
        from ssv_agent.prompts.system import DEFAULT_SYSTEM_PROMPT
        from ssv_agent.prompts.templates import STRATEGY_SYSTEM_TEMPLATES

        system_text = DEFAULT_SYSTEM_PROMPT.render()
        strategy_append = STRATEGY_SYSTEM_TEMPLATES.get(strategy)
        if strategy_append:
            system_text += "\n\n" + strategy_append

        return ContextPack(
            system_prompt=SystemPromptBlock(
                content=system_text,
                versions={"role": "1.0.0"},
            ),
            tool_definitions=ToolDefinitionsBlock(),
            history=HistoryBlock(),
            retrieval_context=RetrievalBlock(
                items=[
                    RetrievalItem(content=r, source="rule", source_type="regulation")
                    for r in (context.rule_snippets or [])
                ]
            ),
            user_input=UserInputBlock(
                event_context=context.prompt_context,
                evidence_summary=context.evidence_summary or "",
            ),
            metadata={
                "event": context.event,
                "strategy": strategy,
            },
        )

    # ── 内部方法 ───────────────────────────────────────────────────────

    def _render_examples(self, examples: list[FewShotExample]) -> list[PromptMessage]:
        """将 few-shot 示例渲染为 user/assistant 消息对。"""
        msgs: list[PromptMessage] = []
        for ex in examples:
            msgs.append(
                PromptMessage(role="user", content=f"示例输入:\n{ex.input_context}")
            )
            msgs.append(
                PromptMessage(role="assistant", content=f"示例输出:\n{ex.expected_output}")
            )
        return msgs

    def _select_examples(
        self, strategy: str, pack: ContextPack, max_count: int
    ) -> list[FewShotExample]:
        """为当前上下文选择最相关的 few-shot 示例。"""
        examples = _STRATEGY_EXAMPLE_MAP.get(strategy, [])
        if not examples:
            return []

        tags = self._extract_context_tags(pack)
        bank = ExampleBank(strategy=strategy, examples=examples)
        selected = bank.select_by_tags(tags, max_count)
        if not selected:
            selected = bank.top_quality(max_count)
        return selected

    def _extract_context_tags(self, pack: ContextPack) -> list[str]:
        """从 ContextPack 中提取特征标签，用于示例检索。"""
        tags: list[str] = []
        event = pack.metadata.get("event")
        if event is None:
            return tags

        severity = event.severity.value if event.severity else "low"

        if severity in ("high", "critical"):
            tags.append("high_confidence")
        elif severity == "low":
            tags.append("low_confidence")

        det_count = len(event.detections)
        if det_count == 1:
            tags.append("single_person")
        elif det_count > 1:
            tags.append("multi_person")

        if event.has_head and not event.has_helmet:
            tags.append("no_helmet")
        if event.has_head and event.has_helmet:
            tags.append("rule_conflict")

        if event.trigger_reason:
            reason = event.trigger_reason.value
            if reason == "low_confidence":
                tags.append("low_confidence")
            elif reason == "consecutive_hits":
                tags.append("consecutive_hits")
            elif reason == "rule_conflict":
                tags.append("rule_conflict")

        if self._has_overlapping_boxes(event):
            tags.append("overlapping_boxes")

        return tags

    @staticmethod
    def _has_overlapping_boxes(event) -> bool:
        """检测是否有 bbox 重叠的情况。"""
        dets = event.detections
        if len(dets) < 2:
            return False
        for i in range(len(dets)):
            for j in range(i + 1, len(dets)):
                a, b = dets[i].bbox, dets[j].bbox
                if a[0] < b[2] and a[2] > b[0] and a[1] < b[3] and a[3] > b[1]:
                    return True
        return False

    def _estimate_tokens(self, messages: list[PromptMessage]) -> int:
        """粗略估算 token 数（字符数/2）。

        TODO: 后续接入 tiktoken 精确计数。
        """
        return sum(len(m.content) for m in messages) // 2

    # ── Token 预算截断 ──────────────────────────────────────────────────

    def _apply_token_budget(
        self,
        system_msg: PromptMessage,
        examples: list[FewShotExample],
        pack: ContextPack,
        strategy: str,
        budget: int,
    ) -> tuple[list[PromptMessage], int, list[TruncationAction]]:
        """按优先级逐级截断，直到 token 数不超过预算。

        截断优先级（从低到高，越低越先裁）:
          1. 减少 few-shot 示例条数（N→0）
          2. 历史记录: 非关联→旧事件→全部移除仅留 summary
          3. 检索上下文: relevance 从低到高逐条移除
          4. 检测详情: 精简模式（去 bbox）→ 仅保留前 5 条
          — 系统提示词 / 工具定义 / 事件基本标识 — 永不裁剪

        Returns:
            (messages, estimated_tokens, truncation_actions)
        """
        actions: list[TruncationAction] = []

        # 层级 1: 逐步减少 few-shot 示例
        for n in range(len(examples), -1, -1):
            trimmed_examples = examples[:n]
            example_msgs = self._render_examples(trimmed_examples)
            task_msg = PromptMessage(
                role="user", content=pack.user_input.event_context
            )
            msgs = [system_msg] + example_msgs + [task_msg]
            est = self._estimate_tokens(msgs)

            if est <= budget:
                if n < len(examples):
                    action = (
                        TruncationAction.EXAMPLES_REMOVED
                        if n == 0
                        else TruncationAction.EXAMPLES_REDUCED
                    )
                    actions.append(action)
                    logger.debug(
                        "token budget: examples reduced",
                        from_count=len(examples),
                        to_count=n,
                        estimated_tokens=est,
                        budget=budget,
                    )
                return msgs, est, actions

        # 层级 2: 0 条示例仍超预算 → 精简检索上下文
        if pack.retrieval_context.items:
            # 保留最高 priority 的 1 条
            original_count = len(pack.retrieval_context.items)
            if original_count > 1:
                best = sorted(
                    pack.retrieval_context.items,
                    key=lambda x: x.priority,
                )[0]
                saved = pack.retrieval_context.items
                pack.retrieval_context.items = [best]
                # 需重新渲染 user input（规则部分变化）
                # 由于 pack.user_input.event_context 已经渲染，
                # 这里直接调整 task_msg 内容——精简规则文本
                task_content = pack.user_input.event_context
                task_msg = PromptMessage(role="user", content=task_content)
                msgs = [system_msg] + [task_msg]
                est = self._estimate_tokens(msgs)
                pack.retrieval_context.items = saved

                if est <= budget:
                    actions.append(TruncationAction.RULES_TRIMMED)
                    logger.debug(
                        "token budget: rules trimmed",
                        from_count=original_count,
                        to_count=1,
                        estimated_tokens=est,
                        budget=budget,
                    )
                    return msgs, est, actions

        # 层级 3: 仍超预算 → 精简检测详情（不重新渲染 user input，
        # 而是通过替换 bbox 细节——此处通过字符串截断模拟）
        task_content = pack.user_input.event_context
        # 简化为仅保留前 5 行的基本事件信息 + 类名/置信度
        lines = task_content.split("\n")
        detail_start = -1
        for i, line in enumerate(lines):
            if line.startswith("检测详情:"):
                detail_start = i
                break
        if detail_start >= 0:
            simple_lines = lines[:detail_start + 1]
            event = pack.metadata.get("event")
            if event:
                for i, d in enumerate(event.detections[:5]):
                    simple_lines.append(
                        f"  [{i}] {d.class_name} conf={d.confidence:.2f}"
                    )
                if len(event.detections) > 5:
                    simple_lines.append(f"  ... 共{len(event.detections)}个检测")
            task_content = "\n".join(simple_lines)

        task_msg = PromptMessage(role="user", content=task_content)
        msgs = [system_msg] + [task_msg]
        est = self._estimate_tokens(msgs)

        if est <= budget:
            actions.append(TruncationAction.DETECTIONS_SIMPLIFIED)
            logger.debug(
                "token budget: detections simplified",
                estimated_tokens=est,
                budget=budget,
            )
            return msgs, est, actions

        # 所有截断手段用尽仍超预算 → 返回最精简版本，标记截断
        actions.append(TruncationAction.DETECTIONS_SIMPLIFIED)
        logger.warning(
            "token budget: still over budget after all truncations",
            estimated_tokens=est,
            budget=budget,
            strategy=strategy,
        )
        return msgs, est, actions


def example_count(messages: list[PromptMessage]) -> int:
    """计算消息列表中包含的 few-shot 示例对数。"""
    count = 0
    for m in messages:
        if m.role == "user" and m.content.startswith("示例输入:"):
            count += 1
    return count
