from __future__ import annotations

from pathlib import Path

import pytest

from ssv_agent.knowledge.catalog import RuleCatalogError, discover_rule_documents


def _write_rule(root: Path, relative: str, *, rule_id: str = "rule-1", version: str = "v1") -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"---\nkind: rule\nrule_id: {rule_id}\nversion: {version}\nevent_type: event-1\naliases:\n  - alias\n---\n\n1.1 规则正文。\n",
        encoding="utf-8",
    )
    return path


def test_discover_rule_documents_recurses_and_keeps_versioned_identity(tmp_path: Path) -> None:
    _write_rule(tmp_path, "standard/v1/rule.md")
    _write_rule(tmp_path, "standard/v2/rule.md", version="v2")
    _write_rule(tmp_path, "_templates/example.md", rule_id="template", version="v1")
    _write_rule(tmp_path, ".hidden/ignored.md", rule_id="hidden", version="v1")
    (tmp_path / "README.md").write_text("目录说明，不是规则。", encoding="utf-8")

    documents = discover_rule_documents(tmp_path)

    assert [(item.rule_id, item.version) for item in documents] == [
        ("rule-1", "v1"),
        ("rule-1", "v2"),
    ]
    assert documents[0].source_path == "standard/v1/rule.md"
    assert documents[0].event_types == ("event-1",)
    assert documents[0].content == "1.1 规则正文。"
    assert documents[0].content_hash.startswith("sha256:")


def test_discover_rule_documents_rejects_duplicate_identity(tmp_path: Path) -> None:
    _write_rule(tmp_path, "one.md")
    _write_rule(tmp_path, "two.md")

    with pytest.raises(RuleCatalogError, match="duplicate rule identity"):
        discover_rule_documents(tmp_path)


def test_discover_rule_documents_rejects_incomplete_rule_metadata(tmp_path: Path) -> None:
    path = tmp_path / "broken.md"
    path.write_text("---\nkind: rule\nrule_id: only-id\n---\n正文。", encoding="utf-8")

    with pytest.raises(RuleCatalogError, match="version is required"):
        discover_rule_documents(tmp_path)


def test_discover_rule_documents_returns_empty_for_missing_directory(tmp_path: Path) -> None:
    assert discover_rule_documents(tmp_path / "missing") == ()
