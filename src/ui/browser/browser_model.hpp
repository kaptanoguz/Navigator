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
//   - Phase 5 view rows (docs/architecture.md "Phase 5", "View model per tab"): the view shows
//     ROWS, not entries: listing (or the flattened entries) -> hidden policy (listing / walk)
//     -> type filter (TabState::type_filter, persisted) -> query (filter text, runtime only).
//     Without a filter the rows are the listing itself (identity: no index vector, the Phase 2
//     cost). The folder filter keeps the listing order; up to Options::ui_filter_limit entries
//     it runs inline, above it on the SearchWorker (old rows stay until the newest result).
//     Cursor, anchor, selection and the band work in row space; selection flags belong to the
//     source entries, and entries the filter hides are deselected. Watch changes patch the
//     filtered rows in place (an inserted entry is matched once).
//   - Flatten (search.flatten / search.flatten_volume): a FlattenWalk streams the subtree of a
//     folder (or the volume root; or an IIndex answers queries when the App hands one over),
//     and the SearchWorker orders it: relevance with a query, the tab's sort otherwise, a
//     column-header click switches to that sort until the query changes. Runtime only: never
//     persisted, never a Recents visit; navigation leaves it, F5 re-runs the walk.
//   - Escape (list.clear_selection) undoes one step: notice, running search, selection,
//     flattened view, filter text (docs/keymap.md "Focus scoping (Phase 5)").
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
#include <navigator/index.hpp>
#include <navigator/layout.hpp>
#include <navigator/platform.hpp>
#include <navigator/search.hpp>
#include <navigator/types.hpp>
#include <navigator/walk.hpp>
#include <navigator/watch.hpp>

#include "browser/listing_worker.hpp"
#include "browser/watch_worker.hpp"
#include "search/flatten_walk.hpp"
#include "search/organize.hpp"
#include "search/search_worker.hpp"
#include "widgets/text_edit.hpp"

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

// Phase 5: what a flattened view is built from (BrowserModel::flatten).
struct FlattenRequest {
    Path root;           // absolute folder (search.flatten) or volume mount point
    bool volume = false; // search.flatten_volume ("whole drive")
    // A Ready or Verifying index of the volume: queries are answered from it instead of a walk.
    std::shared_ptr<search::IIndex> index;
};

// Phase 5: progress of the flattened view, for the status bar and the progress bar.
struct FlattenProgress {
    Path root;
    bool volume = false;
    bool from_index = false;
    bool walking = false;    // the walk thread is delivering (not cancelled, not finished)
    bool organizing = false; // a match/order or index job is outstanding
    bool cancelled = false;  // stopped by search.cancel / Escape / suspension
    bool truncated = false;  // the walk hit fs::kDefaultWalkMaxEntries
    std::optional<Error> error;
    fs::WalkStats stats;         // walk source
    std::uint64_t found = 0;     // walked entries (walk) or index matches (index)
    std::uint64_t skipped = 0;   // unreadable + vanished + failing folders
    bool index_complete = false; // index source: the index was Ready for the last query
    bool limited = false;        // index source: more matches than rows shown
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
        // Phase 5. Folder filter: listings up to this many entries filter on the UI thread
        // (search::kUiThreadFilterLimit); tests lower it to exercise the worker path.
        std::size_t ui_filter_limit = search::kUiThreadFilterLimit;
        // Walks for flatten; empty: fs::Walker. Tests stream fake batches.
        WalkFactory walk_factory;
        // Index-backed flatten: a query change waits this long before it runs (typing).
        std::chrono::milliseconds index_debounce{50};
        // Index-backed flatten: rows per query (index.hpp kDefaultIndexQueryLimit).
        std::size_t index_limit = search::kDefaultIndexQueryLimit;
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
    // Phase 4: record_visit makes the first successful listing count as a visit (tabs opened by
    // the user); session restore passes false.
    void restore(const layout::TabState& state, float row_dp, bool record_visit = false);
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
    // Phase 4 (GoTo / address bar on a file): navigate and put the cursor on `cursor_name`.
    void navigate_to(const Path& dir, std::string cursor_name);
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
    // Escape (list.clear_selection), one step per call: dismiss a visible notice, else stop a
    // running search, else clear a non-empty selection, else leave the flattened view, else
    // clear the filter text (docs/keymap.md). The type filter is never cleared here.
    void escape();
    // Executes a list/navigation/view command handled by the model (see CommandId). Returns
    // false for ids the model does not handle (app-level ones such as Quit).
    bool execute(const Command& command);

