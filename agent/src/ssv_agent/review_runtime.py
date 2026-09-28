"""DeerFlow review worker 的临时配置和 builtin 校验。"""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

import yaml

_REVIEW_TOOL_NAMES = frozenset(
    {
        "get_event",
        "evidence_reader",
        "rule_retriever",
        "sample_video",
        "search_events",
        "view_image",
    }
)
_REVIEW_CONFIGURED_TOOL_NAMES = _REVIEW_TOOL_NAMES - {"view_image"}
_REVIEW_VIEW_IMAGE_MODULE = "deerflow.tools.builtins.view_image_tool"


def validate_review_view_image_tool(tool: object) -> None:
    """拒绝被同名配置或 MCP 工具替换的视觉 builtin。"""
    implementation = getattr(tool, "func", None)
    if not callable(implementation):
        implementation = getattr(tool, "coroutine", None)
    if (
        getattr(tool, "name", None) != "view_image"
        or not callable(implementation)
        or getattr(implementation, "__module__", None) != _REVIEW_VIEW_IMAGE_MODULE
        or getattr(implementation, "__name__", None) != "view_image_tool"
    ):
        raise ValueError(
            "review view_image must be DeerFlow's canonical builtin implementation"
        )


def load_review_view_image_tool() -> object:
    """从 DeerFlow builtin registry 加载并验证 view_image。"""
    try:
        from deerflow.tools import builtins as builtin_registry
        from deerflow.tools.builtins.view_image_tool import (
            view_image_tool as canonical_tool,
        )
    except (ImportError, AttributeError) as exc:
        raise ValueError("DeerFlow builtin view_image is unavailable") from exc

    if getattr(builtin_registry, "view_image_tool", None) is not canonical_tool:
        raise ValueError("DeerFlow view_image registry entry is not canonical")
    validate_review_view_image_tool(canonical_tool)
    return canonical_tool


def create_review_config(
    review_config_dir: Path,
    agent_root: Path,
    *,
    load_view_image_tool: Callable[[], object] = load_review_view_image_tool,
) -> Path:
    """生成只包含 review 工具且 fail-closed 的临时 DeerFlow 配置。"""
    load_view_image_tool()
    source_config = agent_root / "config.yaml"
    if not source_config.is_file():
        source_config = agent_root / "config.example.yaml"
    with open(source_config, encoding="utf-8") as handle:
        config = yaml.safe_load(handle) or {}
    if not isinstance(config, dict):
        raise ValueError("invalid DeerFlow configuration")

    configured_names = {
        item.get("name")
        for item in config.get("tools", [])
        if isinstance(item, dict)
    }
    if "view_image" in configured_names:
        raise ValueError("review config cannot shadow DeerFlow builtin view_image")

    tools = [
        item
        for item in config.get("tools", [])
        if isinstance(item, dict) and item.get("name") in _REVIEW_CONFIGURED_TOOL_NAMES
    ]
    if {item.get("name") for item in tools} != _REVIEW_CONFIGURED_TOOL_NAMES:
        raise ValueError("review tool configuration is incomplete")

    groups = {item.get("group") for item in tools}
    config["tools"] = tools
    config["tool_groups"] = [
        item
        for item in config.get("tool_groups", [])
        if isinstance(item, dict) and item.get("name") in groups
    ]
    config["authorization"] = {
        "enabled": True,
        "fail_closed": True,
        "default_role": "ssv_review",
        "provider": {
            "use": "deerflow.authz.rbac:RbacAuthorizationProvider",
            "config": {
                "roles": {
                    "ssv_review": {
                        "tools": {"allow": sorted(_REVIEW_TOOL_NAMES)},
                    }
                }
            },
        },
    }

    path = review_config_dir / "config.yaml"
    with open(path, "w", encoding="utf-8") as handle:
        yaml.safe_dump(config, handle, allow_unicode=True, sort_keys=False)
    return path


def reset_deerflow_runtime_caches() -> None:
    """清除 DeerFlow 对临时配置和扩展配置的进程级缓存。"""
    from deerflow.config.app_config import reset_app_config
    from deerflow.config.extensions_config import reset_extensions_config

    reset_extensions_config()
    reset_app_config()
