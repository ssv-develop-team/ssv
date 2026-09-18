from __future__ import annotations

from pathlib import Path

from ssv_agent.config import RecordingEvidenceConfig
from ssv_agent.dedup import DedupDecision
from ssv_agent.event_episode import EventEpisodeAggregator
from ssv_agent.event_store import EpisodeCloseReason, EpisodeState, EventLedger, JobKind
from ssv_agent.review_context import ReviewContext


def _config() -> RecordingEvidenceConfig:
    return RecordingEvidenceConfig(
        enabled=True,
        clip_before_ms=2_500,
        clip_after_ms=2_500,
        merge_gap_ms=3_000,
        lost_grace_ms=3_000,
        silence_timeout_ms=10_000,
        max_episode_ms=30_000,
    )


def _context(
    event_id: str,
    ingress_id: str,
    source_pts: int,
    timestamp_ms: int,
    *,
    track_state: int = 1,
    generation: int = 7,
    event_phase: str | None = None,
) -> ReviewContext:
    return ReviewContext(
        event_id=event_id,
        ingress_id=ingress_id,
        source="camera-1",
        timestamp_ms=timestamp_ms,
        frame_id=source_pts // 1_000_000,
        stream_generation=generation,
        source_pts=source_pts,
        event_type="person_without_helmet",
        rule_id="helmet",
        rule_version="v1",
        event_phase=event_phase,
        detections=[
            {
                "class": "person",
                "class_id": 0,
                "confidence": 0.9,
                "track_id": 5,
                "track_state": track_state,
            }
        ],
    )


def _jobs(ledger: EventLedger, event_id: str, kind: JobKind) -> list[object]:
    return [
        job
        for job in ledger.jobs_for_event(event_id)
        if job.kind is kind
    ]


def test_skip_observation_extends_one_episode_and_dead_creates_one_job(tmp_path: Path) -> None:
    config = _config()
    aggregator = EventEpisodeAggregator(config)
    db_path = tmp_path / "events.db"

    with EventLedger(db_path, recording_evidence=config) as ledger:
        first = aggregator.ingest(
            ledger,
            _context("evt-1", "1-0", 1_000_000_000, 1_000, track_state=0),
            DedupDecision.RUN,
            now_ms=1_000,
        )
        second = aggregator.ingest(
            ledger,
            _context("evt-2", "2-0", 2_000_000_000, 2_000),
            DedupDecision.SKIP,
            now_ms=2_000,
        )
        closed = aggregator.ingest(
            ledger,
            _context(
                "evt-3",
                "3-0",
                2_100_000_000,
                2_100,
                track_state=3,
            ),
            DedupDecision.SKIP,
            now_ms=2_100,
        )
        episode = ledger.get_episode("evt-1")
        jobs = _jobs(ledger, "evt-1", JobKind.EVIDENCE_EXTRACT)

    assert first.created is True
    assert second.updated is True
    assert closed.closed is True
    assert episode is not None
    assert episode.state is EpisodeState.CLOSED
    assert episode.close_reason is EpisodeCloseReason.DEAD
    assert episode.event_ids == ("evt-1", "evt-2", "evt-3")
    assert episode.evidence_window_start == 0
    assert episode.evidence_window_end == 4_600_000_000
    assert len(jobs) == 1


def test_redelivery_is_idempotent_and_does_not_move_last_seen(tmp_path: Path) -> None:
    config = _config()
    aggregator = EventEpisodeAggregator(config)

    with EventLedger(tmp_path / "events.db", recording_evidence=config) as ledger:
        aggregator.ingest(
            ledger,
            _context("evt-1", "1-0", 1_000_000_000, 1_000),
            now_ms=1_000,
        )
        duplicate = aggregator.ingest(
            ledger,
            _context("evt-2", "1-0", 5_000_000_000, 5_000),
            DedupDecision.RUN,
            now_ms=5_000,
        )
        episode = ledger.get_episode("evt-1")

    assert duplicate.duplicate is True
    assert duplicate.updated is False
    assert episode is not None
    assert episode.last_seen_pts == 1_000_000_000
    assert episode.event_ids == ("evt-1",)