    // ---- Phase 5: filter, type filter, flatten (frame end) ----------------------------------
    // The filter field's text state. Edit it (App: field keys, type-to-filter), then call
    // filter_edited(); the rows update at once (inline) or when the worker answers.
    [[nodiscard]] TextEdit& filter_edit() noexcept { return filter_; }
    [[nodiscard]] const TextEdit& filter_edit() const noexcept { return filter_; }
    void filter_edited();
    void set_filter_text(std::string_view text); // tests and scripts: replace and apply
    void clear_filter();                         // list.clear_filter
    [[nodiscard]] bool filter_active() const noexcept { return !filter_.empty(); }
    // Match mode of the current view (folder: Substring by default, flattened: Fuzzy).
    [[nodiscard]] search::MatchMode match_mode() const noexcept;
    void toggle_match_mode(); // search.toggle_fuzzy
    // The newest compiled query (highlights use it). While a worker re-match or a debounced index
    // query is pending, the shown rows may still be the previous query's for a frame or two; no
    // wrong rows are ever shown, but highlights can lag.
    [[nodiscard]] const search::Query& query() const noexcept;
    // Bumps whenever the query or the type filter changes (view caches key on it).
    [[nodiscard]] std::uint64_t filter_generation() const noexcept { return filter_gen_; }
    // A worker result for the folder filter is outstanding and the old rows are not valid for
    // the current listing (the view shows a quiet "filtering" state, not "no matches").
    [[nodiscard]] bool rows_pending() const noexcept;

    [[nodiscard]] const search::TypeFilter& type_filter() const noexcept { return types_; }
    void set_type_filter(search::TypeFilter filter);
    // Types popup rows for the current view (listing or flattened entries; the hidden policy is
    // already applied). Cached per content and filter change.
    [[nodiscard]] const std::vector<search::TypeCount>& type_summary() const;

    void flatten(FlattenRequest request);
    void leave_flatten();
    void cancel_search();            // search.cancel: stop the walk / index query, keep the rows
    void cancel_for_quit() noexcept; // asks a running walk to stop; never waits
    [[nodiscard]] bool flattened() const noexcept { return flat_ != nullptr; }
    // Root of the flattened view (empty when not flattened).
    [[nodiscard]] const Path& flatten_root() const noexcept;
    [[nodiscard]] bool flatten_is_volume() const noexcept; // search.flatten_volume
    [[nodiscard]] FlattenProgress flatten_progress() const;
    // A walk, an organize job, a folder-filter job or a debounced index query is outstanding.
    [[nodiscard]] bool searching() const noexcept;
    // Tests: frame_start() in a loop until !searching() or the timeout.
    [[nodiscard]] bool settle_search(std::chrono::milliseconds timeout);
    // Flattened view: a column header click sorted by that column instead of relevance.
    [[nodiscard]] bool header_sorted() const noexcept;

    // ---- Rows (what the view shows) --------------------------------------------------------
    [[nodiscard]] std::size_t row_count() const noexcept;
    [[nodiscard]] const fs::DirEntry& row_entry(std::size_t row) const noexcept;
    // Relative parent path of a flattened row ("" for children of the root and listings).
    [[nodiscard]] std::string_view row_parent(std::size_t row) const noexcept;
    [[nodiscard]] std::uint32_t row_depth(std::size_t row) const noexcept;
    // Absolute path of a row.
    [[nodiscard]] Path row_path(std::size_t row) const;
    // Rows the hidden policy shows before the type filter and the query ("12 of N"): the
    // listing size, the walked entries, or the index matches.
    [[nodiscard]] std::size_t source_count() const noexcept;
    // Row of the entry named `name` (raw bytes) in the current view, or kNoRow.
    [[nodiscard]] std::size_t find_row(std::string_view name) const noexcept;

