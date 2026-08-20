"""Docker Compose adapter for the local Redis development service."""

from __future__ import annotations

import json
import time

from ..config import RedisSettings
from ..context import ProjectContext
from ..output import CliError, info
from ..process import require_command, run_command
from .redis_admin import RedisConnection, RedisError


def _compose_argv(context: ProjectContext, *args: str) -> list[str]:
    return ["docker", "compose", "-f", str(context.compose_file), *args]


def _compose(
    context: ProjectContext,
    *args: str,
    capture_output: bool = False,
    environment: dict[str, str] | None = None,
):
    require_command("docker", "请安装 Docker 和 Docker Compose plugin")
    result = run_command(
        context,
        _compose_argv(context, *args),
        capture_output=capture_output,
        environment=environment,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() if capture_output else ""
        raise CliError(f"docker compose 命令失败: {' '.join(args)}{': ' + detail if detail else ''}")
    return result


def _is_running(context: ProjectContext) -> bool:
    result = run_command(
        context,
        _compose_argv(context, "ps", "--format", "json"),
        capture_output=True,
    )
    if result.returncode != 0:
        return False
    output = result.stdout.strip()
    if not output:
        return False
    records: list[object] = []
    try:
        parsed = json.loads(output)
        records = parsed if isinstance(parsed, list) else [parsed]
    except json.JSONDecodeError:
        for line in output.splitlines():
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    for record in records:
        if isinstance(record, dict):
            state = str(record.get("State", record.get("state", ""))).lower()
            status = str(record.get("Status", record.get("status", ""))).lower()
            if state == "running" or status.startswith("up") or "healthy" in status:
                return True
    return False


def start_redis(context: ProjectContext, settings: RedisSettings) -> int:
    compose_environment = context.child_environment(REDIS_PORT=str(settings.port))
    _compose(context, "up", "-d", environment=compose_environment)
    info("等待 Redis 就绪...")
    last_error: Exception | None = None
    for _ in range(15):
        try:
            with RedisConnection(settings) as connection:
                connection.execute("PING")
            info("Redis 已就绪")
            return 0
        except RedisError as exc:  # connection can race container startup
            last_error = exc
            time.sleep(1)
    raise CliError(f"Redis 启动超时: {last_error}")


def stop_redis(context: ProjectContext) -> int:
    if not _is_running(context):
        info("Redis 未在运行")
        return 0
    _compose(context, "down")
    info("Redis 已停止")
    return 0
