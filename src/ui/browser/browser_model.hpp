// src/ui/browser/browser_model.hpp
//
// Headless state of the single-pane Details browser (docs/architecture.md, "Phase 2 browser
// data flow"). No SDL and no renderer: the view reads it, the App mutates it only between frames
// (frame start: finished work; frame end: pointer intents, then commands).
//
//   - Location: current directory, sorted entries, hidden count, listing error of the current
//     directory (a start path that cannot be listed).
//   - Selection: one flag per entry plus cursor and anchor rows. Carried across re-lists and
//     re-sorts by entry name; watch updates patch flags and rows in place.
//   - History: back/forward stacks of {dir, cursor name, scroll}; Up remembers the folder it came
//     from and puts the cursor on it.
//   - Navigation is asynchronous: a request lists the target on the ListingWorker; only a
//     successful listing commits (history, current dir, entries). A failure leaves everything as
//     it was and raises a notice (`tr("error." + to_string(code))`). Exception: the start path
//     commits even when it fails, as an empty list with the notice.
//   - Watching: the shown folder keeps its watch while a navigation is pending; the target gets
//     its own watch beside it (tagged with the request's generation) and the old one is dropped
//     only when the new listing commits, so a failed navigation loses no change. The listing
//     of a folder starts once its watch is in place (or after 200 ms); a watch that became
//     ready after its listing began triggers one re-list.
//   - Watch batches from the WatchWorker are applied incrementally (insert at the sorted
//     position, remove, rename, replace); overflow or a large batch re-lists; a vanished
//     directory raises a notice and keeps the stale list.
//   - Suspension (Phase 3, one model per tab): only the active tab of each pane is live. A
//     suspended model has no worker threads and no watch; it keeps its entries, selection,
//     history, cursor and scroll. A navigation requested while suspended (session restore,
//     duplicate of a tab that was still loading) is deferred and issued by resume(); resuming a
//     loaded model re-lists its folder (selection and cursor carried by name, scroll kept).
//
// Thread-safety: UI thread only (the workers it owns have their own threads and mailboxes).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/layout.hpp>
#include <navigator/platform.hpp>
#include <navigator/types.hpp>
#include <navigator/watch.hpp>

#include "browser/listing_worker.hpp"
#include "browser/watch_worker.hpp"

namespace navigator::ui {

inline constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

// What a notice is about; selects the title text (notice.* keys).
enum class NoticeKind : std::uint8_t {
    OpenFolderFailed, // navigation target could not be listed
    OpenFileFailed,   // open_with_default failed
    RefreshFailed,    // re-listing the current directory failed
    FolderGone,       // the watched directory vanished
};

struct Notice {
    NoticeKind kind = NoticeKind::OpenFolderFailed;
    ErrorCode code = ErrorCode::Unknown;
    Path path;                // the folder or file the notice is about
    std::uint64_t serial = 0; // increases with every new notice
};

struct Location {
    Path dir;
    std::string cursor_name; // entry under the cursor when we left (raw bytes)
    float scroll = 0.0F;     // view scroll offset in dp when we left
};

class BrowserModel {
public:
    struct Options {
        // Directory watching; empty: no live updates (tests that do not need a watcher).
        WatcherFactory watcher_factory;
        // Launcher for Activate on files. Not owned; must outlive the model. nullptr: opening a
        // file fails with ErrorCode::Unsupported (never spawns anything), so tests cannot run
        // xdg-open by accident. The app passes platform::create_default_launcher().
        platform::IAppLauncher* launcher = nullptr;
        // Called from worker threads when a listing or watch batch is ready (thread-safe).
        std::function<void()> wake;
        // A watch batch with more changes than this re-lists instead of patching.
        std::size_t max_incremental_changes = 256;
        // Phase 3: construct suspended (no threads until resume()). Background tabs.
        bool start_suspended = false;
    };

    explicit BrowserModel(Options options);
    ~BrowserModel();
    BrowserModel(const BrowserModel&) = delete;
    BrowserModel& operator=(const BrowserModel&) = delete;
    BrowserModel(BrowserModel&&) = delete;
    BrowserModel& operator=(BrowserModel&&) = delete;

    // ---- Frame start ------------------------------------------------------------------------
    // Applies finished listings (current generation only) and watch batches. Returns true when
    // anything visible changed.
    bool frame_start();
    // A listing request is in flight.
    [[nodiscard]] bool busy() const noexcept { return pending_.has_value(); }
    // Tests and scripted runs: blocks until the pending listing has a result (does not apply
    // it; call frame_start()). True when nothing is pending or the result arrived in time.
    [[nodiscard]] bool wait_listing(std::chrono::milliseconds timeout);
    // Tests: blocks until the watch worker has a batch waiting / has applied the last target.
    [[nodiscard]] bool wait_watch_batch(std::chrono::milliseconds timeout);
    [[nodiscard]] bool wait_watch_ready(std::chrono::milliseconds timeout);

