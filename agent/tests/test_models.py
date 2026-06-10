"""Tests for T4 event domain models."""

from __future__ import annotations

import json

import pytest

from ssv_agent.models.event import (
    Detection,
    DetectionEvent,
    EventState,
    ReviewContext,
    ReviewResult,
    ReviewStrategy,
    Severity,
    ToolResult,
    TriggerReason,
)


# ── Detection ──────────────────────────────────────────────────────────────


class TestDetection:
    def test_from_redis_dict(self) -> None:
        """解析 ssvpub C++ 插件输出的 JSON 格式。"""
        data = {
            "class": "helmet",
            "class_id": 0,
            "confidence": 0.95,
            "bbox": [0.1, 0.2, 0.5, 0.8],
            "track_id": 5,
        }
        d = Detection.model_validate(data)
        assert d.class_name == "helmet"
        assert d.class_id == 0
        assert d.confidence == 0.95
        assert d.bbox == [0.1, 0.2, 0.5, 0.8]
        assert d.track_id == 5
        assert d.is_tracked is True

    def test_defaults(self) -> None:
        d = Detection()
        assert d.class_name == ""
        assert d.class_id == -1
        assert d.confidence == 0.0
        assert d.bbox == [0.0, 0.0, 0.0, 0.0]
        assert d.track_id == -1
        assert d.is_tracked is False

    def test_area(self) -> None:
        d = Detection(bbox=[0.0, 0.0, 0.5, 0.5])
        assert d.area == pytest.approx(0.25)

    def test_zero_area(self) -> None:
        d = Detection(bbox=[0.1, 0.1, 0.1, 0.1])
        assert d.area == 0.0

    def test_confidence_bounds(self) -> None:
        """置信度必须在 [0,1] 范围内。"""
        with pytest.raises(Exception):
            Detection(confidence=1.5)
        with pytest.raises(Exception):
            Detection(confidence=-0.1)

    def test_not_tracked_by_default(self) -> None:
        d = Detection(track_id=-1)
        assert d.is_tracked is False


# ── DetectionEvent ─────────────────────────────────────────────────────────


class TestDetectionEvent:
    def make_event(self, **kwargs) -> DetectionEvent:
        defaults = {
            "source": "camera-1",
            "frame_id": 42,
            "timestamp_ms": 1700000000000,
            "detections": [],
        }
        defaults.update(kwargs)
        return DetectionEvent.model_validate(defaults)

    def test_parse_from_redis_json(self) -> None:
        """解析 Redis Stream 中的完整事件 JSON。"""
        raw = json.dumps({
            "type": "detection",
            "source": "pipeline-0",
            "timestamp_ms": 1700000000000,
            "frame_id": 100,
            "detections": [
                {"class": "helmet", "class_id": 0, "confidence": 0.92, "bbox": [0.1, 0.2, 0.3, 0.4], "track_id": 1},
                {"class": "head", "class_id": 1, "confidence": 0.88, "bbox": [0.15, 0.25, 0.35, 0.45], "track_id": 2},
            ],
        })
        data = json.loads(raw)
        event = DetectionEvent.model_validate(data)
        assert event.source == "pipeline-0"
        assert event.frame_id == 100
        assert len(event.detections) == 2
        assert event.has_helmet is True
        assert event.has_head is True

    def test_no_helmet_detected(self) -> None:
        event = self.make_event(detections=[
            Detection(class_name="head", class_id=1, confidence=0.9, bbox=[0.1, 0.2, 0.3, 0.4])
        ])
        assert event.has_helmet is False
        assert event.has_head is True

    def test_empty_detections(self) -> None:
        event = self.make_event()
        assert event.has_helmet is False
        assert event.has_head is False
        assert event.max_confidence == 0.0
        assert event.helmet_detections == []
        assert event.head_detections == []

    def test_max_confidence(self) -> None:
        event = self.make_event(detections=[
            Detection(class_name="head", confidence=0.5),
            Detection(class_name="helmet", confidence=0.9),
        ])
        assert event.max_confidence == 0.9

    def test_tracked_objects(self) -> None:
        event = self.make_event(detections=[
            Detection(class_name="head", track_id=1),
            Detection(class_name="helmet", track_id=-1),
            Detection(class_name="head", track_id=2),
        ])
        tracked = event.tracked_objects
        assert len(tracked) == 2
        assert all(d.is_tracked for d in tracked)

    def test_infer_severity_no_helmet_high_conf(self) -> None:
        """有 head 无 helmet + 高置信度 → HIGH。"""
        event = self.make_event(detections=[
            Detection(class_name="head", confidence=0.95, bbox=[0.1, 0.1, 0.3, 0.3])
        ])
        assert event.infer_severity() == Severity.HIGH

    def test_infer_severity_both_present(self) -> None:
        """head + helmet 都存在 → LOW。"""
        event = self.make_event(detections=[
            Detection(class_name="head", confidence=0.9),
            Detection(class_name="helmet", confidence=0.85),
        ])
        assert event.infer_severity() == Severity.LOW

    def test_infer_severity_low_conf(self) -> None:
        """低置信度 → LOW 或 VISUAL_REVIEW 策略。"""
        event = self.make_event(detections=[
            Detection(class_name="head", confidence=0.5)
        ])
        assert event.infer_severity() == Severity.MEDIUM

    def test_infer_trigger_no_helmet(self) -> None:
        event = self.make_event(detections=[
            Detection(class_name="head", confidence=0.9),
        ])
        assert event.infer_trigger_reason() == TriggerReason.NO_HELMET

    def test_infer_trigger_low_conf(self) -> None:
        event = self.make_event(detections=[
            Detection(class_name="head", confidence=0.5),
        ])
        assert event.infer_trigger_reason() == TriggerReason.LOW_CONFIDENCE

    def test_select_strategy_direct_confirm(self) -> None:
        """高置信度违规 → 直接确认。"""
        event = self.make_event(detections=[
            Detection(class_name="head", confidence=0.95, bbox=[0.1, 0.1, 0.3, 0.3])
        ])
        assert event.select_strategy() == ReviewStrategy.DIRECT_CONFIRM

    def test_select_strategy_visual_review(self) -> None:
        """低置信度 → 视觉复核。"""
        event = self.make_event(detections=[
            Detection(class_name="head", confidence=0.5)
        ])
        assert event.select_strategy() == ReviewStrategy.VISUAL_REVIEW

    def test_select_strategy_rule_explain(self) -> None:
        """中等严重度但置信度较高 → 规则解释。"""
        event = self.make_event(detections=[
            Detection(class_name="head", confidence=0.75),
        ])
        assert event.select_strategy() == ReviewStrategy.RULE_EXPLAIN


