from __future__ import annotations

import json
from typing import Any

from ssv_agent.config import SsvConfig
from ssv_agent.event_consumer import EventConsumer


class FakeRedis:
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        self.acked: list[tuple[str, str, str]] = []
        self.created: list[tuple[str, str, str, bool]] = []

    def xgroup_create(self, stream: str, group: str, id: str, mkstream: bool) -> None:
        self.created.append((stream, group, id, mkstream))

    def xack(self, stream: str, group: str, msg_id: str) -> None:
        self.acked.append((stream, group, msg_id))


def make_consumer(monkeypatch: Any) -> tuple[EventConsumer, FakeRedis]:
    fake = FakeRedis()
    monkeypatch.setattr("ssv_agent.event_consumer.Redis", lambda **_kwargs: fake)
    consumer = EventConsumer(SsvConfig())
    return consumer, fake


def test_ensure_group_creates_stream_group(monkeypatch: Any) -> None:
    consumer, fake = make_consumer(monkeypatch)

    consumer._ensure_group()

    assert fake.created == [("ssv:events", "ssv-agent", "0", True)]


def test_handle_event_parses_detection_and_acks(monkeypatch: Any) -> None:
    consumer, fake = make_consumer(monkeypatch)
    payload = {
        "source": "camera-1",
        "frame_id": 42,
        "detections": [
            {"class": "person", "confidence": 0.91, "track_id": 5},
        ],
    }

    consumer._handle_event("123-0", {"event": json.dumps(payload)})

    assert fake.acked == [("ssv:events", "ssv-agent", "123-0")]


def test_handle_event_rejects_malformed_json_without_ack(monkeypatch: Any) -> None:
    consumer, fake = make_consumer(monkeypatch)

    consumer._handle_event("123-0", {"event": "{"})

    assert fake.acked == []
