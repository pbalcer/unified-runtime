# Copyright (C) 2024 Intel Corporation
# Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.TXT
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from utils.utils import git_clone
from .base import Benchmark
from .result import Result
from utils.utils import run
import os
import re

class VelocityBench:
    def __init__(self, directory):
        self.directory = directory
        self.repo_path = git_clone(self.directory, "velocity-bench-repo", "https://github.com/oneapi-src/Velocity-Bench.git", "7e440b794faac36e1bfd33831c021626e614b936")

class VelocityBase(Benchmark):
    def __init__(self, name: str, bin_name: str, vb: VelocityBench):
        self.vb = vb
        self.bench_name = name
        self.bin_name = bin_name
        super().__init__(vb.directory)

    def setup(self):
        code_path = os.path.join(self.vb.repo_path, self.bench_name, 'SYCL')
        build_path = self.create_build_path(self.bench_name)

        configure_command = [
            "cmake",
            f"-B {build_path}",
            f"-S {code_path}",
            f"-DCMAKE_BUILD_TYPE=Release"
        ]
        run(configure_command, {'CXX':'clang++'}, add_sycl=True)
        run(f"cmake --build {build_path} -j", add_sycl=True)

        self.benchmark_bin = os.path.join(build_path, self.bin_name)

    def bin_args(self) -> list[str]:
        return []

    def parse_output(self, stdout: str) -> float:
        raise NotImplementedError()

    def run(self, env_vars) -> list[Result]:
        command = [
            f"{self.benchmark_bin}",
        ]
        command += self.bin_args()

        result = self.run_bench(command, env_vars)

        return [Result(label=self.bench_name, value=self.parse_output(result), command=command, env=env_vars, stdout=result)]

    def teardown(self):
        return
