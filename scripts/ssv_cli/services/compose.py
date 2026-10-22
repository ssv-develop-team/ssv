"""Docker Compose adapter for local development services."""

from __future__ import annotations

import json
import time
from urllib.parse import urlsplit
from urllib.request import urlopen

from ..config import RedisSettings
from ..context import ProjectContext
from ..output import CliError, info
from ..process import require_command, run_command
from .redis_admin import RedisConnection, RedisError

_LOCAL_QDRANT_HOSTS = {"localhost", "127.0.0.1", "::1"}
_LOCAL_MINIO_HOSTS = {"localhost", "127.0.0.1", "::1"}
_DEFAULT_MINIO_IMAGE = (
    "ghcr.io/ssv-develop-team/ssv-minio:RELEASE.2025-04-22T22-12-26Z"
)


def _compose_argv(context: ProjectContext, *args: str) -> list[str]:
    argv = ["docker", "compose"]
    env_file = context.root / "docker" / ".env"
    if env_file.is_file():
        argv.extend(["--env-file", str(env_file)])
    argv.extend(["-f", str(context.compose_file), *args])
    return argv


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


def _local_qdrant_port(qdrant_url: str | None) -> int:
    if not qdrant_url:
        return 6333
    try:
        parsed = urlsplit(qdrant_url)
        if parsed.hostname not in _LOCAL_QDRANT_HOSTS:
            return 6333
        return parsed.port or (443 if parsed.scheme == "https" else 80)
    except ValueError as exc:
        raise CliError(f"Qdrant URL 无效: {qdrant_url}") from exc


def _qdrant_ready(port: int) -> bool:
    try:
        with urlopen(f"http://127.0.0.1:{port}/readyz", timeout=1) as response:
            return 200 <= response.status < 300
    except OSError:
        return False


def _image_present(context: ProjectContext, image: str) -> bool:
    result = run_command(
        context,
        ["docker", "image", "inspect", image],
        capture_output=True,
    )
    return result.returncode == 0


def _resolve_minio_image(context: ProjectContext) -> str:
    """优先显式配置，其次复用本地旧镜像，最后使用共享 GHCR 镜像。"""
    configured = context.environment.get("SSV_MINIO_IMAGE")
    if configured:
        return configured
    if _image_present(context, "ssv-minio:minio"):
        return "ssv-minio:minio"
    return _DEFAULT_MINIO_IMAGE


def _ensure_minio_image(
    context: ProjectContext,
    compose_environment: dict[str, str],
    minio_image: str,
) -> None:
    """优先拉取共享镜像；不可用时从仓库内 Dockerfile 构建。"""
    if _image_present(context, minio_image):
        return
    info("拉取 MinIO 镜像...")
    pull = run_command(
        context,
        ["docker", "pull", minio_image],
        capture_output=True,
    )
    if pull.returncode == 0 and _image_present(context, minio_image):
        return
    info("无法拉取共享镜像，使用 docker/minio 本地构建...")
    _compose(
        context,
        "build",
        "minio",
        capture_output=True,
        environment=compose_environment,
    )


def _minio_ready(port: int) -> bool:
    try:
        with urlopen(f"http://127.0.0.1:{port}/minio/health/ready", timeout=1) as response:
            return 200 <= response.status < 300
    except OSError:
        return False


def start_infrastructure(
    context: ProjectContext,
    settings: RedisSettings,
    *,
    qdrant_url: str | None = None,
) -> int:
    qdrant_port = _local_qdrant_port(qdrant_url)
    minio_user = context.environment.get("MINIO_ROOT_USER", "")
    minio_password = context.environment.get("MINIO_ROOT_PASSWORD", "")
    if len(minio_user) < 3 or len(minio_password) < 8:
        raise CliError(
            "MinIO root 凭据无效：MINIO_ROOT_USER 至少 3 个字符，"
            "MINIO_ROOT_PASSWORD 至少 8 个字符；请在 docker/.env 中配置"
        )
    minio_api_port = context.environment.get("MINIO_API_PORT", "9000")
    minio_console_port = context.environment.get("MINIO_CONSOLE_PORT", "9001")
    minio_image = _resolve_minio_image(context)
    compose_environment = context.child_environment(
        REDIS_PORT=str(settings.port),
        QDRANT_PORT=str(qdrant_port),
        MINIO_API_PORT=minio_api_port,
        MINIO_CONSOLE_PORT=minio_console_port,
        SSV_MINIO_IMAGE=minio_image,
    )
    _ensure_minio_image(context, compose_environment, minio_image)
    _compose(context, "up", "-d", environment=compose_environment)
    info("等待 Redis 就绪...")
    last_error: Exception | None = None
    for _ in range(15):
        try:
            with RedisConnection(settings) as connection:
                connection.execute("PING")
            info("Redis 已就绪")
            break
        except RedisError as exc:  # connection can race container startup
            last_error = exc
            time.sleep(1)
    else:
        raise CliError(f"Redis 启动超时: {last_error}")

    info("等待 Qdrant 就绪...")
    for _ in range(15):
        if _qdrant_ready(qdrant_port):
            info("Qdrant 已就绪")
            break
        time.sleep(1)
    else:
        raise CliError("Qdrant 启动超时")

    info("等待 MinIO 就绪...")
    for _ in range(15):
        if _minio_ready(int(minio_api_port)):
            info("MinIO 已就绪")
            break
        time.sleep(1)
    else:
        raise CliError("MinIO 启动超时")

    info("Redis、Qdrant 和 MinIO 已就绪")
    return 0


def stop_infrastructure(context: ProjectContext) -> int:
    if not _is_running(context):
        info("Redis/Qdrant 基础设施未在运行")
        return 0
    _compose(context, "down")
    info("Redis/Qdrant 基础设施已停止")
    return 0


def status_infrastructure(context: ProjectContext) -> int:
    result = _compose(context, "ps", capture_output=True)
    output = result.stdout.strip()
    info(output or "Redis/Qdrant/MinIO 基础设施未在运行")
    return 0
