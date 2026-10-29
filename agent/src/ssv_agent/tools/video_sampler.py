"""受控的视频粗采样与精采样工具。

模型只能通过 ``event_id`` 和账本登记的 clip evidence 读取视频。工具把有限数量的
JPEG 写入当前 DeerFlow review thread 的 outputs 目录，随后由内置 ``view_image``
逐张查看；它不会把 MP4 直接作为模型输入，也不会登记新的长期 evidence。
"""

from __future__ import annotations

import errno
import json
import os
import shutil
import stat
import subprocess
import tempfile
from hashlib import sha256
from pathlib import Path
from typing import Annotated, Literal

from deerflow.config.paths import VIRTUAL_PATH_PREFIX
from deerflow.sandbox.tools import get_thread_data
from deerflow.tools.types import Runtime
from langchain.tools import tool
from pydantic import Field

from ssv_agent.event_store import EventCase, EventLedger, EvidenceRef

_MAX_FRAMES = 8
_DEFAULT_FRAMES = 4
_FRAME_WIDTH = 640
_COMMAND_TIMEOUT_SECONDS = 30
_MILLISECONDS_PER_SECOND = 1_000
_NANOSECONDS_PER_MILLISECOND = 1_000_000


def _token(value: str) -> str:
    """Return a path-safe, short token for an event or evidence identifier."""
    return sha256(value.encode("utf-8")).hexdigest()[:24]


def _response(
    event_id: str,
    *,
    available: bool,
    reason: str,
    evidence_id: str | None = None,
    mode: str | None = None,
    clip_duration_ms: int | None = None,
    interval_start_ms: int | None = None,
    interval_end_ms: int | None = None,
    event_offset_ms: int | None = None,
    recommended_fine_interval_ms: tuple[int, int] | None = None,
    frames: list[dict[str, object]] | None = None,
) -> str:
    """Serialize a stable tool response without exposing host paths."""
    return json.dumps(
        {
            "available": available,
            "event_id": event_id,
            "evidence_id": evidence_id,
            "mode": mode,
            "reason": reason,
            "clip_duration_ms": clip_duration_ms,
            "interval_start_ms": interval_start_ms,
            "interval_end_ms": interval_end_ms,
            "event_offset_ms": event_offset_ms,
            "recommended_fine_interval_ms": (
                list(recommended_fine_interval_ms)
                if recommended_fine_interval_ms is not None
                else None
            ),
            "frames": frames or [],
        },
        ensure_ascii=False,
    )


def _error(event_id: str, reason: str, *, mode: str | None = None) -> str:
    return _response(event_id, available=False, reason=reason, mode=mode)


def _clip_duration_ms(reference: EvidenceRef) -> int | None:
    start = reference.source_pts_start
    end = reference.source_pts_end
    if not isinstance(start, int) or not isinstance(end, int) or end <= start:
        return None
    # Ceil so a sub-millisecond tail is not silently excluded from the legal window.
    return max(
        1,
        (end - start + _NANOSECONDS_PER_MILLISECOND - 1)
        // _NANOSECONDS_PER_MILLISECOND,
    )


