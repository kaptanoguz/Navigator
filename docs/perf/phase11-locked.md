# Locked Performance Baseline — v0.1.0 (Phase 11)

This document locks the performance targets verified across Phases 1–10 for the `0.1.0` `.deb` release.

| Metric | Target | Reference |
|---|---|---|
| **Startup (headless / X11 smoke)** | < 250 ms to first interactive frame | `smoke.app_headless`, CI X11 smoke |
| **50k–100k directory list + sort** | Non-blocking worker; UI thread stays at 60+ FPS | [phase1-listing.md](phase1-listing.md), [phase2-scroll.md](phase2-scroll.md) |
| **200k-entry flatten walk + cancel** | Cancellation latency < 50 ms | [phase5-walk.md](phase5-walk.md) |
| **Name index query** | Sub-10 ms incremental query on 100k+ entries | [phase5-search.md](phase5-search.md) |
| **Preview / thumbnail decode** | Off-thread, bounded GPU & memory budget (256 MiB default) | [phase6-preview.md](phase6-preview.md) |
| **Bulk file ops (100k small files)** | Copy, undo, redo, trash, delete without UI stall | [phase7-ops.md](phase7-ops.md) |
| **Single-instance IPC adoption** | < 50 ms second-process handoff (`--new-tab`) | Phase 10 two-process IPC test (24.4 ms) |