# ── ReviewResult ───────────────────────────────────────────────────────────


class TestReviewResult:
    def test_terminal_states(self) -> None:
        for state in (EventState.COMPLETED, EventState.FAILED, EventState.NEEDS_HUMAN):
            r = ReviewResult(final_state=state)
            assert r.is_terminal is True

    def test_non_terminal_states(self) -> None:
        for state in (EventState.PARSING, EventState.TOOL_CALLING, EventState.PENDING):
            r = ReviewResult(final_state=state)
            assert r.is_terminal is False

    def test_succeeded(self) -> None:
        r = ReviewResult(final_state=EventState.COMPLETED)
        assert r.succeeded is True

    def test_failed(self) -> None:
        r = ReviewResult(final_state=EventState.FAILED)
        assert r.succeeded is False

    def test_with_tool_results(self) -> None:
        r = ReviewResult(
            event_id="evt-1",
            final_state=EventState.COMPLETED,
            conclusion="安全帽佩戴正常",
            tool_results=[
                ToolResult(tool_name="visual_review", success=True, output="确认佩戴"),
            ],
        )
        assert len(r.tool_results) == 1


# ── ReviewContext ──────────────────────────────────────────────────────────


class TestReviewContext:
    def test_prompt_context_generation(self) -> None:
        event = DetectionEvent(
            source="pipeline-0",
            frame_id=42,
            detections=[
                Detection(class_name="helmet", confidence=0.92, bbox=[0.1, 0.1, 0.3, 0.3], track_id=5),
            ],
        )
        ctx = ReviewContext(
            event=event,
            strategy=ReviewStrategy.DIRECT_CONFIRM,
            evidence_summary="关键帧: frame_0042.jpg",
        )
        prompt = ctx.prompt_context
        assert "pipeline-0" in prompt
        assert "42" in prompt
        assert "helmet" in prompt
        assert "0.92" in prompt
        assert "frame_0042.jpg" in prompt

    def test_prompt_context_with_rules(self) -> None:
        event = DetectionEvent(source="cam-1", frame_id=1)
        ctx = ReviewContext(
            event=event,
            strategy=ReviewStrategy.RULE_EXPLAIN,
            rule_snippets=["安全帽佩戴规范: 进入施工区域必须佩戴安全帽"],
        )
        prompt = ctx.prompt_context
        assert "安全帽佩戴规范" in prompt


# ── ToolResult ─────────────────────────────────────────────────────────────


class TestToolResult:
    def test_successful_tool(self) -> None:
        tr = ToolResult(tool_name="visual_review", success=True, output="通过")
        assert tr.tool_name == "visual_review"
        assert tr.success is True
        assert tr.error == ""

    def test_failed_tool(self) -> None:
        tr = ToolResult(tool_name="visual_review", success=False, error="模型不可用")
        assert tr.success is False
        assert tr.error == "模型不可用"
