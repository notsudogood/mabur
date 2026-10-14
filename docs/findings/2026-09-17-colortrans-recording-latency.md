# Burned-DVR colortrans stage: display latency while recording

**Status:** all three problems fixed on 2026-09-17. The current mechanisms (explicit IN_FENCE_FD, the pipelined ct thread, the GPU governor pin, the import cache, the memoised palette, the schedule-based fps cap) are summarised in docs/colortrans.md ("Keeping the burned stage off the display path").

Extracted from docs/colortrans.md during the 2026-10-11 docs tidy.

The first build of this stage made `dsp` p99 sit at 22 ms (baseline 5) for
every second a recording ran: one vsync slip on ~10 % of flips. Three things
were found and fixed, all on the bench with the drone streaming:

1. **Implicit fence on the decoder's frame.** The GPU stage imports the
   decoder's NV12 dmabuf as a texture; panfrost (6.1) attaches a
   `DMA_RESV_USAGE_WRITE` fence to every BO in a job, the PRIME import shares
   one `dma_resv` between that texture and the KMS framebuffer of the same
   frame, VOP2 has no `prepare_fb` of its own, so `drm_gem_plane_helper_prepare_fb`
   picked the fence up and the commit worker blocked 7-15 ms on a job that only
   reads. ftrace showed 66 `dma_fence_wait` calls per 10 s in the DRM commit
   workers, p90 12.5 ms; zero with recording off. Fix: `DrmPresenter` mints
   one already-signalled `sync_file` (a `DRM_SYNCOBJ_CREATE_SIGNALED` syncobj
   on the panfrost render node, exported; rockchip-drm lacks
   `DRIVER_SYNCOBJ`) and sets it as `IN_FENCE_FD` on every video-plane
   commit — with an explicit fence the helper only takes KERNEL-usage fences,
   of which there are none. After: 576 waits per 10 s (one per commit, the
   signalled fence), max 79 µs, `dsp` 5/5. Log line at startup:
   `DrmPresenter: video plane commits carry an explicit IN_FENCE_FD (...)`.
2. **Serial pipeline.** GPU import 2.7 + draw/finish 14.6 + RGA 4.9 + encode
   13.0 ms = 35 ms per frame on one thread → 26-32 fps recorded, 30 % of
   admitted frames dropped, and the GPU idle enough that devfreq never left
   300 MHz. The stage now runs on its own thread (EGL is thread-bound) with
   three destination buffers and a latest-wins handoff to the encode thread
   (`burn_recorder.h`, THREADING). Two more shavings were needed once the
   admission cap (below) let the full 60 fps through: the stage pins the GPU
   devfreq governor to `performance` for its lifetime (simple_ondemand never
   clocks up at the ~60 % utilisation a pipelined stage shows; restored at
   stop, logged both ways), and the per-frame EGLImage import of the
   decoder's dma-buf is cached per (fd, inode) -- the pool is 24 fixed
   buffers, so imports happen 24 times per recording instead of 60 times a
   second. After all three: stage 12.2 ms mean (import 0, draw+finish 7.0,
   RGA 5.1) against 13.2 ms encode; 1770 admitted / 1760 encoded / 9 dropped
   over 30 s at a 58-60 fps source, `dsp` 5/5. The stage is memory-bound now
   (11 MB GPU + 11 MB RGA traffic per 1080p frame), so the next lever would
   be feeding the encoder BGRA directly and deleting the RGA pass, untested.
3. **Record-start spike.** Every start blocked the main loop ~240 ms: 147 ms
   of it was `build_palette()` histogramming the whole MSP atlas, now built
   once at startup and memoised (`maburplay: burn palette built: N entries`
   at boot); the remaining ~75 ms is the first full OSD quantize (32-39 ms
   with a cold cache) plus the GPU bring-up (~165 ms on the ct thread, of
   which drm+gbm ~50, program ~65, targets ~30) and is accepted as a one-shot
   cost of pressing record. A similar ~80 ms one-shot happens at stop.

Also fixed the same day, pre-existing: `dvr.burned.fps_cap = 60` admitted
only ~41-45 fps of a 60 fps stream — the cap rejected any frame arriving less
than `interval - interval/8` after the last admitted one, and decoded frames
arrive in pairs. The cap is now schedule-based (`gs/player/src/fps_cap.h`,
host-tested by `test_fps_cap`): a frame is admitted when its slot is due,
slots advance one interval per admit, so a source at or under the cap passes
whole and a faster one is thinned to exactly the cap.

## OSD-size scaling (2026-09-27)

Bench 720p→1080p after the ct thread started scaling to the OSD surface:
ct 11.4 ms / enc 13.6 ms mean with colortrans, ~1 % dropped at the 60 fps
cap; without colortrans the scale is one RGA NV12 resize (~5 ms 720p→1080p).
