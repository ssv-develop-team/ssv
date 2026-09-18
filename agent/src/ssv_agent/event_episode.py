"""把检测事件去重结果连接到可持久化的 episode 生命周期。"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from typing import Protocol

from ssv_agent.config import RecordingEvidenceConfig
from ssv_agent.dedup import DedupDecision
from ssv_agent.event_store import (
    EpisodeCloseReason,
    EpisodeIngestOutcome,
    EpisodeState,
    EventEpisode,
    EventLedger,
)
from ssv_agent.review_context import ReviewContext


class EpisodeAction(StrEnum):
    """一次观测对当前 episode 的结构性影响。"""

    OPEN = "open"
    UPDATE = "update"
    CLOSE = "close"
    ROLLOVER = "rollover"


@dataclass(frozen=True)
class EpisodeTransition:
    """episode 状态机的一次无副作用决策。"""

    action: EpisodeAction
    state: EpisodeState
    close_reason: EpisodeCloseReason | None = None
    close_after_open: bool = False


class EpisodePolicy(Protocol):
    """Ledger 管理 episode 所需的最小策略接口。"""

    @property
    def post_roll_ms(self) -> int: ...

    @property
    def generation_change_reason(self) -> EpisodeCloseReason: ...

    def transition(
        self,
        context: ReviewContext,
        current: EventEpisode | None,
        *,
        now_ms: int,
    ) -> EpisodeTransition: ...

    def expiration_reason(
        self,
        episode: EventEpisode,
        *,
        now_ms: int,
    ) -> EpisodeCloseReason | None: ...

    def evidence_window(
        self,
        episode: EventEpisode,
        end_pts: int,
    ) -> tuple[int, int]: ...


_TRACK_STATE_NAMES = {
    0: "NEW",
    1: "MATCHED",
    2: "LOST",
    3: "DEAD",
}
_EXPLICIT_END_PHASES = frozenset({"end", "ended", "close", "closed"})
_TERMINAL_PHASES = _EXPLICIT_END_PHASES | {"dead"}
_LOST_PHASES = frozenset({"lost", "lost_grace"})


def _normalise_track_state(value: str | int | None) -> str | None:
    if value is None:
        return None
    if isinstance(value, int):
        return _TRACK_STATE_NAMES.get(value, str(value))
    text = str(value).strip().upper()
    if text.isdigit():
        return _TRACK_STATE_NAMES.get(int(text), text)
    return text.rsplit("_", 1)[-1]


def _normalise_event_phase(value: str | None) -> str:
    return (value or "").strip().lower()


def _lifecycle_flags(context: ReviewContext) -> tuple[bool, bool]:
    """返回 ``(is_lost, is_terminal)``，保留 tracker 的关联结果。"""
    phase = _normalise_event_phase(context.event_phase)
    if phase in _TERMINAL_PHASES:
        return False, True
    if phase in _LOST_PHASES:
        return True, False

    states = {
        _normalise_track_state(detection.track_state)
        for detection in context.detections
    }
    if "NEW" in states or "MATCHED" in states:
        return False, False
    if "LOST" in states:
        return True, False
    if "DEAD" in states:
        return False, True
    return False, False


class EventEpisodeAggregator:
    """以很小的调用接口管理 episode 策略，持久化交给 ``EventLedger``。

    ``ingest`` 和 ``flush`` 都接收调用方拥有的 ledger。Aggregator 不持有数据库
    连接，因此 Consumer、测试和 worker 使用同一个 SQLite 事务接口观察生命周期。
    """

    def __init__(self, config: RecordingEvidenceConfig) -> None:
        self._config = config

    @property
    def post_roll_ms(self) -> int:
        return self._config.clip_after_ms

    @property
    def generation_change_reason(self) -> EpisodeCloseReason:
        return EpisodeCloseReason.GENERATION_CHANGE

    def ingest(
        self,
        ledger: EventLedger,
        context: ReviewContext,
        dedup_decision: DedupDecision = DedupDecision.RUN,
        *,
        now_ms: int | None = None,
    ) -> EpisodeIngestOutcome:
        """写入一条观测；``SKIP`` 仍会更新未处理过的 episode ingress。"""
        return ledger.ingest_episode(
            context,
            policy=self,
            dedup_decision=dedup_decision,
            now_ms=now_ms,
        )

    def flush(
        self,
        ledger: EventLedger,
        *,
        now_ms: int | None = None,
    ) -> tuple[EventEpisode, ...]:
        """关闭已经超过生命周期时限的 episode。"""
        return ledger.close_expired_episodes(policy=self, now_ms=now_ms)

    def transition(
        self,
        context: ReviewContext,
        current: EventEpisode | None,
        *,
        now_ms: int,
    ) -> EpisodeTransition:
        phase = _normalise_event_phase(context.event_phase)
        is_lost, is_terminal = _lifecycle_flags(context)
        if current is None:
            return EpisodeTransition(
                action=EpisodeAction.OPEN,
                state=EpisodeState.LOST_GRACE if is_lost else EpisodeState.OPEN,
                close_reason=(
                    EpisodeCloseReason.EXPLICIT_END if is_terminal else None
                ),
                close_after_open=is_terminal,
            )

        source_pts = context.source_pts
        if source_pts is None:
            raise ValueError("episode transition requires exact source PTS")
        if is_terminal:
            return EpisodeTransition(
                action=EpisodeAction.CLOSE,
                state=EpisodeState.CLOSED,
                close_reason=(
                    EpisodeCloseReason.EXPLICIT_END
                    if phase in _EXPLICIT_END_PHASES
                    else EpisodeCloseReason.DEAD
                ),
            )
        elapsed_pts_ms = (source_pts - current.start_pts) // 1_000_000
        if source_pts - current.last_seen_pts > self._config.merge_gap_ms * 1_000_000:
            return EpisodeTransition(
                action=EpisodeAction.ROLLOVER,
                state=EpisodeState.OPEN,
                close_reason=EpisodeCloseReason.MERGE_GAP,
            )
        if elapsed_pts_ms > self._config.max_episode_ms:
            return EpisodeTransition(
                action=EpisodeAction.ROLLOVER,
                state=EpisodeState.OPEN,
                close_reason=EpisodeCloseReason.MAX_DURATION,
            )
        return EpisodeTransition(
            action=EpisodeAction.UPDATE,
            state=EpisodeState.LOST_GRACE if is_lost else EpisodeState.OPEN,
        )

    def expiration_reason(
        self,
        episode: EventEpisode,
        *,
        now_ms: int,
    ) -> EpisodeCloseReason | None:
        """按墙钟近似调度生命周期关闭，不参与媒体时间计算。"""
        if episode.state is EpisodeState.CLOSED:
            return None
        elapsed_pts_ms = (episode.last_seen_pts - episode.start_pts) // 1_000_000
        if elapsed_pts_ms >= self._config.max_episode_ms:
            return EpisodeCloseReason.MAX_DURATION
        silence_ms = max(0, now_ms - episode.last_seen_timestamp_ms)
        if (
            episode.state is EpisodeState.LOST_GRACE
            and silence_ms >= self._config.lost_grace_ms
        ):
            return EpisodeCloseReason.LOST_TIMEOUT
        if silence_ms >= self._config.silence_timeout_ms:
            return EpisodeCloseReason.SILENCE_TIMEOUT
        return None

    def evidence_window(
        self,
        episode: EventEpisode,
        end_pts: int,
    ) -> tuple[int, int]:
        """使用 episode 首尾观测生成冻结的半开 PTS 窗口。"""
        window_start = max(
            0,
            episode.start_pts - self._config.clip_before_ms * 1_000_000,
        )
        window_end = end_pts + self._config.clip_after_ms * 1_000_000
        if window_end <= window_start:
            raise ValueError("episode evidence window is invalid")
        return window_start, window_end


__all__ = [
    "EpisodeAction",
    "EpisodePolicy",
    "EpisodeTransition",
    "EventEpisodeAggregator",
]
