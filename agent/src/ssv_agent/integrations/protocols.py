"""跨主线集成接口 —— T1/T2/T3 数据注入 T4 上下文的 Protocol 定义。

设计原则:
  1. 所有接口使用 Protocol，当前 mock，将来切换零改动
  2. 每个接口独立，可分别接通
  3. 空值时静默跳过，不影响其他接口
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Protocol


# ── Layer 0 接口（本阶段实现，接通即用） ────────────────────────────────────


class EvidenceProvider(Protocol):
    """T3 证据模块 → T4 上下文。"""

    def get_evidence_summary(self, event) -> str:
        """获取证据文件摘要文本。"""
        ...

    def get_keyframe_path(self, event) -> str:
        """获取关键帧文件路径。"""
        ...


class TrackContextProvider(Protocol):
    """T2 跟踪模块 → T4 上下文。

    为历史管理器关联度计算提供精确输入。
    T2 未接通时 mock 返回 0/"unknown"，不影响正常运作。
    """

    def get_consecutive_count(self, event, track_id: int) -> int:
        """获取目标连续检测帧数。"""
        ...

    def get_track_state(self, event, track_id: int) -> str:
        """获取目标跟踪状态。"""
        ...

    def get_track_age(self, event, track_id: int) -> float:
        """获取目标跟踪持续时间（秒）。"""
        ...


# ── Layer 1 接口（M5 激活） ─────────────────────────────────────────────────


class SourceMetadataProvider(Protocol):
    """T1 视频链路 → T4 上下文。"""

    def get_scene_info(self, event) -> str:
        """获取场景信息文本（位置/光照/场景类型）。"""
        ...


class EventSequenceProvider(Protocol):
    """T3 事件边界 → T4 上下文。"""

    def get_pending_events(self, event) -> list[dict]:
        """获取相邻 pending 事件列表。"""
        ...


# ── 集成数据容器 ────────────────────────────────────────────────────────────


@dataclass
class IntegrationData:
    """ContextEngine 收集的集成数据汇总。

    各字段为空时静默跳过，不影响其他要素。
    """

    evidence_summary: str = ""
    keyframe_path: str = ""
    track_states: list[dict] = field(default_factory=list)
    scene_info: str = ""
    pending_events: list[dict] = field(default_factory=list)
