"""证据集成实现 —— 从事件字段读取证据路径。"""

from __future__ import annotations

from pathlib import Path


class EventEvidenceProvider:
    """从 `DetectionEvent.evidence_paths` 提供证据摘要和关键帧路径。"""

    def get_evidence_summary(self, event) -> str:
        paths = getattr(event, "evidence_paths", []) or []
        if not paths:
            return ""
        existing = [str(path) for path in paths if Path(path).exists()]
        missing = [str(path) for path in paths if not Path(path).exists()]
        parts: list[str] = []
        if existing:
            parts.append("可用证据: " + ", ".join(existing))
        if missing:
            parts.append("缺失证据: " + ", ".join(missing))
        return "；".join(parts)

    def get_keyframe_path(self, event) -> str:
        for raw_path in getattr(event, "evidence_paths", []) or []:
            path = Path(raw_path)
            if path.suffix.lower() in {".jpg", ".jpeg", ".png", ".bmp", ".webp"}:
                return str(path)
        return ""
