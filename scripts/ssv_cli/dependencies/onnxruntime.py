"""ONNX Runtime artifact、provider 和 OpenVINO 构建实现。"""

from __future__ import annotations

import os
import platform
import re
import shlex
import shutil
import sys
import tempfile
from pathlib import Path

from ..output import warn
from .contracts import DependencyResult
from .policy import (
    _PROVIDER_LIBRARY_NAMES,
    DEFAULT_ONNXRUNTIME_VERSION,
    OPENVINO_ARCHIVE,
    OPENVINO_URL,
    OPENVINO_VERSION,
    ORT_INTEL_ARCHIVE,
    ORT_INTEL_URL,
    DependencyError,
    _error,
    join_unique,
    validate_provider_set,
)

_ORT_PROBE_SOURCE = r"""#include <algorithm>
#include <iostream>
#include <onnxruntime_cxx_api.h>
int main() {
    auto providers = Ort::GetAvailableProviders();
    std::sort(providers.begin(), providers.end());
    std::cout << "version=" << OrtGetApiBase()->GetVersionString() << "\nproviders=";
    for (std::size_t i = 0; i < providers.size(); ++i) {
        if (i != 0) std::cout << ",";
        std::cout << providers[i];
    }
    std::cout << "\n";
    return 0;
}
"""

