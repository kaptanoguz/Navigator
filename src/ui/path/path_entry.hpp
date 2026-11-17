// src/ui/path/path_entry.hpp
//
// Controller of one path text field: the GoTo popup (nav.goto) or the address bar edit mode
// (nav.focus_address_bar). Headless; the views draw it, the App feeds it keys and polls it.
//
// GoTo list (docs/architecture.md "GoTo and the address bar"):
//   - empty text: Recents (newest first), then Places & Bookmarks;
//   - text that looks like a path (starts with '/', '~', '$', '%', '.', or contains '/'): a
//     Typed row first (the expand_user_path() preview; Enter on it resolves the text), then
//     folder-name completions, then recents / bookmarks whose path contains the expanded text;
//   - other text: recents, bookmarks and places whose label or path contains it (fs::fold_name),
//     then completions relative to the active folder, with the Typed row first.
//   A path shown in an earlier section is not repeated. The highlight starts on row 0.
// Address bar: the list is the completions only, shown once the user edits; nothing is
// highlighted until Up / Down, so Enter resolves the typed text.
//
// Completion: parse_completion_input() runs on the UI thread with a context that has no
// "~user" lookup; when the typed directory part differs from the cached listing's, a worker
// request is sent after a short debounce (kDebounceMs); otherwise match_completions() filters
// the cached listing at once.
// Enter (submit): a highlighted non-Typed row goes to its path (offline rows show an inline
// notice instead); otherwise the worker runs resolve_input() and poll() turns the reply into a
// PathOutcome, or an inline error that keeps the typed text (not found, unknown variables).
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/path_input.hpp>
#include <navigator/places.hpp>
#include <navigator/types.hpp>

#include "path/path_worker.hpp"
#include "widgets/text_edit.hpp"

namespace navigator::ui {

enum class PathEntryMode : std::uint8_t { GoTo, AddressBar };

// A recent / bookmark / place offered by GoTo (built by the App from the PlacesModel).
struct GotoSource {
    enum class Kind : std::uint8_t { Recent, Bookmark, Place };
    Kind kind = Kind::Place;
    std::string label;
    Path path; // effective path
    places::LocationStatus status = places::LocationStatus::Online;
    places::PlaceKind place = places::PlaceKind::Home;
};

struct PathItem {
    enum class Kind : std::uint8_t { Typed, Completion, Recent, Bookmark, Place };
    enum class Section : std::uint8_t { Typed, Matches, Recents, Places };
    Kind kind = Kind::Typed;
    Section section = Section::Typed;
    std::string label;  // UTF-8
    std::string detail; // UTF-8 path text (home shown as "~")
    Path path;
    std::string completion_text; // Completion: replacement for the whole input
    places::LocationStatus status = places::LocationStatus::Online;
    places::PlaceKind place = places::PlaceKind::Home;
    bool is_symlink = false;
};

struct PathOutcome {
    enum class Kind : std::uint8_t {
        Navigate,   // path is a folder: show it (new_tab: in a new background tab)
        RevealFile, // path is a file: show its folder with the cursor on cursor_name
        OpenFile,   // Ctrl+Enter on a file: open it with the default app
    };
    Kind kind = Kind::Navigate;
    Path path;
    std::string cursor_name;
    bool new_tab = false;
    bool case_corrected = false;
};

// GoTo's "looks like a path" rule.
[[nodiscard]] bool looks_like_path(std::string_view text) noexcept;
// "~/x" for paths under home, else the path; UTF-8 (lossy for non-UTF-8 bytes).
[[nodiscard]] std::string display_path(const Path& path, const Path& home);

class PathEntry {
public:
    static constexpr double kDebounceMs = 60.0;
    static constexpr std::size_t kMaxCompletions = 64;

    explicit PathEntry(PathEntryMode mode) : mode_(mode) {}

    void attach(PathWorker* worker) noexcept { worker_ = worker; }
    [[nodiscard]] PathEntryMode mode() const noexcept { return mode_; }

