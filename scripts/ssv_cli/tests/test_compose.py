from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, call, patch

from scripts.ssv_cli.config import RedisSettings
from scripts.ssv_cli.context import ProjectContext
from scripts.ssv_cli.services.compose import start_infrastructure, status_infrastructure


def _context(root: Path) -> ProjectContext:
    return ProjectContext(
        root=root,
        environment={
            "PATH": "/usr/bin",
            "MINIO_ROOT_USER": "test-root",
            "MINIO_ROOT_PASSWORD": "test-password",
            "SSV_MINIO_ACCESS_KEY": "test-application",
            "SSV_MINIO_SECRET_KEY": "test-application-password",
        },
        config_path=None,
        build_dir=root / "build",
        compose_file=root / "docker" / "compose.yaml",
    )


class ComposeInfrastructureTest(unittest.TestCase):
    def test_start_passes_resolved_ports_and_waits_for_services(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = _context(root)
            settings = RedisSettings(port=6380)
            connection = MagicMock()
            connection.__enter__.return_value = connection
            connection.execute.return_value = "PONG"
            compose_result = subprocess.CompletedProcess([], 0, "", "")

            with (
                patch(
                    "scripts.ssv_cli.services.compose._compose",
                    return_value=compose_result,
                ) as compose,
                patch(
                    "scripts.ssv_cli.services.compose.RedisConnection",
                    return_value=connection,
                ),
                patch(
                    "scripts.ssv_cli.services.compose._qdrant_ready",
                    return_value=True,
                ) as ready,
                patch(
                    "scripts.ssv_cli.services.compose._resolve_minio_image",
                    return_value="test/minio:tag",
                ),
                patch("scripts.ssv_cli.services.compose._ensure_minio_image"),
                patch(
                    "scripts.ssv_cli.services.compose._minio_ready",
                    return_value=True,
                ) as minio_ready,
                patch(
                    "scripts.ssv_cli.services.compose.run_command",
                    return_value=subprocess.CompletedProcess(
                        [],
                        0,
                        '[{"Service":"minio-init","State":"exited","ExitCode":0}]',
                        "",
                    ),
                ),
            ):
                self.assertEqual(
                    start_infrastructure(context, settings, qdrant_url="http://localhost:7444"),
                    0,
                )

            environment = {
                "PATH": "/usr/bin",
                "MINIO_ROOT_USER": "test-root",
                "MINIO_ROOT_PASSWORD": "test-password",
                "SSV_MINIO_ACCESS_KEY": "test-application",
                "SSV_MINIO_SECRET_KEY": "test-application-password",
                "REDIS_PORT": "6380",
                "QDRANT_PORT": "7444",
                "MINIO_API_PORT": "9000",
                "MINIO_CONSOLE_PORT": "9001",
                "SSV_MINIO_IMAGE": "test/minio:tag",
            }
            self.assertEqual(
                compose.call_args_list,
                [
                    call(context, "up", "-d", "redis", "qdrant", "minio", environment=environment),
                    call(
                        context,
                        "up",
                        "-d",
                        "--no-deps",
                        "--force-recreate",
                        "minio-init",
                        environment=environment,
                    ),
                ],
            )
            ready.assert_called_once_with(7444)
            minio_ready.assert_called_once_with(9000)


class ComposeMinioReadinessFailureTest(unittest.TestCase):
    def test_start_fails_when_minio_is_not_ready(self) -> None:
        from scripts.ssv_cli.output import CliError

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = _context(root)
            connection = MagicMock()
            connection.__enter__.return_value = connection

            with (
                patch(
                    "scripts.ssv_cli.services.compose._compose",
                    return_value=subprocess.CompletedProcess([], 0, "", ""),
                ),
                patch(
                    "scripts.ssv_cli.services.compose.RedisConnection",
                    return_value=connection,
                ),
                patch(
                    "scripts.ssv_cli.services.compose._qdrant_ready",
                    return_value=True,
                ),
                patch(
                    "scripts.ssv_cli.services.compose._resolve_minio_image",
                    return_value="test/minio:tag",
                ),
                patch("scripts.ssv_cli.services.compose._ensure_minio_image"),
                patch(
                    "scripts.ssv_cli.services.compose._minio_ready",
                    return_value=False,
                ),
                patch("scripts.ssv_cli.services.compose.time.sleep"),
            ):
                with self.assertRaises(CliError):
                    start_infrastructure(context, RedisSettings())


class ComposeMinioInitializationTest(unittest.TestCase):
    def test_status_accepts_compose_json_output_formats(self) -> None:
        from scripts.ssv_cli.services.compose import _minio_init_exit_code

        for output in (
            '[{"Service":"minio-init","State":"exited","ExitCode":0}]',
            '{"Service":"minio-init","State":"exited","ExitCode":0}',
            '{"Service":"minio","State":"running","ExitCode":0}\n'
            '{"Service":"minio-init","State":"exited","ExitCode":0}\n',
        ):
            with self.subTest(output=output), tempfile.TemporaryDirectory() as directory:
                context = _context(Path(directory))
                with patch(
                    "scripts.ssv_cli.services.compose.run_command",
                    return_value=subprocess.CompletedProcess([], 0, output, ""),
                ):
                    self.assertEqual(_minio_init_exit_code(context, context.environment), 0)

    def test_missing_minio_initialization_container_is_reported(self) -> None:
        from scripts.ssv_cli.output import CliError
        from scripts.ssv_cli.services.compose import _minio_init_exit_code

        with tempfile.TemporaryDirectory() as directory:
            context = _context(Path(directory))
            missing = subprocess.CompletedProcess([], 0, "", "")
            with patch(
                "scripts.ssv_cli.services.compose.run_command",
                return_value=missing,
            ):
                with self.assertRaisesRegex(CliError, "初始化容器不存在"):
                    _minio_init_exit_code(context, context.environment)

    def test_start_fails_when_minio_initialization_exits_nonzero(self) -> None:
        from scripts.ssv_cli.output import CliError

        with tempfile.TemporaryDirectory() as directory:
            context = _context(Path(directory))
            connection = MagicMock()
            connection.__enter__.return_value = connection
            init_status = subprocess.CompletedProcess(
                [],
                0,
                '[{"Service":"minio-init","State":"exited","ExitCode":7}]',
                "",
            )
            init_logs = subprocess.CompletedProcess(
                [],
                0,
                "minio-init-1 | MinIO 初始化阶段失败：创建应用账号\n",
                "",
            )

            with (
                patch(
                    "scripts.ssv_cli.services.compose._compose",
                    return_value=subprocess.CompletedProcess([], 0, "", ""),
                ),
                patch(
                    "scripts.ssv_cli.services.compose.RedisConnection",
                    return_value=connection,
                ),
                patch(
                    "scripts.ssv_cli.services.compose._qdrant_ready",
                    return_value=True,
                ),
                patch(
                    "scripts.ssv_cli.services.compose._resolve_minio_image",
                    return_value="test/minio:tag",
                ),
                patch("scripts.ssv_cli.services.compose._ensure_minio_image"),
                patch(
                    "scripts.ssv_cli.services.compose._minio_ready",
                    return_value=True,
                ),
                patch(
                    "scripts.ssv_cli.services.compose.run_command",
                    side_effect=[init_status, init_logs],
                ),
            ):
                with self.assertRaisesRegex(CliError, "初始化失败.*7.*创建应用账号"):
                    start_infrastructure(context, RedisSettings())

    def test_start_times_out_while_minio_initialization_is_running(self) -> None:
        from scripts.ssv_cli.output import CliError

        with tempfile.TemporaryDirectory() as directory:
            context = _context(Path(directory))
            connection = MagicMock()
            connection.__enter__.return_value = connection
            running = subprocess.CompletedProcess(
                [],
                0,
                '[{"Service":"minio-init","State":"running","ExitCode":0}]',
                "",
            )
            with (
                patch(
                    "scripts.ssv_cli.services.compose._compose",
                    return_value=subprocess.CompletedProcess([], 0, "", ""),
                ),
                patch(
                    "scripts.ssv_cli.services.compose.RedisConnection",
                    return_value=connection,
                ),
                patch(
                    "scripts.ssv_cli.services.compose._qdrant_ready",
                    return_value=True,
                ),
                patch(
                    "scripts.ssv_cli.services.compose._resolve_minio_image",
                    return_value="test/minio:tag",
                ),
                patch("scripts.ssv_cli.services.compose._ensure_minio_image"),
                patch(
                    "scripts.ssv_cli.services.compose._minio_ready",
                    return_value=True,
                ),
                patch(
                    "scripts.ssv_cli.services.compose.run_command",
                    return_value=running,
                ),
                patch("scripts.ssv_cli.services.compose.time.sleep"),
            ):
                with self.assertRaisesRegex(CliError, "初始化超时"):
                    start_infrastructure(context, RedisSettings())


class ComposeMinioCredentialTest(unittest.TestCase):
    def test_start_rejects_invalid_minio_root_credentials(self) -> None:
        from scripts.ssv_cli.output import CliError

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = ProjectContext(
                root=root,
                environment={"PATH": "/usr/bin", "MINIO_ROOT_USER": "ab"},
                config_path=None,
                build_dir=root / "build",
                compose_file=root / "docker" / "compose.yaml",
            )

            with self.assertRaises(CliError):
                start_infrastructure(context, RedisSettings())

    def test_start_rejects_application_access_key_matching_root_user(self) -> None:
        from scripts.ssv_cli.output import CliError

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = _context(root)
            context.environment["SSV_MINIO_ACCESS_KEY"] = "test-root"

            with self.assertRaisesRegex(CliError, "必须不同"):
                start_infrastructure(context, RedisSettings())

    def test_start_rejects_invalid_application_credentials(self) -> None:
        from scripts.ssv_cli.output import CliError

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = _context(root)
            context.environment.pop("SSV_MINIO_SECRET_KEY")

            with self.assertRaisesRegex(CliError, "SSV_MINIO_SECRET_KEY"):
                start_infrastructure(context, RedisSettings())


class ComposeEnvFileTest(unittest.TestCase):
    def test_compose_uses_root_credentials_file_and_resolved_port_environment(self) -> None:
        from scripts.ssv_cli.services.compose import _compose_argv

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docker").mkdir()
            (root / ".env").write_text("MINIO_ROOT_USER=root-admin\n")
            (root / "docker" / ".env").write_text("REDIS_PORT=6379\n")

            argv = _compose_argv(_context(root), "up", "-d")

            self.assertEqual(
                argv,
                [
                    "docker",
                    "compose",
                    "--env-file",
                    str(root / ".env"),
                    "-f",
                    str(root / "docker" / "compose.yaml"),
                    "up",
                    "-d",
                ],
            )

    def test_status_prints_compose_service_state(self) -> None:
        root = Path("/repo")
        context = ProjectContext(
            root=root,
            environment={"PATH": "/usr/bin"},
            config_path=None,
            build_dir=root / "build",
            compose_file=root / "docker" / "compose.yaml",
        )
        compose_result = subprocess.CompletedProcess([], 0, "ssv-redis Up\nssv-qdrant Up", "")

        with (
            patch("scripts.ssv_cli.services.compose._compose", return_value=compose_result) as compose,
            patch("scripts.ssv_cli.services.compose.info") as output,
        ):
            self.assertEqual(status_infrastructure(context), 0)

        compose.assert_called_once_with(context, "ps", capture_output=True)
        output.assert_called_once_with("ssv-redis Up\nssv-qdrant Up")


if __name__ == "__main__":
    unittest.main()


class ComposeMinioImageTest(unittest.TestCase):
    def test_existing_image_skips_pull_and_build(self) -> None:
        from scripts.ssv_cli.services.compose import _ensure_minio_image

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = _context(root)
            with (
                patch(
                    "scripts.ssv_cli.services.compose._image_present",
                    return_value=True,
                ),
                patch(
                    "scripts.ssv_cli.services.compose.run_command"
                ) as run_command,
                patch(
                    "scripts.ssv_cli.services.compose._compose"
                ) as compose,
            ):
                _ensure_minio_image(context, {"PATH": "/usr/bin"}, "example/minio:tag")

            run_command.assert_not_called()
            compose.assert_not_called()

    def test_pull_failure_falls_back_to_local_build(self) -> None:
        from scripts.ssv_cli.services.compose import _ensure_minio_image

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = _context(root)
            pull = subprocess.CompletedProcess([], 1, "", "denied")
            with (
                patch(
                    "scripts.ssv_cli.services.compose._image_present",
                    side_effect=[False, False],
                ),
                patch(
                    "scripts.ssv_cli.services.compose.run_command",
                    return_value=pull,
                ),
                patch(
                    "scripts.ssv_cli.services.compose._compose"
                ) as compose,
            ):
                _ensure_minio_image(context, {"PATH": "/usr/bin"}, "example/minio:tag")

            compose.assert_called_once_with(
                context,
                "build",
                "minio",
                capture_output=True,
                environment={"PATH": "/usr/bin"},
            )


class ComposeMinioImageResolutionTest(unittest.TestCase):
    def test_explicit_image_wins(self) -> None:
        from scripts.ssv_cli.services.compose import _resolve_minio_image

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            environment = dict(_context(root).environment)
            environment["SSV_MINIO_IMAGE"] = "registry.local/minio:tag"
            context = ProjectContext(
                root=root,
                environment=environment,
                config_path=None,
                build_dir=root / "build",
                compose_file=root / "docker" / "compose.yaml",
            )
            self.assertEqual(_resolve_minio_image(context), "registry.local/minio:tag")

    def test_legacy_local_image_is_not_reused(self) -> None:
        from scripts.ssv_cli.services.compose import (
            _DEFAULT_MINIO_IMAGE,
            _resolve_minio_image,
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = _context(root)
            with patch(
                "scripts.ssv_cli.services.compose._image_present",
                return_value=True,
            ):
                self.assertEqual(_resolve_minio_image(context), _DEFAULT_MINIO_IMAGE)

    def test_defaults_to_shared_image_when_nothing_is_local(self) -> None:
        from scripts.ssv_cli.services.compose import (
            _DEFAULT_MINIO_IMAGE,
            _resolve_minio_image,
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            context = _context(root)
            with patch(
                "scripts.ssv_cli.services.compose._image_present",
                return_value=False,
            ):
                self.assertEqual(_resolve_minio_image(context), _DEFAULT_MINIO_IMAGE)
