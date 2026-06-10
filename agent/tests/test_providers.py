"""Tests for T4 provider abstractions."""

from __future__ import annotations

from ssv_agent.models.event import (
    Detection,
    DetectionEvent,
    ReviewContext,
    ReviewStrategy,
)
from ssv_agent.providers.base import BaseProvider, ReviewConclusion
from ssv_agent.providers.mock import MockProvider, MockVLMProvider


class TestMockProvider:
    def test_provider_name(self) -> None:
        p = MockProvider()
        assert p.provider_name == "mock-text-provider"

    def test_fixed_response(self) -> None:
        p = MockProvider(fixed_response="custom response")
        ctx = make_context(ReviewStrategy.DIRECT_CONFIRM)
        result = p.analyze(ctx)
        assert result == "custom response"

    def test_direct_confirm_response(self) -> None:
        p = MockProvider()
        ctx = make_context(ReviewStrategy.DIRECT_CONFIRM)
        result = p.analyze(ctx)
        assert "直接确认" in result

    def test_visual_review_response(self) -> None:
        p = MockProvider()
        ctx = make_context(ReviewStrategy.VISUAL_REVIEW)
        result = p.analyze(ctx)
        assert "视觉复核" in result

    def test_rule_explain_response(self) -> None:
        p = MockProvider()
        ctx = make_context(ReviewStrategy.RULE_EXPLAIN)
        result = p.analyze(ctx)
        assert "规则解释" in result
        assert "安全帽佩戴管理规定" in result

    def test_notify_report_response(self) -> None:
        p = MockProvider()
        ctx = make_context(ReviewStrategy.NOTIFY_REPORT)
        result = p.analyze(ctx)
        assert "通知报告" in result

    def test_call_count(self) -> None:
        p = MockProvider()
        ctx = make_context(ReviewStrategy.DIRECT_CONFIRM)
        assert p.call_count == 0
        p.analyze(ctx)
        p.analyze(ctx)
        assert p.call_count == 2

    def test_latency_simulation(self) -> None:
        import time
        p = MockProvider(simulate_latency_ms=50)
        ctx = make_context(ReviewStrategy.DIRECT_CONFIRM)
        start = time.time()
        p.analyze(ctx)
        elapsed = (time.time() - start) * 1000
        assert elapsed >= 40  # allow some variance


class TestMockVLMProvider:
    def test_provider_name(self) -> None:
        p = MockVLMProvider()
        assert p.provider_name == "mock-vlm-provider"

    def test_default_conclusion(self) -> None:
        p = MockVLMProvider()
        result = p.review_keyframe("/tmp/frame_42.jpg", {})
        assert isinstance(result, ReviewConclusion)
        assert result.is_helmet_worn is False
        assert "未佩戴安全帽" in result.explanation
        assert result.confidence > 0

    def test_fixed_conclusion(self) -> None:
        fixed = ReviewConclusion(
            is_helmet_worn=True,
            explanation="安全帽佩戴正常",
            confidence=0.95,
        )
        p = MockVLMProvider(fixed_conclusion=fixed)
        result = p.review_keyframe("/tmp/frame_42.jpg", {})
        assert result.is_helmet_worn is True
        assert result.explanation == "安全帽佩戴正常"

    def test_call_count(self) -> None:
        p = MockVLMProvider()
        assert p.call_count == 0
        p.review_keyframe("/tmp/frame_1.jpg", {})
        p.review_keyframe("/tmp/frame_2.jpg", {})
        assert p.call_count == 2

    def test_key_observations(self) -> None:
        p = MockVLMProvider()
        result = p.review_keyframe("/tmp/frame_42.jpg", {})
        assert len(result.key_observations) == 2


# ── Helpers ──────────────────────────────────────────────────────────────────


def make_context(strategy: ReviewStrategy) -> ReviewContext:
    event = DetectionEvent(
        source="cam-1",
        frame_id=42,
        detections=[
            Detection(class_name="head", confidence=0.95, bbox=[0.1, 0.1, 0.3, 0.3], track_id=1),
        ],
    )
    return ReviewContext(event=event, strategy=strategy)
