"""Tests for prompt debug logger."""

from __future__ import annotations

import pytest

from ssv_agent.prompts.assembler import PromptAssembly, PromptMessage
from ssv_agent.prompts.logger import CallRecord, PromptLogger


def make_assembly(
    strategy: str = "direct_confirm",
    messages: list[PromptMessage] | None = None,
    truncated: bool = False,
) -> PromptAssembly:
    """Helper: create a minimal PromptAssembly for testing."""
    if messages is None:
        messages = [
            PromptMessage(role="system", content="test system prompt"),
            PromptMessage(role="user", content="test user message"),
        ]
    return PromptAssembly(
        messages=messages,
        strategy=strategy,
        template_version="1.0.0",
        example_count=0,
        estimated_tokens=10,
        truncated=truncated,
    )


class TestCallRecord:
    def test_create_minimal(self) -> None:
        record = CallRecord(call_id="call_001")
        assert record.call_id == "call_001"
        assert record.strategy == ""
        assert record.response == ""
        assert record.error == ""
        assert record.truncated is False

    def test_create_full(self) -> None:
        msgs = [PromptMessage(role="user", content="hello")]
        record = CallRecord(
            call_id="call_002",
            strategy="visual_review",
            template_version="1.0.0",
            messages=msgs,
            response='{"conclusion": "ok"}',
            latency_ms=1500.0,
            model_name="gpt-4",
            truncated=True,
        )
        assert record.strategy == "visual_review"
        assert record.template_version == "1.0.0"
        assert len(record.messages) == 1
        assert record.response == '{"conclusion": "ok"}'
        assert record.latency_ms == 1500.0
        assert record.model_name == "gpt-4"
        assert record.truncated is True

    def test_with_error(self) -> None:
        record = CallRecord(call_id="call_003", error="API timeout")
        assert record.error == "API timeout"
        assert record.response == ""


class TestPromptLoggerEnabled:
    def test_log_call_returns_call_id(self) -> None:
        plog = PromptLogger(enabled=True)
        assembly = make_assembly()
        call_id = plog.log_call(assembly, response="ok", latency_ms=100, model_name="test")
        assert call_id.startswith("call_")
        assert len(call_id) > 5

    def test_log_call_stores_record(self) -> None:
        plog = PromptLogger(enabled=True)
        assembly = make_assembly(strategy="rule_explain")
        call_id = plog.log_call(
            assembly,
            response='{"conclusion": "conflict resolved"}',
            latency_ms=500,
            model_name="claude-3",
            parsed_result={"is_violation": False},
        )

        record = plog.get_call(call_id)
        assert record is not None
        assert record.strategy == "rule_explain"
        assert record.template_version == "1.0.0"
        assert record.response == '{"conclusion": "conflict resolved"}'
        assert record.latency_ms == 500
        assert record.model_name == "claude-3"
        assert record.parsed_result == {"is_violation": False}
        assert record.error == ""
        assert record.truncated is False

    def test_log_call_stores_truncated_flag(self) -> None:
        plog = PromptLogger(enabled=True)
        assembly = make_assembly(truncated=True)
        call_id = plog.log_call(assembly, response="ok")
        record = plog.get_call(call_id)
        assert record is not None
        assert record.truncated is True

    def test_log_error_returns_call_id(self) -> None:
        plog = PromptLogger(enabled=True)
        assembly = make_assembly()
        call_id = plog.log_error(assembly, error="Connection refused", latency_ms=2000)
        assert call_id.startswith("call_")

    def test_log_error_stores_error(self) -> None:
        plog = PromptLogger(enabled=True)
        assembly = make_assembly(strategy="notify_report")
        call_id = plog.log_error(
            assembly,
            error="Rate limit exceeded",
            latency_ms=100,
            model_name="gpt-4",
        )

        record = plog.get_call(call_id)
        assert record is not None
        assert record.strategy == "notify_report"
        assert record.error == "Rate limit exceeded"
        assert record.response == ""
        assert record.model_name == "gpt-4"

    def test_get_call_missing(self) -> None:
        plog = PromptLogger(enabled=True)
        assert plog.get_call("nonexistent") is None

    def test_record_count(self) -> None:
        plog = PromptLogger(enabled=True)
        assert plog.record_count == 0
        assembly = make_assembly()
        plog.log_call(assembly, response="ok")
        assert plog.record_count == 1
        plog.log_call(assembly, response="ok2")
        assert plog.record_count == 2

    def test_max_records_eviction(self) -> None:
        plog = PromptLogger(enabled=True, max_records=3)
        assembly = make_assembly()

        ids = []
        for i in range(5):
            cid = plog.log_call(assembly, response=f"resp_{i}")
            ids.append(cid)

        assert plog.record_count == 3
        # oldest 2 should be evicted
        assert plog.get_call(ids[0]) is None
        assert plog.get_call(ids[1]) is None
        # newest 3 should remain
        assert plog.get_call(ids[2]) is not None
        assert plog.get_call(ids[3]) is not None
        assert plog.get_call(ids[4]) is not None


class TestPromptLoggerDisabled:
    def test_log_call_returns_empty_string(self) -> None:
        plog = PromptLogger(enabled=False)
        assembly = make_assembly()
        call_id = plog.log_call(assembly, response="ok")
        assert call_id == ""

    def test_log_error_returns_empty_string(self) -> None:
        plog = PromptLogger(enabled=False)
        assembly = make_assembly()
        call_id = plog.log_error(assembly, error="fail")
        assert call_id == ""

    def test_get_call_returns_none(self) -> None:
        plog = PromptLogger(enabled=False)
        assert plog.get_call("anything") is None

    def test_record_count_stays_zero(self) -> None:
        plog = PromptLogger(enabled=False)
        assembly = make_assembly()
        plog.log_call(assembly, response="ok")
        plog.log_call(assembly, response="ok2")
        assert plog.record_count == 0

    def test_enabled_property(self) -> None:
        plog = PromptLogger(enabled=False)
        assert plog.enabled is False

    def test_default_enabled(self) -> None:
        plog = PromptLogger()
        assert plog.enabled is True
