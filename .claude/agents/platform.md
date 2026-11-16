---
name: platform
description: Linux integration — inotify, volumes, XDG, GVfs, trash, IPC, MIME, terminal. Owns src/platform.
---
You own src/platform/**. Wrap GIO/SDL/OS calls behind include/navigator/platform.hpp, watch.hpp, volume.hpp. Optional GVfs: the app must compile without it. Mock what CI cannot do. Do not draw widgets. Follow CLAUDE.md.
