"""Tests for T4 provider abstractions."""

from __future__ import annotations

import sys
import types

from ssv_agent.models.event import (
    Detection,
    DetectionEvent,
    ReviewContext,
    ReviewStrategy,
)
from ssv_agent.providers.base import ReviewConclusion
from ssv_agent.providers.local_yolo import LocalYoloProvider
from ssv_agent.providers.mock import MockProvider, MockVLMProvider
from ssv_agent.providers.openai_compatible import OpenAICompatibleProvider


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


class TestLocalYoloProvider:
    def test_provider_name(self) -> None:
        p = LocalYoloProvider(model_path="/tmp/model.pt")
        assert p.provider_name == "local-yolo-provider"

    def test_missing_dependency_reports_clear_error(self, tmp_path, monkeypatch) -> None:
        model_path = tmp_path / "model.pt"
        image_path = tmp_path / "frame.jpg"
        model_path.write_bytes(b"fake")
        image_path.write_bytes(b"fake")
        monkeypatch.setitem(sys.modules, "ultralytics", None)

        p = LocalYoloProvider(model_path=str(model_path))
        try:
            p.review_keyframe(str(image_path), {})
        except RuntimeError as exc:
            assert "ultralytics is required" in str(exc)
        else:
            raise AssertionError("expected RuntimeError")

    def test_analyze_with_fake_ultralytics(self, tmp_path, monkeypatch) -> None:
        model_path = tmp_path / "model.pt"
        image_path = tmp_path / "frame.jpg"
        model_path.write_bytes(b"fake")
        image_path.write_bytes(b"fake")

        class FakeBox:
            cls = 0
            conf = 0.91

        class FakeResult:
            names = {0: "helmet"}
            boxes = [FakeBox()]

        class FakeYOLO:
            def __init__(self, path):
                self.path = path

            def __call__(self, *args, **kwargs):
                return [FakeResult()]

        fake_module = types.SimpleNamespace(YOLO=FakeYOLO)
        monkeypatch.setitem(sys.modules, "ultralytics", fake_module)

        p = LocalYoloProvider(model_path=str(model_path))
        ctx = make_context(ReviewStrategy.VISUAL_REVIEW)
        ctx.event.evidence_paths = [str(image_path)]

        result = p.analyze(ctx)
        assert "已佩戴安全帽" in result
        assert "helmet conf=0.91" in result


class TestOpenAICompatibleProvider:
    def test_provider_name(self) -> None:
        p = OpenAICompatibleProvider(
            base_url="https://example.test/compatible-mode/v1",
            text_model="qwen3.7-plus",
        )
        assert p.provider_name == "openai-compatible-provider"

    def test_missing_api_key_reports_clear_error(self, monkeypatch) -> None:
        monkeypatch.delenv("SSV_AGENT_API_KEY", raising=False)
        monkeypatch.delenv("DASHSCOPE_API_KEY", raising=False)
        p = OpenAICompatibleProvider(
            base_url="https://example.test/compatible-mode/v1",
            text_model="qwen3.7-plus",
        )

        try:
            p.analyze(make_context(ReviewStrategy.RULE_EXPLAIN))
        except RuntimeError as exc:
            assert "SSV_AGENT_API_KEY" in str(exc)
        else:
            raise AssertionError("expected RuntimeError")

    def test_analyze_posts_chat_completion(self, monkeypatch) -> None:
        captured = {}

        class FakeResponse:
            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def read(self):
                return (
                    '{"choices":[{"message":{"content":"AI 复核结论"}}]}'
                ).encode("utf-8")

        def fake_urlopen(request, timeout):
            captured["url"] = request.full_url
            captured["headers"] = dict(request.header_items())
            captured["timeout"] = timeout
            captured["body"] = request.data
            return FakeResponse()

        monkeypatch.setenv("SSV_AGENT_API_KEY", "test-key")
        monkeypatch.setattr("ssv_agent.providers.openai_compatible.urlopen", fake_urlopen)

        p = OpenAICompatibleProvider(
            base_url="https://example.test/compatible-mode/v1/",
            text_model="qwen3.7-plus",
            timeout_seconds=12,
            temperature=0.1,
            max_tokens=512,
        )
        result = p.analyze(make_context(ReviewStrategy.RULE_EXPLAIN))

        assert "AI 复核结论" in result
        assert captured["url"] == "https://example.test/compatible-mode/v1/chat/completions"
        assert captured["timeout"] == 12
        assert captured["headers"]["Authorization"] == "Bearer test-key"

        body = captured["body"].decode("utf-8")
        assert '"model": "qwen3.7-plus"' in body
        assert '"temperature": 0.1' in body
        assert '"max_tokens": 512' in body
        assert "检测目标数" in body


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
