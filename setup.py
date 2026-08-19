"""Build script for jdoc Python extension module."""

import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext

# Single source of truth for the version is CMakeLists.txt.
VERSION = re.search(
    r"^project\(jdoc VERSION (\d+\.\d+\.\d+)",
    (Path(__file__).parent / "CMakeLists.txt").read_text(),
    re.MULTILINE,
).group(1)


class CMakeExtension(Extension):
    def __init__(self, name, sourcedir=""):
        super().__init__(name, sources=[])
        self.sourcedir = os.fspath(Path(sourcedir).resolve())


class CMakeBuild(build_ext):
    def build_extension(self, ext):
        extdir = os.fspath(Path(self.get_ext_fullpath(ext.name)).parent.resolve())
        expected_name = Path(self.get_ext_fullpath(ext.name)).name
        # bdist_wheel collects files from build/lib. Remove only stale native
        # modules for this extension; otherwise a previous cpXY build can be
        # bundled alongside the current ABI even when CMake now builds the
        # correct file.
        for candidate in Path(extdir).glob(f"{ext.name}.*"):
            if (candidate.name != expected_name and
                    candidate.suffix.lower() in {".so", ".pyd", ".dylib"}):
                candidate.unlink()
        cfg = "Release"

        import pybind11
        cmake_args = [
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extdir}",
            # Multi-config generators (notably Visual Studio) otherwise place
            # the module in an extra Release/ directory where wheel assembly
            # does not find it.
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_{cfg.upper()}={extdir}",
            # Pass both modern FindPython and legacy spellings. The build
            # directory may contain a cache from another interpreter; without
            # the modern variable a cpXY wheel can accidentally contain an
            # extension carrying a different CPython ABI suffix.
            f"-DPython_EXECUTABLE={sys.executable}",
            f"-DPython3_EXECUTABLE={sys.executable}",
            f"-DPYTHON_EXECUTABLE={sys.executable}",
            f"-DPython_ROOT_DIR={sys.prefix}",
            f"-Dpybind11_DIR={pybind11.get_cmake_dir()}",
            f"-DCMAKE_BUILD_TYPE={cfg}",
            "-DBUILD_PYTHON=ON",
        ]
        # Honor extra -D flags from the environment (used by the macOS wheel
        # build to force the static libjpeg-turbo path).
        cmake_args += shlex.split(os.environ.get("CMAKE_ARGS", ""))

        build_args = ["--config", cfg, "--target", "_jdoc"]

        # Never reuse a CMake cache created for another CPython ABI.
        build_temp = (Path(self.build_temp) /
                      f"{ext.name}-{sys.implementation.cache_tag}")
        build_temp.mkdir(parents=True, exist_ok=True)

        subprocess.run(
            ["cmake", ext.sourcedir, *cmake_args],
            cwd=build_temp, check=True
        )
        subprocess.run(
            ["cmake", "--build", ".", *build_args, f"-j{os.cpu_count()}"],
            cwd=build_temp, check=True
        )


setup(
    version=VERSION,
    ext_modules=[CMakeExtension("_jdoc")],
    cmdclass={"build_ext": CMakeBuild},
    package_dir={"": "python"},
    packages=["jdoc"],
)
