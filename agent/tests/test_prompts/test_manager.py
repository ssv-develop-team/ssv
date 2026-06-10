"""Tests for prompt manager (validate + get_versions)."""

from __future__ import annotations

from ssv_agent.prompts.manager import get_versions, validate


class TestValidate:
    def test_validate_returns_dict(self) -> None:
        result = validate()
        assert isinstance(result, dict)
        assert "valid" in result
        assert "issues" in result
        assert isinstance(result["valid"], bool)
        assert isinstance(result["issues"], list)

    def test_validate_all_strategies_present(self) -> None:
        """当前默认配置应该验证通过。"""
        result = validate()
        assert result["valid"] is True, f"Unexpected issues: {result['issues']}"
        assert result["issues"] == []


class TestGetVersions:
    def test_get_versions_returns_dict(self) -> None:
        versions = get_versions()
        assert isinstance(versions, dict)

    def test_get_versions_has_system(self) -> None:
        versions = get_versions()
        assert "system" in versions
        assert versions["system"] == "1.0.0"

    def test_get_versions_has_all_strategies(self) -> None:
        versions = get_versions()
        for strategy in ("direct_confirm", "visual_review", "rule_explain", "notify_report"):
            assert strategy in versions, f"Missing version for {strategy}"
            assert versions[strategy] == "1.0.0"

    def test_get_versions_all_non_empty(self) -> None:
        versions = get_versions()
        for key, val in versions.items():
            assert val, f"Version for '{key}' is empty"
            assert val != "missing", f"Version for '{key}' is 'missing'"
