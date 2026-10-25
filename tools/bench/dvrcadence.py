#!/usr/bin/env python3
"""Checks a burned DVR fMP4's sample-duration cadence against a grid.

Written by mabur's DvrMux (common/include/mabur/dvr_mux.h): one hvc1
track, mdhd timescale, and a moof/mdat pair per recording fragment whose
trun carries each sample's duration (or, for a fragment whose trun leaves
the per-sample flag off, tfhd's default_sample_duration for the whole
fragment -- DvrMux itself never does this, but the parser below follows
the general ISOBMFF rule rather than this muxer's current shape). The
burned recorder is meant to land every sample on an integer multiple of
one vblank period ("the grid"); this tool is the outside-the-process gate
for that claim, the same role tools/bench/ausniff.py plays for the AU
ring.

Usage:
  python3 dvrcadence.py FILE.mp4 [--grid-us US]

The recorder stamps sample k at llround(k * period) us, and a vblank
period is rarely a whole number of microseconds (1080p60 CEA:
16666.667 us), so one grid unit measures 16666 or 16667 us and a hold of
n units is n * period within the rounding of its two endpoints. The grid
is therefore fitted, not read off one sample: it starts at the minimum
measured duration, each duration takes k = round(d / grid) units, and the
grid becomes sum(d) / sum(k) over the durations within a twentieth of a
period of a whole multiple (a genuine miss, half a period out, is left
out of the fit), repeated until it settles. --grid-us US skips the fit
and uses the period given (fractional allowed, e.g. 16666.667).

A duration counts as on grid when it is within 2 ticks of the file's
timescale (2 us for DvrMux's microsecond timescale) of k grid periods:
one tick for the rounding of its two endpoints together, one more for
the fitted grid's own error, which over a hold of k units is at most
k / K ticks with K the units fitted (never more than one). Anything
else is reported under "off-grid" together with the (0-based) index of
the sample whose duration missed.

The file's LAST sample has no successor to measure a duration against --
its trun entry only exists because some players require a nonzero one --
so it is parsed (to walk the box tree correctly) but excluded from every
count this tool prints.

Exit 0 when every measured duration is on grid, 1 otherwise.
"""
import argparse
import struct
import sys


def read_u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def read_u64(buf, off):
    return struct.unpack_from(">Q", buf, off)[0]


def iter_boxes(buf, begin, end):
    """Yields (type, payload_off, box_end) for a flat run of boxes in
    [begin, end). Does not recurse -- callers narrow the range themselves
    (skipping any FullBox header first) and call again for a container."""
    p = begin
    while p + 8 <= end:
        size = read_u32(buf, p)
        header = 8
        if size == 1:
            if p + 16 > end:
                sys.exit(f"truncated largesize box at offset {p}")
            size = read_u64(buf, p + 8)
            header = 16
        elif size == 0:
            size = end - p  # box extends to the end of its parent
        if size < header or p + size > end:
            sys.exit(f"bad box size {size} at offset {p}")
        box_type = buf[p + 4:p + 8].decode("latin1")
        yield box_type, p + header, p + size
        p += size
    if p != end:
        sys.exit(f"trailing/truncated bytes after the last box ({end - p})")


def find_box(buf, begin, end, want):
    for t, poff, pend in iter_boxes(buf, begin, end):
        if t == want:
            return poff, pend
    return None


def find_path(buf, begin, end, path):
    off, e = begin, end
    for name in path:
        found = find_box(buf, off, e, name)
        if found is None:
            return None
        off, e = found
    return off, e


def parse_mdhd_timescale(buf, moov_off, moov_end):
    found = find_path(buf, moov_off, moov_end, ["trak", "mdia", "mdhd"])
    if found is None:
        sys.exit("no moov/trak/mdia/mdhd box -- not a DvrMux file?")
    off, _ = found
    version = buf[off]
    # mdhd is a FullBox: version(1) + flags(3), then two times (4 or 8
    # bytes each depending on version) before timescale.
    return read_u32(buf, off + (20 if version == 1 else 12))


def parse_tfhd_default_duration(buf, off, end):
    flags = read_u32(buf, off) & 0xFFFFFF
    p = off + 8  # version/flags(4) + track_ID(4)
    if flags & 0x000001:  # base-data-offset-present
        p += 8
    if flags & 0x000002:  # sample-description-index-present
        p += 4
    if flags & 0x000008:  # default-sample-duration-present
        return read_u32(buf, p)
    return None


