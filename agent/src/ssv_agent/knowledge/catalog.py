"""Discover versioned rule documents from the Agent-owned rule directory."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
from typing import Any

import yaml

SUPPORTED_RULE_EXTENSIONS = frozenset({".md", ".txt"})
DEFAULT_RULES_DIR = Path(__file__).resolve().parents[3] / "knowledge" / "rules"


class RuleCatalogError(ValueError):
    """A rule file cannot be represented as a stable versioned document."""


@dataclass(frozen=True, kw_only=True)
class RuleReference:
    """Stable identity and matching metadata for one rule version."""

    rule_id: str
    version: str
    source_path: str
    content_hash: str
    event_type: str | None = None
    event_types: tuple[str, ...] = ()
    severity: str | None = None
    aliases: tuple[str, ...] = ()

    def matches_event_type(self, event_type: str | None) -> bool:
        """Return whether this rule explicitly declares the business event type."""
        return bool(event_type) and event_type in self.event_types


@dataclass(frozen=True, kw_only=True)
class RuleDocument(RuleReference):
    """A read-only rule reference together with its normalized body."""

    content: str


def discover_rule_documents(rules_dir: str | Path = DEFAULT_RULES_DIR) -> tuple[RuleDocument, ...]:
    """Recursively load unique versioned rule documents in deterministic order.

    Files without a ``kind: rule`` front matter block are treated as directory
    documentation and skipped. Once a file identifies itself as a rule, its
    identity and body must be complete; duplicate ``(rule_id, version)`` pairs
    fail instead of silently choosing one file.
    """
    root = Path(rules_dir).expanduser()
    if not root.is_dir():
        return ()

    documents: list[RuleDocument] = []
    identities: dict[tuple[str, str], str] = {}
    for path in _candidate_paths(root):
        document = _parse_rule_document(path, root)
        if document is None:
            continue
        identity = (document.rule_id, document.version)
        previous = identities.get(identity)
        if previous is not None:
            raise RuleCatalogError(
                "duplicate rule identity "
                f"({document.rule_id!r}, {document.version!r}) in "
                f"{previous!r} and {document.source_path!r}"
            )
        identities[identity] = document.source_path
        documents.append(document)
    return tuple(documents)


def _candidate_paths(root: Path) -> list[Path]:
    return sorted(
        path
        for path in root.rglob("*")
        if path.is_file()
        and path.suffix.lower() in SUPPORTED_RULE_EXTENSIONS
        and _is_visible_rule_path(path.relative_to(root))
    )


def _is_visible_rule_path(relative_path: Path) -> bool:
    return not any(part == "_templates" or part.startswith(".") for part in relative_path.parts)


def _parse_rule_document(path: Path, root: Path) -> RuleDocument | None:
    source_path = path.relative_to(root).as_posix()
    text = _read_text(path)
    try:
        metadata, body = _split_front_matter(text)
    except yaml.YAMLError as exc:
        raise RuleCatalogError(f"{source_path}: invalid rule front matter") from exc
    if metadata is None:
        return None
    if metadata.get("kind") != "rule":
        return None

    rule_id = _required_string(metadata, "rule_id", source_path)
    version = _required_string(metadata, "version", source_path)
    content = body.strip()
    if not content:
        raise RuleCatalogError(f"{source_path}: rule body must not be empty")

    event_type = _optional_string(metadata, "event_type", source_path)
    event_types = _string_tuple(metadata, "event_types", source_path)
    if event_type and event_type not in event_types:
        event_types = (event_type, *event_types)
    aliases = _string_tuple(metadata, "aliases", source_path)
    severity = _optional_string(metadata, "severity", source_path)
    content_hash = "sha256:" + hashlib.sha256(content.encode("utf-8")).hexdigest()
    return RuleDocument(
        rule_id=rule_id,
        version=version,
        source_path=source_path,
        content_hash=content_hash,
        event_type=event_type,
        event_types=event_types,
        severity=severity,
        aliases=aliases,
        content=content,
    )


def _split_front_matter(text: str) -> tuple[dict[str, Any] | None, str]:
    lines = text.removeprefix("\ufeff").replace("\r\n", "\n").split("\n")
    if not lines or lines[0].strip() != "---":
        return None, text
    try:
        end = next(index for index, line in enumerate(lines[1:], start=1) if line.strip() == "---")
    except StopIteration as exc:
        raise RuleCatalogError("rule front matter is missing the closing '---'") from exc
    raw_metadata = yaml.safe_load("\n".join(lines[1:end])) or {}
    if not isinstance(raw_metadata, dict):
        raise RuleCatalogError("rule front matter must be a mapping")
    return raw_metadata, "\n".join(lines[end + 1 :])


def _required_string(metadata: dict[str, Any], key: str, source_path: str) -> str:
    value = _optional_string(metadata, key, source_path)
    if value is None:
        raise RuleCatalogError(f"{source_path}: {key} is required for a rule")
    return value


def _optional_string(metadata: dict[str, Any], key: str, source_path: str) -> str | None:
    value = metadata.get(key)
    if value is None:
        return None
    if not isinstance(value, str) or not value.strip():
        raise RuleCatalogError(f"{source_path}: {key} must be a non-empty string")
    return value.strip()


def _string_tuple(metadata: dict[str, Any], key: str, source_path: str) -> tuple[str, ...]:
    value = metadata.get(key, ())
    if value is None:
        return ()
    if isinstance(value, str):
        value = (value,)
    if not isinstance(value, (list, tuple)):
        raise RuleCatalogError(f"{source_path}: {key} must be a string list")
    values: list[str] = []
    for item in value:
        if not isinstance(item, str) or not item.strip():
            raise RuleCatalogError(f"{source_path}: {key} must contain non-empty strings")
        normalized = item.strip()
        if normalized not in values:
            values.append(normalized)
    return tuple(values)


def _read_text(path: Path) -> str:
    data = path.read_bytes()
    for encoding in ("utf-8-sig", "utf-8", "gb18030"):
        try:
            return data.decode(encoding)
        except UnicodeDecodeError:
            continue
    return data.decode("utf-8", errors="replace")
