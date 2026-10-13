#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Generated-by: LLM
# Assisted-by: LLM
"""Exercise the production T8140 scanout-withdrawal helper on real libfdt blobs."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


class ExternalScanoutWithdrawal(unittest.TestCase):
    def test_real_fdt(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "src/kboot.c").read_text()
        start = source.index("/* A specifier occupies its provider phandle")
        end = source.index("/* Refusal aliases must identify the role", start)
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = shlex.split(os.environ.get("CPPFLAGS", ""))
        link = shlex.split(os.environ.get("LDFLAGS", ""))
        if sys.platform == "darwin" and Path("/opt/homebrew/include/libfdt.h").is_file():
            flags += ["-I/opt/homebrew/include"]
            link += ["-L/opt/homebrew/lib"]
        fixture = root / "tests/data/t8140-iommu-withdrawal.c"
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "helper.c").write_text(source[start:end])
            binary = work / "withdrawal-test"
            subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror"] + flags +
                           ["-I" + directory, str(fixture)] + link +
                           ["-lfdt", "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
            self.assertEqual(result.stdout.count("PASS "), 15, result.stdout)
            print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
