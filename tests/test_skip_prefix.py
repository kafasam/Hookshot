"""Compile and run the program-key prefix list helper (no game/GPU required): what RX_VK_SKIP_PROG and the first person
head hide's HOS_FP_HEAD_HIDE_KEYS parse their lists with (gpu/vk/skip_prefix.h, vk_draw.cpp).

python -I -B tests/test_skip_prefix.py [--cxx ...]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="skip-prefix-") as tmp:
        exe = Path(tmp) / "skip-prefix"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_skip_prefix.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