    // Opens with `text` (address bar: the folder path, all selected). `cwd` is the active
    // folder (relative input, completions), `home` expands "~".
    void open(std::string_view text, bool select_all, const Path& cwd, const Path& home,
              std::vector<GotoSource> sources, double now_ms);
    void close();
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    // The tab whose address bar is edited (AddressBar mode; set by the caller).
    std::uint32_t tab = 0;

    [[nodiscard]] TextEdit& edit() noexcept { return edit_; }
    [[nodiscard]] const TextEdit& edit() const noexcept { return edit_; }
    // Call after every change of edit() (typing, paste, undo).
    void changed(double now_ms);
    // Debounce: sends the pending completion request when due.
    void tick(double now_ms);
    // Applies worker replies. True when anything shown changed.
    bool poll(double now_ms);
    // The outcome of a finished Enter, once.
    [[nodiscard]] std::optional<PathOutcome> take_outcome();

    // Up / Down / PageUp / PageDown (clamped). Address bar: from "none" Down goes to row 0 and
    // Up from row 0 back to none.
    void move(std::ptrdiff_t delta);
    void set_highlight(std::size_t index) noexcept;
    // Tab: the highlighted completion (else the first one) replaces the text. False when there
    // is none.
    bool accept_completion(double now_ms);
    // Enter / Ctrl+Enter.
    void submit(bool new_tab);

    [[nodiscard]] const std::vector<PathItem>& items() const noexcept { return items_; }
    // npos: nothing highlighted.
    [[nodiscard]] std::size_t highlight() const noexcept { return highlight_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    // expand_user_path() of the text (without "~user" lookups), when it succeeds.
    [[nodiscard]] const std::optional<path_input::ExpandedPath>& preview() const noexcept {
        return preview_;
    }
    // A debounce, a worker job or a resolve is pending.
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool resolving() const noexcept { return resolve_id_ != 0; }
    [[nodiscard]] bool edited() const noexcept { return edited_; }
    [[nodiscard]] const Path& cwd() const noexcept { return cwd_; }
    [[nodiscard]] const Path& home() const noexcept { return home_; }

    // The contexts used (exposed for tests): UI side (no user_home lookup) and worker side.
    [[nodiscard]] path_input::ExpandContext ui_context() const;
    [[nodiscard]] path_input::ExpandContext worker_context() const;
    // Tests: apply a completion reply as if the worker sent it.
    void apply_completion(CompletionReply reply);
    void apply_resolve(ResolveReply reply, bool new_tab);

private:
    struct Cache {
        bool valid = false;
        std::string key; // typed_dir as typed when requested
        path_input::CompletionQuery query;
        std::vector<fs::DirEntry> entries;
    };
    void rebuild();
    [[nodiscard]] std::size_t default_highlight() const;
    void request_completion(double now_ms);

    PathEntryMode mode_;
    PathWorker* worker_ = nullptr;
    bool open_ = false;
    bool edited_ = false;
    TextEdit edit_{4096};
    Path cwd_;
    Path home_;
    std::vector<GotoSource> sources_;
    std::vector<path_input::Completion> completions_;
    std::vector<PathItem> items_;
    std::size_t highlight_ = 0;
    bool user_moved_ = false; // Up / Down / pointer moved the highlight since the last edit
    std::string error_;
    std::optional<path_input::ExpandedPath> preview_;
    Cache cache_;
    std::string wanted_key_;    // typed_dir the completions should come from
    std::string requested_key_; // wanted_key_ when the newest request was sent
    std::optional<double> debounce_at_;
    std::string pending_input_;
    std::uint64_t completion_id_ = 0; // newest request sent
    std::uint64_t resolve_id_ = 0;    // pending Enter
    bool resolve_new_tab_ = false;
    std::optional<PathOutcome> outcome_;
};

} // namespace navigator::ui
