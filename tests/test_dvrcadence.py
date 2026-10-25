#!/usr/bin/env python3
# Runs tools/bench/dvrcadence.py against the three fixtures that
# dvr_mux_writes_the_cadence_fixture (tests/test_dvr_mux.cpp) writes via
# DvrMux: a clean on-grid recording, one with a single off-grid hold, and
# one on a fractional period (16666.667us) stamped with llround.
# DEPENDS test_dvr_mux in tests/CMakeLists.txt makes that test run first;
# without it (e.g. `ctest -R test_dvrcadence` alone in a fresh build dir)
# the fixtures do not exist yet, so run() below fails with a clear message
# instead of handing subprocess a missing path and tracing back.
import os
import subprocess
import sys

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "bench",
                     "dvrcadence.py")


def run(path, *extra_args):
    if not os.path.exists(path):
        sys.exit(f"fixture missing: {path} (run ctest -R test_dvr_mux first)")
    return subprocess.run([sys.executable, TOOL, path, *extra_args],
                           capture_output=True, text=True)


def check_on_grid_histogram(out):
    assert "x1=3" in out, out
    assert "x2=1" in out, out
    assert "x3=1" in out, out


def check_fractional_histogram(out):
    assert "x1=5" in out, out
    assert "x4=1" in out, out
    assert "x10=1" in out, out


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: test_dvrcadence.py <clean.mp4> <offgrid.mp4> <fractional.mp4>")
    clean_path, offgrid_path, fractional_path = sys.argv[1], sys.argv[2], sys.argv[3]

    r = run(clean_path)
    assert r.returncode == 0, r.stdout + r.stderr
    check_on_grid_histogram(r.stdout)
    assert "off-grid: 0" in r.stdout, r.stdout
    assert "(inferred)" in r.stdout, r.stdout

    r = run(offgrid_path)
    assert r.returncode == 1, r.stdout + r.stderr
    check_on_grid_histogram(r.stdout)
    assert "off-grid: 1 at sample 5" in r.stdout, r.stdout
    assert "(inferred)" in r.stdout, r.stdout

    # Same file, grid pinned explicitly: same counts, "(given)" instead.
    r = run(offgrid_path, "--grid-us", "20000")
    assert r.returncode == 1, r.stdout + r.stderr
    check_on_grid_histogram(r.stdout)
    assert "off-grid: 1 at sample 5" in r.stdout, r.stdout
    assert "(given)" in r.stdout, r.stdout

    # Fractional period: durations of 16666/16667us per unit, holds of 4
    # and 10 units. The fitted grid accepts every one of them.
    r = run(fractional_path)
    assert r.returncode == 0, r.stdout + r.stderr
    check_fractional_histogram(r.stdout)
    assert "off-grid: 0" in r.stdout, r.stdout
    assert "(inferred)" in r.stdout, r.stdout
    grid = float(r.stdout.split(" grid ", 1)[1].split()[0])
    assert abs(grid - 16666.667) < 0.5, r.stdout

    # The same file against the operator's exact fractional period.
    r = run(fractional_path, "--grid-us", "16666.667")
    assert r.returncode == 0, r.stdout + r.stderr
    check_fractional_histogram(r.stdout)
    assert "off-grid: 0" in r.stdout, r.stdout
    assert "grid 16666.667 us (given)" in r.stdout, r.stdout

    print("ok")


if __name__ == "__main__":
    main()