    // ---- Suspension (Phase 3) ----------------------------------------------------------------
    // Stops the workers (joins their threads) and drops the watch. A pending navigation is kept
    // and re-issued by resume(). No-op when already suspended.
    void suspend();
    // Starts the workers again and issues the deferred navigation, or re-lists the folder.
    void resume();
    [[nodiscard]] bool suspended() const noexcept { return suspended_; }

    // First location of a tab restored from the session (or created by tab.new): sort, hidden
    // setting, cursor name and scroll (rows * row_dp) come from `state`. Lists right away when
    // live, on resume() when suspended.
    void restore(const layout::TabState& state, float row_dp);
    // Full copy of `other`'s browsing state (folder, entries, sort, selection, cursor, anchor,
    // scroll, back/forward history, pending navigation) for tab.duplicate. Call on a fresh,
    // suspended model; resume() then re-lists the copy.
    void copy_from(const BrowserModel& other);
    // The persisted subset (layout::TabState) of this tab; scroll in rows of `row_dp`.
    [[nodiscard]] layout::TabState tab_state(float row_dp) const;

    // ---- Navigation (frame end) -------------------------------------------------------------
    // First location. Relative paths resolve against the working directory.
    void open_start(const Path& dir);
    void navigate_to(const Path& dir); // push history on success
    void back();
    void forward();
    void up();
    void refresh();
    void toggle_hidden();
    // Sort by `key`; the same key again reverses. A new Size or Modified sort starts
    // descending (largest/newest first), Name and Extension ascending.
    void sort_by(fs::SortKey key);
    // Activate the cursor entry: folder (or link to a folder) -> navigate_to; anything else ->
    // platform::open_with_default(current_dir / name). No-op without a cursor.
    void activate();

    // ---- Cursor and selection (frame end) ---------------------------------------------------
    void cursor_by(std::ptrdiff_t delta, bool extend); // clamps at both ends
    void cursor_page(int direction, bool extend);      // +-1 page of page_rows()
    void cursor_home(bool extend);
    void cursor_end(bool extend);
    void select_all();
    void clear_selection(); // cursor stays
    // Escape (list.clear_selection): dismisses a visible banner first; with no banner, clears
    // the selection.
    void escape();
    // Executes a list/navigation/view command handled by the model (see CommandId). Returns
    // false for ids the model does not handle (app-level ones such as Quit).
    bool execute(const Command& command);

    // Pointer gestures (docs/keymap.md, "Mouse"). `row` must be < entries().size().
    void click_row(std::size_t row, Modifiers mods);
    void band_begin(bool additive);
    // Rows [first, last] (inclusive) are touched by the band; first > last: none.
    void band_update(std::size_t first, std::size_t last);
    void band_end();
    void dismiss_notice() noexcept;
    // Middle-click on a row: cursor and anchor to `row`, selection unchanged.
    void cursor_to(std::size_t row);

    // View geometry reported by the details view (frame end).
    void set_page_rows(std::size_t rows) noexcept { page_rows_ = rows > 0 ? rows : 1; }
    void set_scroll(float dp) noexcept { scroll_ = dp; }

    // Applies one watch batch (normally called by frame_start; public for tests).
    bool apply_watch_batch(const WatchBatch& batch);

    // ---- Read-only state for the view -------------------------------------------------------
    [[nodiscard]] const Path& current_dir() const noexcept { return current_.dir; }
    [[nodiscard]] const std::vector<fs::DirEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] bool is_selected(std::size_t row) const noexcept {
        return row < selected_.size() && selected_[row] != 0;
    }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::size_t anchor() const noexcept { return anchor_; }
    [[nodiscard]] std::size_t selected_count() const noexcept { return selected_count_; }
    // Total size of selected non-folder entries (folders count 0 until recursive sizes exist).
    [[nodiscard]] std::uint64_t selected_bytes() const noexcept { return selected_bytes_; }
    // Hidden names in the directory, whether shown or not.
    [[nodiscard]] std::size_t hidden_count() const noexcept { return hidden_names_.size(); }
    [[nodiscard]] bool show_hidden() const noexcept { return show_hidden_; }
    [[nodiscard]] const fs::SortOptions& sort() const noexcept { return sort_; }
    [[nodiscard]] bool can_back() const noexcept { return !back_.empty(); }
    [[nodiscard]] bool can_forward() const noexcept { return !forward_.empty(); }
    [[nodiscard]] bool can_up() const noexcept;
    [[nodiscard]] const std::optional<Notice>& notice() const noexcept { return notice_; }
    // The notice the banner shows: notice() minus an OpenFolderFailed about the folder being
    // shown when its own listing failed (the empty state already says it, with a next action).
    [[nodiscard]] bool notice_shown() const noexcept;
    // Listing error of the directory shown (start path that failed); nullopt when listed.
    [[nodiscard]] const std::optional<Error>& listing_error() const noexcept {
        return listing_error_;
    }
    [[nodiscard]] bool loaded() const noexcept { return loaded_; } // first listing committed
    [[nodiscard]] std::size_t page_rows() const noexcept { return page_rows_; }
    [[nodiscard]] float scroll() const noexcept { return scroll_; }
    // The entry under the cursor, or nullptr.
    [[nodiscard]] const fs::DirEntry* cursor_entry() const noexcept {
        return cursor_ < entries_.size() ? &entries_[cursor_] : nullptr;
    }
    [[nodiscard]] const std::vector<Location>& back_stack() const noexcept { return back_; }
    [[nodiscard]] const std::vector<Location>& forward_stack() const noexcept { return forward_; }
    // Index of the entry named `name` (raw bytes), or kNoRow.
    [[nodiscard]] std::size_t find(std::string_view name) const noexcept;