def _event_offset_ms(case: EventCase, reference: EvidenceRef, duration_ms: int) -> int | None:
    if not isinstance(case.source_pts, int) or not isinstance(reference.source_pts_start, int):
        return None
    delta = case.source_pts - reference.source_pts_start
    if delta >= 0:
        offset = (delta + _NANOSECONDS_PER_MILLISECOND // 2) // _NANOSECONDS_PER_MILLISECOND
    else:
        offset = -(
            (-delta + _NANOSECONDS_PER_MILLISECOND // 2)
            // _NANOSECONDS_PER_MILLISECOND
        )
    return max(0, min(duration_ms, offset))


def _recommended_fine_interval(
    event_offset_ms: int | None,
    duration_ms: int,
) -> tuple[int, int]:
    if event_offset_ms is None:
        return (0, duration_ms)
    start = max(0, event_offset_ms - 1_000)
    end = min(duration_ms, event_offset_ms + 1_000)
    if end <= start:
        return (0, duration_ms)
    return (start, end)


def _select_clip(
    ledger: EventLedger,
    event_id: str,
    evidence_id: str | None,
) -> tuple[EventCase | None, EvidenceRef | None, Path | None, str | None]:
    """Refresh and select one registered, available clip evidence."""
    ledger.refresh_evidence(event_id, evidence_id)
    case = ledger.get_case(event_id)
    if case is None:
        return None, None, None, "event not found"

    candidates = [
        item
        for item in case.evidence
        if item.kind == "clip"
        and item.available
        and (evidence_id is None or item.evidence_id == evidence_id)
    ]
    if evidence_id is not None and not candidates:
        return case, None, None, "registered clip evidence not found or unavailable"
    if len(candidates) != 1:
        return case, None, None, "event must have exactly one available clip evidence"

    reference = candidates[0]
    path = ledger.resolve_evidence_path(reference.path)
    if path is None:
        return case, reference, None, "clip evidence path is outside configured roots"
    try:
        details = path.lstat()
    except OSError:
        return case, reference, None, "clip evidence file is unavailable"
    if stat.S_ISLNK(details.st_mode) or not stat.S_ISREG(details.st_mode):
        return case, reference, None, "clip evidence is not a regular file"
    return case, reference, path, None


def _fsync_file(path: Path) -> None:
    with path.open("rb") as handle:
        os.fsync(handle.fileno())


def _pin_clip(source: Path, destination: Path) -> None:
    """Hold a stable input inode while ffmpeg reads it."""
    try:
        os.link(source, destination, follow_symlinks=False)
    except OSError as exc:
        if exc.errno != errno.EXDEV:
            raise
        with source.open("rb") as source_handle, destination.open("wb") as target_handle:
            shutil.copyfileobj(source_handle, target_handle)
            target_handle.flush()
            os.fsync(target_handle.fileno())

    details = destination.lstat()
    if stat.S_ISLNK(details.st_mode) or not stat.S_ISREG(details.st_mode):
        raise OSError("pinned clip is not a regular file")


def _sampling_window(
    mode: Literal["coarse", "fine"],
    duration_ms: int,
    interval_start_ms: int | None,
    interval_end_ms: int | None,
) -> tuple[int, int] | str:
    if mode == "coarse":
        return 0, duration_ms
    if interval_start_ms is None or interval_end_ms is None:
        return "fine sampling requires interval_start_ms and interval_end_ms"
    if interval_start_ms < 0 or interval_end_ms <= interval_start_ms:
        return "fine sampling interval must satisfy 0 <= start < end"
    if interval_end_ms > duration_ms:
        return "fine sampling interval exceeds clip duration"
    return interval_start_ms, interval_end_ms


def _frame_offsets(start_ms: int, end_ms: int, frames: int) -> list[int]:
    span = end_ms - start_ms
    return [start_ms + (index * 2 + 1) * span // (frames * 2) for index in range(frames)]


def _extract_frame(
    ffmpeg: str,
    input_path: Path,
    output_path: Path,
    offset_ms: int,
) -> None:
    temporary_path = output_path.with_suffix(".tmp.jpg")
    args = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-ss",
        f"{offset_ms / _MILLISECONDS_PER_SECOND:.3f}",
        "-i",
        str(input_path),
        "-frames:v",
        "1",
        "-vf",
        f"scale={_FRAME_WIDTH}:-1",
        "-q:v",
        "2",
        str(temporary_path),
    ]
    try:
        completed = subprocess.run(
            args,
            check=False,
            capture_output=True,
            text=True,
            timeout=_COMMAND_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired):
        temporary_path.unlink(missing_ok=True)
        raise
    if completed.returncode != 0:
        temporary_path.unlink(missing_ok=True)
        raise RuntimeError("ffmpeg failed to extract a video frame")
    try:
        details = temporary_path.lstat()
    except OSError as exc:
        raise RuntimeError("ffmpeg did not produce a video frame") from exc
    if stat.S_ISLNK(details.st_mode) or not stat.S_ISREG(details.st_mode) or details.st_size <= 0:
        temporary_path.unlink(missing_ok=True)
        raise RuntimeError("ffmpeg produced an invalid video frame")
    os.replace(temporary_path, output_path)
    _fsync_file(output_path)


def _sample_clip(
    *,
    runtime: Runtime,
    event_id: str,
    evidence_id: str | None,
    mode: Literal["coarse", "fine"],
    frames: int,
    interval_start_ms: int | None,
    interval_end_ms: int | None,
) -> str:
    thread_data = get_thread_data(runtime)
    outputs_path = (thread_data or {}).get("outputs_path")
    if not isinstance(outputs_path, (str, os.PathLike)) or not outputs_path:
        return _error(event_id, "thread outputs directory unavailable", mode=mode)

    try:
        with EventLedger() as ledger:
            case, reference, source, reason = _select_clip(ledger, event_id, evidence_id)
    except Exception:
        return _error(event_id, "event evidence lookup failed", mode=mode)
    if reason is not None or case is None or reference is None or source is None:
        return _error(event_id, reason or "clip evidence unavailable", mode=mode)

    duration_ms = _clip_duration_ms(reference)
    if duration_ms is None:
        return _error(event_id, "clip evidence timing metadata unavailable", mode=mode)
    window = _sampling_window(
        mode,
        duration_ms,
        interval_start_ms,
        interval_end_ms,
    )
    if isinstance(window, str):
        return _response(
            event_id,
            available=False,
            reason=window,
            evidence_id=reference.evidence_id,
            mode=mode,
            clip_duration_ms=duration_ms,
        )
    start_ms, end_ms = window
    event_offset_ms = _event_offset_ms(case, reference, duration_ms)
    recommended = _recommended_fine_interval(event_offset_ms, duration_ms)

    try:
        ffmpeg = shutil.which("ffmpeg")
        if ffmpeg is None:
            return _response(
                event_id,
                available=False,
                reason="ffmpeg is unavailable",
                evidence_id=reference.evidence_id,
                mode=mode,
                clip_duration_ms=duration_ms,
                interval_start_ms=start_ms,
                interval_end_ms=end_ms,
                event_offset_ms=event_offset_ms,
                recommended_fine_interval_ms=recommended,
            )

        output_root = Path(outputs_path)
        sample_parent = output_root / "samples" / _token(event_id)
        sample_parent.mkdir(parents=True, exist_ok=True)
        temporary_input = Path(tempfile.mkdtemp(dir=output_root, prefix=".ssv-sample-input-"))
        temporary_output: Path | None = Path(
            tempfile.mkdtemp(dir=sample_parent, prefix=".ssv-sample-output-")
        )
        destination = sample_parent / (
            f"{_token(reference.evidence_id)}-{mode}-{start_ms}-{end_ms}-{frames}"
        )
        pinned_input = temporary_input / "context.mp4"
        offsets = _frame_offsets(start_ms, end_ms, frames)
        try:
            _pin_clip(source, pinned_input)
            frame_rows: list[dict[str, object]] = []
            for index, offset_ms in enumerate(offsets, start=1):
                frame_path = temporary_output / f"frame-{index:03d}.jpg"
                _extract_frame(ffmpeg, pinned_input, frame_path, offset_ms)
                frame_rows.append(
                    {
                        "index": index,
                        "offset_ms": offset_ms,
                        "source_pts": (
                            min(
                                reference.source_pts_end - 1,
                                reference.source_pts_start
                                + offset_ms * _NANOSECONDS_PER_MILLISECOND,
                            )
                        ),
                        "virtual_path": (
                            f"{VIRTUAL_PATH_PREFIX}/outputs/samples/"
                            f"{_token(event_id)}/{destination.name}/{frame_path.name}"
                        ),
                    }
                )
            if destination.exists() or destination.is_symlink():
                if destination.is_symlink() or not destination.is_dir():
                    raise RuntimeError("sample output path is unsafe")
                shutil.rmtree(destination)
            os.replace(temporary_output, destination)
            temporary_output = None
            return _response(
                event_id,
                available=True,
                reason="ok",
                evidence_id=reference.evidence_id,
                mode=mode,
                clip_duration_ms=duration_ms,
                interval_start_ms=start_ms,
                interval_end_ms=end_ms,
                event_offset_ms=event_offset_ms,
                recommended_fine_interval_ms=recommended,
                frames=frame_rows,
            )
        finally:
            shutil.rmtree(temporary_input, ignore_errors=True)
            if temporary_output is not None and temporary_output.exists():
                shutil.rmtree(temporary_output, ignore_errors=True)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired):
        return _response(
            event_id,
            available=False,
            reason="video sampling failed",
            evidence_id=reference.evidence_id,
            mode=mode,
            clip_duration_ms=duration_ms,
            interval_start_ms=start_ms,
            interval_end_ms=end_ms,
            event_offset_ms=event_offset_ms,
            recommended_fine_interval_ms=recommended,
        )


@tool("sample_video", parse_docstring=True)
def sample_video_tool(
    runtime: Runtime,
    event_id: str,
    evidence_id: str | None = None,
    mode: Literal["coarse", "fine"] = "coarse",
    frames: Annotated[int, Field(ge=1, le=_MAX_FRAMES)] = _DEFAULT_FRAMES,
    interval_start_ms: int | None = None,
    interval_end_ms: int | None = None,
) -> str:
    """从登记的 context.mp4 生成有限数量的粗采样或精采样 JPEG。

    粗采样覆盖整个 clip；精采样只允许在 clip 相对时间窗口内抽帧。返回的虚拟路径
    只能交给 DeerFlow 内置 ``view_image``，不能传入宿主机路径或 ffmpeg 参数。

    Args:
        runtime: DeerFlow 运行时（自动注入）。
        event_id: SQLite 账本中的事件 ID。
        evidence_id: 可选的、该事件已登记的 clip evidence ID；省略时必须只有一个可用 clip。
        mode: ``coarse`` 覆盖全 clip，``fine`` 只采样指定窗口。
        frames: 抽取帧数，范围 1 至 8。
        interval_start_ms: 精采样窗口起点，相对 clip 起点的毫秒数。
        interval_end_ms: 精采样窗口终点，相对 clip 起点的毫秒数。

    Returns:
        JSON 字符串，包含有限的虚拟图片路径和 source PTS；失败时 ``available`` 为 false。
    """
    if not isinstance(mode, str) or mode not in {"coarse", "fine"}:
        return _error(event_id, "mode must be coarse or fine")
    try:
        frame_count = int(frames)
    except (TypeError, ValueError):
        return _error(event_id, "frames must be an integer", mode=mode)
    if not 1 <= frame_count <= _MAX_FRAMES:
        return _error(event_id, "frames must be between 1 and 8", mode=mode)
    if interval_start_ms is not None and not isinstance(interval_start_ms, int):
        return _error(event_id, "interval_start_ms must be an integer", mode=mode)
    if interval_end_ms is not None and not isinstance(interval_end_ms, int):
        return _error(event_id, "interval_end_ms must be an integer", mode=mode)
    return _sample_clip(
        runtime=runtime,
        event_id=event_id,
        evidence_id=evidence_id,
        mode=mode,
        frames=frame_count,
        interval_start_ms=interval_start_ms,
        interval_end_ms=interval_end_ms,
    )
