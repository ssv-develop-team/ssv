"""Redis lifecycle, status and backlog cleanup commands."""

from __future__ import annotations

from argparse import Namespace

from ..config import load_runtime_config
from ..context import ProjectContext
from ..output import CliError, header, info
from ..services.compose import start_redis, stop_redis
from ..services.redis_admin import (
    RedisAdmin,
    RedisCleanupError,
    RedisConnection,
    RedisError,
)


def _settings(context: ProjectContext, args: Namespace):
    return load_runtime_config(
        context,
        path=getattr(args, "config", None),
        host=getattr(args, "host", None),
        port=getattr(args, "port", None),
        db=getattr(args, "db", None),
        stream=getattr(args, "stream_key", None),
        group=getattr(args, "group", None),
    ).redis


def start(context: ProjectContext, args: Namespace) -> int:
    header("启动 Docker Redis")
    return start_redis(context, _settings(context, args))


def stop(context: ProjectContext, _args: Namespace) -> int:
    header("停止 Docker Redis")
    return stop_redis(context)


def status(context: ProjectContext, args: Namespace) -> int:
    settings = _settings(context, args)
    header("Redis 状态")
    try:
        with RedisConnection(settings) as connection:
            admin = RedisAdmin(
                connection,
                stream_key=settings.stream_key,
                consumer_group=settings.consumer_group,
            )
            admin.ping()
            state = admin.status()
    except RedisError as exc:
        raise CliError(str(exc)) from exc
    info(f"地址: {settings.host}:{settings.port}/{settings.db}")
    info(f"stream: {settings.stream_key} length={state.stream_length}")
    info(f"group: {state.group} pending={state.pending}")
    return 0


def clean(context: ProjectContext, args: Namespace) -> int:
    if args.include_stream and not args.yes:
        raise CliError("--stream 会删除 stream 历史数据，请同时提供 --yes")
    settings = _settings(context, args)
    header("清理 Redis 积压")
    try:
        with RedisConnection(settings) as connection:
            admin = RedisAdmin(
                connection,
                stream_key=settings.stream_key,
                consumer_group=settings.consumer_group,
            )
            result = admin.clean(
                include_stream=args.include_stream,
                dry_run=args.dry_run,
            )
    except RedisCleanupError as exc:
        result = exc.result
        info(
            f"失败但已保留统计: stream={settings.stream_key} group={settings.consumer_group} "
            f"pending_before={result.pending_before} acknowledged={result.acknowledged} "
            f"trimmed={result.trimmed} error={exc}"
        )
        raise CliError(str(exc)) from exc
    except RedisError as exc:
        raise CliError(str(exc)) from exc
    mode = "预览" if result.dry_run else "完成"
    info(
        f"{mode}: stream={settings.stream_key} group={settings.consumer_group} "
        f"pending_before={result.pending_before} acknowledged={result.acknowledged} "
        f"trimmed={result.trimmed}"
    )
    return 0
