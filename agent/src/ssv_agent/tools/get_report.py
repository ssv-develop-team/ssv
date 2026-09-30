"""get_report 工具：读取已生成的分析报告并提供受控展示资源。"""

from __future__ import annotations

import json
import os
import stat
import tempfile
from hashlib import sha256
from pathlib import Path
from typing import Any

from deerflow.config.paths import VIRTUAL_PATH_PREFIX
from deerflow.sandbox.tools import get_thread_data
from deerflow.tools.types import Runtime
from langchain.tools import tool

from ssv_agent.event_store import EventLedger, ReportRecord
from ssv_agent.report import read_analysis_report


def _token(value: str) -> str:
    return sha256(value.encode("utf-8")).hexdigest()[:24]


def _response(
    event_id: str,
    *,
    found: bool,
    reason: str,
    report: ReportRecord | None = None,
    content: str | None = None,
    virtual_path: str | None = None,
    evidence_refs: list[dict[str, str]] | None = None,
) -> str:
    payload: dict[str, Any] = {
        "found": found,
        "event_id": event_id,
        "reason": reason,
        "virtual_path": virtual_path,
        "evidence_refs": evidence_refs or [],
    }
    if report is not None:
        payload.update(
            {
                "report_id": report.report_id,
                "review_id": report.review_id,
                "review_revision": report.review_revision,
                "template_version": report.template_version,
                "sha256": report.sha256,
                "size_bytes": report.size_bytes,
                "created_ms": report.created_ms,
            }
        )
    if content is not None:
        payload["content"] = content
    return json.dumps(payload, ensure_ascii=False)


def _select_report(
    reports: tuple[ReportRecord, ...],
    review_id: str | None,
) -> ReportRecord | None:
    if review_id is not None:
        return next((item for item in reports if item.review_id == review_id), None)
    return reports[-1] if reports else None


def _copy_report_to_thread_outputs(
    runtime: Runtime,
    report: ReportRecord,
    content: str,
) -> str | None:
    thread_data = get_thread_data(runtime)
    outputs_path = (thread_data or {}).get("outputs_path")
    if not isinstance(outputs_path, (str, os.PathLike)) or not outputs_path:
        return None

    output_root = Path(outputs_path).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    report_dir = output_root / "reports" / _token(report.event_id)
    report_dir.mkdir(parents=True, exist_ok=True)
    if report_dir.is_symlink() or not report_dir.is_dir():
        raise ValueError("thread report directory is unsafe")
    if not report_dir.resolve().is_relative_to(output_root):
        raise ValueError("thread report directory escapes outputs root")
    filename = f"{_token(report.review_id)}-{report.sha256[:16]}.md"
    destination = report_dir / filename
    if not destination.resolve().is_relative_to(output_root):
        raise ValueError("thread report output escapes outputs root")
    try:
        details = destination.lstat()
    except FileNotFoundError:
        details = None
    if details is not None and (
        stat.S_ISLNK(details.st_mode) or not stat.S_ISREG(details.st_mode)
    ):
        raise ValueError("thread report output is not a regular file")
    if destination.exists():
        if destination.read_text(encoding="utf-8") != content:
            raise ValueError("thread report output has conflicting contents")
    else:
        descriptor, temporary_name = tempfile.mkstemp(
            dir=report_dir,
            prefix=f".{destination.name}.",
            suffix=".tmp",
        )
        temporary = Path(temporary_name)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
                handle.write(content)
            os.replace(temporary, destination)
        finally:
            temporary.unlink(missing_ok=True)
    return (
        f"{VIRTUAL_PATH_PREFIX}/outputs/reports/"
        f"{_token(report.event_id)}/{filename}"
    )


@tool("get_report", parse_docstring=True)
def get_report_tool(
    runtime: Runtime,
    event_id: str,
    review_id: str | None = None,
    include_content: bool = True,
) -> str:
    """读取已登记的确定性分析报告并返回受控展示资源。

    Args:
        runtime: DeerFlow 运行时（自动注入）。
        event_id: SQLite 账本中的事件 ID。
        review_id: 可选的历史复核 ID；省略时选择最新报告。
        include_content: 是否在响应中包含 Markdown 正文。

    Returns:
        JSON 字符串，包含报告元数据、正文、线程虚拟路径和证据资源引用。
    """
    with EventLedger() as ledger:
        case = ledger.get_case(event_id)
        if case is None:
            return _response(event_id, found=False, reason="event not found")
        reports = ledger.report_records_for_event(event_id)
        report = _select_report(reports, review_id)
        if report is None:
            reason = "report not found" if review_id else "report not ready"
            return _response(event_id, found=False, reason=reason)
        review = ledger.get_review_record(event_id, report.review_revision)

    if review is None:
        return _response(event_id, found=False, reason="review history not found")
    try:
        content = read_analysis_report(
            report.artifact_path,
            sha256=report.sha256,
            size_bytes=report.size_bytes,
        )
        virtual_path = _copy_report_to_thread_outputs(runtime, report, content)
    except (OSError, ValueError, UnicodeError):
        return _response(event_id, found=False, reason="report artifact unavailable")

    evidence_refs = [
        {
            "event_id": event_id,
            "evidence_id": evidence_id,
            "reader": "evidence_reader",
        }
        for evidence_id in review.evidence_ids
    ]
    return _response(
        event_id,
        found=True,
        reason="ok",
        report=report,
        content=content if include_content else None,
        virtual_path=virtual_path,
        evidence_refs=evidence_refs,
    )
