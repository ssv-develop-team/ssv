"""Tests for T4 context builder, tool router, and result writer."""

from __future__ import annotations

from ssv_agent.context.builder import ContextBuilder
from ssv_agent.models.event import (
    Detection,
    DetectionEvent,
    EventState,
    ReviewResult,
    ReviewStrategy,
    ToolResult,
    TriggerReason,
)
from ssv_agent.tools.base import BaseTool
from ssv_agent.tools.router import ToolRouter
from ssv_agent.writer.result_writer import ResultWriter


# ── ContextBuilder ─────────────────────────────────────────────────────────


class TestContextBuilder:
    def test_build_basic_context(self) -> None:
        builder = ContextBuilder()
        event = DetectionEvent(
            source="cam-1", frame_id=42,
            detections=[Detection(class_name="head", confidence=0.9)],
        )
        ctx = builder.build(event, ReviewStrategy.DIRECT_CONFIRM)
        assert ctx.strategy == ReviewStrategy.DIRECT_CONFIRM
        assert ctx.event.frame_id == 42

    def test_build_with_evidence_paths(self) -> None:
        builder = ContextBuilder()
        event = DetectionEvent(
            source="cam-1", frame_id=1,
            evidence_paths=["/tmp/frames/frame_0001.jpg"],
        )
        ctx = builder.build(event, ReviewStrategy.VISUAL_REVIEW)
        assert "frame_0001.jpg" in ctx.evidence_summary

    def test_rules_for_no_helmet(self) -> None:
        builder = ContextBuilder()
        event = DetectionEvent(
            source="cam-1", frame_id=1,
            trigger_reason=TriggerReason.NO_HELMET,
        )
        ctx = builder.build(event, ReviewStrategy.RULE_EXPLAIN)
        assert len(ctx.rule_snippets) > 0
        assert any("安全帽佩戴规范" in r for r in ctx.rule_snippets)

    def test_rules_for_rule_conflict(self) -> None:
        builder = ContextBuilder()
        event = DetectionEvent(
            source="cam-1", frame_id=1,
            trigger_reason=TriggerReason.RULE_CONFLICT,
        )
        ctx = builder.build(event, ReviewStrategy.RULE_EXPLAIN)
        assert any("冲突判定" in r for r in ctx.rule_snippets)

    def test_build_count(self) -> None:
        builder = ContextBuilder()
        assert builder.build_count == 0
        event = DetectionEvent(source="cam-1", frame_id=1)
        builder.build(event, ReviewStrategy.DIRECT_CONFIRM)
        assert builder.build_count == 1


# ── ToolRouter ──────────────────────────────────────────────────────────────


class MockEchoTool(BaseTool):
    @property
    def name(self) -> str:
        return "echo"

    @property
    def description(self) -> str:
        return "回显工具 —— 返回输入的参数。"

    @property
    def parameters_schema(self) -> dict:
        return {"type": "object", "properties": {"key": {"type": "string"}}}

    @property
    def returns_description(self) -> str:
        return "返回参数字典的字符串表示。"

    @property
    def side_effects(self) -> str:
        return ""

    def execute(self, params: dict) -> ToolResult:
        return ToolResult(tool_name="echo", success=True, output=str(params))


class MockFailingTool(BaseTool):
    @property
    def name(self) -> str:
        return "failing"

    @property
    def description(self) -> str:
        return "始终失败的工具。"

    @property
    def parameters_schema(self) -> dict:
        return {"type": "object"}

    @property
    def returns_description(self) -> str:
        return "总是抛出异常。"

    @property
    def side_effects(self) -> str:
        return ""

    def execute(self, params: dict) -> ToolResult:
        raise RuntimeError("intentional failure")


class TestToolRouter:
    def test_register_and_call(self) -> None:
        router = ToolRouter()
        router.register(MockEchoTool())
        assert "echo" in router.registered_tools
        assert router.tool_definitions[0].name == "echo"

        result = router.call_tool("echo", {"key": "value"})
        assert result.success is True
        assert "value" in result.output

    def test_call_unregistered_tool(self) -> None:
        router = ToolRouter()
        result = router.call_tool("nonexistent", {})
        assert result.success is False
        assert "未注册" in result.error

    def test_failing_tool(self) -> None:
        router = ToolRouter()
        router.register(MockFailingTool())
        result = router.call_tool("failing", {})
        assert result.success is False
        assert "intentional failure" in result.error

    def test_call_count(self) -> None:
        router = ToolRouter()
        router.register(MockEchoTool())
        assert router.call_count == 0
        router.call_tool("echo", {})
        router.call_tool("echo", {})
        assert router.call_count == 2

    def test_unregister(self) -> None:
        router = ToolRouter()
        router.register(MockEchoTool())
        assert "echo" in router.registered_tools
        router.unregister("echo")
        assert "echo" not in router.registered_tools

    def test_overwrite_registration(self) -> None:
        router = ToolRouter()
        router.register(MockEchoTool())
        router.register(MockEchoTool())  # 覆盖，不报错
        assert "echo" in router.registered_tools


# ── ResultWriter ────────────────────────────────────────────────────────────


class TestResultWriter:
    def test_write_to_log(self) -> None:
        writer = ResultWriter()  # 无 Redis
        result = ReviewResult(
            event_id="evt-1",
            source="cam-1",
            final_state=EventState.COMPLETED,
            conclusion="测试结论",
        )
        writer.write(result)
        assert writer.write_count == 1

    def test_write_count_increments(self) -> None:
        writer = ResultWriter()
        result = ReviewResult(final_state=EventState.COMPLETED)
        writer.write(result)
        writer.write(result)
        assert writer.write_count == 2
