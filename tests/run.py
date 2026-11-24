#!/usr/bin/env python3
"""Test the portable kernels extracted from each standalone Windows source."""
import os
import pathlib
import shlex
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
compiler = shlex.split(os.environ.get("CXX", "c++"))
flags = shlex.split(os.environ.get("CXXFLAGS", "-std=c++17 -O2 -Wall -Wextra"))
begin = "// BEGIN TESTABLE PIXEL AND TRAIL KERNELS\n"
end = "// END TESTABLE PIXEL AND TRAIL KERNELS"

with tempfile.TemporaryDirectory(prefix="curmb-test-") as temp:
    temp = pathlib.Path(temp)
    for variant in ("curmb", "curmb_1", "curmb_2"):
        source = (root / f"{variant}.cpp").read_text()
        assert source.count(begin) == source.count(end) == 1
        assert '#include "' not in source, f"{variant}.cpp is not standalone"
        kernels = source.split(begin, 1)[1].split(end, 1)[0]
        (temp / "kernels.h").write_text("#pragma once\n#include <cstdint>\n#include <emmintrin.h>\n" + kernels)
        for test in ("blend_test", "trail_test"):
            binary = temp / test
            subprocess.run(compiler + flags + ["-I", str(temp), str(root / "tests" / f"{test}.cpp"), "-o", str(binary)], check=True)
            print(f"{variant}: {test}", flush=True)
            subprocess.run([str(binary)], check=True)
