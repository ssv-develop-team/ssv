"""TensorRT、cuDNN 和 CUDA provider 实现。"""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

from .contracts import DependencyResult
from .policy import (
    TENSORRT_CUDA_MAJOR,
    TENSORRT_CUDNN_REVISION,
    TENSORRT_MANAGED_VERSION,
    TENSORRT_PACKAGE_REVISION,
    TENSORRT_REPOSITORY,
    DependencyError,
    _error,
    join_unique,
)


class TensorRtProvider:
    # ------------------------------------------------------------------
    # TensorRT and CUDA

    @staticmethod
    def _tensorrt_manifest() -> tuple[tuple[str, str], ...]:
        return (
            (
                f"libnvinfer-headers-dev_{TENSORRT_PACKAGE_REVISION}_amd64.deb",
                "4945a01b9be143091c89fd8366224159cd01648c25befabe7e1271ff6b9774ca",
            ),
            (
                f"libnvinfer10_{TENSORRT_PACKAGE_REVISION}_amd64.deb",
                "8232ccdc8be82815411879a589ba08df039b9f01a997effdb3f60d3b8a05e6bd",
            ),
            (
                f"libnvonnxparsers10_{TENSORRT_PACKAGE_REVISION}_amd64.deb",
                "1f70eb168ec0c5a2714585e2458464476d6529ef11fc54cad51fac2396a09e6f",
            ),
            (
                f"libcudnn9-cuda-13_{TENSORRT_CUDNN_REVISION}_amd64.deb",
                "5b2ef35d332c903fb81f000c883c36f41fd57de3972aa82e007f944478db0099",
            ),
        )

    def _tensorrt_find_library(self, root: Path, soname: str) -> Path:
        stem = soname.split(".so", 1)[0]
        matches: list[Path] = []
        for candidate in root.rglob(f"{stem}.so*"):
            if not candidate.is_file():
                continue
            try:
                dynamic = self._readelf("-d", candidate)
            except DependencyError:
                continue
            if re.search(rf"Library soname: \[{re.escape(soname)}\]", dynamic):
                resolved = candidate.resolve()
                if resolved not in matches:
                    matches.append(resolved)
        if len(matches) > 1:
            raise _error(f"multiple NVIDIA libraries provide {soname} under {root}")
        if not matches:
            raise _error(f"NVIDIA runtime {soname} not found under {root}")
        return matches[0]

    @staticmethod
    def _macro_value(header: Path, macro: str) -> str:
        values: dict[str, str] = {}
        for raw_line in header.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = raw_line.split(maxsplit=2)
            if len(parts) == 3 and parts[0] == "#define":
                # NVIDIA's Enterprise headers annotate aliased macros with //!< comments.
                values[parts[1]] = parts[2].split("//", 1)[0].strip()
        value = values.get(macro, "")
        for _ in range(5):
            if value.isdecimal():
                return value
            if not value or value not in values:
                return ""
            value = values[value]
        return ""

    def _tensorrt_version(self, header: Path) -> str:
        major = self._macro_value(header, "NV_TENSORRT_MAJOR")
        minor = self._macro_value(header, "NV_TENSORRT_MINOR")
        patch = self._macro_value(header, "NV_TENSORRT_PATCH")
        if major and minor and patch:
            return f"{major}.{minor}.{patch}"
        encoded = self._macro_value(header, "NV_TENSORRT_VERSION_INT")
        if encoded.isdecimal():
            number = int(encoded)
            return f"{number // 10000}.{(number // 100) % 100}.{number % 100}"
        raise _error(f"unable to read TensorRT version from {header}")

    def _cuda_version(self, header: Path) -> str:
        encoded = self._macro_value(header, "CUDART_VERSION")
        if not encoded.isdecimal():
            raise _error(f"unable to read CUDA Runtime version from {header}")
        number = int(encoded)
        return f"{number // 1000}.{(number % 1000) // 10}"

    def _locate_cuda(self, root: Path) -> tuple[Path, Path]:
        candidates: list[Path] = []
        if self.config.cuda_home is not None:
            candidates.append(self.config.cuda_home)
        candidates.extend([root, Path("/usr/local/cuda"), *sorted(Path("/usr/local").glob("cuda-*"))])
        for candidate in candidates:
            if not candidate.is_dir():
                continue
            header = next(iter(candidate.rglob("cuda_runtime_api.h")), None)
            library = next(iter(candidate.rglob("libcudart.so")), None)
            if header is not None and library is not None:
                return header.parent.resolve(), library.parent.resolve()
        raise _error("CUDA Runtime (cuda_runtime_api.h and libcudart.so) not found for TensorRT")

    def _make_tensorrt_pc(
        self,
        root: Path,
        version: str,
        include_dir: Path,
        lib_dir: Path,
        cuda_include: Path,
        cuda_lib: Path,
        pc_dir: Path | None = None,
    ) -> Path:
        target = pc_dir or root / "lib/pkgconfig"
        self._write_pc(
            target / "nvinfer.pc",
            (
                f"prefix={root}",
                "exec_prefix=${prefix}",
                f"libdir={lib_dir}",
                f"includedir={include_dir}",
                f"cudaincludedir={cuda_include}",
                f"cudalibdir={cuda_lib}",
                "",
                "Name: nvinfer",
                "Description: NVIDIA TensorRT and CUDA Runtime",
                f"Version: {version}",
                "Libs: -L${libdir} -l:libnvinfer.so.10 -L${cudalibdir} -lcudart",
                "Cflags: -I${includedir} -I${cudaincludedir}",
            ),
        )
        return target.resolve()

    def _validate_tensorrt_layout(self, root: Path) -> DependencyResult:
        self._require_command("readelf")
        self._require_command("ldd")
        header = self._find_unique(root, "NvInfer.h", description="TensorRT NvInfer.h")
        version_header = self._find_unique(root, "NvInferVersion.h", description="TensorRT NvInferVersion.h")
        version = self._tensorrt_version(version_header)
        if not version.startswith("10."):
            raise _error(f"NVIDIA runtime requires TensorRT ABI major 10, got {version}")
        nvinfer = self._tensorrt_find_library(root, "libnvinfer.so.10")
        nvonnxparser = self._tensorrt_find_library(root, "libnvonnxparser.so.10")
        cudnn = self._tensorrt_find_library(root, "libcudnn.so.9")
        cuda_include, cuda_lib = self._locate_cuda(root)
        cuda_version = self._cuda_version(cuda_include / "cuda_runtime_api.h")
        if not cuda_version.startswith(f"{TENSORRT_CUDA_MAJOR}."):
            raise _error(f"NVIDIA runtime requires CUDA ABI major {TENSORRT_CUDA_MAJOR}, got {cuda_version}")
        cudart = self._tensorrt_find_library(cuda_lib, f"libcudart.so.{TENSORRT_CUDA_MAJOR}")
        include_dir = header.parent.resolve()
        lib_dir = nvinfer.parent.resolve()
        pc_dir = self._make_tensorrt_pc(root, version, include_dir, lib_dir, cuda_include, cuda_lib)
        self._prepend_pkg_config(str(pc_dir))
        actual_pc_dir = Path(self._pkg_dir("nvinfer"))
        if actual_pc_dir != pc_dir:
            raise _error(f"TensorRT pkg-config source mismatch: {actual_pc_dir}")
        self._pkg_at_least("nvinfer", version)
        runtime_dirs = self._runtime_dirs(lib_dir, nvonnxparser.parent, cudnn.parent, cuda_lib)
        for library in (nvinfer, nvonnxparser, cudnn, cudart):
            environment = dict(self.environment)
            environment["LD_LIBRARY_PATH"] = join_unique(runtime_dirs, environment.get("LD_LIBRARY_PATH", ""))
            result = self._command(["ldd", "-r", library], env=environment)
            output = (result.stdout or "") + (result.stderr or "")
            if result.returncode != 0 or "not found" in output or "undefined symbol:" in output:
                raise _error(f"NVIDIA runtime closure is incomplete for {library.name}: {output.strip()}")
        self._compile_probe(
            """#include <NvInfer.h>
#include <NvInferVersion.h>
#include <cuda_runtime_api.h>
int main() {
    int cuda_version = 0;
    if (cudaRuntimeGetVersion(&cuda_version) != cudaSuccess)
        return 1;
    return getInferLibVersion() > 0 && cuda_version >= 13000 && cuda_version < 14000 ? 0 : 1;
}
""",
            "nvinfer",
            runtime_dirs,
        )
        return DependencyResult(
            source=self.config.tensorrt_source,
            version=version,
            pc_dir=str(actual_pc_dir),
            runtime_dirs=runtime_dirs,
        )

    def _install_managed_tensorrt(self, destination: Path) -> None:
        self._require_command("dpkg-deb")
        cache_dir = self.root / ".deps" / "downloads" / "tensorrt" / "10.16.1-cuda13.2-cudnn9"
        for package, expected_hash in self._tensorrt_manifest():
            cache_file = cache_dir / package
            url = f"{TENSORRT_REPOSITORY}/{package}"
            self._download(url, cache_file)
            digest = hashlib.sha256(cache_file.read_bytes()).hexdigest()
            if digest != expected_hash:
                cache_file.unlink(missing_ok=True)
                raise _error(f"NVIDIA package checksum mismatch: {package}")
            check = self._command(["dpkg-deb", "--info", cache_file])
            if check.returncode != 0:
                raise _error(f"NVIDIA package is not a readable Debian archive: {package}")
            result = self._command(["dpkg-deb", "--extract", cache_file, destination])
            if result.returncode != 0:
                raise _error(f"failed to extract NVIDIA package: {package}")

    def _prepare_managed_tensorrt(self) -> DependencyResult:
        root = self.config.tensorrt_root
        archive = self.config.tensorrt_archive
        url = self.config.tensorrt_url
        if archive is None and url is None and root.is_dir():
            try:
                result = self._validate_tensorrt_layout(root)
                if result.version != TENSORRT_MANAGED_VERSION:
                    raise _error(f"managed TensorRT version mismatch: expected {TENSORRT_MANAGED_VERSION}, got {result.version}")
                return result
            except DependencyError:
                pass
        self._require_replaceable(root, self._validate_tensorrt_layout, description="TensorRT root")
        candidate = self._candidate(root.parent, "tensorrt")
        try:
            if archive is None and url is None:
                self._install_managed_tensorrt(candidate)
            else:
                source_archive = archive
                if url is not None:
                    identity = hashlib.sha256(url.encode()).hexdigest()
                    filename = url.rsplit("/", 1)[-1].split("?", 1)[0]
                    source_archive = self.root / ".deps" / "downloads" / "tensorrt" / f"{identity}-{filename}"
                    self._download(url, source_archive)
                assert source_archive is not None
                if not source_archive.is_file():
                    raise _error(f"TensorRT archive does not exist: {source_archive}")
                self._extract_archive(source_archive, candidate)
            result = self._validate_tensorrt_layout(candidate)
            if result.version != TENSORRT_MANAGED_VERSION:
                raise _error(f"managed TensorRT version mismatch: expected {TENSORRT_MANAGED_VERSION}, got {result.version}")
            self._atomic_replace(candidate, root)
            return self._validate_tensorrt_layout(root)
        except BaseException:
            self._remove_path(candidate)
            raise

    def _prepare_tensorrt(self) -> DependencyResult:
        if self.config.tensorrt_mode == "disabled":
            return DependencyResult(source="disabled", version="disabled")
        if self.config.tensorrt_mode == "auto" and self.config.tensorrt_archive is None and self.config.tensorrt_url is None:
            if self.config.tensorrt_source == "managed":
                if not self.config.tensorrt_root.exists() or self._is_empty_directory(self.config.tensorrt_root):
                    return DependencyResult(source="managed", version="unavailable")
                self._require_replaceable(
                    self.config.tensorrt_root,
                    self._validate_tensorrt_layout,
                    description="TensorRT root",
                )
                try:
                    result = self._validate_tensorrt_layout(self.config.tensorrt_root)
                except DependencyError:
                    return DependencyResult(source="managed", version="unavailable")
                if not result.version.startswith("10."):
                    return DependencyResult(source="managed", version="unavailable")
            elif self._pkg("--exists", "nvinfer").returncode != 0:
                return DependencyResult(source="system", version="unavailable")
        try:
            if self.config.tensorrt_source == "managed":
                result = self._prepare_managed_tensorrt()
            else:
                result = self._system_result("nvinfer", "1.0")
        except DependencyError:
            if self.config.tensorrt_mode == "auto" and self.config.tensorrt_archive is None and self.config.tensorrt_url is None:
                return DependencyResult(source=self.config.tensorrt_source, version="unavailable")
            raise _error(
                f"TensorRT is unavailable for mode={self.config.tensorrt_mode} source={self.config.tensorrt_source}"
            )
        if self.config.tensorrt_mode == "auto" and not result.version.startswith("10."):
            return DependencyResult(source=self.config.tensorrt_source, version="unavailable")
        if self.config.tensorrt_mode == "enabled" and not result.version.startswith("10."):
            raise _error(f"NVIDIA runtime requires TensorRT ABI major 10, got {result.version}")
        return result
