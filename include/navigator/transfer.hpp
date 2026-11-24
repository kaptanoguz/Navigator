// navigator/transfer.hpp
//
// Purpose: data formats for moving file references between Navigator and other programs
//          (Phase 7): file:// URIs, text/uri-list, the GNOME / Nautilus / Nemo copied-files
//          clipboard format, KDE's cut marker, and the pure decision of which action a drop
//          performs. No SDL types: the ui offers these bytes through SDL_SetClipboardData and
//          reads them back with SDL_GetClipboardData / SDL drop events.
// Owner:   contracts (header); implementation in src/platform (role: platform; pure code, no
//          GIO), tests in tests/platform. The ui (src/ui) owns the SDL clipboard and drag and
//          drop plumbing (docs/architecture.md "Phase 7", "Clipboard and drag and drop").
// Thread-safety: every function is pure (no I/O, no shared state) and callable from any
//                thread, including the UI thread.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
//
// MIME types Navigator OFFERS on the clipboard for edit.copy / edit.cut, in this order
// (make_clipboard_offer):
//   x-special/gnome-copied-files   "copy\n" or "cut\n" + URIs joined by "\n" (no trailing
//                                  newline): GNOME Files, Nemo, Caja, Thunar, PCManFM read it
//   text/uri-list                  RFC 2483: URIs, each followed by "\r\n"
//   application/x-kde-cutselection "1" (only for cut): Dolphin's move marker next to uri-list
//   text/plain;charset=utf-8       absolute paths joined by "\n" when every path is valid
//                                  UTF-8, else the URIs (pasting into an editor)
//   text/plain, UTF8_STRING        same bytes as text/plain;charset=utf-8 (older X11 clients)
// MIME types Navigator ACCEPTS on edit.paste, first match wins (read_clipboard):
//   x-special/gnome-copied-files; then text/uri-list (cut when application/x-kde-cutselection
//   is "1"); then text/plain starting with "x-special/nautilus-clipboard\n" (the form GNOME
//   Files 3.28-42 put on the text clipboard). Plain-text paths are NOT treated as files: a
//   pasted path is text (the GoTo box takes it), never an implicit copy.
// Drag and drop: SDL3 delivers incoming drops as SDL_EVENT_DROP_FILE (one local path per
// event; SDL decodes text/uri-list itself) and SDL_EVENT_DROP_TEXT (text/plain). The ui feeds
// DROP_TEXT through decode_uri_list() so a uri-list that arrives as text still works. SDL3 has
// no drag SOURCE API; outgoing drags to other programs are documented in
// docs/architecture.md "Phase 7" (limitation and the attempted X11 / Wayland paths).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/types.hpp>

namespace navigator::transfer {

inline constexpr std::string_view kMimeGnomeCopiedFiles = "x-special/gnome-copied-files";
inline constexpr std::string_view kMimeUriList = "text/uri-list";
inline constexpr std::string_view kMimeKdeCutSelection = "application/x-kde-cutselection";
inline constexpr std::string_view kMimeTextUtf8 = "text/plain;charset=utf-8";
inline constexpr std::string_view kMimeText = "text/plain";
inline constexpr std::string_view kMimeUtf8String = "UTF8_STRING";
inline constexpr std::string_view kNautilusTextHeader = "x-special/nautilus-clipboard";

// ---------------------------------------------------------------------------------------------
// file:// URIs
// ---------------------------------------------------------------------------------------------

// "file://" + the percent-encoded lexically normal absolute path with GLib's
// g_filename_to_uri() rules; byte-for-byte the same output as thumbnail::file_uri()
// (thumbnail.hpp), which tests/integration asserts. Kept here as well so that src/platform,
// src/ops and src/ui do not depend on src/preview. Pure.
// Errors: InvalidArgument (empty or relative path), OutOfMemory.
[[nodiscard]] Result<std::string> file_uri(const Path& absolute_path);

// Local path of a file URI. Accepted: scheme "file" (ASCII case-insensitive) with an empty
// authority or "localhost" ("file:///a", "file://localhost/a"); %XX in either case decoded to
// raw bytes; the result lexically normalized. Errors: InvalidArgument (not a file URI,
// malformed escape, %00, relative or empty path, a query or fragment), Unsupported (another
// host: "file://server/share" is a network location, not a local path).
[[nodiscard]] Result<Path> path_from_file_uri(std::string_view uri);

// ---------------------------------------------------------------------------------------------
// text/uri-list (RFC 2483)
// ---------------------------------------------------------------------------------------------

// Each URI followed by "\r\n". Pure.
[[nodiscard]] std::string encode_uri_list(const std::vector<std::string>& uris);

// Convenience: file_uri() of every path, then encode_uri_list(). Errors: those of file_uri().
[[nodiscard]] Result<std::string> encode_uri_list(const std::vector<Path>& paths);

struct UriList {
    std::vector<Path> paths;         // local files, in order, duplicates kept once (first)
    std::vector<std::string> remote; // well-formed non-local URIs (other scheme or host)
    std::size_t malformed = 0;       // lines that are not URIs or not decodable
};

// Lines split at "\r\n" or "\n"; surrounding spaces/tabs trimmed; empty lines and lines
// starting with '#' skipped; each line through path_from_file_uri(). Pure; never fails.
[[nodiscard]] UriList decode_uri_list(std::string_view text);

// ---------------------------------------------------------------------------------------------
// File clipboard
// ---------------------------------------------------------------------------------------------

enum class ClipboardAction : std::uint8_t {
    Copy = 0,
    Cut = 1, // paste moves; the ui dims cut rows until the paste or until the clipboard changes
};

struct FileClipboard {
    ClipboardAction action = ClipboardAction::Copy;
    std::vector<Path> paths; // absolute, lexically normal, in selection (list) order

