"""Redis-py connection adapter and safe Redis Stream administration operations."""

from __future__ import annotations

import importlib
from dataclasses import dataclass
from typing import Any, Protocol, Self

from ..config import RedisSettings

RedisValue = Any


class RedisError(RuntimeError):
    """Base class for connection and server errors."""


class RedisCommandError(RedisError):
    """Redis returned a command error."""


def _load_redis_module() -> Any:
    """仅在 Redis 命令实际连接时加载 redis-py。"""

    try:
        return importlib.import_module("redis")
    except ModuleNotFoundError as exc:
        raise RedisError(
            "Redis 管理命令需要 redis-py；请安装项目运行依赖或执行 pip install redis"
        ) from exc


class RedisConnectionProtocol(Protocol):
    def execute(self, *args: str | int) -> RedisValue:
        ...

    def close(self) -> None:
        ...


class RedisConnection:
    """供 ``RedisAdmin`` 使用的同步 redis-py 适配器。"""

    def __init__(self, settings: RedisSettings, timeout: float = 3.0) -> None:
        self._settings = settings
        self._timeout = timeout
        self._client: Any | None = None
        self._redis_module: Any | None = None

    def __enter__(self) -> Self:
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
        self.close()

    def _client_or_create(self) -> Any:
        if self._client is None:
            self._redis_module = _load_redis_module()
            self._client = self._redis_module.Redis(
                host=self._settings.host,
                port=self._settings.port,
                db=self._settings.db,
                password=self._settings.password,
                socket_timeout=self._timeout,
                socket_connect_timeout=self._timeout,
                decode_responses=True,
            )
        return self._client

    def close(self) -> None:
        if self._client is not None:
            self._client.close()
            self._client = None
        self._redis_module = None

    def execute(self, *args: str | int) -> RedisValue:
        client = self._client_or_create()
        assert self._redis_module is not None
        try:
            options: dict[str, bool] = {}
            if args and str(args[0]).upper() == "XPENDING" and len(args) > 3:
                options["parse_detail"] = True
            return client.execute_command(*args, **options)
        except self._redis_module.exceptions.ResponseError as exc:
            raise RedisCommandError(str(exc)) from exc
        except self._redis_module.exceptions.RedisError as exc:
            self.close()
            raise RedisError(f"Redis 命令失败: {args[0] if args else '<empty>'}: {exc}") from exc
        except OSError as exc:
            self.close()
            raise RedisError(f"Redis 命令失败: {args[0] if args else '<empty>'}: {exc}") from exc


def _as_int(value: object, field: str) -> int:
    try:
        return int(value or 0)
    except (TypeError, ValueError) as exc:
        raise RedisError(f"invalid Redis {field}: {value!r}") from exc


def _as_text(value: object) -> str:
    return value.decode("utf-8") if isinstance(value, bytes) else str(value)


@dataclass(frozen=True)
class RedisStatus:
    stream_length: int
    pending: int
    group: str


@dataclass(frozen=True)
class RedisCleanupResult:
    pending_before: int
    acknowledged: int
    trimmed: int
    dry_run: bool


@dataclass
class _CleanupProgress:
    pending_before: int
    acknowledged: int = 0
    trimmed: int = 0
    dry_run: bool = False

    def result(self) -> RedisCleanupResult:
        return RedisCleanupResult(
            pending_before=self.pending_before,
            acknowledged=self.acknowledged,
            trimmed=self.trimmed,
            dry_run=self.dry_run,
        )


class RedisCleanupError(RedisError):
    """A cleanup command failed after some operations may have completed."""

    def __init__(self, message: str, *, result: RedisCleanupResult) -> None:
        super().__init__(message)
        self.result = result


