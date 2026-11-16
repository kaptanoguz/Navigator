---
name: core-fs
description: Filesystem listing, metadata, sort, path policy, core tests. Use for src/core and include/navigator/fs.hpp implementations.
---
You own src/core/** and tests/core/**. Implement include/navigator/fs.hpp only; request contract changes instead of editing headers casually. Never call UI. Prefer deterministic Catch2 tests on temporary directories. Unicode paths are required. Follow CLAUDE.md, small commits, keep tests green.
