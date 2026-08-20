"""Project test orchestration."""

from __future__ import annotations

import importlib
import subprocess
import sys
from argparse import Namespace

from ..context import ProjectContext
from ..output import CliError, header, info, warn
from ..process import require_command, run_command
from ..services.native_build import NativeBuildService
from ..services.runtime_env import load_runtime_environment


def _require_model_test_dependencies() -> None:
    """校验模型契约测试文件使用的 optional Python 包。"""

    packages = ("numpy", "onnx", "onnxruntime")
    for package in packages:
        try:
            importlib.import_module(package)
        except ModuleNotFoundError as exc:
            missing = (exc.name or package).split(".", 1)[0]
            if missing in packages:
                raise CliError(
                    "运行 ./ssv test 的模型契约测试需要 Python 依赖 "
                    f"{missing}；请安装 python -m pip install -e '.[model]'"
                ) from exc
            raise CliError(
                f"加载模型测试依赖 {package} 失败：内部依赖 {missing} 不可用；请检查项目环境"
            ) from exc
        except (ImportError, OSError) as exc:
            raise CliError(f"加载模型测试依赖 {package} 失败: {exc}") from exc


def run(context: ProjectContext, _args: Namespace) -> int:
    header("运行测试套件")
    root = context.root
    info("检查模型契约测试依赖")
    _require_model_test_dependencies()
    info("运行 Python CLI 与服务测试")
    result = run_command(
        context,
        [
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            root / "scripts" / "ssv_cli" / "tests",
            "-p",
            "test_*.py",
        ],
    )
    if result.returncode != 0:
        return result.returncode

    for name, test_file in (
        ("模型准备契约测试", root / "scripts" / "ssv_cli" / "tests" / "ssv_prepare_model_test.py"),
        ("TensorRT manifest 契约测试", root / "scripts" / "ssv_cli" / "tests" / "ssv_tensorrt_manifest_test.py"),
    ):
        info(f"运行: {name}")
        result = run_command(context, [sys.executable, test_file])
        if result.returncode != 0:
            return result.returncode

    info("运行 C++ 构建")
    NativeBuildService(context).build("auto")

    info("运行 Meson 测试")
    environment = load_runtime_environment(context)
    result = run_command(
        context,
        ["meson", "test", "-C", context.build_dir, "--print-errorlogs"],
        environment=environment,
    )
    if result.returncode != 0:
        return result.returncode

    info("运行 Python Agent 测试")
    uv = require_command("uv", "请安装 uv，或在 agent 环境中安装 pytest")
    agent_dir = root / "agent"
    result = run_command(context, [uv, "run", "--extra", "dev", "pytest"], cwd=agent_dir)
    if result.returncode != 0:
        return result.returncode

    if context.config_path and context.config_path.is_file():
        timeout = require_command("timeout", "请安装 coreutils")
        info("运行 30 秒无头 runner smoke")
        environment = load_runtime_environment(context)
        smoke = subprocess.run(
            [
                timeout,
                "--foreground",
                "--signal=INT",
                "--kill-after=5s",
                "30s",
                root / "ssv",
                "run",
                "--config",
                str(context.config_path),
                "--headless",
            ],
            cwd=root,
            env=environment,
            text=True,
            check=False,
        )
        if smoke.returncode not in (0, 124):
            if context.environment.get("SSV_REQUIRE_SMOKE", "false").lower() == "true":
                return smoke.returncode
            warn(f"链路冒烟测试失败，已作为警告继续: status={smoke.returncode}")
    else:
        warn("跳过链路冒烟测试: 未找到本地运行配置")
    info("测试套件完成")
    return 0
