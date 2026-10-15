"""确定性分析报告渲染与 Markdown artifact 写入。"""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path, PurePosixPath, PureWindowsPath
from urllib.parse import urlsplit
from typing import Any

from ssv_agent.event_store import EventCase, ReviewRecord
from ssv_agent.result import write_content_addressed_artifact


REPORT_TEMPLATE_VERSION = "report-v1"
_SAFE_REVIEW_ID = re.compile(r"[A-Za-z0-9._-]{1,128}\Z")
_POSIX_PATH_WITH_SPACES = re.compile(
    r"(?<![A-Za-z0-9:/])/(?:[^<>\"'`|;!?()\[\]{}]+?)"
    r"\.[A-Za-z0-9]{1,12}(?=$|[\s,;:!?)}\]])"
)
_POSIX_PATH = re.compile(r"(?<![A-Za-z0-9:/])/(?:[A-Za-z0-9._-]+/)*[A-Za-z0-9._-]+")
_WINDOWS_PATH_WITH_SPACES = re.compile(
    r"(?i)(?<![A-Za-z0-9])[A-Z]:\\(?:[^<>\"'`|;!?()\[\]{}]+?)"
    r"\.[A-Za-z0-9]{1,12}(?=$|[\s,;:!?)}\]])"
)
_WINDOWS_PATH = re.compile(r"(?i)(?<![A-Za-z0-9])[A-Z]:\\(?:[^\\\s]+\\)*[^\\\s]+")
_MARKDOWN_PUNCTUATION = frozenset(r"!\"#$%&'()*+,-./:;<=>?@[\]^_`{|}~\\")
_VERDICT_LABELS = {
    "compliant": "符合规则",
    "violation": "违规",
    "uncertain": "无法确定",
}


@dataclass(frozen=True)
class ReportArtifact:
    """已写入 outputs root 的不可变报告 artifact 摘要。"""

    path: Path
    sha256: str
    size_bytes: int


def render_analysis_report(case: EventCase, review: ReviewRecord) -> str:
    """将账本案件事实和指定历史复核版本渲染为稳定 Markdown。"""
    if case.event_id != review.event_id:
        raise ValueError("report case and review must belong to the same event")

    lines = [
        "# 视频事件分析报告",
        "",
        f"- 模板版本：{REPORT_TEMPLATE_VERSION}",
        f"- 事件 ID：{_inline(case.event_id)}",
        f"- 复核 ID：{_inline(review.review_id)}",
        f"- 复核版本：{review.revision}",
        f"- 事件时间（UTC）：{_inline(_format_timestamp(case.timestamp_ms))}",
        f"- 复核时间（UTC）：{_inline(_format_timestamp(review.created_ms))}",
        f"- 视频源：{_inline(case.source)}",
        f"- 帧 ID：{case.frame_id}",
        f"- stream_generation：{_inline(_optional(case.stream_generation))}",
        f"- source_pts：{_inline(_optional(case.source_pts))}",
        "",
        "## 事件记录",
        "",
        f"- 事件类型：{_inline(_optional(case.event_type))}",
        f"- 严重程度：{_inline(_optional(case.severity))}",
        f"- 规则 ID：{_inline(_optional(case.rule_id))}",
        f"- 规则版本：{_inline(_optional(case.rule_version))}",
    ]

    if case.rule_facts:
        lines.append(f"- 规则事实：{_inline(_json_text(case.rule_facts))}")

    lines.extend(["", "### 检测记录", ""])
    if case.detections:
        for detection in case.detections:
            bbox = _detection_bbox(detection)
            lines.append(
                "- "
                f"类别：{_inline(_optional(detection.get('class_name')))}；"
                f"类别 ID：{_inline(_optional(detection.get('class_id')))}；"
                f"置信度：{_inline(_optional(detection.get('confidence')))}；"
                f"bbox：{_inline(_json_text(bbox) if bbox is not None else '未提供')}；"
                f"track ID：{_inline(_optional(detection.get('track_id')))}；"
                f"track 状态：{_inline(_optional(detection.get('track_state')))}；"
                f"遮挡：{_inline(_occlusion_label(detection.get('occluded')))}"
            )
    else:
        lines.append("- 未提供检测记录")

    lines.extend(
        [
            "",
            "## 复核结论",
            "",
            f"- 结论：{_VERDICT_LABELS[review.verdict]}",
            f"- 置信度：{review.confidence}",
            f"- 证据状态：{_evidence_status_label(review.evidence_status)}",
            "",
            "### 说明",
            "",
            _plain_text(review.explanation),
            "",
            "### 结构化陈述",
            "",
        ]
    )

    if review.claims:
        for claim in review.claims:
            text = _plain_text(claim.get("text"))
            evidence_ids = claim.get("evidence_ids") or []
            rendered_ids = ", ".join(_inline(item) for item in evidence_ids)
            suffix = f"（证据 ID：{rendered_ids}）" if rendered_ids else ""
            lines.append(f"- {text}{suffix}")
    else:
        lines.append("- 未提供结构化陈述")

    lines.extend(["", "### 规则引用", ""])
    if review.rule_citations:
        for citation in review.rule_citations:
            source = _rule_source(citation.get("source"))
            lines.append(
                "- "
                f"chunk：{_inline(_optional(citation.get('chunk_id')))}；"
                f"规则：{_inline(_optional(citation.get('rule_id')))}；"
                f"版本：{_inline(_optional(citation.get('rule_version')))}；"
                f"章节：{_inline(_optional(citation.get('section')))}；"
                f"来源：{_inline(source)}"
            )
    else:
        lines.append("- 未提供规则引用")

    lines.extend(["", "### 证据 ID", ""])
    if review.evidence_ids:
        lines.extend(f"- {_inline(evidence_id)}" for evidence_id in review.evidence_ids)
    else:
        lines.append("- 未提供证据 ID")

    lines.extend(
        [
            "",
            "## 限制说明",
            "",
            "本报告仅整理账本中的事件记录与指定复核版本，不进行二次判断。",
            "证据状态和引用反映该复核版本的记录；本报告不替代结构化复核结果。",
            "",
        ]
    )
    return "\n".join(lines)


