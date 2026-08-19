"""get_event 工具：按 event_id 查询事件详情。"""

from __future__ import annotations

import json
from typing import Any

from langchain.tools import tool

from ssv_agent.event_store import EventCase, EventLedger


def event_case_payload(case: EventCase) -> dict[str, Any]:
    """为只读工具投影权威案件，刻意排除宿主机证据路径。"""
    detections = []
    for detection in case.detections:
        item = dict(detection)
        item["bbox"] = json.loads(item.pop("bbox_json", "[]"))
        item.pop("id", None)
        item.pop("event_id", None)
        detections.append(item)
    evidence = [
        {
            "evidence_id": item.evidence_id,
            "kind": item.kind,
            "mime_type": item.mime_type,
            "available": item.available,
            "size": item.size,
            "mtime": item.mtime,
            "sha256": item.sha256,
            "source_pts_start": item.source_pts_start,
            "source_pts_end": item.source_pts_end,
            "stream_generation": item.stream_generation,
        }
        for item in case.evidence
    ]
    return {
        "event_id": case.event_id,
        "ingress_id": case.ingress_id,
        "source": case.source,
        "timestamp_ms": case.timestamp_ms,
        "frame_id": case.frame_id,
        "stream_generation": case.stream_generation,
        "source_pts": case.source_pts,
        "event_type": case.event_type,
        "severity": case.severity,
        "rule_id": case.rule_id,
        "rule_version": case.rule_version,
        "rule_facts": case.rule_facts,
        "revision": case.revision,
        "status": case.status,
        "detections": detections,
        "evidence": evidence,
        "review": case.review,
    }


@tool("get_event", parse_docstring=True)
def get_event_tool(event_id: str) -> str:
    """按 event_id 查询事件详情，包含检测列表与证据元信息。

    Args:
        event_id: 事件 ID。

    Returns:
        JSON 字符串；未找到时返回 found=false。
    """
    with EventLedger() as ledger:
        case = ledger.get_case(event_id)
    if case is None:
        return json.dumps({"found": False, "event_id": event_id}, ensure_ascii=False)
    return json.dumps({"found": True, **event_case_payload(case)}, ensure_ascii=False)
