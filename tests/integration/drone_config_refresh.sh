#!/bin/sh
# bundle/S96mabur refresh: a new image replaces an /etc/mabur.toml edited
# under an older one (backed up, calibration kept), never one edited on
# this image, never with the opt-out file there.
set -eu
S96=$(dirname "$0")/../../bundle/S96mabur
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export MABUR_CONF=$T/mabur.toml MABUR_IMAGE_CONF=$T/rom.toml MABUR_CONF_KEEP=$T/keep MABUR_CONF_BACKUP=$T \
  MABUR_CONF_EPOCH=$T/epoch
fail() { echo "FAIL: $*"; exit 1; }
run() { sh "$S96" refresh; }

printf '[radio]\nchannels = [36, 100, 116, 132]\npower_mode = "none"\nrate_walls_rel = [63, 63, 62, 45, 22, 6, 9, 5]\nlegacy_wall_rel = 63\nwall_margin_db  = 1.0\n' > "$T/rom.toml"

# 1. Missing config: the image default.
run; cmp -s "$T/rom.toml" "$T/mabur.toml" || fail "missing config not restored"
# 2. Same as the image: untouched, stamp written.
run; cmp -s "$T/rom.toml" "$T/mabur.toml.image" || fail "no stamp"
# 3. Edited on this image: kept.
sed -i 's/36, 100, 116, 132/132/' "$T/mabur.toml"
run; grep -q 'channels = \[132\]' "$T/mabur.toml" || fail "same-image edit not kept"
[ ! -e "$T/mabur.toml.old1" ] || fail "same-image edit backed up"
# 4. A new image: reset, old copy kept, calibration not in use -> not carried.
sed -i 's/36, 100/36, 104/' "$T/rom.toml"
run; cmp -s "$T/rom.toml" "$T/mabur.toml" || fail "new image did not reset"
grep -q 'channels = \[132\]' "$T/mabur.toml.old1" || fail "no backup"
# 5. Calibrated on that image, then another image: the four calibration lines carry.
sed -i -e 's/power_mode = "none"/power_mode = "offset"/' -e 's/rate_walls_rel = .*/rate_walls_rel = [60, 60, 59, 40, 20, 5, 8, 4]/' \
       -e 's/legacy_wall_rel = 63/legacy_wall_rel = 58/' -e 's/channels = .*/channels = [149]/' "$T/mabur.toml"
sed -i 's/36, 104/36, 108/' "$T/rom.toml"
run
grep -q 'power_mode = "offset"' "$T/mabur.toml" || fail "power_mode not carried"
grep -q 'rate_walls_rel = \[60, 60, 59, 40, 20, 5, 8, 4\]' "$T/mabur.toml" || fail "walls not carried"
grep -q 'legacy_wall_rel = 58' "$T/mabur.toml" || fail "legacy wall not carried"
grep -q 'channels = \[36, 108, 116, 132\]' "$T/mabur.toml" || fail "the rest is not the new default"
[ "$(grep -c rate_walls_rel "$T/mabur.toml")" = 1 ] || fail "duplicated key"
[ -e "$T/mabur.toml.old2" ] || fail "second backup missing"
# 6. A hand-set gain floor (per radio unit) carries; the image's -1 does not.
sed -i 's/36, 108/36, 110/' "$T/rom.toml"
printf 'wide_igi_floor = -1    # the image default\n' >> "$T/rom.toml"
cp "$T/rom.toml" "$T/mabur.toml"; cp "$T/rom.toml" "$T/mabur.toml.image"
sed -i 's/^wide_igi_floor *=.*/wide_igi_floor = 53/' "$T/mabur.toml"
sed -i 's/36, 110/36, 111/' "$T/rom.toml"
run
grep -q '^wide_igi_floor = 53$' "$T/mabur.toml" || fail "hand-set floor not carried"
grep -q 'channels = \[36, 111, 116, 132\]' "$T/mabur.toml" || fail "the rest is not the new default (floor)"
[ "$(grep -c wide_igi_floor "$T/mabur.toml")" = 1 ] || fail "duplicated floor key"
sed -i 's/36, 111/36, 112/' "$T/rom.toml"
sed -i 's/^wide_igi_floor *=.*/wide_igi_floor = -1/' "$T/mabur.toml"
sed -i 's/^channels = .*/channels = [157]/' "$T/mabur.toml"
run
grep -q '^wide_igi_floor = -1    # the image default$' "$T/mabur.toml" || fail "image-default floor was carried"
# 7. Opt-out: a new image leaves the edited file alone.
sed -i 's/channels = .*/channels = [157]/' "$T/mabur.toml"
sed -i 's/36, 112/36, 113/' "$T/rom.toml"
touch "$T/keep"
run; grep -q 'channels = \[157\]' "$T/mabur.toml" || fail "keep file ignored"
# 8. A clean-slate epoch (an older image's drone: no epoch recorded) wipes
# everything: calibration, a hand-set floor and the keep file go too.
cp "$T/rom.toml" "$T/mabur.toml"
sed -i -e 's/power_mode = "none"/power_mode = "offset"/' -e 's/legacy_wall_rel = 63/legacy_wall_rel = 58/' \
       -e 's/^wide_igi_floor *=.*/wide_igi_floor = 0/' -e 's/channels = .*/channels = [149]/' "$T/mabur.toml"
rm -f "$T/epoch"
n_before=$(ls "$T"/mabur.toml.old* | wc -l)
run
cmp -s "$T/rom.toml" "$T/mabur.toml" || fail "epoch wipe did not restore the exact image default"
[ ! -e "$T/keep" ] && [ -e "$T/mabur.keep.old" ] || fail "keep file not removed by the epoch wipe"
[ "$(ls "$T"/mabur.toml.old* | wc -l)" = $((n_before + 1)) ] || fail "epoch wipe made no backup"
[ -s "$T/epoch" ] || fail "epoch not recorded"
# ...once: an edit after it survives the next start on the same image.
sed -i 's/channels = .*/channels = [100]/' "$T/mabur.toml"
run; grep -q 'channels = \[100\]' "$T/mabur.toml" || fail "edit after the epoch wipe lost"
echo "drone config refresh: OK"
