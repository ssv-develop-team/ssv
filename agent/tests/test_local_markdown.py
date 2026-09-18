import asyncio

from ssv_agent.knowledge.backends.local_markdown import LocalMarkdownRetriever, _split_clauses


def test_local_markdown_returns_helmet_clause_first() -> None:
    result = asyncio.run(
        LocalMarkdownRetriever().retrieve(
            "作业现场监控未佩戴安全帽",
            top_k=2,
        )
    )

    assert result.success is True
    assert len(result.chunks) == 2
    assert result.chunks[0].metadata["source"] == "gb26860-helmet/v1/rule.md"
    assert result.chunks[0].metadata["rule_id"] == "gb26860-helmet"
    assert result.chunks[0].metadata["rule_version"] == "v1"
    assert result.chunks[0].metadata["section"] == "1.1"
    assert "未佩戴安全帽" in result.chunks[0].content
    assert result.chunks[1].metadata["source"] == "gb26860-helmet/v2/rule.md"
    assert result.chunks[1].metadata["rule_version"] == "v2"
    assert result.chunks[1].metadata["section"] == "9.2.11"
    assert "绝缘安全帽" in result.chunks[1].content


def test_local_markdown_filters_rule_identity_and_event_type(tmp_path) -> None:
    rules = tmp_path / "rules"
    (rules / "standard" / "v1").mkdir(parents=True)
    (rules / "standard" / "v1" / "rule.md").write_text(
        "---\nkind: rule\nrule_id: standard\nversion: v1\n"
        "event_type: event-a\n---\n\n1.1 目标条款。\n",
        encoding="utf-8",
    )
    (rules / "standard" / "v2").mkdir(parents=True)
    (rules / "standard" / "v2" / "rule.md").write_text(
        "---\nkind: rule\nrule_id: standard\nversion: v2\n"
        "event_type: event-b\n---\n\n1.1 目标条款。\n",
        encoding="utf-8",
    )

    result = asyncio.run(
        LocalMarkdownRetriever(rules).retrieve(
            "目标条款",
            top_k=5,
            filters={"rule_id": "standard", "rule_version": "v2", "event_type": "event-b"},
        )
    )

    assert len(result.chunks) == 1
    assert result.chunks[0].metadata["rule_version"] == "v2"


def test_clause_splitter_ignores_pdf_page_noise_and_parses_markdown_numbers() -> None:
    passages = _split_clauses(
        "rules.md",
        "# 规则标题\n## ? 1 ?\n1 第一条。\n1\nGB26860—2011\n"
        "## ? 2 ?\n# 2 第二条。\n",
    )

    numbered = [passage for passage in passages if passage.section in {"1", "2"}]
    assert [passage.section for passage in numbered] == ["1", "2"]
    assert any(passage.section == "规则标题" for passage in passages)
    assert all("GB26860" not in passage.content for passage in numbered)