class RedisAdmin:
    """Operations scoped to one configured stream and consumer group."""

    def __init__(
        self,
        connection: RedisConnectionProtocol,
        *,
        stream_key: str,
        consumer_group: str,
        batch_size: int = 500,
    ) -> None:
        if batch_size < 1:
            raise ValueError("batch_size must be positive")
        self._connection = connection
        self.stream_key = stream_key
        self.consumer_group = consumer_group
        self.batch_size = batch_size

    def ping(self) -> str:
        return _as_text(self._connection.execute("PING"))

    def stream_length(self) -> int:
        return _as_int(self._connection.execute("XLEN", self.stream_key), "stream length")

    def pending_total(self) -> int:
        response = self._connection.execute("XPENDING", self.stream_key, self.consumer_group)
        if isinstance(response, dict):
            if "pending" not in response:
                raise RedisError("unexpected XPENDING summary response")
            return _as_int(response["pending"], "pending total")
        if not isinstance(response, (list, tuple)) or not response:
            raise RedisError("unexpected XPENDING summary response")
        return _as_int(response[0], "pending total")

    def status(self) -> RedisStatus:
        return RedisStatus(
            stream_length=self.stream_length(),
            pending=self.pending_total(),
            group=self.consumer_group,
        )

    def _pending_rows(self, start: str, consumer: str | None = None) -> list[tuple[str, str]]:
        args: list[str | int] = [
            "XPENDING",
            self.stream_key,
            self.consumer_group,
            start,
            "+",
            self.batch_size,
        ]
        if consumer:
            args.append(consumer)
        response = self._connection.execute(*args)
        if response is None:
            return []
        if not isinstance(response, (list, tuple)):
            raise RedisError("unexpected XPENDING range response")
        rows: list[tuple[str, str]] = []
        for row in response:
            if isinstance(row, dict):
                message_id = row.get("message_id")
                consumer = row.get("consumer")
                if message_id is None or consumer is None:
                    raise RedisError("unexpected XPENDING row")
                rows.append((_as_text(message_id), _as_text(consumer)))
                continue
            if not isinstance(row, (list, tuple)) or len(row) < 2:
                raise RedisError("unexpected XPENDING row")
            rows.append((_as_text(row[0]), _as_text(row[1])))
        return rows

    def clear_pending(
        self,
        *,
        dry_run: bool = False,
        pending_before: int | None = None,
    ) -> int:
        if pending_before is None:
            pending_before = self.pending_total()
        progress = _CleanupProgress(
            pending_before=pending_before,
            dry_run=dry_run,
        )
        self._clear_pending(progress)
        return progress.acknowledged

    def _clear_pending(self, progress: _CleanupProgress) -> None:
        if progress.dry_run or progress.pending_before == 0:
            return
        start = "-"
        while True:
            rows = self._pending_rows(start)
            if not rows:
                break
            ids = [row[0] for row in rows]
            response = self._connection.execute("XACK", self.stream_key, self.consumer_group, *ids)
            progress.acknowledged += _as_int(response, "acknowledged count")
            if len(rows) < self.batch_size:
                break
            start = f"({rows[-1][0]}"

    def trim_stream(self, *, dry_run: bool = False) -> int:
        if dry_run:
            return 0
        return _as_int(
            self._connection.execute("XTRIM", self.stream_key, "MAXLEN", "=", 0),
            "trimmed count",
        )

    def clean(self, *, include_stream: bool = False, dry_run: bool = False) -> RedisCleanupResult:
        pending_before = self.pending_total()
        progress = _CleanupProgress(
            pending_before=pending_before,
            dry_run=dry_run,
        )
        try:
            self._clear_pending(progress)
            if include_stream:
                progress.trimmed = self.trim_stream(dry_run=dry_run)
        except RedisError as exc:
            raise RedisCleanupError(str(exc), result=progress.result()) from exc
        return progress.result()


def redis_admin_from_settings(
    settings: RedisSettings,
    *,
    batch_size: int = 500,
) -> tuple[RedisConnection, RedisAdmin]:
    connection = RedisConnection(settings)
    return connection, RedisAdmin(
        connection,
        stream_key=settings.stream_key,
        consumer_group=settings.consumer_group,
        batch_size=batch_size,
    )
