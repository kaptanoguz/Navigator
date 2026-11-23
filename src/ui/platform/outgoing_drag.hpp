// src/ui/platform/outgoing_drag.hpp
//
// Dragging files OUT of Navigator into other programs (Phase 7, docs/dnd.md). SDL 3.4 has no drag
// source API (it only receives drops), so this is Navigator's own implementation per display
// server, kept optional and behind one interface:
//   X11      an XDND source on SDL's Display* / Window (xdnd_source.cpp): the XdndSelection owner,
//            XdndEnter / Position / Leave / Drop towards the window under the pointer, the
//            text/uri-list, x-special/gnome-copied-files and text/plain targets, XdndActionCopy.
//            Compiled when the X11 development headers exist (NAVIGATOR_UI_HAVE_X11).
//   Wayland  wayland_drag.cpp: an attempt to start a wl_data_device drag from a second seat
//            binding on SDL's wl_display; where the compositor refuses (or the headers are
//            missing) the drag is not available and the ui says "Ctrl+C, then paste in the other
//            app" in the drag badge.
// Navigator never deletes anything on the strength of an outgoing drag: only XdndActionCopy /
// wl_data_device_manager copy are offered, a "move" is a copy (the receiver may ignore Shift).
// Thread-safety: UI thread only (the X11 hook runs inside SDL's event pump on the UI thread).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <navigator/types.hpp>

struct SDL_Window;

namespace navigator::ui {

enum class OutgoingBackend : std::uint8_t { None, X11, Wayland };

class OutgoingDrag {
public:
    OutgoingDrag() = default;
    virtual ~OutgoingDrag() = default;
    OutgoingDrag(const OutgoingDrag&) = delete;
    OutgoingDrag& operator=(const OutgoingDrag&) = delete;
    OutgoingDrag(OutgoingDrag&&) = delete;
    OutgoingDrag& operator=(OutgoingDrag&&) = delete;

    [[nodiscard]] virtual OutgoingBackend backend() const noexcept = 0;
    // Takes the drag selection / data source for `paths`. False: cannot (the ui falls back).
    [[nodiscard]] virtual bool begin(const std::vector<Path>& paths) = 0;
    // The pointer moved (call every frame while active; coordinates are read from the display
    // server, not passed in). True while a target window under the pointer accepts the drop.
    virtual bool motion() = 0;
    // The button was released: drop on the accepting target, or leave. After a drop the source
    // stays active until the receiver finished (or a timeout); poll() ends it.
    virtual void finish(bool drop) = 0;
    // Called every frame; handles timeouts. False when nothing is in progress any more.
    virtual bool poll(double now_ms) = 0;
    [[nodiscard]] virtual bool active() const noexcept = 0;
    // Diagnostics for docs/dnd.md and the tests ("accepted", "finished", "refused: ...").
    [[nodiscard]] virtual std::string state() const = 0;
};

// nullptr when the video driver is neither X11 nor Wayland or the backend is not compiled in.
[[nodiscard]] std::unique_ptr<OutgoingDrag> create_outgoing_drag(SDL_Window* window);

} // namespace navigator::ui