def parse_trun_durations(buf, off, default_duration):
    flags = read_u32(buf, off) & 0xFFFFFF
    sample_count = read_u32(buf, off + 4)
    p = off + 8
    if flags & 0x000001:  # data-offset-present
        p += 4
    if flags & 0x000004:  # first-sample-flags-present
        p += 4
    per_sample_duration = flags & 0x000100
    has_size = flags & 0x000200
    has_flags = flags & 0x000400
    has_cts = flags & 0x000800

    durations = []
    for _ in range(sample_count):
        if per_sample_duration:
            durations.append(read_u32(buf, p))
            p += 4
        else:
            if default_duration is None:
                sys.exit("trun sample has no duration: no per-sample flag and no tfhd default")
            durations.append(default_duration)
        if has_size:
            p += 4
        if has_flags:
            p += 4
        if has_cts:
            p += 4
    return durations


def read_durations(buf):
    """Returns (timescale, [duration, ...]) in file (sample) order."""
    end = len(buf)
    timescale = None
    durations = []
    for t, poff, pend in iter_boxes(buf, 0, end):
        if t == "moov":
            timescale = parse_mdhd_timescale(buf, poff, pend)
        elif t == "moof":
            traf = find_box(buf, poff, pend, "traf")
            if traf is None:
                continue
            traf_off, traf_end = traf
            tfhd = find_box(buf, traf_off, traf_end, "tfhd")
            default_duration = (parse_tfhd_default_duration(buf, tfhd[0], tfhd[1])
                                 if tfhd is not None else None)
            trun = find_box(buf, traf_off, traf_end, "trun")
            if trun is not None:
                durations.extend(parse_trun_durations(buf, trun[0], default_duration))
    if timescale is None:
        sys.exit("no mdhd timescale found -- not a DvrMux file?")
    return timescale, durations


def to_us(duration, timescale):
    if timescale == 1_000_000:
        return duration
    return duration * 1_000_000 / timescale


def fit_grid(durations_us, grid_us, tolerance_us):
    """Least-squares-free grid fit: sum(d) / sum(k) over the durations
    near a whole multiple of the current grid, until it stops moving.
    Summing whole durations makes the endpoint rounding telescope, so the
    fitted period is off by at most one tick over all K units fitted."""
    for _ in range(8):
        near = max(tolerance_us, grid_us / 20)
        sum_d = 0.0
        sum_k = 0
        for d in durations_us:
            k = round(d / grid_us)
            if k >= 1 and abs(d - k * grid_us) <= near:
                sum_d += d
                sum_k += k
        if sum_k == 0:
            break
        refined = sum_d / sum_k
        if abs(refined - grid_us) < 1e-6:
            grid_us = refined
            break
        grid_us = refined
    return grid_us


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--grid-us", type=float, default=None,
                     help="grid period in us, fractional allowed (default: fitted to the "
                          "file's durations)")
    args = ap.parse_args()

    try:
        with open(args.file, "rb") as f:
            buf = f.read()
    except OSError as e:
        sys.exit(f"{args.file}: {e.strerror}")
    if not buf:
        sys.exit(f"{args.file}: empty file")

    timescale, raw_durations = read_durations(buf)
    total_samples = len(raw_durations)
    if total_samples == 0:
        sys.exit(f"{args.file}: no samples found")

    # Drop the last sample's entry: it has no successor to be measured
    # against (see module docstring).
    durations_us = [to_us(d, timescale) for d in raw_durations[:-1]]

    # Two ticks: see the module docstring.
    tolerance_us = 2 * max(1.0, 1_000_000 / timescale)

    if args.grid_us is not None:
        if args.grid_us <= 0:
            sys.exit("--grid-us must be positive")
        grid_us = args.grid_us
        grid_source = "given"
    else:
        if not durations_us:
            sys.exit(f"{args.file}: only one sample -- nothing to measure a grid from")
        if min(durations_us) <= 0:
            sys.exit(f"{args.file}: a zero sample duration -- no grid to fit")
        grid_us = fit_grid(durations_us, min(durations_us), tolerance_us)
        grid_source = "inferred"

    histogram = {}
    off_grid_positions = []
    for i, d in enumerate(durations_us):
        k = round(d / grid_us)
        if k < 1 or abs(d - k * grid_us) > tolerance_us:
            off_grid_positions.append(i)
            continue
        histogram[k] = histogram.get(k, 0) + 1

    longest_hold = max(histogram) if histogram else 0

    print(f"samples {total_samples}  timescale {timescale}  "
          f"grid {grid_us:.3f} us ({grid_source})")
    counts = " ".join(f"x{k}={histogram.get(k, 0)}" for k in range(1, longest_hold + 1))
    print(f"durations: {counts}{' ' if counts else ''}longest hold {longest_hold} grid")
    if off_grid_positions:
        at = ", ".join(str(i) for i in off_grid_positions)
        print(f"off-grid: {len(off_grid_positions)} at sample {at}")
    else:
        print("off-grid: 0")

    sys.exit(0 if not off_grid_positions else 1)


if __name__ == "__main__":
    main()