def test_lost_enters_grace_and_matching_observation_reopens_episode(tmp_path: Path) -> None:
    config = _config()
    aggregator = EventEpisodeAggregator(config)

    with EventLedger(tmp_path / "events.db", recording_evidence=config) as ledger:
        aggregator.ingest(
            ledger,
            _context("evt-1", "1-0", 1_000_000_000, 1_000),
            now_ms=1_000,
        )
        lost = aggregator.ingest(
            ledger,
            _context("evt-2", "2-0", 2_000_000_000, 2_000, track_state=2),
            now_ms=2_000,
        )
        not_yet_closed = aggregator.flush(ledger, now_ms=4_500)
        recovered = aggregator.ingest(
            ledger,
            _context("evt-3", "3-0", 2_500_000_000, 2_500),
            now_ms=4_500,
        )
        episode = ledger.get_episode("evt-1")

    assert lost.episode is not None
    assert lost.episode.state is EpisodeState.LOST_GRACE
    assert not_yet_closed == ()
    assert recovered.closed is False
    assert episode is not None
    assert episode.state is EpisodeState.OPEN
    assert episode.last_seen_pts == 2_500_000_000


def test_first_lost_observation_starts_in_grace(tmp_path: Path) -> None:
    config = _config()
    aggregator = EventEpisodeAggregator(config)

    with EventLedger(tmp_path / "events.db", recording_evidence=config) as ledger:
        outcome = aggregator.ingest(
            ledger,
            _context(
                "evt-1",
                "1-0",
                1_000_000_000,
                1_000,
                track_state=2,
            ),
            now_ms=1_000,
        )
        episode = ledger.get_episode("evt-1")

    assert outcome.episode is not None
    assert episode is not None
    assert episode.state is EpisodeState.LOST_GRACE
    assert episode.close_reason is None


def test_lost_timeout_closes_episode_and_schedules_delayed_extract(tmp_path: Path) -> None:
    config = _config()
    aggregator = EventEpisodeAggregator(config)

    with EventLedger(tmp_path / "events.db", recording_evidence=config) as ledger:
        aggregator.ingest(
            ledger,
            _context("evt-1", "1-0", 1_000_000_000, 1_000),
            now_ms=1_000,
        )
        aggregator.ingest(
            ledger,
            _context("evt-2", "2-0", 2_000_000_000, 2_000, track_state=2),
            now_ms=2_000,
        )
        closed = aggregator.flush(ledger, now_ms=5_000)
        jobs = _jobs(ledger, "evt-1", JobKind.EVIDENCE_EXTRACT)

    assert len(closed) == 1
    assert closed[0].close_reason is EpisodeCloseReason.LOST_TIMEOUT
    assert len(jobs) == 1
    assert jobs[0].available_at_ms == 5_000


def test_generation_change_closes_old_episode_before_opening_new_one(tmp_path: Path) -> None:
    config = _config()
    aggregator = EventEpisodeAggregator(config)

    with EventLedger(tmp_path / "events.db", recording_evidence=config) as ledger:
        aggregator.ingest(
            ledger,
            _context("evt-1", "1-0", 1_000_000_000, 1_000, generation=7),
            now_ms=1_000,
        )
        second = aggregator.ingest(
            ledger,
            _context("evt-2", "2-0", 1_500_000_000, 1_500, generation=8),
            now_ms=1_500,
        )
        old = ledger.get_episode("evt-1")
        new = ledger.get_episode("evt-2")
        jobs = _jobs(ledger, "evt-1", JobKind.EVIDENCE_EXTRACT)

    assert second.episode is not None
    assert second.episode.episode_id == "evt-2"
    assert old is not None
    assert old.close_reason is EpisodeCloseReason.GENERATION_CHANGE
    assert new is not None
    assert new.state is EpisodeState.OPEN
    assert new.stream_generation == 8
    assert len(jobs) == 1


def test_missing_exact_anchor_is_recorded_without_episode_job(tmp_path: Path) -> None:
    config = _config()
    aggregator = EventEpisodeAggregator(config)
    context = _context("evt-1", "1-0", 1_000_000_000, 1_000).model_copy(
        update={"source_pts": None}
    )

    with EventLedger(tmp_path / "events.db", recording_evidence=config) as ledger:
        outcome = aggregator.ingest(ledger, context, now_ms=1_000)
        case = ledger.get_case("evt-1")
        jobs = _jobs(ledger, "evt-1", JobKind.EVIDENCE_EXTRACT)

    assert outcome.episode is None
    assert outcome.case is not None
    assert case is not None
    assert jobs == []
