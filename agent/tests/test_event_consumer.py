"""Tests for Redis event consumer (pure I/O layer)."""

from __future__ import annotations

import json
from typing import Any

from ssv_agent.config import SsvConfig
from ssv_agent.event_consumer import EventConsumer


class FakeRedis:
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        self.acked: list[tuple[str, str, str]] = []
        self.created: list[tuple[str, str, str, bool]] = []
        self.published: list[tuple[str, dict]] = []

    def xgroup_create(self, stream: str, group: str, id: str, mkstream: bool) -> None:
        self.created.append((stream, group, id, mkstream))

    def xack(self, stream: str, group: str, msg_id: str) -> None:
        self.acked.append((stream, group, msg_id))

    def xadd(self, stream: str, fields: dict) -> None:
        self.published.append((stream, fields))


def make_consumer(monkeypatch: Any) -> tuple[EventConsumer, FakeRedis]:
    fake = FakeRedis()
    monkeypatch.setattr("ssv_agent.event_consumer.Redis", lambda **_kwargs: fake)
    consumer = EventConsumer(SsvConfig())
    return consumer, fake


def test_ensure_group_creates_stream_group(monkeypatch: Any) -> None:
    consumer, fake = make_consumer(monkeypatch)
    consumer._ensure_group()
    assert fake.created == [("ssv:events", "ssv-agent", "0", True)]


def test_ack_confirms_message(monkeypatch: Any) -> None:
    consumer, fake = make_consumer(monkeypatch)
    consumer.ack("123-0")
    assert fake.acked == [("ssv:events", "ssv-agent", "123-0")]


def test_redis_client_access(monkeypatch: Any) -> None:
    consumer, fake = make_consumer(monkeypatch)
    assert consumer.redis_client is fake


def test_handler_callback_is_invoked(monkeypatch: Any) -> None:
    """验证业务回调被正确调用。"""
    consumer, fake = make_consumer(monkeypatch)
    # Fake xreadgroup to return one event then stop
    call_args: list = []

    def handler(msg_id: str, fields: dict[str, str]) -> None:
        call_args.append((msg_id, fields))
        consumer.stop()  # 处理一条就停

    def fake_start(handler_fn):
        consumer._running = True
        payload = {
            "source": "cam-1",
            "frame_id": 42,
            "detections": [{"class": "head", "confidence": 0.9}],
        }
        handler_fn("123-0", {"event": json.dumps(payload)})
        consumer._running = False
    monkeypatch.setattr(consumer, "start", fake_start)

    consumer.start(handler)
    assert len(call_args) == 1
    assert call_args[0][0] == "123-0"
    assert "cam-1" in call_args[0][1]["event"]