    friend bool operator==(const FileClipboard&, const FileClipboard&) = default;
};

// x-special/gnome-copied-files payload: "copy" or "cut", then one URI per line, lines joined
// by "\n", no trailing newline. Errors: InvalidArgument (no paths), those of file_uri().
[[nodiscard]] Result<std::string> encode_gnome_copied_files(const FileClipboard& clipboard);

// Inverse. First line "copy" or "cut" (trailing "\r" and spaces ignored), then URIs as in
// decode_uri_list(). nullopt when the first line is neither or no local path remains. Pure.
[[nodiscard]] std::optional<FileClipboard> decode_gnome_copied_files(std::string_view text);

// The text/plain form "x-special/nautilus-clipboard\n{copy|cut}\n<uri>\n...". nullopt when the
// header line is missing, the action is unknown or no local path remains. Pure.
[[nodiscard]] std::optional<FileClipboard> decode_nautilus_text(std::string_view text);

// (mime type, bytes) pairs in the offer order listed in the header comment. The ui keeps the
// vector alive while it owns the clipboard and answers SDL's data callback from it.
// Errors: InvalidArgument (no paths), those of file_uri().
[[nodiscard]] Result<std::vector<std::pair<std::string, std::string>>>
make_clipboard_offer(const FileClipboard& clipboard);

// Reads clipboard contents through `get` (mime type -> bytes; nullopt when not offered), in
// the acceptance order listed in the header comment. nullopt when no format yields at least
// one local path. Pure (as pure as `get`).
using ClipboardGetter = std::function<std::optional<std::string>(std::string_view mime)>;
[[nodiscard]] std::optional<FileClipboard> read_clipboard(const ClipboardGetter& get);

// ---------------------------------------------------------------------------------------------
// Drop action (pure decision; the ui owns the state machine)
// ---------------------------------------------------------------------------------------------

enum class DropAction : std::uint8_t {
    None = 0, // not allowed here (drawn as "no drop"): into itself, onto a file, read-only, ...
    Copy = 1,
    Move = 2,
    Link = 3, // symlink(s) in the target folder (OpKind::CreateSymlink)
    Ask = 4,  // drop menu: Copy here, Move here, Link here, Cancel
};

struct DropContext {
    bool internal = false;           // the drag started in this Navigator process
    bool same_filesystem = false;    // every source on the target folder's filesystem (st_dev)
    bool target_writable = true;     // best-effort hint for the target folder
    bool into_source_folder = false; // the target folder is the folder the sources are in
    bool into_itself = false;        // the target is a source or below one (ops::is_same_or_inside)
    bool ctrl = false;               // modifiers held at drop (or hover) time
    bool shift = false;
    bool alt = false;
};

// GNOME Files conventions (docs/review/phase-6.md "Drag and drop"):
//   into_itself or !target_writable              -> None
//   alt                                          -> Ask
//   ctrl && shift                                -> Link
//   ctrl                                         -> Copy
//   shift                                        -> Move
//   into_source_folder (no modifier)             -> None (dropping back where it came from)
//   internal && same_filesystem                  -> Move
//   otherwise                                    -> Copy
// External drops therefore copy unless Shift is held: SDL3 does not report the source's
// proposed action, and Navigator never deletes another program's files on a guess. Pure.
[[nodiscard]] constexpr DropAction choose_drop_action(const DropContext& c) noexcept {
    if (c.into_itself || !c.target_writable) {
        return DropAction::None;
    }
    if (c.alt) {
        return DropAction::Ask;
    }
    if (c.ctrl && c.shift) {
        return DropAction::Link;
    }
    if (c.ctrl) {
        return DropAction::Copy;
    }
    if (c.shift) {
        return DropAction::Move;
    }
    if (c.into_source_folder) {
        return DropAction::None;
    }
    if (c.internal && c.same_filesystem) {
        return DropAction::Move;
    }
    return DropAction::Copy;
}

} // namespace navigator::transfer
