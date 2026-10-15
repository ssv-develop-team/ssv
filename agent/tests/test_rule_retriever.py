from __future__ import annotations

import json
from pathlib import Path

from ssv_agent.tools.rule_retriever import rule_retriever_tool


def test_rule_retriever_tool_accepts_version_filters(tmp_path: Path, monkeypatch) -> None:
    rules = tmp_path / "rules"
    for version, text in (("v1", "旧版条款"), ("v2", "新版条款")):
        path = rules / "standard" / version / "rule.md"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            f"---\nkind: rule\nrule_id: standard\nversion: {version}\n"
            "event_type: event-a\n---\n\n1.1 " + text + "。\n",
            encoding="utf-8",
        )
    monkeypatch.setenv("SSV_KNOWLEDGE_BACKEND", "local_markdown")
    monkeypatch.setenv("SSV_KNOWLEDGE_RULES_DIR", str(rules))

    result = json.loads(
        rule_retriever_tool.invoke(
            {
                "query": "条款",
                "top_k": 5,
                "rule_id": "standard",
                "rule_version": "v2",
                "event_type": "event-a",
            }
        )
    )

    assert result["success"] is True
    assert [chunk["metadata"]["rule_version"] for chunk in result["chunks"]] == ["v2"]
