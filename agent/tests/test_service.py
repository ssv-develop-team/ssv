"""Tests for Agent service integration."""

from __future__ import annotations

import json
import time
from typing import Any

from ssv_agent.config import SsvConfig
from ssv_agent.service import AgentService


class FakeRedis:
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        self.acked: list[tuple[str, str, str]] = []
        self.created: list = []
        self.published: list[tuple[str, dict]] = []

    def xgroup_create(self, stream, group, id, mkstream):
        self.created.append((stream, group, id, mkstream))

    def xack(self, stream, group, msg_id):
        self.acked.append((stream, group, msg_id))

    def xadd(self, stream, fields):
        self.published.append((stream, fields))


def test_agent_service_initialization(monkeypatch: Any) -> None:
    """验证 AgentService 正确初始化依赖链。"""
    fake = FakeRedis()
    monkeypatch.setattr("ssv_agent.event_consumer.Redis", lambda **_kw: fake)

    config = SsvConfig()
    service = AgentService(config, mock_provider=True)
    assert service.consumer is not None
    assert service.machine is not None


def test_handle_event_processes_detection(monkeypatch: Any) -> None:
    """验证单条事件处理：解析 → 状态机 → ACK。"""
    fake = FakeRedis()
    monkeypatch.setattr("ssv_agent.event_consumer.Redis", lambda **_kw: fake)
    # 注入 Redis 给 writer（writer 需要用同一个 fake）
    monkeypatch.setattr("ssv_agent.writer.result_writer.Redis", lambda **_kw: fake)

    config = SsvConfig()
    service = AgentService(config, mock_provider=True)

    payload = json.dumps({
        "type": "detection",
        "source": "cam-1",
        "timestamp_ms": 1700000000000,
        "frame_id": 42,
        "detections": [
            {"class": "head", "class_id": 1, "confidence": 0.95, "bbox": [0.1, 0.1, 0.3, 0.3], "track_id": 5},
        ],
    })
    service._handle_event("123-0", {"event": payload})

    # 验证 ACK
    assert ("ssv:events", "ssv-agent", "123-0") in fake.acked

    # 验证复核结果已回写
    assert len(fake.published) == 1
    stream, fields = fake.published[0]
    assert "review" in fields


def test_handle_event_rejects_malformed_json(monkeypatch: Any) -> None:
    """畸形 JSON 不应 ACK。"""
    fake = FakeRedis()
    monkeypatch.setattr("ssv_agent.event_consumer.Redis", lambda **_kw: fake)

    config = SsvConfig()
    service = AgentService(config, mock_provider=True)

    service._handle_event("123-0", {"event": "{"})
    assert fake.acked == []  # 不应 ACK 畸形消息


def test_end_to_end_direct_confirm_flow(monkeypatch: Any) -> None:
    """端到端：mock 事件 → 状态机 → 结果回写 → ACK。"""
    fake = FakeRedis()
    monkeypatch.setattr("ssv_agent.event_consumer.Redis", lambda **_kw: fake)
    monkeypatch.setattr("ssv_agent.writer.result_writer.Redis", lambda **_kw: fake)

    config = SsvConfig()
    service = AgentService(config, mock_provider=True)

    # 高置信度 head-only（无helmet）→ DIRECT_CONFIRM
    payload = json.dumps({
        "source": "cam-1",
        "frame_id": 1,
        "detections": [
            {"class": "head", "confidence": 0.95, "bbox": [0.1, 0.1, 0.3, 0.3], "track_id": 1},
        ],
    })
    service._handle_event("msg-1", {"event": payload})

    # ACK 确认
    assert ("ssv:events", "ssv-agent", "msg-1") in fake.acked

    # 复核结果（高置信度+tracked → CRITICAL → NOTIFY_REPORT）
    assert len(fake.published) >= 1
    result = fake.published[0][1].get("review", "")
    assert "notify_report" in result
    assert "cam-1" in result
