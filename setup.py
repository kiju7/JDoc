"""Build script for jdoc Python extension module."""

import os
import subprocess
import sys
from pathlib import Path

from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext


class CMakeExtension(Extension):
    def __init__(self, name, sourcedir=""):
        super().__init__(name, sources=[])
        self.sourcedir = os.fspath(Path(sourcedir).resolve())


class CMakeBuild(build_ext):
    def build_extension(self, ext):
        extdir = os.fspath(Path(self.get_ext_fullpath(ext.name)).parent.resolve())
        cfg = "Release"

        cmake_args = [
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extdir}",
            f"-DPYTHON_EXECUTABLE={sys.executable}",
            f"-DCMAKE_BUILD_TYPE={cfg}",
            "-DBUILD_PYTHON=ON",
        ]

        build_args = ["--config", cfg, "--target", "_jdoc"]

        build_temp = Path(self.build_temp) / ext.name
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
    ext_modules=[CMakeExtension("_jdoc")],
    cmdclass={"build_ext": CMakeBuild},
    package_dir={"": "python"},
    packages=["jdoc"],
)
