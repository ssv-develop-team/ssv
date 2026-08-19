from __future__ import annotations

from ssv_agent.review_context import ReviewContext


def test_from_event_maps_publish_fields() -> None:
    payload = {
        "event_id": "case-7",
        "source": "camera-1",
        "timestamp_ms": 1000,
        "frame_id": 3,
        "generation": 12,
        "source_pts": 34,
        "rule_id": "hard-hat",
        "rule_version": "2026.08",
        "rule_facts": {"zone": "north"},
        "detections": [
            {
                "class": "person",
                "class_id": 0,
                "confidence": 0.9,
                "bbox": [1, 2, 3, 4],
                "track_id": 5,
            }
        ],
    }

    context = ReviewContext.from_event("123-0", payload)

    assert context.event_id == "case-7"
    assert context.ingress_id == "123-0"
    assert context.source == "camera-1"
    assert context.timestamp_ms == 1000
    assert context.frame_id == 3
    assert len(context.detections) == 1
    assert context.detections[0].class_name == "person"
    assert context.detections[0].track_id == 5
    assert context.stream_generation == 12
    assert context.source_pts == 34
    assert context.rule_id == "hard-hat"
    assert context.rule_version == "2026.08"
    assert context.rule_facts == {"zone": "north"}


def test_from_event_prefers_explicit_event_type_then_publisher_type() -> None:
    legacy = ReviewContext.from_event(
        "1-0",
        {"source": "camera-1", "type": "legacy-alarm"},
    )
    explicit = ReviewContext.from_event(
        "2-0",
        {
            "source": "camera-1",
            "type": "legacy-alarm",
            "event_type": "new-alarm",
        },
    )

    assert legacy.event_type == "legacy-alarm"
    assert explicit.event_type == "new-alarm"


def test_from_event_accepts_integer_track_state() -> None:
    context = ReviewContext.from_event(
        "1-0",
        {
            "source": "camera-1",
            "detections": [
                {
                    "class": "person",
                    "class_id": 0,
                    "confidence": 0.9,
                    "track_state": 1,
                }
            ],
        },
    )

    assert context.detections[0].track_state == 1


def test_from_event_empty_detections() -> None:
    context = ReviewContext.from_event("1-0", {"source": "camera-1"})

    assert context.detections == []


def test_from_event_keeps_missing_timeline_values_missing() -> None:
    context = ReviewContext.from_event(
        "1-0",
        {"source": "camera-1", "timestamp_ms": 1000, "frame_id": 3},
    )

    assert context.stream_generation is None
    assert context.source_pts is None
