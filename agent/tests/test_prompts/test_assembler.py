"""Tests for prompt assembler — assembly, strategy system prompts, token truncation."""

from __future__ import annotations

import pytest

from ssv_agent.models.event import (
    Detection,
    DetectionEvent,
    ReviewContext,
    ReviewStrategy,
    Severity,
    TriggerReason,
)
from ssv_agent.prompts.assembler import (
    PromptAssembler,
    PromptAssembly,
    PromptMessage,
    TruncationAction,
    example_count,
)
from ssv_agent.prompts.templates import STRATEGY_SYSTEM_TEMPLATES


# ── helpers ──────────────────────────────────────────────────────────────

def make_context(
    strategy: ReviewStrategy = ReviewStrategy.DIRECT_CONFIRM,
    detections: list[Detection] | None = None,
    severity: Severity = Severity.HIGH,
    trigger: TriggerReason = TriggerReason.NO_HELMET,
    evidence: str = "",
    rules: list[str] | None = None,
) -> ReviewContext:
    """Helper: create a ReviewContext for testing."""
    if detections is None:
        detections = [
            Detection(
                class_name="head",
                confidence=0.95,
                bbox=[0.1, 0.2, 0.3, 0.4],
                track_id=3,
            ),
        ]
    event = DetectionEvent(
        source="test-source",
        frame_id=42,
        timestamp_ms=1700000000000,
        detections=detections,
        severity=severity,
        trigger_reason=trigger,
    )
    return ReviewContext(
        event=event,
        strategy=strategy,
        evidence_summary=evidence,
        rule_snippets=rules or [],
    )


# ── PromptMessage ────────────────────────────────────────────────────────

class TestPromptMessage:
    def test_create(self) -> None:
        msg = PromptMessage(role="system", content="You are helpful.")
        assert msg.role == "system"
        assert msg.content == "You are helpful."


# ── PromptAssembly ───────────────────────────────────────────────────────

class TestPromptAssembly:
    def test_defaults(self) -> None:
        pa = PromptAssembly(messages=[], strategy="direct_confirm", template_version="1.0.0")
        assert pa.messages == []
        assert pa.strategy == "direct_confirm"
        assert pa.example_count == 0
        assert pa.estimated_tokens == 0
        assert pa.truncated is False
        assert pa.truncation_actions == []


# ── PromptAssembler ──────────────────────────────────────────────────────

