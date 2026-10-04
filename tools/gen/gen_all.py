"""Run all generators of tools/gen (pass --check to verify only). Used by the CMake target fut_gen."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
GENERATORS = ["gen_kernel_table.py", "gen_fs_patches.py", "gen_functions.py"]


def main():
    extra = sys.argv[1:]
    rc = 0
    for g in GENERATORS:
        r = subprocess.run([sys.executable, str(HERE / g), *extra], cwd=HERE)
        rc = rc or r.returncode
    sys.exit(rc)


if __name__ == "__main__":
    main()
