"""从 SSV GStreamer 短时缓存生成事件上下文证据。"""

from __future__ import annotations

import errno
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal, Sequence

from ssv_agent.config import EvidenceCacheConfig, RecordingEvidenceConfig
from ssv_agent.evidence_provider import (
    CommandRunner,
    EvidenceArtifact,
    EvidenceExtractionError,
)
from ssv_agent.event_store import EventCase


_CACHE_SCHEMA = "ssv.evidence-segment.v1"
_SEGMENT_NAME = re.compile(r"^segment-[0-9]+\.mp4$")
_COMMAND_TIMEOUT_SECONDS = 30
_FRAME_OFFSETS_NS: tuple[int, ...] = (
    -1_000_000_000,
    0,
    1_000_000_000,
)
_EXPECTED_ARTIFACTS: tuple[tuple[Literal["clip", "frame"], str, str], ...] = (
    ("clip", "context.mp4", "video/mp4"),
    ("frame", "frame-01.jpg", "image/jpeg"),
    ("frame", "frame-02.jpg", "image/jpeg"),
    ("frame", "frame-03.jpg", "image/jpeg"),
)


@dataclass(frozen=True)
class _CacheSegment:
    path: Path
    source_pts_start: int
    source_pts_end: int


class SsvCacheEvidenceExtractor:
    """按 source、generation 和 source PTS 读取 finalized SSV 缓存分段。"""

    def __init__(
        self,
        config: RecordingEvidenceConfig,
        evidence_cache: EvidenceCacheConfig,
        evidence_roots: Sequence[str],
        *,
        runner: CommandRunner | None = None,
    ) -> None:
        self._config = config
        self._evidence_cache = evidence_cache
        self._evidence_roots = tuple(evidence_roots)
        self._runner = runner or _SubprocessRunner()

    def extract(self, case: EventCase) -> tuple[EvidenceArtifact, ...]:
        """生成完整证据集；缓存窗口未完整 finalized 时要求重试。"""
        event_id = _safe_event_id(case.event_id)
        source_pts, generation = _event_anchor(case)
        window_start, window_end = _requested_window(
            case,
            source_pts,
            before_ms=self._config.clip_before_ms,
            after_ms=self._config.clip_after_ms,
        )
        output_root = self._output_root()
        derived_parent = self._prepare_derived_parent(output_root)
        self._cleanup_recovery_temporary(derived_parent, event_id, output_root)
        existing = self._existing_destination(
            derived_parent / event_id,
            case,
            source_pts,
            generation,
            output_root,
        )
        if existing is not None:
            return existing

        cache_root = self._cache_root(output_root)
        source_dir = self._source_directory(cache_root, case.source)
        segments = self._select_segments(
            source_dir,
            case.source,
            generation,
            window_start,
            window_end,
        )

        temporary_dir = Path(
            tempfile.mkdtemp(dir=derived_parent, prefix=f".{event_id}.")
        )
        input_dir: Path | None = None
        try:
            input_dir = Path(
                tempfile.mkdtemp(dir=derived_parent, prefix=f".{event_id}.inputs.")
            )
            pinned_segments = _pin_segments(input_dir, segments)
            artifacts = self._extract_to_temporary_dir(
                temporary_dir,
                input_dir,
                pinned_segments,
                window_start,
                window_end,
                source_pts,
                generation,
            )
            self._write_manifest(
                temporary_dir,
                case.source,
                generation,
                window_start,
                window_end,
                pinned_segments,
                artifacts,
            )
            destination = derived_parent / event_id
            try:
                os.replace(temporary_dir, destination)
                _fsync_directory(derived_parent)
            except OSError as exc:
                raise EvidenceExtractionError(
                    "derived output could not be published",
                    retryable=False,
                ) from exc
            return tuple(
                EvidenceArtifact(
                    kind=artifact.kind,
                    path=destination / artifact.path.name,
                    sha256=artifact.sha256,
                    mime_type=artifact.mime_type,
                    size=artifact.size,
                    mtime=(destination / artifact.path.name).stat().st_mtime,
                    source_pts_start=window_start,
                    source_pts_end=window_end,
                    stream_generation=generation,
                )
                for artifact in artifacts
            )
        except EvidenceExtractionError:
            raise
        except OSError as exc:
            raise EvidenceExtractionError("media extraction failed", retryable=True) from exc
        finally:
            if temporary_dir.exists():
                shutil.rmtree(temporary_dir, ignore_errors=True)
            if input_dir is not None and input_dir.exists():
                shutil.rmtree(input_dir, ignore_errors=True)

    def _output_root(self) -> Path:
        if not self._evidence_roots:
            raise EvidenceExtractionError(
                "recording evidence root is unavailable",
                retryable=False,
            )
        root = Path(self._evidence_roots[0])
        return _validated_directory(
            root,
            error_message="recording evidence root is unavailable",
            retryable=False,
        )

    def _cache_root(self, output_root: Path) -> Path:
        configured = Path(self._evidence_cache.directory)
        if not configured.is_absolute():
            raise EvidenceExtractionError(
                "evidence cache directory is unsafe",
                retryable=False,
            )
        try:
            resolved = configured.resolve(strict=False)
        except (OSError, RuntimeError) as exc:
            raise EvidenceExtractionError(
                "evidence cache directory is unsafe",
                retryable=False,
            ) from exc

        trusted_roots = [output_root]
        for raw_root in self._evidence_roots[1:]:
            try:
                trusted_roots.append(
                    _validated_directory(
                        Path(raw_root),
                        error_message="recording evidence root is unavailable",
                        retryable=False,
                    )
                )
            except EvidenceExtractionError:
                continue
        if not any(_is_within(resolved, root) for root in trusted_roots):
            raise EvidenceExtractionError(
                "evidence cache directory is outside evidence roots",
                retryable=False,
            )

        try:
            details = configured.lstat()
        except FileNotFoundError:
            raise EvidenceExtractionError(
                "evidence cache directory is unavailable",
                retryable=True,
            )
        except OSError as exc:
            raise EvidenceExtractionError(
                "evidence cache directory is unavailable",
                retryable=True,
            ) from exc
        if stat.S_ISLNK(details.st_mode) or not stat.S_ISDIR(details.st_mode):
            raise EvidenceExtractionError(
                "evidence cache directory is unsafe",
                retryable=False,
            )
        return _validated_directory(
            configured,
            error_message="evidence cache directory is unavailable",
            retryable=True,
        )

    @staticmethod
    def _source_directory(cache_root: Path, source: str) -> Path:
        path = cache_root / _encode_path_component(source)
        try:
            details = path.lstat()
        except FileNotFoundError as exc:
            raise EvidenceExtractionError(
                "evidence cache source directory is unavailable",
                retryable=True,
            ) from exc
        except OSError as exc:
            raise EvidenceExtractionError(
                "evidence cache source directory is unavailable",
                retryable=True,
            ) from exc
        if stat.S_ISLNK(details.st_mode) or not stat.S_ISDIR(details.st_mode):
            raise EvidenceExtractionError(
                "evidence cache source directory is unsafe",
                retryable=False,
            )
        return _validated_directory(
            path,
            error_message="evidence cache source directory is unavailable",
            retryable=True,
        )

    def _select_segments(
        self,
        source_dir: Path,
        source: str,
        generation: int,
        window_start: int,
        window_end: int,
    ) -> tuple[_CacheSegment, ...]:
        try:
            entries = sorted(source_dir.iterdir(), key=lambda entry: entry.name)
        except OSError as exc:
            raise EvidenceExtractionError(
                "evidence cache window is unavailable",
                retryable=True,
            ) from exc

        segments: list[_CacheSegment] = []
        for entry in entries:
            if _SEGMENT_NAME.fullmatch(entry.name) is None:
                continue
            segment = self._read_finalized_segment(
                entry,
                source_dir,
                source,
                generation,
            )
            if segment is not None:
                segments.append(segment)

        cursor = window_start
        selected: list[_CacheSegment] = []
        for segment in sorted(segments, key=lambda item: item.source_pts_start):
            if segment.source_pts_end <= cursor:
                continue
            if segment.source_pts_start > cursor:
                break
            selected.append(segment)
            cursor = max(cursor, segment.source_pts_end)
            if cursor >= window_end:
                return tuple(selected)
        raise EvidenceExtractionError(
            "evidence cache window is unavailable",
            retryable=True,
        )

    @staticmethod
    def _read_finalized_segment(
        media_path: Path,
        source_dir: Path,
        source: str,
        generation: int,
    ) -> _CacheSegment | None:
        media = _validated_cache_file(media_path, source_dir)
        if media is None:
            return None
        sidecar_path = source_dir / f"{media_path.stem}.json"
        sidecar = _validated_cache_file(sidecar_path, source_dir)
        if sidecar is None:
            return None
        try:
            with sidecar.open("r", encoding="utf-8") as handle:
                payload = json.load(handle)
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise EvidenceExtractionError(
                "evidence cache sidecar is invalid",
                retryable=False,
            ) from exc
        if not isinstance(payload, dict):
            raise EvidenceExtractionError(
                "evidence cache sidecar is invalid",
                retryable=False,
            )
        if payload.get("schema") != _CACHE_SCHEMA or payload.get("source") != source:
            raise EvidenceExtractionError(
                "evidence cache sidecar does not match its source",
                retryable=False,
            )
        if payload.get("media") != media_path.name:
            raise EvidenceExtractionError(
                "evidence cache sidecar does not match its media",
                retryable=False,
            )
        if payload.get("finalized") is not True:
            return None

        sidecar_generation = _integer_field(payload, "stream_generation")
        start = _integer_field(payload, "source_pts_start")
        end = _integer_field(payload, "source_pts_end")
        if sidecar_generation is None or start is None or end is None:
            return None
        if sidecar_generation != generation:
            return None
        if start < 0 or end <= start:
            raise EvidenceExtractionError(
                "evidence cache sidecar has an invalid PTS interval",
                retryable=False,
            )
        return _CacheSegment(media, start, end)

    def _existing_destination(
        self,
        destination: Path,
        case: EventCase,
        source_pts: int,
        generation: int,
        output_root: Path,
    ) -> tuple[EvidenceArtifact, ...] | None:
        try:
            details = destination.lstat()
        except FileNotFoundError:
            return None
        except OSError as exc:
            raise EvidenceExtractionError(
                "derived output is unavailable",
                retryable=False,
            ) from exc
        destination_resolved = _validated_directory(
            destination,
            error_message="derived output is unsafe",
            retryable=False,
        )
        if stat.S_ISLNK(details.st_mode) or not _is_within(destination_resolved, output_root):
            raise EvidenceExtractionError("derived output is unsafe", retryable=False)

        manifest = destination / "manifest.json"
        manifest_path = _validated_output_file(manifest, destination_resolved)
        try:
            with manifest_path.open("r", encoding="utf-8") as handle:
                payload = json.load(handle)
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise EvidenceExtractionError("derived manifest is invalid", retryable=False) from exc
        if not isinstance(payload, dict):
            raise EvidenceExtractionError("derived manifest is invalid", retryable=False)
        try:
            window_start, window_end = _requested_window(
                case,
                source_pts,
                before_ms=self._config.clip_before_ms,
                after_ms=self._config.clip_after_ms,
            )
        except EvidenceExtractionError:
            raise
        if (
            payload.get("time_basis") != "source_pts_exact"
            or payload.get("source") != case.source
            or payload.get("stream_generation") != generation
            or payload.get("requested_source_pts") != [window_start, window_end]
        ):
            raise EvidenceExtractionError("derived manifest is invalid", retryable=False)

        entries = payload.get("artifacts")
        if not isinstance(entries, list) or len(entries) != len(_EXPECTED_ARTIFACTS):
            raise EvidenceExtractionError("derived manifest is invalid", retryable=False)
        by_name = {entry.get("name"): entry for entry in entries if isinstance(entry, dict)}
        if set(by_name) != {name for _, name, _ in _EXPECTED_ARTIFACTS}:
            raise EvidenceExtractionError("derived manifest is invalid", retryable=False)

        artifacts: list[EvidenceArtifact] = []
        for kind, name, mime_type in _EXPECTED_ARTIFACTS:
            path = _validated_output_file(destination / name, destination_resolved)
            artifact = _artifact(
                kind,
                path,
                mime_type,
                source_pts_start=window_start,
                source_pts_end=window_end,
                stream_generation=generation,
            )
            entry = by_name[name]
            if (
                entry.get("kind") != kind
                or entry.get("mime_type") != mime_type
                or entry.get("sha256") != artifact.sha256
                or entry.get("size") != artifact.size
                or entry.get("mtime") != artifact.mtime
            ):
                raise EvidenceExtractionError("derived manifest is invalid", retryable=False)
            artifacts.append(artifact)
        return tuple(artifacts)

    @staticmethod
    def _prepare_derived_parent(output_root: Path) -> Path:
        derived_parent = output_root / "derived"
        try:
            derived_parent.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            raise EvidenceExtractionError(
                "derived output directory is unavailable",
                retryable=False,
            ) from exc
        return _validated_directory(
            derived_parent,
            error_message="derived output directory is unavailable",
            retryable=False,
        )

    @staticmethod
    def _cleanup_recovery_temporary(
        derived_parent: Path,
        event_id: str,
        output_root: Path,
    ) -> None:
        prefix = f".{event_id}."
        try:
            entries = list(derived_parent.iterdir())
        except OSError as exc:
            raise EvidenceExtractionError(
                "derived output is unavailable",
                retryable=False,
            ) from exc
        for entry in entries:
            if not entry.name.startswith(prefix):
                continue
            try:
                details = entry.lstat()
            except OSError as exc:
                raise EvidenceExtractionError(
                    "derived output cleanup failed",
                    retryable=False,
                ) from exc
            if (
                stat.S_ISLNK(details.st_mode)
                or not stat.S_ISDIR(details.st_mode)
            ):
                raise EvidenceExtractionError("derived output is unsafe", retryable=False)
            try:
                resolved = entry.resolve(strict=True)
            except (OSError, RuntimeError) as exc:
                raise EvidenceExtractionError(
                    "derived output cleanup failed",
                    retryable=False,
                ) from exc
            if not _is_within(resolved, output_root):
                raise EvidenceExtractionError("derived output is unsafe", retryable=False)
            try:
                shutil.rmtree(entry)
            except OSError as exc:
                raise EvidenceExtractionError(
                    "derived output cleanup failed",
                    retryable=False,
                ) from exc

    def _extract_to_temporary_dir(
        self,
        temporary_dir: Path,
        input_dir: Path,
        segments: tuple[_CacheSegment, ...],
        window_start: int,
        window_end: int,
        source_pts: int,
        generation: int,
    ) -> tuple[EvidenceArtifact, ...]:
        concat_list = input_dir / "segments.txt"
        _write_concat_list(concat_list, segments)
        self._run_ffmpeg(
            concat_list,
            _seconds(window_start - segments[0].source_pts_start),
            _seconds(window_end - window_start),
            temporary_dir / "context.mp4",
            ["-c:v", "libx264", "-an"],
        )
        artifacts: list[EvidenceArtifact] = [
            _artifact(
                "clip",
                temporary_dir / "context.mp4",
                "video/mp4",
                source_pts_start=window_start,
                source_pts_end=window_end,
                stream_generation=generation,
            )
        ]
        for index, offset_ns in enumerate(_FRAME_OFFSETS_NS, start=1):
            frame_path = temporary_dir / f"frame-{index:02d}.jpg"
            self._run_ffmpeg(
                concat_list,
                _seconds(source_pts + offset_ns - segments[0].source_pts_start),
                "",
                frame_path,
                ["-frames:v", "1", "-q:v", "2"],
            )
            artifacts.append(
                _artifact(
                    "frame",
                    frame_path,
                    "image/jpeg",
                    source_pts_start=window_start,
                    source_pts_end=window_end,
                    stream_generation=generation,
                )
            )
        return tuple(artifacts)

    def _run_ffmpeg(
        self,
        concat_list: Path,
        seek_seconds: str,
        duration_seconds: str,
        output: Path,
        output_args: list[str],
    ) -> None:
        args = [
            "ffmpeg",
            "-y",
            "-f",
            "concat",
            "-safe",
            "0",
            "-i",
            str(concat_list),
            "-ss",
            seek_seconds,
        ]
        if duration_seconds:
            args.extend(["-t", duration_seconds])
        args.extend([*output_args, str(output)])
        try:
            completed = self._runner.run(
                args,
                check=False,
                capture_output=True,
                text=True,
                timeout=_COMMAND_TIMEOUT_SECONDS,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise EvidenceExtractionError("media extraction failed", retryable=True) from exc
        if completed.returncode != 0:
            raise EvidenceExtractionError("media extraction failed", retryable=True)
        _fsync_file(output)

    @staticmethod
    def _write_manifest(
        temporary_dir: Path,
        source: str,
        generation: int,
        window_start: int,
        window_end: int,
        segments: tuple[_CacheSegment, ...],
        artifacts: tuple[EvidenceArtifact, ...],
    ) -> None:
        payload = {
            "time_basis": "source_pts_exact",
            "source": source,
            "stream_generation": generation,
            "requested_source_pts": [window_start, window_end],
            "actual_source_pts": [
                segments[0].source_pts_start,
                segments[-1].source_pts_end,
            ],
            "inputs": [segment.path.name for segment in segments],
            "artifacts": [
                {
                    "name": artifact.path.name,
                    "kind": artifact.kind,
                    "sha256": artifact.sha256,
                    "mime_type": artifact.mime_type,
                    "size": artifact.size,
                    "mtime": artifact.mtime,
                }
                for artifact in artifacts
            ],
        }
        manifest = temporary_dir / "manifest.json"
        with manifest.open("w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=True, sort_keys=True)
            handle.flush()
            os.fsync(handle.fileno())


class _SubprocessRunner:
    def run(self, args: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        return subprocess.run(args, **kwargs)


def _event_anchor(case: EventCase) -> tuple[int, int]:
    if case.source_pts is None or case.stream_generation is None:
        raise EvidenceExtractionError(
            "event has no exact source PTS or stream generation",
            retryable=False,
        )
    if case.source_pts < 0 or case.stream_generation < 0:
        raise EvidenceExtractionError(
            "event has an invalid source timing anchor",
            retryable=False,
        )
    return case.source_pts, case.stream_generation


def _requested_window(
    case: EventCase,
    source_pts: int,
    *,
    before_ms: int,
    after_ms: int,
) -> tuple[int, int]:
    """优先使用 closed episode 冻结的窗口，兼容历史单事件案件。"""
    start = case.evidence_window_start
    end = case.evidence_window_end
    if start is None and end is None:
        return (
            max(0, source_pts - before_ms * 1_000_000),
            source_pts + after_ms * 1_000_000,
        )
    if start is None or end is None:
        raise EvidenceExtractionError(
            "event evidence window is incomplete",
            retryable=False,
        )
    if start < 0 or end <= start:
        raise EvidenceExtractionError(
            "event evidence window is invalid",
            retryable=False,
        )
    return start, end


def _encode_path_component(value: str) -> str:
    hex_digits = "0123456789ABCDEF"
    encoded: list[str] = []
    for byte in value.encode("utf-8"):
        character = chr(byte)
        if character.isascii() and (
            character.isalnum() or character in {"-", "_", "."}
        ):
            encoded.append(character)
        else:
            encoded.extend(("~", hex_digits[byte >> 4], hex_digits[byte & 0x0F]))
    return "".join(encoded) or "source"


def _safe_event_id(event_id: str) -> str:
    if (
        not event_id
        or event_id in {".", ".."}
        or "/" in event_id
        or "\\" in event_id
        or "\x00" in event_id
    ):
        raise EvidenceExtractionError("event identifier is unsafe", retryable=False)
    return event_id


def _is_within(candidate: Path, parent: Path) -> bool:
    try:
        candidate.relative_to(parent)
    except ValueError:
        return False
    return True


def _validated_directory(
    path: Path,
    *,
    error_message: str,
    retryable: bool,
) -> Path:
    try:
        details = path.lstat()
        resolved = path.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise EvidenceExtractionError(error_message, retryable=retryable) from exc
    if stat.S_ISLNK(details.st_mode) or not stat.S_ISDIR(details.st_mode):
        raise EvidenceExtractionError(error_message, retryable=retryable)
    return resolved


def _validated_cache_file(path: Path, parent: Path) -> Path | None:
    try:
        details = path.lstat()
    except FileNotFoundError:
        return None
    except OSError as exc:
        raise EvidenceExtractionError(
            "evidence cache file is unavailable",
            retryable=True,
        ) from exc
    if stat.S_ISLNK(details.st_mode) or not stat.S_ISREG(details.st_mode):
        raise EvidenceExtractionError("evidence cache file is unsafe", retryable=False)
    try:
        resolved = path.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise EvidenceExtractionError("evidence cache file is unavailable", retryable=True) from exc
    if not _is_within(resolved, parent):
        raise EvidenceExtractionError("evidence cache file is unsafe", retryable=False)
    return resolved


def _validated_output_file(path: Path, parent: Path) -> Path:
    resolved = _validated_cache_file(path, parent)
    if resolved is None:
        raise EvidenceExtractionError("derived output is incomplete", retryable=False)
    return resolved


def _integer_field(payload: dict[str, object], name: str) -> int | None:
    value = payload.get(name)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise EvidenceExtractionError(
            f"evidence cache sidecar field {name} is invalid",
            retryable=False,
        )
    return value


def _seconds(nanoseconds: int) -> str:
    return f"{nanoseconds / 1_000_000_000:.9f}"


def _pin_segments(
    temporary_dir: Path,
    segments: tuple[_CacheSegment, ...],
) -> tuple[_CacheSegment, ...]:
    """获取窗口输入，隔离后续缓存清理对 ffmpeg 的影响。"""
    return tuple(
        _pin_segment(temporary_dir, segment)
        for segment in segments
    )


def _pin_segment(
    temporary_dir: Path,
    segment: _CacheSegment,
) -> _CacheSegment:
    """优先用硬链接持有 inode，跨文件系统时复制 finalized 文件。"""
    target = temporary_dir / segment.path.name
    try:
        os.link(segment.path, target, follow_symlinks=False)
    except OSError as exc:
        if exc.errno != errno.EXDEV:
            raise EvidenceExtractionError(
                "evidence cache segment disappeared during acquisition",
                retryable=True,
            ) from exc
        try:
            shutil.copyfile(segment.path, target, follow_symlinks=False)
            _fsync_file(target)
        except (OSError, shutil.Error) as copy_error:
            raise EvidenceExtractionError(
                "evidence cache segment disappeared during acquisition",
                retryable=True,
            ) from copy_error

    try:
        details = target.lstat()
    except OSError as exc:
        raise EvidenceExtractionError(
            "evidence cache segment acquisition failed",
            retryable=True,
        ) from exc
    if stat.S_ISLNK(details.st_mode) or not stat.S_ISREG(details.st_mode):
        raise EvidenceExtractionError(
            "evidence cache segment acquisition produced an unsafe file",
            retryable=False,
        )
    return _CacheSegment(
        path=target,
        source_pts_start=segment.source_pts_start,
        source_pts_end=segment.source_pts_end,
    )


def _write_concat_list(path: Path, segments: tuple[_CacheSegment, ...]) -> None:
    with path.open("w", encoding="utf-8") as handle:
        for segment in segments:
            handle.write("file '" + str(segment.path).replace("'", "'\\''") + "'\n")
        handle.flush()
        os.fsync(handle.fileno())


def _artifact(
    kind: Literal["clip", "frame"],
    path: Path,
    mime_type: str,
    *,
    source_pts_start: int,
    source_pts_end: int,
    stream_generation: int,
) -> EvidenceArtifact:
    try:
        data = path.read_bytes()
        details = path.stat()
    except OSError as exc:
        raise EvidenceExtractionError("media extraction failed", retryable=True) from exc
    return EvidenceArtifact(
        kind=kind,
        path=path,
        sha256=hashlib.sha256(data).hexdigest(),
        mime_type=mime_type,
        size=details.st_size,
        mtime=details.st_mtime,
        source_pts_start=source_pts_start,
        source_pts_end=source_pts_end,
        stream_generation=stream_generation,
    )


def _fsync_file(path: Path) -> None:
    try:
        with path.open("rb") as handle:
            os.fsync(handle.fileno())
    except OSError as exc:
        raise EvidenceExtractionError("media extraction failed", retryable=True) from exc


def _fsync_directory(path: Path) -> None:
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    except OSError as exc:
        raise EvidenceExtractionError(
            "derived output could not be published",
            retryable=False,
        ) from exc
    try:
        os.fsync(descriptor)
    except OSError as exc:
        raise EvidenceExtractionError(
            "derived output could not be published",
            retryable=False,
        ) from exc
    finally:
        os.close(descriptor)