    // Change counters the view uses to react (scroll to cursor, restore scroll, ...).
    [[nodiscard]] std::uint64_t location_serial() const noexcept { return location_serial_; }
    [[nodiscard]] std::uint64_t cursor_serial() const noexcept { return cursor_serial_; }
    [[nodiscard]] std::uint64_t content_serial() const noexcept { return content_serial_; }
    // Scroll offset (dp) to restore after the last location change (Back/Forward).
    [[nodiscard]] float restored_scroll() const noexcept { return restored_scroll_; }

private:
    enum class NavKind : std::uint8_t { Start, Push, Back, Forward, Up, Refresh, Relist };
    struct Pending {
        std::uint64_t generation = 0;
        NavKind kind = NavKind::Refresh;
        Location target;
        std::uint64_t watch_tag = 0;    // tag of the watch on the target directory
        std::vector<WatchBatch> replay; // batches for the target seen while pending
    };

    // fold: a Refresh/Relist while a navigation is pending re-issues that navigation with the
    // new options instead of replacing it (sort/hidden toggles); false: the request replaces it.
    void request(NavKind kind, Location target, bool fold = true);
    [[nodiscard]] std::vector<WatchChange>
    pair_moves(const std::vector<WatchChange>& changes) const;
    void commit(Pending& pending, ListingResult& result);
    void fail_request(Pending& pending, ListingResult& result);
    void raise(NoticeKind kind, ErrorCode code, Path path);
    void set_cursor(std::size_t row, bool extend);
    void select_only(std::size_t row);
    void select_range(std::size_t a, std::size_t b, bool add);
    void recount();
    [[nodiscard]] Location here() const;
    // Entry order (same chain as fs::sort_entries).
    [[nodiscard]] bool before(const fs::DirEntry& a, const fs::DirEntry& b) const noexcept;
    std::size_t insert_sorted(fs::DirEntry entry, bool selected);
    void erase_row(std::size_t row);
    bool apply_change(const WatchChange& change);

    struct Deferred {
        NavKind kind = NavKind::Start;
        Location target;
    };
    void start_workers();

    Options options_;
    // Destroyed after lister_ (whose thread may call into it through before_read). Both are
    // null while suspended.
    std::unique_ptr<WatchWorker> watcher_;
    std::unique_ptr<ListingWorker> lister_;
    bool suspended_ = false;
    std::optional<Deferred> deferred_; // navigation requested while suspended

    std::uint64_t generation_ = 0;
    std::optional<Pending> pending_;
    std::uint64_t watch_tag_ = 0; // tag of the watch on current_.dir
    bool watch_gone_ = false;     // the kernel dropped that watch (WatchedDirGone)
    // When the committed listing began, and the options it was made with.
    std::chrono::steady_clock::time_point listed_at_{};
    fs::SortOptions listed_sort_;
    bool listed_hidden_ = false;

    Location current_;
    std::vector<fs::DirEntry> entries_;
    std::vector<std::uint8_t> selected_;
    std::size_t cursor_ = kNoRow;
    std::size_t anchor_ = kNoRow;
    std::size_t selected_count_ = 0;
    std::uint64_t selected_bytes_ = 0;
    std::unordered_set<std::string> hidden_names_;
    bool show_hidden_ = false;
    fs::SortOptions sort_;
    bool loaded_ = false;
    std::optional<Error> listing_error_;
    std::optional<Notice> notice_;
    std::uint64_t notice_serial_ = 0;

    std::vector<Location> back_;
    std::vector<Location> forward_;

    std::vector<std::uint8_t> band_base_; // selection when the band started (additive band)
    bool band_active_ = false;
    bool band_additive_ = false;

    std::size_t page_rows_ = 20;
    float scroll_ = 0.0F;
    float restored_scroll_ = 0.0F;
    std::uint64_t location_serial_ = 0;
    std::uint64_t cursor_serial_ = 0;
    std::uint64_t content_serial_ = 0;
};

} // namespace navigator::ui
