from __future__ import annotations

from typing import Any

import pytest
from redis.exceptions import ConnectionError

from ssv_agent.dedup import DedupDecision, EventDeduper, _track_key
from ssv_agent.review_context import ReviewContext


class FakeRedis:
    def __init__(self) -> None:
        self.keys: dict[str, int] = {}
        self.values: dict[str, str] = {}
        self.set_calls: list[tuple[str, str, int]] = []

    def set(self, key: str, value: str, nx: bool = False, px: int = 0) -> bool | None:
        self.set_calls.append((key, value, px))
        if nx and key in self.keys:
            return None
        self.keys[key] = px
        self.values[key] = value
        return True

    def get(self, key: str) -> str | None:
        return self.values.get(key)


class FailingRedis:
    def set(self, *args: Any, **kwargs: Any) -> bool | None:
        raise ConnectionError("redis down")


def make_context(
    source: str = "camera-1",
    tracks: tuple[int, ...] = (5,),
    *,
    event_id: str = "1-0",
    ingress_id: str | None = "1-0",
) -> ReviewContext:
    return ReviewContext(
        event_id=event_id,
        ingress_id=ingress_id,
        source=source,
        timestamp_ms=1000,
        frame_id=1,
        detections=[
            {
                "class": "person",
                "class_id": 0,
                "confidence": 0.9,
                "track_id": track_id,
            }
            for track_id in tracks
        ],
    )


def test_new_track_runs_and_marks_all_tracks() -> None:
    redis = FakeRedis()
    deduper = EventDeduper(redis, cooldown_seconds=30.0)

    decision = deduper.decide(make_context(tracks=(1, 2)))

    assert decision is DedupDecision.RUN
    assert len(redis.keys) == 2
    assert all(ttl_ms == 30000 for ttl_ms in redis.keys.values())


def test_repeat_tracks_within_cooldown_are_skipped() -> None:
    redis = FakeRedis()
    deduper = EventDeduper(redis)

    assert deduper.decide(make_context()) is DedupDecision.RUN
    assert (
        deduper.decide(make_context(event_id="2-0", ingress_id="2-0"))
        is DedupDecision.SKIP
    )


def test_same_ingress_redelivery_runs_again_after_a_failed_ledger_commit() -> None:
    redis = FakeRedis()
    deduper = EventDeduper(redis)
    redelivery = make_context(event_id="case-1", ingress_id="100-0")

    assert deduper.decide(redelivery) is DedupDecision.RUN
    assert deduper.decide(redelivery) is DedupDecision.RUN


def test_new_track_bypasses_existing_cooldown() -> None:
    redis = FakeRedis()
    deduper = EventDeduper(redis)

    deduper.decide(make_context(tracks=(1,)))
    decision = deduper.decide(make_context(tracks=(1, 2)))

    assert decision is DedupDecision.RUN
    assert _track_key("ssv:agent:dedup", "camera-1", 2) in redis.keys


def test_empty_detections_run_without_marking_keys() -> None:
    redis = FakeRedis()
    deduper = EventDeduper(redis)

    decision = deduper.decide(make_context(tracks=()))

    assert decision is DedupDecision.RUN
    assert redis.keys == {}


def test_redis_failure_fails_open() -> None:
    deduper = EventDeduper(FailingRedis())  # type: ignore[arg-type]

    decision = deduper.decide(make_context())

    assert decision is DedupDecision.FAIL_OPEN


def test_non_positive_cooldown_is_rejected() -> None:
    with pytest.raises(ValueError):
        EventDeduper(FakeRedis(), cooldown_seconds=0)  # type: ignore[arg-type]


def test_track_key_distinguishes_source_and_track() -> None:
    assert _track_key("ssv:agent:dedup", "camera-1", 1) != _track_key(
        "ssv:agent:dedup", "camera-1", 2
    )
    assert _track_key("ssv:agent:dedup", "camera-1", 1) != _track_key(
        "ssv:agent:dedup", "camera-2", 1
    )
