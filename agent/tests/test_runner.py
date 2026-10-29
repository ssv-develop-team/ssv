from types import SimpleNamespace

import pytest

from ssv_agent.review_context import ReviewContext, RuleRetrievalContext
from ssv_agent.runner import ModelCallError, classify_model_error, run_review
from ssv_agent.knowledge.schema import RetrievalResult


def test_run_review_passes_rule_context_to_prompt(monkeypatch) -> None:
    seen: dict[str, str] = {}

    def prompt(context, rule_context=None):
        seen["message"] = f"{context.event_id}:{rule_context.query}"
        return "prompt"

    monkeypatch.setattr("ssv_agent.runner.build_review_prompt", prompt)

    class Client:
        def stream(self, **kwargs):
            assert kwargs["message"] == "prompt"
            return [SimpleNamespace(type="values", data={"messages": [
                {"type": "ai", "content": "answer"}
            ]})]

    rule_context = RuleRetrievalContext.from_result("helmet", RetrievalResult(query="helmet"))
    result = run_review(Client(), ReviewContext(
        event_id="evt-1", source="camera", timestamp_ms=1, frame_id=1
    ), rule_context)

    assert result == "answer"
    assert seen["message"] == "evt-1:helmet"


@pytest.mark.parametrize(
    ("message", "code"),
    [
        ("403 INSUFFICIENT_BALANCE", "model_insufficient_balance"),
        ("401 invalid api key", "model_authentication_failed"),
        ("403 forbidden", "model_access_denied"),
        ("429 rate limit exceeded", "model_rate_limited"),
        ("request timed out", "model_timeout"),
    ],
)
def test_classify_model_error_returns_stable_code_without_raw_message(
    message: str, code: str
) -> None:
    error = classify_model_error(RuntimeError(message))

    assert error.code == code
    assert message not in error.user_message
    assert error.diagnostic == message


def test_run_review_marks_empty_stream_as_no_response() -> None:
    class Client:
        def stream(self, **kwargs):
            return []

    with pytest.raises(ModelCallError) as raised:
        run_review(
            Client(),
            ReviewContext(event_id="evt-1", source="camera", timestamp_ms=1, frame_id=1),
        )

    assert raised.value.code == "model_no_response"
    assert str(raised.value) == "模型服务未返回可解析结果。"
