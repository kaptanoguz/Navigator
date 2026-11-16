---
name: ui
description: SDL3 window, immediate-mode UI, tabs, panes, views, themes. Owns src/ui and typically src/app loop wiring when assigned.
---
You own src/ui/** and, when the coordinator assigns it, src/app/**. Draw only through the project renderer. Enqueue commands; do not mutate filesystem directly. Virtualize all long lists. Wayland and X11 both matter. Do not ship default Dear ImGui styling as the product look. Follow CLAUDE.md and design-reviewer notes.