class OnnxRuntimeProvider:
    @staticmethod
    def _normalize_onnx_version(version: str) -> str:
        if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-gpu)?", version) is None:
            raise _error(f"SSV_ONNXRUNTIME_VERSION must be x.y.z or x.y.z-gpu: {version}")
        return version

    @staticmethod
    def _onnx_archive_info(version: str) -> tuple[str, str]:
        machine = platform.machine().lower()
        if machine in {"x86_64", "amd64"}:
            architecture = "x64"
        elif machine in {"aarch64", "arm64"}:
            architecture = "aarch64"
        else:
            raise _error(f"managed ONNX Runtime supports Linux x86_64 and aarch64, got {machine}")
        base = version.removesuffix("-gpu")
        if version.endswith("-gpu"):
            if architecture != "x64":
                raise _error("managed NVIDIA ONNX Runtime supports Linux x86_64 only")
            archive = f"onnxruntime-linux-x64-gpu_cuda13-{base}.tgz"
        else:
            archive = f"onnxruntime-linux-{architecture}-{base}.tgz"
        return archive, f"https://github.com/microsoft/onnxruntime/releases/download/v{base}/{archive}"

    @staticmethod
    def _onnx_libdir(root: Path) -> Path:
        matches: list[Path] = []
        for name in ("lib", "lib64"):
            directory = root / name
            if not directory.is_dir():
                continue
            if any(path.name == "libonnxruntime.so" or path.name.startswith("libonnxruntime.so.") for path in directory.iterdir()):
                matches.append(directory)
        if len(matches) > 1:
            raise _error(f"multiple ONNX Runtime library directories found under {root}")
        if not matches:
            raise _error(f"ONNX Runtime library not found under {root}")
        return matches[0].resolve()

    def _make_onnx_pc(self, root: Path, version: str, include_dir: Path, lib_dir: Path) -> Path:
        pc_dir = root / "lib" / "pkgconfig"
        self._write_pc(
            pc_dir / "onnxruntime.pc",
            (
                f"prefix={root}",
                "exec_prefix=${prefix}",
                f"libdir={lib_dir}",
                f"includedir={include_dir}",
                "",
                "Name: onnxruntime",
                "Description: ONNX Runtime C/C++ inference runtime",
                f"Version: {version.removesuffix('-gpu')}",
                "Libs: -L${libdir} -lonnxruntime",
                "Cflags: -I${includedir}",
            ),
        )
        return pc_dir.resolve()

    def _ort_provider_libraries(self, profile: str, lib_dir: Path) -> str:
        libraries: list[str] = []
        for name in _PROVIDER_LIBRARY_NAMES[profile]:
            path = lib_dir / name
            if not path.is_file():
                raise _error(f"{profile} profile is missing Provider library: {name}")
            self._validate_elf(path, f"{profile} profile Provider library")
            if name != "libonnxruntime_providers_shared.so":
                symbols = self._readelf("--dyn-syms", "--wide", path)
                found = any(
                    "GLOBAL" in line
                    and "DEFAULT" in line
                    and "UND" not in line
                    and re.search(r"\bGetProvider(?:@|\s|$)", line) is not None
                    for line in symbols.splitlines()
                )
                if not found:
                    raise _error(f"{profile} profile Provider library has no GetProvider entry point: {name}")
            value = str(path.resolve())
            if value not in libraries:
                libraries.append(value)
        return ":".join(libraries)

    def _ort_runtime_dirs(self, profile: str, lib_dir: Path, provider_libraries: str) -> str:
        libraries = [lib_dir / "libonnxruntime.so"]
        provider_paths = [Path(value) for value in provider_libraries.split(":") if value]
        libraries.extend(provider_paths)
        dependency_dirs: list[Path] = []
        shared = lib_dir / "libonnxruntime_providers_shared.so"
        for library in libraries:
            if not library.is_file():
                raise _error(f"ONNX Runtime link library not found: {library}")
            self._validate_elf(library, "ONNX Runtime library")
            preload = shared if library.name.startswith("libonnxruntime_providers_") and library != shared else None
            output = self._ldd(library, preload=preload)
            dependency_dirs.extend(path.parent for path in self._dependency_paths(output))
        return self._runtime_dirs(lib_dir, *dependency_dirs)

    def _ort_probe(self, runtime_dirs: str) -> tuple[str, str]:
        with tempfile.TemporaryDirectory(prefix="ssv-ort-probe-") as temporary_name:
            temporary = Path(temporary_name)
            source_file = temporary / "probe.cpp"
            source_file.write_text(_ORT_PROBE_SOURCE, encoding="utf-8")
            compiler_value = self.environment.get("CXX", "")
            compiler = compiler_value.split()[0] if compiler_value else "c++"
            flags_result = self._pkg("--cflags", "--libs", "--static", "onnxruntime")
            if flags_result.returncode != 0:
                raise _error("ONNX Runtime probe flags are unavailable")
            binary = temporary / "probe"
            compile_result = self._command(
                [compiler, "-std=c++17", source_file, "-o", binary, *shlex.split(flags_result.stdout)],
                env=self._pkg_env(),
            )
            if compile_result.returncode != 0:
                detail = (compile_result.stderr or compile_result.stdout).strip()
                raise _error(f"ONNX Runtime compile/load probe failed: {detail}")
            environment = dict(self.environment)
            environment["LD_LIBRARY_PATH"] = join_unique(runtime_dirs, environment.get("LD_LIBRARY_PATH", ""))
            result = self._command([binary], env=environment)
            if result.returncode != 0:
                detail = (result.stderr or result.stdout).strip()
                raise _error(f"ONNX Runtime compile/load probe failed: {detail}")
            values: dict[str, str] = {}
            for line in result.stdout.splitlines():
                key, separator, value = line.partition("=")
                if not separator or key not in {"version", "providers"} or key in values:
                    raise _error(f"ONNX Runtime probe returned an invalid line: {line}")
                values[key] = value
            if set(values) != {"version", "providers"}:
                raise _error("ONNX Runtime probe returned an incomplete result")
            return values["version"], values["providers"]

    def _validate_onnx_artifact(
        self,
        profile: str,
        expected_version: str,
        include_dir: Path,
        lib_dir: Path,
        expected_pc_dir: Path,
    ) -> DependencyResult:
        if not (include_dir / "onnxruntime_cxx_api.h").is_file():
            raise _error(f"ONNX Runtime header not found: {include_dir / 'onnxruntime_cxx_api.h'}")
        link_library = lib_dir / "libonnxruntime.so"
        if not link_library.is_file():
            raise _error(f"ONNX Runtime link library not found: {link_library}")
        actual_pc_dir = Path(self._pkg_dir("onnxruntime"))
        if actual_pc_dir != expected_pc_dir.resolve():
            raise _error(f"onnxruntime pkg-config source mismatch: expected {expected_pc_dir}, got {actual_pc_dir}")
        pc_version = self._pkg_version("onnxruntime")
        if pc_version != expected_version:
            raise _error(f"ONNX Runtime pkg-config ABI mismatch: expected {expected_version}, got {pc_version}")
        provider_libraries = self._ort_provider_libraries(profile, lib_dir)
        runtime_dirs = self._ort_runtime_dirs(profile, lib_dir, provider_libraries)
        runtime_version, providers = self._ort_probe(runtime_dirs)
        if runtime_version != expected_version:
            raise _error(f"ONNX Runtime ABI mismatch: expected {expected_version}, got {runtime_version or 'empty'}")
        validate_provider_set(profile, providers)
        return DependencyResult(
            source=self.config.onnx_source,
            version=runtime_version,
            pc_dir=str(actual_pc_dir),
            runtime_dirs=runtime_dirs,
            providers=providers,
            provider_libraries=provider_libraries,
            include_dir=str(include_dir.resolve()),
            lib_dir=str(lib_dir.resolve()),
        )

    def _validate_onnx_layout(self, root: Path, expected_version: str, profile: str) -> DependencyResult:
        include_dir = root / "include"
        lib_dir = self._onnx_libdir(root)
        version_file = root / "VERSION_NUMBER"
        if not version_file.is_file():
            raise _error(f"ONNX Runtime VERSION_NUMBER not found under {root}")
        actual_version = version_file.read_text(encoding="utf-8").strip()
        expected_base = expected_version.removesuffix("-gpu")
        if actual_version != expected_base:
            raise _error(f"ONNX Runtime version mismatch: expected {expected_base}, got {actual_version}")
        if profile == "cpu" and expected_version.endswith("-gpu"):
            raise _error("CPU profile cannot use an ONNX Runtime GPU archive")
        if profile == "nvidia" and not expected_version.endswith("-gpu"):
            raise _error("NVIDIA profile requires an ONNX Runtime GPU archive")
        if profile == "intel" and expected_version.endswith("-gpu"):
            raise _error("Intel profile cannot use an ONNX Runtime GPU archive")
        link_library = lib_dir / "libonnxruntime.so"
        if not link_library.exists():
            versioned = sorted(lib_dir.glob("libonnxruntime.so.*"))
            if not versioned:
                raise _error(f"ONNX Runtime link library not found under {lib_dir}")
            link_library.symlink_to(versioned[0].name)
        pc_dir = self._make_onnx_pc(root, expected_version, include_dir, lib_dir)
        self._prepend_pkg_config(str(pc_dir))
        return self._validate_onnx_artifact(profile, expected_base, include_dir, lib_dir, pc_dir)

    def _stage_onnx_archive(self, root: Path, archive: Path, expected_version: str, profile: str) -> Path:
        candidate = self._candidate(root.parent, f"onnxruntime-{expected_version}")
        try:
            self._extract_archive(archive, candidate)
            header = next(iter(candidate.rglob("onnxruntime_cxx_api.h")), None)
            if header is None:
                raise _error("ONNX Runtime archive has no expected header")
            extracted_root = header.parent.parent
            if extracted_root != candidate:
                normalized = self._candidate(root.parent, "onnxruntime-normalize")
                shutil.copytree(extracted_root, normalized, dirs_exist_ok=True)
                self._remove_path(candidate)
                candidate = normalized
            self._validate_onnx_layout(candidate, expected_version, profile)
            return candidate
        except BaseException:
            self._remove_path(candidate)
            raise

    def _prepare_local_onnx(self) -> DependencyResult:
        root = self.config.onnx_root
        expected = self.config.onnx_version.removesuffix("-gpu")
        if not root.is_dir():
            raise _error(f"local ONNX Runtime artifact not found: {root}")
        version_file = root / "VERSION_NUMBER"
        if not version_file.is_file():
            raise _error(f"local ONNX Runtime VERSION_NUMBER not found: {root}")
        actual = version_file.read_text(encoding="utf-8").strip()
        if actual != expected:
            raise _error(f"local ONNX Runtime version mismatch: expected {expected}, got {actual}")
        include_dir = root / "include"
        lib_dir = self._onnx_libdir(root)
        adapter_root = self.build_dir / "ssv-deps" / f"onnxruntime-{self.config.profile}"
        pc_dir = self._make_onnx_pc(adapter_root, expected, include_dir, lib_dir)
        self._prepend_pkg_config(str(pc_dir))
        result = self._validate_onnx_artifact(self.config.profile, expected, include_dir, lib_dir, pc_dir)
        return DependencyResult(
            source="local",
            version=result.version,
            pc_dir=result.pc_dir,
            runtime_dirs=result.runtime_dirs,
            providers=result.providers,
            provider_libraries=result.provider_libraries,
            include_dir=result.include_dir,
            lib_dir=result.lib_dir,
        )

    def _prepare_system_onnx(self) -> DependencyResult:
        profile = self.config.profile
        expected = self.config.onnx_version.removesuffix("-gpu")
        if self._pkg("--exists", "onnxruntime").returncode != 0:
            raise _error(f"system ONNX Runtime artifact is required for profile={profile}")
        pc_dir = Path(self._pkg_dir("onnxruntime"))
        project_paths = {(self.root / ".deps").resolve(), self.build_dir.resolve()}
        if any(pc_dir.is_relative_to(path) for path in project_paths):
            raise _error(f"system ONNX Runtime resolved to a project-local artifact: {pc_dir}")
        include_dir = Path(self._pkg_variable("onnxruntime", "includedir"))
        lib_dir = Path(self._pkg_variable("onnxruntime", "libdir"))
        if not include_dir.is_dir() or not lib_dir.is_dir():
            raise _error("system ONNX Runtime pkg-config must define existing includedir and libdir")
        return self._validate_onnx_artifact(profile, expected, include_dir, lib_dir, pc_dir)

    def _prepare_openvino(self, openvino_dir: Path) -> None:
        if (openvino_dir / "runtime/cmake/OpenVINOConfig.cmake").is_file() and (
            openvino_dir / "runtime/lib/intel64/libopenvino.so"
        ).is_file():
            return
        cache = self.root / ".deps" / "downloads" / "openvino" / OPENVINO_VERSION / OPENVINO_ARCHIVE
        self._download(OPENVINO_URL, cache)
        candidate = self._candidate(openvino_dir.parent, f"openvino-{OPENVINO_VERSION}")
        try:
            self._extract_archive(cache, candidate)
            config_file = next(iter(candidate.rglob("OpenVINOConfig.cmake")), None)
            if config_file is None:
                raise _error("OpenVINO archive has no OpenVINOConfig.cmake")
            extracted_root = config_file.parent.parent.parent
            if extracted_root != candidate:
                normalized = self._candidate(openvino_dir.parent, "openvino-normalize")
                shutil.copytree(extracted_root, normalized, dirs_exist_ok=True)
                self._remove_path(candidate)
                candidate = normalized
            if not (
                candidate / "runtime/cmake/OpenVINOConfig.cmake"
            ).is_file() or not (candidate / "runtime/lib/intel64/libopenvino.so").is_file():
                raise _error("OpenVINO archive layout is incomplete")
            self._atomic_replace(candidate, openvino_dir)
        except BaseException:
            self._remove_path(candidate)
            raise

    def _prepare_intel_onnx(self) -> DependencyResult:
        workspace = self.config.onnx_root
        source_parent = workspace / "source"
        source_dir = source_parent / f"onnxruntime-{DEFAULT_ONNXRUNTIME_VERSION}"
        build_dir = workspace / "build"
        install_dir = workspace / "install"
        openvino_dir = workspace / "openvino"
        if install_dir.is_dir():
            try:
                return self._validate_onnx_layout(install_dir, self.config.onnx_version, "intel")
            except DependencyError:
                pass
        self._require_replaceable(install_dir, lambda path: self._validate_onnx_layout(path, self.config.onnx_version, "intel"), description="ONNX Runtime install root")
        self._prepare_openvino(openvino_dir)
        self._require_command("cmake")
        self._require_command(sys.executable)
        compiler_value = self.environment.get("CXX", "")
        compiler = compiler_value.split()[0] if compiler_value else next((candidate for candidate in ("c++", "g++", "clang++") if shutil.which(candidate)), "")
        if not compiler:
            raise _error("ONNX Runtime source build requires a C++ compiler (c++, g++, or clang++)")
        self.environment["CXX"] = compiler
        if not (shutil.which("make") or shutil.which("ninja")):
            raise _error("ONNX Runtime source build requires make or ninja")
        if not source_dir.is_dir():
            cache = self.root / ".deps" / "downloads" / "onnxruntime" / DEFAULT_ONNXRUNTIME_VERSION / ORT_INTEL_ARCHIVE
            self._download(ORT_INTEL_URL, cache)
            candidate = self._candidate(source_parent, f"onnxruntime-source-{DEFAULT_ONNXRUNTIME_VERSION}")
            try:
                self._extract_archive(cache, candidate)
                extracted = candidate / f"onnxruntime-{DEFAULT_ONNXRUNTIME_VERSION}"
                if not (extracted / "tools/ci_build/build.py").is_file():
                    raise _error("ONNX Runtime source archive has no expected build.py")
                self._atomic_replace(extracted, source_dir)
                self._remove_path(candidate)
            except BaseException:
                self._remove_path(candidate)
                raise
        jobs = self._build_jobs("SSV_ONNXRUNTIME_BUILD_JOBS")
        build_candidate = self._candidate(build_dir.parent, f"onnxruntime-build-{DEFAULT_ONNXRUNTIME_VERSION}")
        install_candidate = self._candidate(install_dir.parent, f"onnxruntime-install-{DEFAULT_ONNXRUNTIME_VERSION}")
        command = [
            sys.executable,
            source_dir / "tools/ci_build/build.py",
            "--config", "Release",
            "--build_dir", build_candidate,
            "--use_openvino", "GPU",
            "--build_shared_lib",
            "--skip_tests",
            "--compile_no_warning_as_error",
            "--parallel", str(jobs),
            "--cmake_extra_defines",
            f"OpenVINO_DIR={openvino_dir / 'runtime/cmake'}",
            "FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER",
        ]
        if shutil.which("ninja"):
            command.extend(["--cmake_generator", "Ninja"])
        result = self._command(command)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise _error(f"ONNX Runtime OpenVINO source build failed: {detail[-4000:]}")
        config_build = build_candidate / "Release"
        include_dir = install_candidate / "include"
        lib_dir = install_candidate / "lib"
        include_dir.mkdir(parents=True, exist_ok=True)
        lib_dir.mkdir(parents=True, exist_ok=True)
        source_include = source_dir / "include/onnxruntime/core/session"
        if not source_include.is_dir():
            raise _error(f"ONNX Runtime source headers not found: {source_include}")
        shutil.copytree(source_include, include_dir, dirs_exist_ok=True)
        provider_include = source_dir / "include/onnxruntime/core/providers"
        (include_dir / "core/providers").mkdir(parents=True, exist_ok=True)
        shutil.copytree(provider_include, include_dir / "core/providers", dirs_exist_ok=True)
        for name in (
            "libonnxruntime.so*",
            "libonnxruntime_providers_shared.so*",
            "libonnxruntime_providers_openvino.so*",
        ):
            matches = list(config_build.glob(name))
            if not matches:
                raise _error(f"ONNX Runtime build artifact not found: {config_build / name}")
            for match in matches:
                target = lib_dir / match.name
                if match.is_symlink():
                    target.symlink_to(os.readlink(match))
                else:
                    shutil.copy2(match, target)
        (install_candidate / "VERSION_NUMBER").write_text(DEFAULT_ONNXRUNTIME_VERSION + "\n", encoding="utf-8")
        try:
            result = self._validate_onnx_layout(install_candidate, self.config.onnx_version, "intel")
            self._atomic_replace(build_candidate, build_dir)
            self._atomic_replace(install_candidate, install_dir)
            return self._validate_onnx_layout(install_dir, self.config.onnx_version, "intel")
        except BaseException:
            self._remove_path(build_candidate)
            self._remove_path(install_candidate)
            raise

    def _prepare_onnxruntime(self) -> DependencyResult:
        profile = self.config.profile
        if self.config.onnx_source == "system":
            return self._prepare_system_onnx()
        if self.config.onnx_source == "local":
            return self._prepare_local_onnx()
        if profile == "intel":
            return self._prepare_intel_onnx()
        root = self.config.onnx_root
        expected = self.config.onnx_version
        try:
            if root.is_dir():
                return self._validate_onnx_layout(root, expected, profile)
        except DependencyError:
            pass
        self._require_replaceable(root, lambda path: self._validate_onnx_layout(path, expected, profile), description="ONNX Runtime root")
        archive_name, url = self._onnx_archive_info(expected)
        cache = self.root / ".deps" / "downloads" / "onnxruntime" / expected / archive_name
        self._download(url, cache)
        try:
            candidate = self._stage_onnx_archive(root, cache, expected, profile)
        except DependencyError:
            warn(f"cached ONNX Runtime archive is invalid; downloading it once more: {archive_name}")
            cache.unlink(missing_ok=True)
            self._download(url, cache)
            candidate = self._stage_onnx_archive(root, cache, expected, profile)
        self._atomic_replace(candidate, root)
        return self._validate_onnx_layout(root, expected, profile)
