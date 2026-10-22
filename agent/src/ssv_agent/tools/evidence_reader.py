"""evidence_reader 工具：校验事件证据路径。"""

from __future__ import annotations

import json
import os
import shutil
import tempfile
from hashlib import sha256
from pathlib import Path
from typing import IO

from langchain.tools import tool
from deerflow.config.paths import VIRTUAL_PATH_PREFIX
from deerflow.sandbox.tools import get_thread_data
from deerflow.tools.types import Runtime

from ssv_agent.artifact_storage import (
    ArtifactStorageError,
    create_artifact_reader,
)
from ssv_agent.config import load_config
from ssv_agent.event_store import EventLedger


def _token(value: str) -> str:
    return sha256(value.encode("utf-8")).hexdigest()[:24]


def _response(event_id: str, *, reason: str, found: list[dict[str, object]]) -> str:
    return json.dumps(
        {
            "event_id": event_id,
            "available": bool(found),
            "reason": reason,
            "found": found,
            "view_image_path": found[0]["virtual_path"] if found else None,
        },
        ensure_ascii=False,
    )


@tool("evidence_reader", parse_docstring=True)
def evidence_reader_tool(
    runtime: Runtime,
    event_id: str,
    evidence_id: str | None = None,
) -> str:
    """复制账本登记的证据到 DeerFlow 输出目录，返回虚拟路径。

    Args:
        runtime: DeerFlow 运行时（自动注入）。
        event_id: 事件 ID。
        evidence_id: 账本中的证据 ID；省略时读取该事件全部已登记证据。

    Returns:
        JSON 字符串，包含 available、证据元数据与可用于视觉工具的虚拟路径。
    """
    thread_data = get_thread_data(runtime)
    outputs_path = (thread_data or {}).get("outputs_path")
    if not outputs_path:
        return _response(event_id, reason="thread outputs unavailable", found=[])

    with EventLedger() as ledger:
        ledger.refresh_evidence(event_id, evidence_id)
        case = ledger.get_case(event_id)
        if case is None:
            return _response(event_id, reason="event not found", found=[])

        evidence = [
            item
            for item in case.evidence
            if evidence_id is None or item.evidence_id == evidence_id
        ]
        if not evidence:
            return _response(event_id, reason="registered evidence not found", found=[])

        evidence_dir = Path(outputs_path) / "evidence" / _token(event_id)
        evidence_dir.mkdir(parents=True, exist_ok=True)
        needs_reader = any(item.artifact_id is not None for item in evidence)
        reader = (
            create_artifact_reader(ledger, load_config()) if needs_reader else None
        )
        issues: list[str] = []
        found: list[dict[str, object]] = []
        for reference in evidence:
            source = ledger.resolve_evidence_path(reference.path)
            if reference.artifact_id is not None:
                destination = evidence_dir / (
                    f"{_token(reference.evidence_id)}{source.suffix if source else '.jpg'}"
                )
                try:
                    assert reader is not None
                    with reader.open(reference.artifact_id) as stream:
                        _write_copy(destination, stream)
                except (ArtifactStorageError, OSError):
                    issues.append(f"{reference.evidence_id}: artifact unavailable")
                    continue
            else:
                if source is None:
                    issues.append(f"{reference.evidence_id}: path outside configured roots")
                    continue
                if not source.is_file():
                    issues.append(f"{reference.evidence_id}: file missing")
                    continue
                destination = evidence_dir / f"{_token(reference.evidence_id)}{source.suffix or ''}"
                try:
                    shutil.copy2(source, destination)
                except OSError:
                    issues.append(f"{reference.evidence_id}: copy failed")
                    continue
            if destination.is_file():
                found.append(
                    {
                        "evidence_id": reference.evidence_id,
                        "kind": reference.kind,
                        "mime_type": reference.mime_type,
                        "virtual_path": (
                            f"{VIRTUAL_PATH_PREFIX}/outputs/evidence/"
                            f"{_token(event_id)}/{destination.name}"
                        ),
                        "size_bytes": destination.stat().st_size,
                    }
                )

        return _response(event_id, reason="; ".join(issues) if issues else "ok", found=found)


def _write_copy(destination: Path, source: IO[bytes]) -> None:
    """原子写入已校验的图片副本。"""
    descriptor, temporary_name = tempfile.mkstemp(
        dir=destination.parent, prefix=f".{destination.name}.", suffix=".tmp"
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            shutil.copyfileobj(source, handle)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
