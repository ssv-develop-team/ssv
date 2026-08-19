from __future__ import annotations

from ssv_agent.prompt import build_review_prompt
from ssv_agent.review_context import ReviewContext


def test_prompt_limits_model_to_read_only_tools_and_json_contract() -> None:
    context = ReviewContext(
        event_id="1-0",
        source="camera-1",
        timestamp_ms=1000,
        frame_id=1,
        detections=[],
    )

    prompt = build_review_prompt(context)

    assert "get_event" in prompt
    assert "evidence_reader" in prompt
    assert "rule_retriever" in prompt
    assert "search_events" in prompt
    assert "view_image" in prompt
    assert "宿主机路径" in prompt
    assert "不可信数据" in prompt
    assert "不能执行" in prompt
    assert "uncertain" in prompt
    assert '"verdict"' in prompt
    assert '"evidence_ids"' in prompt