    // Pointer gestures (docs/keymap.md, "Mouse"). `row` must be < row_count().
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

    // Phase 4 recents: folders whose listing committed as a NEW location since the last call
    // (navigation, back/forward, up, and the first listing of a tab restored with
    // record_visit). Refresh, re-lists, resume and session restore never appear here.
    [[nodiscard]] std::vector<Path> take_visits();

    // Applies one watch batch (normally called by frame_start; public for tests).
    bool apply_watch_batch(const WatchBatch& batch);

    // ---- Read-only state for the view -------------------------------------------------------
    [[nodiscard]] const Path& current_dir() const noexcept { return current_.dir; }
    [[nodiscard]] const std::vector<fs::DirEntry>& entries() const noexcept { return entries_; }
    // Row-space selection (Phase 5: rows may be filtered or flattened).
    [[nodiscard]] bool is_selected(std::size_t row) const noexcept;
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
    // The row entry under the cursor, or nullptr.
    [[nodiscard]] const fs::DirEntry* cursor_entry() const noexcept {
        return cursor_ < row_count() ? &row_entry(cursor_) : nullptr;
    }
    [[nodiscard]] const std::vector<Location>& back_stack() const noexcept { return back_; }
    [[nodiscard]] const std::vector<Location>& forward_stack() const noexcept { return forward_; }
    // Index of the LISTING entry named `name` (raw bytes), or kNoRow.
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

    // ---- Phase 5 (browser_model_search.cpp) --------------------------------------------------
    enum class CursorPolicy : std::uint8_t { Top, Keep };
    struct ListingFilterResult {
        std::uint64_t job = 0;
        std::uint64_t version = 0;
        Result<std::vector<std::uint32_t>> rows = std::vector<std::uint32_t>{};
    };
    struct FlatResult {
        std::uint64_t config = 0;
        Result<OrganizeOutput> out = OrganizeOutput{};
        FlatChunkPtr chunk; // index source: the new rows' data (replaces the tables)
        std::uint64_t matched = 0;
        bool complete = false;
        std::uint64_t index_generation = 0;
    };
    struct Flat;
    [[nodiscard]] bool listing_identity() const noexcept { return list_identity_; }
    [[nodiscard]] bool listing_shown() const noexcept { return flat_ == nullptr; }
    [[nodiscard]] std::uint8_t& flag(std::size_t row) noexcept;
    [[nodiscard]] std::size_t list_src(std::size_t row) const noexcept;
    [[nodiscard]] std::size_t list_row_of(std::size_t src) const noexcept;
    [[nodiscard]] std::size_t flat_row_of(std::uint32_t id) const noexcept;
    [[nodiscard]] bool listing_filtering() const noexcept;
    [[nodiscard]] bool entry_shown(const fs::DirEntry& e);
    void compile_query();
    // The filter text, the match mode or the type filter changed. text: the query text or mode
    // changed (a flattened view goes back to relevance order).
    void query_changed(bool text);
    // Recomputes the listing rows; `cursor_src` / `anchor_src` are listing indices to keep.
    void rebuild_listing_rows(CursorPolicy policy, std::size_t cursor_src, std::size_t anchor_src);
    void set_listing_rows(std::vector<std::uint32_t> rows, CursorPolicy policy,
                          std::size_t cursor_src, std::size_t anchor_src);
    void listing_inserted(std::size_t src);
    void listing_erased(std::size_t src);
    [[nodiscard]] std::shared_ptr<const std::vector<fs::DirEntry>> listing_snapshot();
    void reset_search_view(); // navigation: clear the filter text, leave the flattened view
    bool apply_search_results();
    void start_walk();
    void post_flat_job(bool reset);
    void post_index_query();
    void apply_flat_result(FlatResult& result);
    void place_flat_cursor(std::uint32_t cursor_id, std::uint32_t anchor_id);
    [[nodiscard]] SearchWorker& worker();

