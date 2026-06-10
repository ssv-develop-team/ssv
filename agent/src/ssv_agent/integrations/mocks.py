"""Mock 集成实现 —— 所有跨主线接口的空实现。

设计原则:
  1. 每个 mock 返回空/默认值，不抛异常
  2. 将来真实实现按同样接口替换，零改动
"""

from __future__ import annotations


class MockEvidenceProvider:
    """Mock 证据提供者 —— 返空。"""

    def get_evidence_summary(self, event) -> str:
        return ""

    def get_keyframe_path(self, event) -> str:
        return ""


class MockTrackContextProvider:
    """Mock 跟踪上下文提供者 —— 返回默认值。

    T2 未接通时 track_id 默认 -1，get_consecutive_count 返回 0，
    get_track_state 返回 "unknown"。
    """

    def get_consecutive_count(self, event, track_id: int) -> int:
        return 0

    def get_track_state(self, event, track_id: int) -> str:
        return "unknown"

    def get_track_age(self, event, track_id: int) -> float:
        return 0.0


class MockSourceMetadataProvider:
    """Mock 场景元数据提供者 —— 返空。"""

    def get_scene_info(self, event) -> str:
        return ""


class MockEventSequenceProvider:
    """Mock 事件序列提供者 —— 返空列表。"""

    def get_pending_events(self, event) -> list[dict]:
        return []
