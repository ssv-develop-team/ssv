"""OpenCV system、local 和 managed provider 实现。"""

from __future__ import annotations

import os
import re
import shutil
from pathlib import Path

from .contracts import DependencyResult
from .policy import DEFAULT_OPENCV_VERSION, OPENCV_URL, DependencyError, _error


class OpenCvProvider:
    # ------------------------------------------------------------------
    # OpenCV

    _OPENCV_MODULES = ("core", "imgproc", "video", "calib3d", "features2d", "flann", "dnn")

    @staticmethod
    def _opencv_include_dir(root: Path) -> Path:
        matches = list(root.rglob("opencv2/core.hpp"))
        if not matches:
            raise _error(f"OpenCV headers not found under {root}")
        return matches[0].parent.parent.resolve()

    @staticmethod
    def _opencv_lib_dir(root: Path) -> Path:
        matches = list(root.rglob("libopencv_core.so")) + list(root.rglob("libopencv_core.so.*"))
        if not matches:
            raise _error(f"OpenCV libraries not found under {root}")
        return matches[0].parent.resolve()

    @staticmethod
    def _opencv_library(lib_dir: Path, module: str) -> Path:
        matches = [path for path in (lib_dir / f"libopencv_{module}.so", *lib_dir.glob(f"libopencv_{module}.so.*")) if path.exists()]
        if not matches:
            raise _error(f"OpenCV library missing: libopencv_{module}.so")
        return matches[0]

    def _validate_opencv_libraries(self, lib_dir: Path) -> None:
        libraries = [self._opencv_library(lib_dir, module) for module in self._OPENCV_MODULES]
        for library in libraries:
            self._validate_elf(library, "OpenCV library")
            dynamic = self._readelf("-d", library)
            needed = re.findall(r"Shared library: \[(libopencv_[^]]+)\]", dynamic)
            for dependency in needed:
                stem = dependency.split(".so", 1)[0]
                if not any(lib_dir.glob(f"{stem}.so")) and not any(lib_dir.glob(f"{stem}.so.*")):
                    raise _error(f"OpenCV runtime closure is incomplete: {library.name} needs {dependency}")
            self._ldd(library)

    def _make_opencv_pc(self, root: Path, version: str, include_dir: Path, lib_dir: Path, pc_dir: Path | None = None) -> Path:
        math_result = self._pkg("--libs", "cblas", "lapack", "blas")
        if math_result.returncode != 0:
            math_result = self._pkg("--libs", "lapack", "blas")
        if math_result.returncode != 0:
            raise _error("OpenCV pkg-config requires cblas/lapack/blas")
        target_dir = pc_dir or (root / "lib/pkgconfig")
        self._write_pc(
            target_dir / "opencv4.pc",
            (
                f"prefix={root}",
                "exec_prefix=${prefix}",
                f"libdir={lib_dir}",
                f"includedir={include_dir}",
                "",
                "Name: opencv4",
                "Description: OpenCV runtime",
                f"Version: {version}",
                "Libs: -L${libdir} -Wl,-rpath-link,${libdir} -lopencv_calib3d -lopencv_video -lopencv_features2d -lopencv_flann -lopencv_imgproc -lopencv_core " + math_result.stdout.strip(),
                'Cflags: -I"${includedir}"',
            ),
        )
        return target_dir.resolve()

    def _probe_opencv(self, expected_version: str, lib_dir: Path, runtime_dirs: str) -> None:
        source = f'''#include <opencv2/core.hpp>
#include <string>
int main() {{
    return std::string(CV_VERSION) == "{expected_version}" &&
                   cv::getVersionString() == "{expected_version}" ? 0 : 1;
}}
'''
        self._compile_probe(source, "opencv4", runtime_dirs)

    def _validate_opencv_layout(self, root: Path, expected_version: str) -> DependencyResult:
        include_dir = self._opencv_include_dir(root)
        lib_dir = self._opencv_lib_dir(root)
        self._validate_opencv_libraries(lib_dir)
        pc_dir = self._make_opencv_pc(root, expected_version, include_dir, lib_dir)
        self._prepend_pkg_config(str(pc_dir))
        actual_pc_dir = Path(self._pkg_dir("opencv4"))
        if actual_pc_dir != pc_dir:
            raise _error(f"OpenCV pkg-config source mismatch: {actual_pc_dir}")
        runtime_dirs = self._runtime_dirs(lib_dir)
        self._probe_opencv(expected_version, lib_dir, runtime_dirs)
        actual_version = self._pkg_version("opencv4")
        return DependencyResult(
            source=self.config.opencv_source,
            version=actual_version,
            pc_dir=str(actual_pc_dir),
            runtime_dirs=runtime_dirs,
            include_dir=str(include_dir),
            lib_dir=str(lib_dir),
        )

    def _prepare_local_opencv(self) -> DependencyResult:
        assert self.config.opencv_include_dir is not None
        assert self.config.opencv_lib_dir is not None
        requested_include = self.config.opencv_include_dir
        include_dir = requested_include if (requested_include / "opencv2/core.hpp").is_file() else requested_include / "opencv4"
        if not (include_dir / "opencv2/core.hpp").is_file():
            raise _error(f"local OpenCV headers not found under: {requested_include}")
        lib_dir = self.config.opencv_lib_dir.resolve()
        self._validate_opencv_libraries(lib_dir)
        root = self.config.opencv_root / "local"
        pc_dir = self._make_opencv_pc(root, DEFAULT_OPENCV_VERSION, include_dir, lib_dir, root / "lib/pkgconfig")
        self._prepend_pkg_config(str(pc_dir))
        runtime_dirs = self._runtime_dirs(lib_dir)
        self._probe_opencv(DEFAULT_OPENCV_VERSION, lib_dir, runtime_dirs)
        return DependencyResult(
            source="local",
            version=self._pkg_version("opencv4"),
            pc_dir=str(Path(self._pkg_dir("opencv4"))),
            runtime_dirs=runtime_dirs,
            include_dir=str(include_dir.resolve()),
            lib_dir=str(lib_dir),
        )

    def _opencv_jobs(self) -> int:
        return self._build_jobs("SSV_OPENCV_BUILD_JOBS")

    def _build_jobs(self, variable: str) -> int:
        value = self.environment.get(variable, "")
        if not value:
            value = str(os.cpu_count() or 1)
        if not value.isdecimal() or int(value) <= 0:
            raise _error(f"{variable} must be a positive integer")
        return int(value)

    def _prepare_managed_opencv(self) -> DependencyResult:
        workspace = self.config.opencv_root
        source_parent = workspace / "source"
        source_dir = source_parent / f"opencv-{DEFAULT_OPENCV_VERSION}"
        build_dir = workspace / "build"
        install_dir = workspace / "install"
        if install_dir.is_dir():
            try:
                return self._validate_opencv_layout(install_dir, DEFAULT_OPENCV_VERSION)
            except DependencyError:
                pass
        self._require_replaceable(install_dir, lambda path: self._validate_opencv_layout(path, DEFAULT_OPENCV_VERSION), description="OpenCV install root")
        if not source_dir.is_dir():
            cache = self.root / ".deps" / "downloads" / "opencv" / DEFAULT_OPENCV_VERSION / f"opencv-{DEFAULT_OPENCV_VERSION}.tar.gz"
            self._download(OPENCV_URL.format(version=DEFAULT_OPENCV_VERSION), cache)
            candidate = self._candidate(source_parent, f"opencv-source-{DEFAULT_OPENCV_VERSION}")
            try:
                self._extract_archive(cache, candidate)
                extracted = candidate / f"opencv-{DEFAULT_OPENCV_VERSION}"
                if not (extracted / "CMakeLists.txt").is_file():
                    raise _error("OpenCV source archive has no expected CMakeLists.txt")
                self._atomic_replace(extracted, source_dir)
                self._remove_path(candidate)
            except BaseException:
                self._remove_path(candidate)
                raise
        self._require_command("cmake")
        if not (shutil.which("make") or shutil.which("ninja")):
            raise _error("OpenCV source build requires make or ninja")
        compiler_value = self.environment.get("CXX", "")
        compiler = compiler_value.split()[0] if compiler_value else next((candidate for candidate in ("c++", "g++", "clang++") if shutil.which(candidate)), "")
        if not compiler:
            raise _error("OpenCV source build requires a C++ compiler (c++, g++, or clang++)")
        self.environment["CXX"] = compiler
        build_candidate = self._candidate(build_dir.parent, f"opencv-build-{DEFAULT_OPENCV_VERSION}")
        install_candidate = self._candidate(install_dir.parent, f"opencv-install-{DEFAULT_OPENCV_VERSION}")
        generator = ["-G", "Ninja"] if not shutil.which("make") and shutil.which("ninja") else []
        cmake_options = [
            "-S", source_dir,
            "-B", build_candidate,
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_INSTALL_PREFIX={install_dir}",
            "-DCMAKE_INSTALL_LIBDIR=lib",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            "-DBUILD_opencv_core=ON", "-DBUILD_opencv_imgproc=ON", "-DBUILD_opencv_video=ON",
            "-DBUILD_opencv_calib3d=ON", "-DBUILD_opencv_features2d=ON", "-DBUILD_opencv_flann=ON",
            "-DBUILD_opencv_dnn=ON", "-DBUILD_opencv_imgcodecs=OFF", "-DBUILD_opencv_videoio=OFF",
            "-DBUILD_opencv_apps=OFF", "-DBUILD_opencv_gapi=OFF", "-DBUILD_opencv_highgui=OFF",
            "-DBUILD_opencv_ml=OFF", "-DBUILD_opencv_objdetect=OFF", "-DBUILD_opencv_photo=OFF",
            "-DBUILD_opencv_stitching=OFF", "-DBUILD_opencv_ts=OFF", "-DBUILD_PROTOBUF=ON",
            "-DBUILD_TESTS=OFF", "-DBUILD_PERF_TESTS=OFF", "-DBUILD_EXAMPLES=OFF", "-DBUILD_DOCS=OFF",
            "-DBUILD_JAVA=OFF", "-DBUILD_OBJC=OFF", "-DWITH_EIGEN=OFF", "-DWITH_1394=OFF",
            "-DWITH_FFMPEG=OFF", "-DWITH_GSTREAMER=OFF", "-DWITH_GTK=OFF", "-DWITH_CUDA=OFF",
            "-DWITH_CUDNN=OFF", "-DWITH_IPP=OFF", "-DWITH_ITT=OFF", "-DWITH_JPEG=OFF",
            "-DWITH_PNG=OFF", "-DWITH_TIFF=OFF", "-DWITH_WEBP=OFF", "-DWITH_OPENEXR=OFF",
            "-DWITH_OPENCL=OFF", "-DWITH_OPENGL=OFF", "-DWITH_OPENMP=OFF", "-DWITH_V4L=OFF",
            "-DWITH_VULKAN=OFF", "-DOPENCV_GENERATE_PKGCONFIG=OFF", "-DCPU_BASELINE=DETECT", "-DCV_TRACE=OFF",
        ]
        configure = self._command(["cmake", *generator, *cmake_options])
        if configure.returncode != 0:
            detail = (configure.stderr or configure.stdout).strip()
            raise _error(f"OpenCV CMake configuration failed: {detail[-4000:]}")
        build = self._command(["cmake", "--build", build_candidate, "--parallel", str(self._opencv_jobs())])
        if build.returncode != 0:
            detail = (build.stderr or build.stdout).strip()
            raise _error(f"OpenCV compilation failed: {detail[-4000:]}")
        install = self._command(["cmake", "--install", build_candidate, "--prefix", install_candidate])
        if install.returncode != 0:
            detail = (install.stderr or install.stdout).strip()
            raise _error(f"OpenCV installation failed: {detail[-4000:]}")
        try:
            self._validate_opencv_layout(install_candidate, DEFAULT_OPENCV_VERSION)
            self._atomic_replace(build_candidate, build_dir)
            self._atomic_replace(install_candidate, install_dir)
            return self._validate_opencv_layout(install_dir, DEFAULT_OPENCV_VERSION)
        except BaseException:
            self._remove_path(build_candidate)
            self._remove_path(install_candidate)
            raise

    def _prepare_opencv(self) -> DependencyResult:
        if self.config.opencv_mode == "disabled":
            return DependencyResult(source="disabled", version="disabled")
        if self.config.opencv_source == "system":
            return self._system_result("opencv4", "4.5")
        if self.config.opencv_source == "local":
            return self._prepare_local_opencv()
        return self._prepare_managed_opencv()