    Options options_;
    // Destroyed after lister_ (whose thread may call into it through before_read). Both are
    // null while suspended.
    std::unique_ptr<WatchWorker> watcher_;
    std::unique_ptr<ListingWorker> lister_;
    bool suspended_ = false;
    std::optional<Deferred> deferred_; // navigation requested while suspended
    bool record_start_ = false;        // the Start listing counts as a visit
    std::vector<Path> visits_;
    // restore()'s scroll as given (persisted form), for tab_state() before the first listing.
    Path restored_dir_;
    std::optional<double> restored_rows_;

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

    // ---- Phase 5 ------------------------------------------------------------------------------
    TextEdit filter_{search::kMaxQueryBytes};
    search::MatchMode list_mode_ = search::MatchMode::Substring;
    search::Query query_;     // the filter text compiled for the LISTING (list_mode_)
    search::Matcher matcher_; // UI-thread matcher of query_ (inline filtering, watch inserts)
    search::TypeFilter types_;
    std::uint64_t filter_gen_ = 0;
    // Listing rows: identity (rows == entries) while nothing filters.
    bool list_identity_ = true;
    std::vector<std::uint32_t> list_rows_;
    search::Query list_rows_query_;       // the query list_rows_ were computed for (narrows)
    search::TypeFilter list_rows_types_;  // and the type filter
    std::uint64_t list_rows_version_ = 0; // listing_version_ list_rows_ are valid for
    bool list_stale_ = false;             // the query changed while flattened: rebuild on leave
    bool listing_job_out_ = false;        // a folder-filter job is outstanding
    bool pending_top_ = false;            // its result puts the cursor on the first row
    std::string pending_cursor_name_;     // else on this entry (and anchor)
    std::string pending_anchor_name_;
    std::uint64_t listing_job_ = 0;
    std::uint64_t listing_version_ = 0; // bumps on every change of entries_
    std::shared_ptr<const std::vector<fs::DirEntry>> snapshot_;
    std::uint64_t snapshot_version_ = 0;
    std::shared_ptr<ResultBox<ListingFilterResult>> listing_box_ =
        std::make_shared<ResultBox<ListingFilterResult>>();
    std::shared_ptr<ResultBox<FlatResult>> flat_box_ = std::make_shared<ResultBox<FlatResult>>();
    std::unique_ptr<SearchWorker> worker_;
    std::unique_ptr<Flat> flat_;
    mutable std::vector<search::TypeCount> type_rows_;
    mutable std::uint64_t type_rows_key_ = 0;
    mutable bool type_rows_valid_ = false;
};

// The flattened view's state (UI thread). Defined here so both browser_model*.cpp see it.
struct BrowserModel::Flat {
    FlattenRequest request;
    search::MatchMode mode = search::MatchMode::Fuzzy;
    search::Query query;             // filter text compiled with `mode`
    FlatTables tables;               // every delivered entry (walk) or the last index result
    std::vector<std::uint8_t> sel;   // selection flag per entry id
    std::vector<std::uint32_t> rows; // ids in display order
    std::unique_ptr<FlattenWalk> walk;
    fs::WalkStats stats;
    bool walking = false;
    bool cancelled = false;
    bool walk_done = false;
    bool interrupted = false; // suspension stopped the walk: resume() walks again
    bool refreshing = false;  // the job re-runs an index query because the index moved on
    bool reveal = false;      // the next result scrolls to the cursor (new query or sort)
    std::optional<Error> error;
    // Organize / index jobs.
    std::uint64_t config = 0;             // bumps on query, type filter, sort or mode changes
    std::shared_ptr<OrganizeState> state; // worker-side incremental state of `config`
    std::size_t sent_chunks = 0;          // chunks handed to the worker for `config`
    bool in_flight = false;               // a job of `config` is outstanding
    bool header_sort = false;             // column header clicked since the query changed
    bool cursor_follows_top = true;       // the user has not moved the cursor yet
    std::vector<search::TypeCount> types; // types popup rows (last result)
    // Index source.
    std::optional<std::chrono::steady_clock::time_point> query_due;
    std::chrono::steady_clock::time_point status_checked{};
    std::uint64_t index_generation = 0;
    std::uint64_t matched = 0;
    bool index_complete = false;
    // Listing state to restore when the view leaves flatten.
    std::string list_cursor_name;
    float list_scroll = 0.0F;
};

} // namespace navigator::ui
