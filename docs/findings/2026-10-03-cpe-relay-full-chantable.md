# CPE510 full 5 GHz channel table (channel 144), 2026-10-03

**Status:** done — the CPE image tunes the whole 20 MHz grid 36–177 (`mabur-openwrt` kmod patch `997-ath9k-full-5ghz-chantable`, regdb domain `XM`). Current rule → docs/cpe510-relay.md "ath9k quirks a consumer must respect".

Extracted from docs/cpe510-relay.md during the 2026-10-11 docs tidy.

## The problem (2026-10-02)

Stock ath9k lists only 36–140 and 149–165: its static
5 GHz table (`ath9k/common-init.c`, not `init.c`) predates channel 144
and was never extended, so a TUNE to 144 failed with `-22` and the relay
stayed put (seen 2026-10-02 after the hop test). Fixed 2026-10-03 in
`mabur-openwrt` (kmod patch `997-ath9k-full-5ghz-chantable`, kmods
`-r5`): the phy now carries the whole 20 MHz grid **36–177** (adds
68–96, 144, 169–177), and the shipped regdb domain `REG=XM` opens all of
it with no DFS/NO-IR (NO-IR would make mac80211 refuse the relay's TX
inject; DFS alone never bothered a monitor vif). A CPE on an older image
still cannot tune 144; any channel in `radio.scan.candidates` or hop
target the CPE cannot tune drops the relay for the session (gracefully:
`owned=false`, USB cards carry the link).

## The fix and the spike

- DONE 2026-10-03: `mabur-openwrt`
  branch `full-5ghz-chantable` (kmod patch `997-ath9k-full-5ghz-chantable`
  + `regdb/db.txt` `XM` domain + `REG=XM` default). What the spike found:
  the table lives in `ath9k/common-init.c`; the ar9003 synthesizer takes any
  centre ≥ 4800 MHz and the EEPROM piers (5180…5825 on this unit) clamp
  past the last one; our kmods are built with `ATH_USER_REGD` and the EEPROM
  region is 0x0, so the kernel regdb (`iw reg set`) was the only limit
  left — and US already allowed 144. Bench record below.
- **Bench record 2026-10-03 (full grid + XM):** CPE v3 flashed from
  `mabur-openwrt` `full-5ghz-chantable` 2c1c51f (kmods `-r5`). Phy lists
  36–177, no Radar/No-IR/disabled flag; TUNE 144 HT40-/177/169 HT40+/68/96
  all land; 20 injected frames on each of 177/68/144/136 give `txecho =
  2 × tx`, `tx_fail` 0 (NO-IR would leave one echo). With drone + Radxa GS
  on **144 HT40-** (scan off): GS ausniff 1215 AUs/20 s, 60.8 fps; relay
  heard 49,201 good / 6 bad-FCS frames in 15 s (mcs4, RSSI −62…−38, 0 seq
  gaps). On **177 HT20** (GS ladder trimmed to 20 MHz rungs): ausniff
  1216 AUs, 0 gaps, 60.8 fps at mcs1; relay 12,040 good / 7 bad-FCS in
  15 s. Not run: the relay as a `maburgs` card on those channels (the
  Radxa had no route to the CPE this session). `iw reg get` keeps printing
  a `phy#0 … US` block — ath9k's own boot hint copy, not what the channel
  flags follow. Details: `mabur-openwrt` `docs/verify-mabur-relay-on-device.md`.
