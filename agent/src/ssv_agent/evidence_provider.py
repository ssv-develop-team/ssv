"""取证接口和共享结果类型。"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Literal, Protocol

if TYPE_CHECKING:
    from ssv_agent.event_store import EventCase


@dataclass(frozen=True)
class EvidenceArtifact:
    """一个已经完整落盘、可由账本登记的派生证据文件。"""

    kind: Literal["clip", "frame"]
    path: Path
    sha256: str
    mime_type: str
    size: int
    mtime: float
    source_pts_start: int | None = None
    source_pts_end: int | None = None
    stream_generation: int | None = None


class EvidenceExtractionError(RuntimeError):
    """取证失败；``retryable`` 决定 worker 是否继续重试。"""

    def __init__(self, message: str, *, retryable: bool) -> None:
        super().__init__(message)
        self.retryable = retryable


class CommandRunner(Protocol):
    """可替换的命令执行边界，参数保持为数组。"""

    def run(self, args: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]: ...


class EvidenceExtractor(Protocol):
    """从权威案件快照生成完整的 clip 与三张帧。"""

    def extract(self, case: EventCase) -> tuple[EvidenceArtifact, ...]: ...
