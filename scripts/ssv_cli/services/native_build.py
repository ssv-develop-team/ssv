"""由 Python CLI 接管的 Meson 构建编排。"""

from __future__ import annotations

import fcntl
import os
import shutil
from collections.abc import Iterator, Sequence
from contextlib import contextmanager
from pathlib import Path

from ..context import ProjectContext
from ..dependencies import DependencySnapshot, load_dependency_manager
from ..output import CliError, info, warn
from ..process import require_command, run_command

BUILD_PLUGINS = (
    ("gst/ssv-template", "libgstssvtemplate.so"),
    ("gst/ssv-infer", "libgstssvinfer.so"),
    ("gst/ssv-track", "libgstssvtrack.so"),
    ("gst/ssv-pub", "libgstssvpub.so"),
    ("gst/ssv-overlay", "libgstssvoverlay.so"),
)


@contextmanager
def _build_lock(path: Path) -> Iterator[None]:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+") as stream:
        info("等待构建锁...")
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


class NativeBuildService:
    """不依赖 Shell 后端地准备依赖并构建 native 目标。"""

    def __init__(self, context: ProjectContext) -> None:
        self.context = context

    def _run(
        self,
        argv: Sequence[str | Path],
        *,
        environment: dict[str, str],
        capture_output: bool = False,
    ):
        result = run_command(
            self.context,
            argv,
            environment=environment,
            capture_output=capture_output,
        )
        return result

    def _previous_signature(self, path: Path) -> str:
        if not path.is_file():
            return ""
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("SSV_DEPS_SIGNATURE="):
                raw = line.partition("=")[2]
                if raw.startswith("'") and raw.endswith("'"):
                    return raw[1:-1].replace("'\\''", "'")
                return raw
        return ""

    def _meson_options(self, snapshot: DependencySnapshot) -> list[str]:
        options = [
            f"-Dopencv_mode={snapshot.opencv_mode}",
            f"-Dtensorrt_mode={snapshot.tensorrt_mode}",
            f"-Donnxruntime_profile={snapshot.profile}",
            f"-Donnxruntime_dependency_signature={snapshot.signature}",
            f"-Ddeps_runtime_path={snapshot.runtime_path}",
        ]
        if snapshot.pkg_config_path:
            options.append(f"-Dpkg_config_path={snapshot.pkg_config_path}")
        return options

    def _configure_meson(
        self,
        snapshot: DependencySnapshot,
        *,
        dependency_cache_changed: bool,
        environment: dict[str, str],
    ) -> None:
        meson = require_command("meson", "请安装 Meson")
        options = self._meson_options(snapshot)
        build_dir = self.context.build_dir
        if (build_dir / "build.ninja").is_file():
            refresh = self._run(
                [meson, "setup", build_dir, "--reconfigure"],
                environment=environment,
                capture_output=True,
            )
            if refresh.returncode != 0:
                detail = (refresh.stderr or refresh.stdout).strip()
                raise CliError(f"Meson 刷新配置失败: {detail}")
            if dependency_cache_changed:
                help_result = self._run(
                    [meson, "setup", "--help"],
                    environment=environment,
                    capture_output=True,
                )
                if help_result.returncode == 0 and "--clearcache" in (help_result.stdout or ""):
                    command = [meson, "setup", build_dir, "--reconfigure", "--clearcache", *options]
                else:
                    command = [meson, "setup", build_dir, "--wipe", *options]
            else:
                command = [meson, "setup", build_dir, "--reconfigure", *options]
        else:
            command = [meson, "setup", build_dir, *options]
        result = self._run(command, environment=environment)
        if result.returncode != 0:
            raise CliError(f"Meson 配置失败，退出码: {result.returncode}")

    def _compile(self, environment: dict[str, str]) -> None:
        meson = require_command("meson", "请安装 Meson")
        result = self._run(
            [meson, "compile", "-C", self.context.build_dir],
            environment=environment,
        )
        if result.returncode != 0:
            raise CliError(f"Meson 编译失败，退出码: {result.returncode}")

    def _check_plugins(self) -> None:
        missing: list[str] = []
        for directory, name in BUILD_PLUGINS:
            path = self.context.build_dir / directory / name
            if path.is_file():
                info(f"编译成功: {self.context.display_path(path)}")
            else:
                missing.append(self.context.display_path(path))
                warn(f"插件未生成: {self.context.display_path(path)}")
        if missing:
            raise CliError("部分插件未生成，构建结果不完整")

    def build(self, requested_profile: str = "auto") -> int:
        root = self.context.root
        build_dir = self.context.build_dir
        pending = build_dir / "ssv-deps.env.pending"
        final_snapshot = build_dir / "ssv-deps.env"
        with _build_lock(root / ".deps" / "build.lock"):
            if build_dir.exists() and not (build_dir / "build.ninja").is_file():
                build_path = build_dir.resolve(strict=False)
                root_path = root.resolve(strict=False)
                if build_path == root_path or root_path.is_relative_to(build_path):
                    raise CliError(
                        "拒绝删除项目根目录或其祖先作为无效构建目录: "
                        f"{self.context.display_path(build_dir)}"
                    )
                warn(f"构建目录不是有效的 Meson build，重新创建: {self.context.display_path(build_dir)}")
                shutil.rmtree(build_dir)
            build_dir.mkdir(parents=True, exist_ok=True)
            manager = load_dependency_manager(self.context, requested_profile)
            manager.check_base_dependencies()
            previous_signature = self._previous_signature(final_snapshot)
            try:
                snapshot = manager.prepare(requested_profile, pending_path=pending)
                environment = manager.build_environment
                self._configure_meson(
                    snapshot,
                    dependency_cache_changed=previous_signature != snapshot.signature,
                    environment=environment,
                )
                self._compile(environment)
                self._check_plugins()
                os.replace(pending, final_snapshot)
            finally:
                pending.unlink(missing_ok=True)
        return 0
