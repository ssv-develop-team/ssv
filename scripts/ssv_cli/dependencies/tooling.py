"""依赖 provider 共用的系统工具、探针和文件事务。"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from collections.abc import Mapping, Sequence
from pathlib import Path

from ..output import info
from .contracts import DependencyResult
from .policy import DependencyError, _error, join_unique


class DependencyTooling:
    def _command(
        self,
        argv: Sequence[str | Path],
        *,
        env: Mapping[str, str] | None = None,
        cwd: Path | None = None,
        capture_output: bool = True,
        check: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        command = [str(item) for item in argv]
        try:
            result = subprocess.run(
                command,
                cwd=str(cwd or self.root),
                env=dict(env or self.environment),
                text=True,
                capture_output=capture_output,
                check=False,
            )
        except FileNotFoundError as exc:
            raise _error(f"required command not found: {command[0]}") from exc
        if check and result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise _error(f"command failed ({result.returncode}): {' '.join(command)}{': ' + detail if detail else ''}")
        return result

    def _require_command(self, name: str) -> str:
        path = shutil.which(name)
        if path is None:
            raise _error(f"required command not found: {name}")
        return path

    def _pkg_env(self) -> dict[str, str]:
        self.pkg_config_dirs = self._active_pkg_config_dirs()
        environment = dict(self.environment)
        environment["PKG_CONFIG_PATH"] = join_unique(
            ":".join(self.pkg_config_dirs), self.base_pkg_config_path
        )
        return environment

    def _active_pkg_config_dirs(self) -> list[str]:
        """丢弃原子发布后已不存在的生成 pkg-config 目录。"""

        return [directory for directory in self.pkg_config_dirs if Path(directory).is_dir()]

    def _prepend_pkg_config(self, directory: str) -> None:
        self.pkg_config_dirs = self._active_pkg_config_dirs()
        if directory and directory not in self.pkg_config_dirs:
            self.pkg_config_dirs.insert(0, directory)
        self.environment["PKG_CONFIG_PATH"] = join_unique(
            ":".join(self.pkg_config_dirs), self.base_pkg_config_path
        )

    def _prepend_runtime(self, directories: str) -> None:
        self.environment["LD_LIBRARY_PATH"] = join_unique(
            directories, self.environment.get("LD_LIBRARY_PATH", "")
        )

    def _pkg(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return self._command(["pkg-config", *arguments], env=self._pkg_env())

    @property
    def build_environment(self) -> dict[str, str]:
        """返回传给 Meson 及其编译子进程的环境。"""

        return self._pkg_env()

    def check_base_dependencies(self) -> None:
        required = (
            "gstreamer-1.0",
            "gstreamer-base-1.0",
            "gstreamer-video-1.0",
            "yaml-cpp",
            "hiredis",
            "nlohmann_json",
            "blas",
            "lapack",
        )
        missing = [package for package in required if self._pkg("--exists", package).returncode != 0]
        if missing:
            raise _error(
                "missing C/C++ development dependencies: "
                + ", ".join(missing)
                + "; install GStreamer, yaml-cpp, hiredis, nlohmann-json, BLAS, and LAPACK development packages"
            )

    def _pkg_dir(self, package: str) -> str:
        result = self._pkg("--variable=pcfiledir", package)
        if result.returncode != 0 or not result.stdout.strip():
            raise _error(f"pkg-config package not found: {package}")
        path = Path(result.stdout.strip()).resolve(strict=False)
        if not path.is_dir():
            raise _error(f"pkg-config pcfiledir does not exist for {package}: {path}")
        return str(path)

    def _pkg_version(self, package: str) -> str:
        result = self._pkg("--modversion", package)
        if result.returncode != 0:
            raise _error(f"pkg-config package not found: {package}")
        return result.stdout.strip()

    def _pkg_at_least(self, package: str, minimum: str) -> None:
        result = self._pkg(f"--atleast-version={minimum}", package)
        if result.returncode != 0:
            raise _error(f"system {package} >= {minimum} is required")

    def _pkg_variable(self, package: str, variable: str) -> str:
        result = self._pkg(f"--variable={variable}", package)
        if result.returncode != 0:
            return ""
        return result.stdout.strip()

    def _pkg_lib_dirs(self, package: str) -> list[str]:
        values: list[str] = []
        lib_dir = self._pkg_variable(package, "libdir")
        if lib_dir:
            values.append(lib_dir)
        result = self._pkg("--libs", package)
        for flag in shlex.split(result.stdout) if result.returncode == 0 else ():
            if flag.startswith("-L") and len(flag) > 2:
                values.append(flag[2:])
        return values

    @staticmethod
    def _is_system_dir(directory: Path) -> bool:
        resolved = directory.resolve(strict=False)
        return resolved == Path("/lib") or resolved == Path("/usr/lib") or resolved == Path("/usr/lib64") or any(
            resolved.is_relative_to(parent)
            for parent in (Path("/lib"), Path("/usr/lib"), Path("/usr/lib64"))
        )

    def _runtime_dirs(self, *directories: str | Path) -> str:
        result: list[str] = []
        seen: set[str] = set()
        for item in directories:
            if not item:
                continue
            path = Path(item).resolve(strict=False)
            if not path.is_dir() or self._is_system_dir(path):
                continue
            value = str(path)
            if value not in seen:
                seen.add(value)
                result.append(value)
        return ":".join(result)

    def _write_pc(self, path: Path, lines: Sequence[str]) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def _compile_probe(self, source: str, package: str, runtime_dirs: str) -> None:
        compiler_value = self.environment.get("CXX", "")
        compiler = compiler_value.split()[0] if compiler_value else "c++"
        self._require_command(compiler)
        with tempfile.TemporaryDirectory(prefix="ssv-deps-probe-") as temporary_name:
            temporary = Path(temporary_name)
            source_file = temporary / "probe.cpp"
            binary = temporary / "probe"
            source_file.write_text(source, encoding="utf-8")
            flags_result = self._pkg("--cflags", "--libs", "--static", package)
            if flags_result.returncode != 0:
                raise _error(f"pkg-config probe flags unavailable for {package}")
            flags = shlex.split(flags_result.stdout)
            compile_result = self._command(
                [compiler, "-std=c++17", source_file, "-o", binary, *flags],
                env=self._pkg_env(),
            )
            if compile_result.returncode != 0:
                detail = (compile_result.stderr or compile_result.stdout).strip()
                raise _error(f"{package} compile probe failed: {detail}")
            environment = dict(self.environment)
            environment["LD_LIBRARY_PATH"] = join_unique(
                runtime_dirs, environment.get("LD_LIBRARY_PATH", "")
            )
            run_result = self._command([binary], env=environment)
            if run_result.returncode != 0:
                detail = (run_result.stderr or run_result.stdout).strip()
                raise _error(f"{package} load probe failed: {detail}")

    def _readelf(self, *arguments: str | Path) -> str:
        self._require_command("readelf")
        result = self._command(["readelf", *arguments])
        if result.returncode != 0:
            raise _error((result.stderr or result.stdout).strip() or "readelf failed")
        return result.stdout

    def _ldd(self, library: Path, *, preload: Path | None = None, check: bool = True) -> str:
        self._require_command("ldd")
        environment = dict(self.environment)
        environment["LD_LIBRARY_PATH"] = join_unique(
            str(library.parent), environment.get("LD_LIBRARY_PATH", "")
        )
        if preload is not None:
            environment["LD_PRELOAD"] = join_unique(
                str(preload), environment.get("LD_PRELOAD", "")
            )
        result = self._command(["ldd", "-r", library], env=environment)
        output = (result.stdout or "") + (result.stderr or "")
        if check and result.returncode != 0:
            raise _error(f"runtime load check failed for {library.name}: {output.strip()}")
        if check:
            missing = re.findall(r"^\s*([^\s]+) => not found$", output, re.MULTILINE)
            symbols = re.findall(r"undefined symbol: ([^\s]+)", output)
            if missing:
                raise _error(f"runtime dependencies unresolved for {library.name}: {','.join(sorted(set(missing)))}")
            if symbols:
                raise _error(f"runtime symbols unresolved for {library.name}: {','.join(sorted(set(symbols)))}")
        return output

    @staticmethod
    def _dependency_paths(ldd_output: str) -> list[Path]:
        paths: list[Path] = []
        for line in ldd_output.splitlines():
            match = re.search(r"=>\s+(/[^\s]+)", line)
            if match is None:
                match = re.match(r"\s*(/[^\s]+)", line)
            if match:
                paths.append(Path(match.group(1)).resolve(strict=False))
        return paths

    def _find_unique(self, root: Path, pattern: str, *, description: str) -> Path:
        matches = [path for path in root.rglob(pattern) if path.is_file()]
        if len(matches) > 1:
            raise _error(f"multiple {description} candidates found under {root}")
        if not matches:
            raise _error(f"{description} not found under {root}")
        return matches[0]

    def _candidate(self, parent: Path, label: str) -> Path:
        parent.mkdir(parents=True, exist_ok=True)
        return Path(tempfile.mkdtemp(prefix=f".{label}.", dir=parent))

    def _atomic_replace(self, candidate: Path, destination: Path) -> None:
        destination.parent.mkdir(parents=True, exist_ok=True)
        backup = destination.with_name(f"{destination.name}.old.{os.getpid()}")
        if backup.exists() or backup.is_symlink():
            self._remove_path(backup)
        if destination.exists() or destination.is_symlink():
            os.replace(destination, backup)
        try:
            os.replace(candidate, destination)
        except BaseException:
            if backup.exists() or backup.is_symlink():
                os.replace(backup, destination)
            raise
        if backup.exists() or backup.is_symlink():
            self._remove_path(backup)

    @staticmethod
    def _remove_path(path: Path) -> None:
        if path.is_symlink() or path.is_file():
            path.unlink(missing_ok=True)
        elif path.is_dir():
            shutil.rmtree(path)

    @staticmethod
    def _is_empty_directory(path: Path) -> bool:
        return not path.exists() or (path.is_dir() and not any(path.iterdir()))

    def _require_replaceable(self, path: Path, validator, *, description: str) -> None:
        if self._is_empty_directory(path):
            return
        try:
            validator(path)
            return
        except DependencyError:
            pass
        if not path.is_dir():
            raise _error(f"refusing to replace non-directory {description}: {path}")
        raise _error(f"refusing to replace non-empty unrecognized {description}: {path}")

    def _download(self, url: str, destination: Path) -> Path:
        if destination.is_file() and destination.stat().st_size > 0:
            return destination
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(f"{destination.name}.tmp.{os.getpid()}")
        temporary.unlink(missing_ok=True)
        info(f"downloading {destination.name}")
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "ssv-cli/0.1"})
            with urllib.request.urlopen(request, timeout=120) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
            if temporary.stat().st_size == 0:
                raise _error(f"downloaded file is empty: {url}")
            os.replace(temporary, destination)
        except (OSError, urllib.error.URLError) as exc:
            temporary.unlink(missing_ok=True)
            raise _error(f"download failed: {url}: {exc}") from exc
        return destination

    @staticmethod
    def _safe_archive_member(destination: Path, member_name: str) -> Path:
        target = (destination / member_name).resolve(strict=False)
        if not target.is_relative_to(destination.resolve()):
            raise _error(f"archive contains an unsafe path: {member_name}")
        return target

    def _extract_archive(self, archive: Path, destination: Path) -> None:
        self._remove_path(destination)
        destination.mkdir(parents=True, exist_ok=True)
        name = archive.name.lower()
        if name.endswith(".zip"):
            try:
                with zipfile.ZipFile(archive) as source:
                    for member in source.infolist():
                        self._safe_archive_member(destination, member.filename)
                    source.extractall(destination)
            except (OSError, zipfile.BadZipFile) as exc:
                raise _error(f"failed to extract archive: {archive}: {exc}") from exc
            return
        if name.endswith((".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2", ".tbz2", ".tar")):
            try:
                with tarfile.open(archive, "r:*") as source:
                    members = source.getmembers()
                    for member in members:
                        self._safe_archive_member(destination, member.name)
                    source.extractall(destination)
            except (OSError, tarfile.TarError) as exc:
                raise _error(f"failed to extract archive: {archive}: {exc}") from exc
            return
        if name.endswith((".tar.zst", ".tzst")):
            self._require_command("tar")
            result = self._command(["tar", "--zstd", "-xf", archive, "-C", destination])
            if result.returncode != 0:
                detail = (result.stderr or result.stdout).strip()
                raise _error(f"failed to extract archive: {archive}: {detail}")
            return
        raise _error(f"unsupported archive format: {archive}")

    def _system_result(self, package: str, minimum: str) -> DependencyResult:
        self._pkg_at_least(package, minimum)
        pc_dir = self._pkg_dir(package)
        project_paths = {(self.root / ".deps").resolve(), self.build_dir.resolve()}
        if any(Path(pc_dir).is_relative_to(path) for path in project_paths):
            raise _error(f"system {package} resolved to a project-local artifact: {pc_dir}")
        runtime_dirs = self._runtime_dirs(*self._pkg_lib_dirs(package))
        if package == "opencv4":
            probe = '#include <opencv2/core.hpp>\nint main() { return CV_VERSION[0] == \'\\0\'; }\n'
        elif package == "nvinfer":
            probe = "#include <NvInfer.h>\n#include <cuda_runtime_api.h>\nint main() { return NV_TENSORRT_MAJOR < 0; }\n"
        else:
            raise _error(f"no system dependency probe is defined for {package}")
        self._compile_probe(probe, package, runtime_dirs)
        return DependencyResult(
            source="system",
            version=self._pkg_version(package),
            pc_dir=pc_dir,
            runtime_dirs=runtime_dirs,
        )

    def _validate_elf(self, path: Path, label: str) -> None:
        try:
            self._readelf("-h", path)
        except DependencyError as exc:
            raise _error(f"{label} is not a readable ELF: {path.name}: {exc}") from exc

    # ------------------------------------------------------------------
    # ONNX Runtime