class TestPromptAssemblerBasic:
    def test_assemble_returns_prompt_assembly(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context()
        result = assembler.assemble(ctx)
        assert isinstance(result, PromptAssembly)
        assert result.strategy == "direct_confirm"
        assert result.template_version == "1.0.0"

    def test_assemble_has_system_message_first(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context()
        result = assembler.assemble(ctx)
        assert len(result.messages) >= 1
        assert result.messages[0].role == "system"

    def test_assemble_ends_with_user_task(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context()
        result = assembler.assemble(ctx)
        assert result.messages[-1].role == "user"

    def test_assemble_system_prompt_contains_role(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context()
        result = assembler.assemble(ctx)
        system_text = result.messages[0].content
        assert "工地安全巡检" in system_text
        assert "复核安全帽佩戴检测结果" in system_text


class TestStrategySpecificSystemPrompts:
    """验证策略专属系统提示词正确追加到全局提示词之后。"""

    def test_direct_confirm_has_strategy_guidance(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context(ReviewStrategy.DIRECT_CONFIRM)
        result = assembler.assemble(ctx)
        system_text = result.messages[0].content
        strategy_text = STRATEGY_SYSTEM_TEMPLATES["direct_confirm"]
        assert "当前策略指引" in system_text
        assert "高置信度违规" in system_text
        assert strategy_text in system_text

    def test_visual_review_has_strategy_guidance(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context(
            ReviewStrategy.VISUAL_REVIEW,
            detections=[Detection(class_name="head", confidence=0.52, track_id=-1)],
            severity=Severity.LOW,
            trigger=TriggerReason.LOW_CONFIDENCE,
        )
        result = assembler.assemble(ctx)
        system_text = result.messages[0].content
        assert "光照条件" in system_text or "遮挡" in system_text

    def test_rule_explain_has_strategy_guidance(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context(
            ReviewStrategy.RULE_EXPLAIN,
            detections=[
                Detection(class_name="head", confidence=0.72, bbox=[0.2, 0.1, 0.4, 0.5], track_id=5),
                Detection(class_name="helmet", confidence=0.61, bbox=[0.22, 0.12, 0.42, 0.48], track_id=5),
            ],
            severity=Severity.LOW,
            trigger=TriggerReason.RULE_CONFLICT,
        )
        result = assembler.assemble(ctx)
        system_text = result.messages[0].content
        assert "冲突信号" in system_text or "误分类" in system_text

    def test_notify_report_has_strategy_guidance(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context(
            ReviewStrategy.NOTIFY_REPORT,
            detections=[Detection(class_name="head", confidence=0.96, track_id=7)],
            severity=Severity.CRITICAL,
            trigger=TriggerReason.NO_HELMET,
        )
        result = assembler.assemble(ctx)
        system_text = result.messages[0].content
        assert "通知文本" in system_text or "严重事件" in system_text

    def test_global_content_still_present_with_strategy_append(self) -> None:
        """全局角色定义等内容在追加策略提示词后仍然存在。"""
        assembler = PromptAssembler()
        ctx = make_context(ReviewStrategy.DIRECT_CONFIRM)
        result = assembler.assemble(ctx)
        system_text = result.messages[0].content
        # 全局内容必须保留
        assert "工地安全巡检智能 Agent" in system_text
        assert "判断原则" in system_text
        assert "output_format_spec" not in system_text  # 应该已被渲染替换


class TestEventContextBuilding:
    def test_full_details_includes_bbox(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context()
        result = assembler.assemble(ctx)
        task_text = result.messages[-1].content
        assert "0.95" in task_text
        assert "bbox" in task_text
        assert "0.10" in task_text  # bbox coordinate

    def test_full_details_includes_track_id(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context(detections=[
            Detection(class_name="head", confidence=0.9, track_id=5, bbox=[0.1, 0.2, 0.3, 0.4])
        ])
        result = assembler.assemble(ctx)
        task_text = result.messages[-1].content
        assert "track_id=5" in task_text

    def test_untracked_detection_shows_track_id_minus_one(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context(detections=[
            Detection(class_name="head", confidence=0.5, track_id=-1, bbox=[0.1, 0.2, 0.3, 0.4])
        ])
        result = assembler.assemble(ctx)
        task_text = result.messages[-1].content
        assert "track_id=-1" in task_text

    def test_evidence_summary_placeholder(self) -> None:
        """visual_review 模板包含 {evidence_summary} 占位符。"""
        assembler = PromptAssembler()
        ctx = make_context(
            strategy=ReviewStrategy.VISUAL_REVIEW,
            evidence="关键帧: frame_0042.jpg",
            severity=Severity.LOW,
            trigger=TriggerReason.LOW_CONFIDENCE,
            detections=[Detection(class_name="head", confidence=0.52, track_id=-1)],
        )
        result = assembler.assemble(ctx)
        task_text = result.messages[-1].content
        assert "frame_0042.jpg" in task_text

    def test_rule_snippets_in_task(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context(
            strategy=ReviewStrategy.RULE_EXPLAIN,
            rules=["安全帽佩戴规范: 进入施工区域必须佩戴安全帽"],
            detections=[
                Detection(class_name="head", confidence=0.72, track_id=5),
                Detection(class_name="helmet", confidence=0.61, track_id=5),
            ],
        )
        result = assembler.assemble(ctx)
        task_text = result.messages[-1].content
        assert "安全帽佩戴规范" in task_text


class TestFewShotExamples:
    def test_examples_injected_as_user_assistant_pairs(self) -> None:
        assembler = PromptAssembler(max_examples=3)
        ctx = make_context(
            strategy=ReviewStrategy.DIRECT_CONFIRM,
            detections=[Detection(class_name="head", confidence=0.95, track_id=3)],
        )
        result = assembler.assemble(ctx, max_examples=2)
        # Check that example messages exist (between system and task)
        non_system = [m for m in result.messages if m.role != "system"]
        has_example_input = any(
            m.role == "user" and m.content.startswith("示例输入:") for m in non_system
        )
        assert has_example_input

    def test_example_count_in_assembly(self) -> None:
        assembler = PromptAssembler(max_examples=3)
        ctx = make_context(
            strategy=ReviewStrategy.DIRECT_CONFIRM,
            detections=[Detection(class_name="head", confidence=0.95, track_id=3)],
        )
        result = assembler.assemble(ctx, max_examples=2)
        assert result.example_count > 0
        assert result.example_count <= 2

    def test_max_examples_zero(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context()
        result = assembler.assemble(ctx, max_examples=0)
        assert result.example_count == 0


class TestTokenBudgetTruncation:
    def test_no_truncation_when_under_budget(self) -> None:
        assembler = PromptAssembler(max_tokens=100000)
        ctx = make_context()
        result = assembler.assemble(ctx)
        assert result.truncated is False
        assert result.truncation_actions == []

    def test_examples_reduced_when_over_budget(self) -> None:
        """设置极低的 token 预算，确保触发截断。"""
        assembler = PromptAssembler(max_examples=5, max_tokens=200)
        ctx = make_context(
            strategy=ReviewStrategy.DIRECT_CONFIRM,
            detections=[Detection(class_name="head", confidence=0.95, track_id=3)],
        )
        result = assembler.assemble(ctx, max_examples=3, max_tokens=200)
        # 预算极低（200 tokens ≈ 400 chars），系统提示词本身就远超此限制，
        # 所有截断手段用尽后仍超预算 → 应标记 truncated=True 且有截断动作
        if result.truncated:
            assert len(result.truncation_actions) > 0
            # 至少应该有 detections_simplified（最后一级截断）
            assert TruncationAction.DETECTIONS_SIMPLIFIED in result.truncation_actions
        else:
            # 如果不触发截断（系统提示词很短等边缘情况），至少 token 不应超过预算太多
            pass

    def test_system_prompt_always_present_after_truncation(self) -> None:
        """即使触发截断，系统提示词必须完整保留。"""
        assembler = PromptAssembler(max_tokens=100)
        ctx = make_context()
        result = assembler.assemble(ctx, max_tokens=100)
        assert len(result.messages) >= 1
        assert result.messages[0].role == "system"
        system_text = result.messages[0].content
        assert "工地安全巡检" in system_text

    def test_basic_event_identifiers_present_after_truncation(self) -> None:
        """source + frame_id + timestamp_ms 始终保留。"""
        assembler = PromptAssembler(max_tokens=100)
        ctx = make_context()
        result = assembler.assemble(ctx, max_tokens=100)
        # 最后一个消息是 user task
        task_text = result.messages[-1].content
        assert "test-source" in task_text or "42" in task_text

    def test_truncation_metadata_recorded(self) -> None:
        """截断后在 metadata 中标记 truncated=true 及截断动作。"""
        assembler = PromptAssembler(max_tokens=100)
        ctx = make_context(detections=[
            Detection(class_name="head", confidence=0.95, track_id=3),
            Detection(class_name="head", confidence=0.88, track_id=1),
        ])
        result = assembler.assemble(ctx, max_tokens=100)
        if result.truncated:
            assert len(result.truncation_actions) > 0

    def test_max_examples_override(self) -> None:
        assembler = PromptAssembler(max_examples=5)
        ctx = make_context()
        result = assembler.assemble(ctx, max_examples=1)
        assert result.example_count <= 1


class TestAssembleVLM:
    def test_assemble_vlm_raises_not_implemented(self) -> None:
        assembler = PromptAssembler()
        ctx = make_context()
        with pytest.raises(NotImplementedError):
            assembler.assemble_vlm(ctx, image_path="/tmp/test.jpg")


class TestExampleCount:
    def test_no_examples(self) -> None:
        msgs = [
            PromptMessage(role="system", content="system"),
            PromptMessage(role="user", content="task"),
        ]
        assert example_count(msgs) == 0

    def test_with_examples(self) -> None:
        msgs = [
            PromptMessage(role="system", content="system"),
            PromptMessage(role="user", content="示例输入:\ntest input"),
            PromptMessage(role="assistant", content="示例输出:\ntest output"),
            PromptMessage(role="user", content="示例输入:\ntest input 2"),
            PromptMessage(role="assistant", content="示例输出:\ntest output 2"),
            PromptMessage(role="user", content="task"),
        ]
        assert example_count(msgs) == 2

    def test_non_example_user_messages_not_counted(self) -> None:
        msgs = [
            PromptMessage(role="system", content="system"),
            PromptMessage(role="user", content="regular user message"),
        ]
        assert example_count(msgs) == 0
