"""从权威案件构造可重建的事件索引文本。"""

from __future__ import annotations

import json
from collections import Counter
from typing import Any


def _value(event: Any, name: str, default: Any = None) -> Any:
    if isinstance(event, dict):
        return event.get(name, default)
    return getattr(event, name, default)


def _object_summary(detections: Any) -> str:
    names = Counter(detection.get("class_name", "unknown") for detection in detections)
    return ",".join(f"{name}:{count}" for name, count in sorted(names.items())) or "none"


def build_event_text(event: Any) -> str:
    """构造不含本地路径的稳定事件语义文本。"""
    review = _value(event, "review", {}) or {}
    facts = _value(event, "rule_facts", {}) or {}
    evidence = _value(event, "evidence", ()) or ()
    available_evidence = sum(
        1
        for item in evidence
        if (item.get("available", False) if isinstance(item, dict) else item.available)
    )
    verdict = review.get("verdict") if isinstance(review, dict) else None
    confidence = review.get("confidence") if isinstance(review, dict) else None
    evidence_status = review.get("evidence_status") if isinstance(review, dict) else None
    verdict = verdict or _value(event, "verdict")
    confidence = confidence if confidence is not None else _value(event, "confidence")
    return " ".join(
        (
            f"event_id={_value(event, 'event_id', '')}",
            f"source={_value(event, 'source', '')}",
            f"timestamp_ms={_value(event, 'timestamp_ms', '')}",
            f"event_type={_value(event, 'event_type', '') or ''}",
            f"rule_id={_value(event, 'rule_id', '') or ''}",
            f"rule_version={_value(event, 'rule_version', '') or ''}",
            f"severity={_value(event, 'severity', '') or ''}",
            f"objects={_object_summary(_value(event, 'detections', ()) or ())}",
            f"rule_facts={json.dumps(facts, ensure_ascii=False, sort_keys=True, separators=(',', ':'))}",
            f"available_evidence={available_evidence}",
            f"evidence_status={evidence_status or ''}",
            f"verdict={verdict or ''}",
            f"confidence={confidence if confidence is not None else ''}",
        )
    )