def write_analysis_report(
    event_id: str,
    review_id: str,
    markdown: str,
) -> ReportArtifact:
    """原子写入不可覆盖的内容寻址报告并返回其摘要。"""
    if not isinstance(review_id, str) or not review_id:
        raise ValueError("review_id must not be empty")

    content = markdown.encode("utf-8")
    safe_review_id = _safe_review_component(review_id)
    path = write_content_addressed_artifact(
        event_id,
        f"report-{safe_review_id}",
        "md",
        content,
    )
    return ReportArtifact(
        path=path,
        sha256=hashlib.sha256(content).hexdigest(),
        size_bytes=len(content),
    )


def read_analysis_report(
    artifact_path: str,
    *,
    sha256: str,
    size_bytes: int,
) -> str:
    """读取并校验已登记的 Markdown 报告 artifact。

    报告路径来自 SQLite，仍必须重新约束到当前 outputs root。读取前后只接受
    普通文件，并用登记的大小和 SHA-256 校验内容，避免把被替换的文件展示给
    Agent。
    """
    path = _validated_report_path(artifact_path)
    if (
        not isinstance(sha256, str)
        or not re.fullmatch(r"[0-9a-f]{64}", sha256)
        or not isinstance(size_bytes, int)
        or isinstance(size_bytes, bool)
        or size_bytes < 0
    ):
        raise ValueError("report metadata is invalid")

    try:
        content = path.read_bytes()
        details = path.lstat()
    except OSError as exc:
        raise ValueError("report artifact is unavailable") from exc
    if stat.S_ISLNK(details.st_mode) or not stat.S_ISREG(details.st_mode):
        raise ValueError("report artifact is not a regular file")
    if len(content) != size_bytes:
        raise ValueError("report artifact size does not match metadata")
    if hashlib.sha256(content).hexdigest() != sha256:
        raise ValueError("report artifact hash does not match metadata")
    try:
        return content.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError("report artifact is not valid UTF-8") from exc


def _validated_report_path(artifact_path: str) -> Path:
    """返回位于 outputs root 内的普通报告路径。"""
    if not isinstance(artifact_path, str) or not artifact_path:
        raise ValueError("report artifact path is invalid")
    root = Path(os.getenv("SSV_OUTPUTS_DIR", "outputs")).resolve()
    path = Path(artifact_path)
    try:
        details = path.lstat()
    except OSError as exc:
        raise ValueError("report artifact is unavailable") from exc
    if stat.S_ISLNK(details.st_mode) or not stat.S_ISREG(details.st_mode):
        raise ValueError("report artifact is not a regular file")
    resolved = path.resolve()
    if not resolved.is_relative_to(root):
        raise ValueError("report artifact path escapes outputs root")
    return resolved


def _safe_review_component(review_id: str) -> str:
    if review_id not in {".", ".."} and _SAFE_REVIEW_ID.fullmatch(review_id):
        return review_id
    return hashlib.sha256(review_id.encode("utf-8")).hexdigest()


def _format_timestamp(timestamp_ms: int) -> str:
    return datetime.fromtimestamp(timestamp_ms / 1000, tz=UTC).isoformat(
        timespec="milliseconds"
    ).replace("+00:00", "Z")


def _optional(value: Any) -> str:
    if value is None or value == "":
        return "未提供"
    return str(value)


def _inline(value: Any) -> str:
    return _plain_text(value)


def _plain_text(value: Any) -> str:
    text = " ".join(str(value).split())
    text = _POSIX_PATH_WITH_SPACES.sub("[本地路径未显示]", text)
    text = _POSIX_PATH.sub("[本地路径未显示]", text)
    text = _WINDOWS_PATH_WITH_SPACES.sub("[本地路径未显示]", text)
    text = _WINDOWS_PATH.sub("[本地路径未显示]", text)
    escaped: list[str] = []
    for character in text:
        if character == "&":
            escaped.append("&amp;")
        elif character in _MARKDOWN_PUNCTUATION:
            escaped.append(f"\\{character}")
        else:
            escaped.append(character)
    return "".join(escaped)


def _json_text(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _detection_bbox(detection: dict[str, Any]) -> Any | None:
    if "bbox" in detection:
        return detection["bbox"]
    value = detection.get("bbox_json")
    if value is None:
        return None
    try:
        return json.loads(value)
    except (TypeError, json.JSONDecodeError):
        return None


def _occlusion_label(value: Any) -> str:
    if value is None:
        return "未提供"
    return "是" if bool(value) else "否"


def _evidence_status_label(value: str) -> str:
    if value in {"available", "missing"}:
        return value
    return "未提供"


def _rule_source(value: Any) -> str:
    source = str(value) if value is not None else ""
    parsed = urlsplit(source)
    if (
        parsed.scheme.lower() == "file"
        or PurePosixPath(source).is_absolute()
        or PureWindowsPath(source).is_absolute()
    ):
        return "本地规则路径未显示"
    return _optional(source)
