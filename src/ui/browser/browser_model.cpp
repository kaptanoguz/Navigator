// src/ui/browser/browser_model.cpp — see browser_model.hpp.
#include "browser/browser_model.hpp"

#include <algorithm>
#include <compare>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "search/filter_rules.hpp"

namespace navigator::ui {
namespace {

// Absolute, lexically normal, no trailing separator (except "/").
Path normalize(const Path& dir) {
    std::error_code ec;
    Path p = dir.is_absolute() ? dir : std::filesystem::absolute(dir, ec);
    if (ec) {
        p = dir;
    }
    p = p.lexically_normal();
    if (!p.has_filename() && p != p.root_path()) {
        p = p.parent_path();
    }
    return p;
}

} // namespace

BrowserModel::BrowserModel(Options options)
    : options_(std::move(options)), tree_(options_.wake) {
    if (options_.start_suspended) {
        suspended_ = true;
        tree_.suspend();
    } else {
        start_workers();
    }
}

void BrowserModel::start_workers() {
    if (options_.watcher_factory) {
        watcher_ = std::make_unique<WatchWorker>(options_.watcher_factory, options_.wake);
    }
    lister_ = std::make_unique<ListingWorker>(options_.wake);
}

// ---------------------------------------------------------------------------------------------
// Suspension
// ---------------------------------------------------------------------------------------------

void BrowserModel::suspend() {
    if (suspended_) {
        return;
    }
    if (pending_) {
        const bool relist = pending_->kind == NavKind::Refresh || pending_->kind == NavKind::Relist;
        if (!relist || !loaded_) {
            deferred_ = Deferred{pending_->kind, pending_->target};
        }
        pending_.reset();
    }
    lister_.reset(); // cancels a running listing and joins
    watcher_.reset();
    watch_tag_ = 0;
    watch_gone_ = true; // the next request adds a fresh watch
    band_active_ = false;
    band_base_.clear();
    // Phase 5: a suspended tab has no threads. A running walk is cancelled (never joined: it
    // detaches) and the rows found so far stay; resume() walks again. Search jobs are CPU-bound
    // and stop within one check interval, so joining the worker is bounded.
    if (flat_ && flat_->walk) {
        flat_->walk->cancel();
        flat_->walk.reset();
        flat_->walking = false;
        flat_->cancelled = true;
        flat_->interrupted = true;
    }
    if (flat_) {
        flat_->in_flight = false;
        flat_->query_due.reset();
    }
    worker_.reset();
    tree_.suspend();
    if (listing_job_out_) {
        listing_job_out_ = false;
        ++listing_job_; // the lost result is re-requested by the next commit (resume re-lists)
    }
    suspended_ = true;
}

void BrowserModel::resume() {
    if (!suspended_) {
        return;
    }
    suspended_ = false;
    tree_.resume();
    start_workers();
    if (deferred_) {
        Deferred d = std::move(*deferred_);
        deferred_.reset();
        request(d.kind, std::move(d.target), /*fold=*/false);
    } else if (loaded_) {
        request(NavKind::Relist, here());
    }
    if (flat_) {
        if (flat_->request.index) {
            post_index_query();
        } else if (flat_->interrupted) {
            start_walk(); // interrupted by suspension: walk again
        } else {
            post_flat_job(true); // suspension may have dropped a job in flight
        }
    }
}

void BrowserModel::restore(const layout::TabState& state, float row_dp, bool record_visit) {
    record_start_ = record_visit;
    sort_ = state.sort;
    show_hidden_ = state.show_hidden;
    types_ = state.type_filter;
    search::normalize(types_);
    Location target;
    target.dir = normalize(state.path);
    target.cursor_name = state.cursor_name;
    restored_dir_ = target.dir;
    restored_rows_ = state.scroll_rows;
    if (state.scroll_rows.has_value()) {
        target.scroll = static_cast<float>(*state.scroll_rows) * row_dp;
    }
    request(NavKind::Start, std::move(target));
}

void BrowserModel::copy_from(const BrowserModel& other) {
    current_ = other.current_;
    entries_ = other.entries_;
    selected_ = other.selected_;
    cursor_ = other.cursor_;
    anchor_ = other.anchor_;
    selected_count_ = other.selected_count_;
    selected_bytes_ = other.selected_bytes_;
    hidden_names_ = other.hidden_names_;
    show_hidden_ = other.show_hidden_;
    sort_ = other.sort_;
    loaded_ = other.loaded_;
    listing_error_ = other.listing_error_;
    back_ = other.back_;
    forward_ = other.forward_;
    scroll_ = other.scroll_;
    restored_scroll_ = other.scroll_;
    listed_sort_ = other.listed_sort_;
    listed_hidden_ = other.listed_hidden_;
    page_rows_ = other.page_rows_;
    restored_dir_ = other.restored_dir_;
    restored_rows_ = other.restored_rows_;
    // Phase 5: the type filter is a persisted view setting and follows the copy; the filter
    // text and the flattened view are runtime-only and do not (tab.duplicate opens unfiltered).
    types_ = other.types_;
    list_mode_ = other.list_mode_;
    view_mode_ = other.view_mode_;
    zoom_percent_ = other.zoom_percent_;
    group_by_ = other.group_by_;
    column_order_ = other.column_order_;
    view_scope_ = other.view_scope_;
    view_rule_path_ = other.view_rule_path_;
    ++listing_version_;
    compile_query();
    {
        const std::size_t cursor_src = other.listing_shown() ? other.list_src(other.cursor_)
                                                             : find(other.flat_->list_cursor_name);
        const std::size_t anchor_src =
            other.listing_shown() ? other.list_src(other.anchor_) : cursor_src;
        list_identity_ = true;
        list_rows_.clear();
        if (listing_filtering()) {
            // Inline even for big folders: the copy is suspended (no worker) and resume()
            // re-lists right away anyway.
            std::vector<std::uint32_t> rows;
            if (filter_listing_rows(entries_, matcher_, types_, nullptr, rows)) {
                list_identity_ = false;
                set_listing_rows(std::move(rows), CursorPolicy::Keep, cursor_src, anchor_src);
            }
        } else {
            cursor_ = cursor_src < entries_.size() ? cursor_src : (entries_.empty() ? kNoRow : 0);
            anchor_ = anchor_src < entries_.size() ? anchor_src : cursor_;
        }
        if (!other.listing_shown()) {
            std::fill(selected_.begin(), selected_.end(), std::uint8_t{0});
            scroll_ = restored_scroll_ = other.flat_->list_scroll;
        }
        recount();
    }
    if (other.pending_ && (!other.loaded_ || (other.pending_->kind != NavKind::Refresh &&
                                              other.pending_->kind != NavKind::Relist))) {
        Deferred d{other.pending_->kind, other.pending_->target};
        if (suspended_) {
            deferred_ = std::move(d);
        } else {
            request(d.kind, std::move(d.target), /*fold=*/false);
        }
    } else if (other.deferred_) {
        if (suspended_) {
            deferred_ = other.deferred_;
        } else {
            request(other.deferred_->kind, other.deferred_->target, /*fold=*/false);
        }
    }
    ++location_serial_;
    ++content_serial_;
    ++cursor_serial_;
}

layout::TabState BrowserModel::tab_state(float row_dp) const {
    layout::TabState s;
    s.sort = sort_;
    s.show_hidden = show_hidden_;
    s.type_filter = types_;
    const Location* unloaded = nullptr;
    if (!loaded_) {
        if (deferred_) {
            unloaded = &deferred_->target;
        } else if (pending_) {
            unloaded = &pending_->target;
        }
    }
    if (unloaded != nullptr) {
        s.path = unloaded->dir;
        s.cursor_name = unloaded->cursor_name;
        // Never listed: hand back exactly what restore() was given (nullopt stays nullopt,
        // 0 stays 0), so save -> load -> save is a fixed point.
        if (restored_rows_ && unloaded->dir == restored_dir_) {
            s.scroll_rows = restored_rows_;
        } else if (unloaded->scroll > 0.0F && row_dp > 0.0F) {
            s.scroll_rows = static_cast<double>(unloaded->scroll / row_dp);
        }
        return s;
    }
    s.path = current_.dir;
    // A flattened view is runtime-only: the tab persists the folder listing under it.
    const float scroll = flat_ ? flat_->list_scroll : scroll_;
    if (flat_) {
        s.cursor_name = flat_->list_cursor_name;
    } else if (cursor_ < row_count()) {
        s.cursor_name = row_entry(cursor_).name;
    }
    if (row_dp > 0.0F) {
        s.scroll_rows = static_cast<double>(std::max(scroll, 0.0F) / row_dp);
    }
    return s;
}

BrowserModel::~BrowserModel() = default;

// ---------------------------------------------------------------------------------------------
// Frame start
// ---------------------------------------------------------------------------------------------

bool BrowserModel::frame_start() {
    bool changed = false;
    if (!lister_) {
        return false; // suspended
    }
    changed = apply_search_results() || changed;
    if (tree_.poll()) {
        if (view_mode_ == view::ViewMode::Tree) {
            rebuild_tree();
        }
        ++content_serial_;
        changed = true;
    }
    for (ListingResult& result : lister_->take()) {
        if (!pending_ || result.generation != pending_->generation) {
            continue; // stale
        }
        Pending pending = std::move(*pending_);
        pending_.reset();
        if (result.entries) {
            commit(pending, result);
        } else {
            fail_request(pending, result);
        }
        changed = true;
    }
    if (watcher_) {
        for (WatchBatch& batch : watcher_->take()) {
            if (pending_ && batch.tag == pending_->watch_tag) {
                if (batch.tag == watch_tag_) {
                    changed = apply_watch_batch(batch) || changed; // same directory: live too
                }
                pending_->replay.push_back(std::move(batch));
            } else if (batch.tag == watch_tag_) {
                changed = apply_watch_batch(batch) || changed;
            }
        }
    }
    return changed;
}

bool BrowserModel::wait_listing(std::chrono::milliseconds timeout) {
    if (!pending_ || !lister_) {
        return true;
    }
    return lister_->wait_for(pending_->generation, timeout);
}

bool BrowserModel::wait_watch_batch(std::chrono::milliseconds timeout) {
    return watcher_ && watcher_->wait_for_batch(timeout);
}

bool BrowserModel::wait_watch_ready(std::chrono::milliseconds timeout) {
    return !watcher_ || watcher_->wait_for_target(timeout);
}

// ---------------------------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------------------------

Location BrowserModel::here() const {
    Location loc;
    loc.dir = current_.dir;
    if (flat_) {
        loc.cursor_name = flat_->list_cursor_name;
        loc.scroll = flat_->list_scroll;
        return loc;
    }
    if (cursor_ < row_count()) {
        loc.cursor_name = row_entry(cursor_).name;
    }
    loc.scroll = scroll_;
    return loc;
}

bool BrowserModel::can_up() const noexcept {
    return loaded_ && !current_.dir.empty() && current_.dir != current_.dir.root_path() &&
           current_.dir.has_parent_path();
}

void BrowserModel::request(NavKind kind, Location target, bool fold) {
    const bool relist = kind == NavKind::Refresh || kind == NavKind::Relist;
    if (suspended_) {
        // Issued by resume(). A re-list is implied by resume() and never replaces a deferred
        // navigation; a navigation replaces whatever was deferred.
        if (!relist) {
            deferred_ = Deferred{kind, std::move(target)};
        }
        return;
    }
    if (fold && relist && pending_ && pending_->kind != NavKind::Refresh &&
        pending_->kind != NavKind::Relist) {
        // A re-list (new sort, hidden toggle) while a navigation is in flight: keep navigating,
        // with the new options.
        kind = pending_->kind;
        target = pending_->target;
    }
    Pending next;
    next.generation = ++generation_;
    next.kind = kind;
    next.target = std::move(target);
    const bool same_dir = loaded_ && next.target.dir == current_.dir;
    // The navigation this request replaces no longer needs the watch it added.
    if (pending_ && pending_->watch_tag != watch_tag_ && watcher_) {
        watcher_->unwatch(pending_->watch_tag);
    }
    if (same_dir && !watch_gone_) {
        next.watch_tag = watch_tag_;
    } else {
        // Another folder, or this one after the kernel dropped its watch (deleted, maybe
        // recreated): add a watch with a fresh tag beside the current one. The current watch
        // stays until the listing commits, so a failed navigation loses nothing.
        next.watch_tag = next.generation;
        if (watcher_) {
            watcher_->watch(next.target.dir, next.watch_tag);
        }
    }
    ListingRequest req{next.generation, next.target.dir, show_hidden_, sort_, {}, {}};
    const bool navigates =
        kind != NavKind::Refresh && kind != NavKind::Relist && kind != NavKind::Start;
    const bool options_changed = sort_.key != listed_sort_.key ||
                                 sort_.descending != listed_sort_.descending ||
                                 show_hidden_ != listed_hidden_;
    if (navigates && options_changed && loaded_) {
        // If this navigation fails we stay here, and the list must still follow the new sort or
        // hidden setting: the worker then lists the current folder in the same job.
        req.fallback = current_.dir;
    }
    if (watcher_) {
        // Read the folder only once its watch is in place (normally microseconds), so changes
        // can't fall between the two. A watch that takes longer than this is covered by the
        // re-list its readiness marker triggers.
        req.before_read = [w = watcher_.get(), tag = next.watch_tag] {
            (void)w->wait_for_tag(tag, std::chrono::milliseconds(200));
        };
    }
    lister_->request(std::move(req));
    pending_ = std::move(next);
}

void BrowserModel::open_start(const Path& dir) {
    Location target;
    target.dir = normalize(dir);
    request(NavKind::Start, std::move(target));
}

void BrowserModel::navigate_to(const Path& dir) { navigate_to(dir, std::string()); }

void BrowserModel::navigate_to(const Path& dir, std::string cursor_name) {
    reset_search_view(); // navigation clears the filter text and leaves flatten (keymap.md)
    const Path target = normalize(dir);
    if (loaded_ && target == current_.dir) {
        // Stay here. This must not fold into a pending navigation elsewhere: the newest request
        // wins.
        if (!cursor_name.empty()) {
            if (const std::size_t row = find_row(cursor_name); row != kNoRow) {
                set_cursor(row, false);
                select_only(row);
                return;
            }
        }
        request(NavKind::Refresh, here(), /*fold=*/false);
        return;
    }
    Location loc;
    loc.dir = target;
    loc.cursor_name = std::move(cursor_name);
    request(NavKind::Push, std::move(loc));
}

std::vector<Path> BrowserModel::take_visits() {
    std::vector<Path> out;
    out.swap(visits_);
    return out;
}

void BrowserModel::back() {
    if (!back_.empty()) {
        reset_search_view();
        request(NavKind::Back, back_.back());
    }
}

void BrowserModel::forward() {
    if (!forward_.empty()) {
        reset_search_view();
        request(NavKind::Forward, forward_.back());
    }
}

void BrowserModel::up() {
    if (!can_up()) {
        return;
    }
    reset_search_view();
    Location target;
    target.dir = current_.dir.parent_path();
    target.cursor_name = current_.dir.filename().string();
    request(NavKind::Up, std::move(target));
}

void BrowserModel::refresh() {
    if (flat_) {
        // F5 re-runs the flattened view (a snapshot, docs/architecture.md "Phase 5").
        if (flat_->request.index) {
            post_index_query();
        } else {
            start_walk();
        }
        return;
    }
    if (loaded_) {
        request(NavKind::Refresh, here());
    }
}

void BrowserModel::toggle_hidden() {
    show_hidden_ = !show_hidden_;
    if (loaded_) {
        request(NavKind::Relist, here());
    }
    if (flat_) {
        refresh(); // the walk (or index query) applies the hidden policy itself
    }
}

void BrowserModel::sort_by(fs::SortKey key) {
    if (sort_.key == key) {
        sort_.descending = !sort_.descending;
    } else {
        sort_.key = key;
        sort_.descending = key == fs::SortKey::Size || key == fs::SortKey::Modified;
    }
    if (loaded_) {
        request(NavKind::Relist, here());
    }
    if (flat_) {
        // A header click sorts by that column instead of relevance until the query changes.
        flat_->header_sort = true;
        flat_->reveal = true;
        if (flat_->request.index) {
            post_index_query();
        } else {
            post_flat_job(true);
        }
    }
}

void BrowserModel::activate() {
    if (cursor_ >= row_count()) {
        return;
    }
    const fs::DirEntry& entry = row_entry(cursor_);
    const Path path = row_path(cursor_);
    if (fs::is_directory_like(entry)) {
        navigate_to(path); // also leaves a flattened view (navigation)
        return;
    }
    // No launcher, no spawn: tests that forget a fake never run xdg-open on the desktop. The
    // app passes platform::create_default_launcher() explicitly.
    const Result<void> opened =
        options_.launcher != nullptr
            ? platform::open_with_default(path, *options_.launcher)
            : Result<void>(fail(ErrorCode::Unsupported, "BrowserModel has no app launcher"));
    if (!opened) {
        raise(NoticeKind::OpenFileFailed, opened.error().code, path);
    }
}

void BrowserModel::commit(Pending& pending, ListingResult& result) {
    const bool relist = pending.kind == NavKind::Refresh || pending.kind == NavKind::Relist;
    const bool first = !loaded_;
    if (!relist && flat_) {
        // A navigation that was already pending when the view flattened: it wins.
        leave_flatten();
    }
    const bool shown = listing_shown();

    // Names to carry over (re-list of the same directory only).
    std::string cursor_name;
    std::string anchor_name;
    std::unordered_set<std::string> selected_names;
    const std::size_t old_cursor = cursor_;
    if (relist) {
        if (!shown) {
            cursor_name = flat_->list_cursor_name;
        } else if (cursor_ < row_count()) {
            cursor_name = row_entry(cursor_).name;
        }
        if (shown && anchor_ < row_count()) {
            anchor_name = row_entry(anchor_).name;
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (i < selected_.size() && selected_[i] != 0) {
                selected_names.insert(entries_[i].name);
            }
        }
    } else if (!first) {
        switch (pending.kind) {
        case NavKind::Push:
        case NavKind::Up:
            back_.push_back(here());
            forward_.clear();
            break;
        case NavKind::Back:
            forward_.push_back(here());
            back_.pop_back();
            break;
        case NavKind::Forward:
            back_.push_back(here());
            forward_.pop_back();
            break;
        case NavKind::Start:
        case NavKind::Refresh:
        case NavKind::Relist: break;
        }
    }

    current_.dir = pending.target.dir;
    if (current_.dir != tree_root_dir_) {
        tree_.reset();
        tree_cursor_ = kNoRow;
        collapsed_groups_.clear();
        tree_root_dir_ = current_.dir;
    }
    entries_ = std::move(*result.entries);
    ++listing_version_;
    hidden_names_.clear();
    hidden_names_.insert(std::make_move_iterator(result.hidden_names.begin()),
                         std::make_move_iterator(result.hidden_names.end()));
    selected_.assign(entries_.size(), 0);
    band_active_ = false;
    band_base_.clear();
    listing_error_.reset();
    loaded_ = true;
    if (pending.watch_tag != watch_tag_) {
        if (watcher_ && watch_tag_ != 0) {
            watcher_->unwatch(watch_tag_); // the folder we left
        }
        watch_tag_ = pending.watch_tag;
        watch_gone_ = false;
    }
    listed_at_ = result.started;
    listed_sort_ = sort_;
    listed_hidden_ = show_hidden_;

    const bool filtering = listing_filtering();
    list_identity_ = true; // entries first; filtered rows are built at the end
    list_rows_.clear();

    std::size_t cursor_src = kNoRow;
    std::size_t anchor_src = kNoRow;
    if (relist) {
        if (!selected_names.empty()) {
            for (std::size_t i = 0; i < entries_.size(); ++i) {
                if (selected_names.contains(entries_[i].name)) {
                    selected_[i] = 1;
                }
            }
        }
        cursor_src = cursor_name.empty() ? kNoRow : find(cursor_name);
        anchor_src = anchor_name.empty() ? kNoRow : find(anchor_name);
    } else {
        cursor_src = pending.target.cursor_name.empty() ? kNoRow : find(pending.target.cursor_name);
        restored_scroll_ = pending.target.scroll;
        scroll_ = pending.target.scroll;
        notice_.reset(); // a successful navigation retires the previous notice
        ++location_serial_;
        // Recents (docs/architecture.md "Phase 4"): a NEW folder was listed. Session restore
        // (Start without record_start_) does not count.
        if (pending.kind != NavKind::Start || record_start_) {
            visits_.push_back(current_.dir);
        }
        record_start_ = false;
    }
    if (shown) {
        cursor_ = cursor_src;
        if (cursor_ == kNoRow && !entries_.empty()) {
            cursor_ = (!relist || old_cursor == kNoRow || filtering)
                          ? 0
                          : std::min(old_cursor, entries_.size() - 1);
        }
        anchor_ = anchor_src == kNoRow ? cursor_ : anchor_src;
        if (entries_.empty()) {
            cursor_ = kNoRow;
            anchor_ = kNoRow;
        }
    }
    // Watch batches seen while the listing ran (identity rows: cursor and anchor follow).
    for (const WatchBatch& batch : pending.replay) {
        (void)apply_watch_batch(batch);
    }
    ++listing_version_;

    if (!shown) {
        // Flattened: the listing updates underneath; its cursor is restored on leave.
        if (filtering) {
            list_stale_ = true;
            list_identity_ = false;
        }
        if (cursor_src != kNoRow && cursor_src < entries_.size()) {
            flat_->list_cursor_name = entries_[cursor_src].name;
        }
        ++content_serial_;
        return;
    }
    if (filtering) {
        const std::size_t cs = cursor_;
        const std::size_t as = anchor_;
        cursor_ = kNoRow;
        anchor_ = kNoRow;
        rebuild_listing_rows(CursorPolicy::Keep, cs, as);
    }
    if (row_count() == 0) {
        cursor_ = kNoRow;
        anchor_ = kNoRow;
    }
    recount();
    if (view_mode_ == view::ViewMode::Tree) {
        rebuild_tree();
    }
    ++content_serial_;
    ++cursor_serial_;
}

void BrowserModel::fail_request(Pending& pending, ListingResult& result) {
    const Error error = result.entries.error();
    switch (pending.kind) {
    case NavKind::Start:
        current_.dir = pending.target.dir;
        entries_.clear();
        ++listing_version_;
        list_identity_ = true;
        list_rows_.clear();
        selected_.clear();
        hidden_names_.clear();
        cursor_ = kNoRow;
        anchor_ = kNoRow;
        loaded_ = true;
        listing_error_ = error;
        watch_tag_ = pending.watch_tag;
        watch_gone_ = false;
        recount();
        ++location_serial_;
        ++content_serial_;
        raise(NoticeKind::OpenFolderFailed, error.code, pending.target.dir);
        return;
    case NavKind::Refresh:
    case NavKind::Relist:
        if (listing_error_) {
            listing_error_ = error; // still unreadable
        }
        raise(NoticeKind::RefreshFailed, error.code, current_.dir);
        break;
    case NavKind::Push:
    case NavKind::Back:
    case NavKind::Forward:
    case NavKind::Up: raise(NoticeKind::OpenFolderFailed, error.code, pending.target.dir); break;
    }
    if (pending.watch_tag != watch_tag_ && watcher_) {
        watcher_->unwatch(pending.watch_tag); // the current folder kept its own watch
    }
    if (result.fallback && result.fallback->entries) {
        // The navigation carried a new sort or hidden setting (see request()): the worker
        // re-listed the folder we stay in, in the same job. Apply it as a re-list.
        Pending relist;
        relist.generation = pending.generation;
        relist.kind = NavKind::Relist;
        relist.target = here();
        relist.watch_tag = watch_tag_;
        commit(relist, *result.fallback);
    }
}

void BrowserModel::raise(NoticeKind kind, ErrorCode code, Path path) {
    notice_ = Notice{kind, code, std::move(path), ++notice_serial_};
}

void BrowserModel::dismiss_notice() noexcept { notice_.reset(); }

void BrowserModel::cursor_to(std::size_t row) {
    if (row >= row_count()) {
        return;
    }
    cursor_ = row;
    anchor_ = row;
    if (flat_) {
        flat_->cursor_follows_top = false;
    }
    ++cursor_serial_;
}

bool BrowserModel::notice_shown() const noexcept {
    if (!notice_) {
        return false;
    }
    const bool about_shown_folder = notice_->kind == NoticeKind::OpenFolderFailed &&
                                    listing_error_.has_value() && notice_->path == current_.dir;
    return !about_shown_folder;
}

void BrowserModel::escape() {
    // One step per press, innermost first (docs/keymap.md "Focus scoping (Phase 5)").
    const bool running =
        flat_ && (flat_->walking || (flat_->request.index && !flat_->refreshing &&
                                     (flat_->in_flight || flat_->query_due.has_value())));
    switch (list_escape_step(notice_shown(), running, selected_count_ > 0, flat_ != nullptr,
                             filter_active())) {
    case ListEscape::DismissNotice: dismiss_notice(); return;
    case ListEscape::CancelSearch: cancel_search(); return;
    case ListEscape::ClearSelection: clear_selection(); return;
    case ListEscape::LeaveFlatten: leave_flatten(); return;
    case ListEscape::ClearFilter: clear_filter(); return;
    case ListEscape::Nothing: return;
    }
}

// ---------------------------------------------------------------------------------------------
// Cursor and selection (row space)
// ---------------------------------------------------------------------------------------------

std::size_t BrowserModel::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) {
            return i;
        }
    }
    return kNoRow;
}

bool BrowserModel::is_selected(std::size_t row) const noexcept {
    if (flat_) {
        return row < flat_->rows.size() && flat_->sel[flat_->rows[row]] != 0;
    }
    const std::size_t src = list_src(row);
    return src < selected_.size() && selected_[src] != 0;
}

std::uint8_t& BrowserModel::flag(std::size_t row) noexcept {
    if (flat_) {
        return flat_->sel[flat_->rows[row]];
    }
    return selected_[list_src(row)];
}

void BrowserModel::recount() {
    selected_count_ = 0;
    selected_bytes_ = 0;
    const std::size_t n = row_count();
    for (std::size_t i = 0; i < n; ++i) {
        if (is_selected(i)) {
            ++selected_count_;
            const fs::DirEntry& e = row_entry(i);
            if (!fs::is_directory_like(e)) {
                selected_bytes_ += e.size;
            }
        }
    }
}

namespace {

void clear_flags(std::vector<std::uint8_t>& flags) {
    std::fill(flags.begin(), flags.end(), std::uint8_t{0});
}

} // namespace

void BrowserModel::select_only(std::size_t row) {
    clear_flags(flat_ ? flat_->sel : selected_);
    // Every cursor move lands here: no O(rows) recount over a 300k-row flattened view.
    selected_count_ = 0;
    selected_bytes_ = 0;
    if (row < row_count()) {
        flag(row) = 1;
        selected_count_ = 1;
        const fs::DirEntry& e = row_entry(row);
        selected_bytes_ = fs::is_directory_like(e) ? 0 : e.size;
    }
}

void BrowserModel::select_range(std::size_t a, std::size_t b, bool add) {
    const std::size_t n = row_count();
    if (!add) {
        clear_flags(flat_ ? flat_->sel : selected_);
    }
    if (n == 0) {
        recount();
        return;
    }
    const std::size_t lo = std::min(a, b);
    const std::size_t hi = std::min(std::max(a, b), n - 1);
    for (std::size_t i = lo; i <= hi && i < n; ++i) {
        flag(i) = 1;
    }
    recount();
}

void BrowserModel::set_cursor(std::size_t row, bool extend) {
    const std::size_t n = row_count();
    if (n == 0) {
        return;
    }
    if (flat_) {
        flat_->cursor_follows_top = false;
    }
    row = std::min(row, n - 1);
    cursor_ = row;
    if (extend) {
        if (anchor_ >= n) {
            anchor_ = row;
        }
        select_range(anchor_, row, false);
    } else {
        anchor_ = row;
        select_only(row);
    }
    ++cursor_serial_;
}

void BrowserModel::cursor_by(std::ptrdiff_t delta, bool extend) {
    const std::size_t n = row_count();
    if (n == 0) {
        return;
    }
    const auto last = static_cast<std::ptrdiff_t>(n - 1);
    std::ptrdiff_t next = 0;
    if (cursor_ >= n) {
        next = delta >= 0 ? 0 : last;
    } else {
        next = std::clamp(static_cast<std::ptrdiff_t>(cursor_) + delta, std::ptrdiff_t{0}, last);
    }
    set_cursor(static_cast<std::size_t>(next), extend);
}

void BrowserModel::cursor_page(int direction, bool extend) {
    const auto step = static_cast<std::ptrdiff_t>(page_rows_ > 1 ? page_rows_ - 1 : 1);
    cursor_by(direction >= 0 ? step : -step, extend);
}

void BrowserModel::cursor_home(bool extend) { set_cursor(0, extend); }

void BrowserModel::cursor_end(bool extend) {
    if (const std::size_t n = row_count(); n > 0) {
        set_cursor(n - 1, extend);
    }
}

void BrowserModel::select_all() {
    // Every SHOWN row (docs/keymap.md: Ctrl+A in a filtered list selects the matches).
    const std::size_t n = row_count();
    for (std::size_t i = 0; i < n; ++i) {
        flag(i) = 1;
    }
    recount();
}

void BrowserModel::clear_selection() {
    clear_flags(flat_ ? flat_->sel : selected_);
    selected_count_ = 0;
    selected_bytes_ = 0;
}

void BrowserModel::click_row(std::size_t row, Modifiers mods) {
    const std::size_t n = row_count();
    if (row >= n) {
        return;
    }
    if (flat_) {
        flat_->cursor_follows_top = false;
    }
    const bool ctrl = has(mods, Modifiers::Ctrl);
    const bool shift = has(mods, Modifiers::Shift);
    if (anchor_ >= n) {
        anchor_ = row;
    }
    if (shift) {
        select_range(anchor_, row, ctrl); // Ctrl+Shift adds the range
    } else if (ctrl) {
        std::uint8_t& f = flag(row);
        f = f != 0 ? 0 : 1;
        anchor_ = row;
        recount();
    } else {
        anchor_ = row;
        select_only(row);
    }
    cursor_ = row;
    ++cursor_serial_;
}

void BrowserModel::band_begin(bool additive) {
    const std::size_t n = row_count();
    band_active_ = true;
    band_additive_ = additive;
    band_base_.assign(n, 0);
    if (additive) {
        for (std::size_t i = 0; i < n; ++i) {
            band_base_[i] = is_selected(i) ? 1 : 0;
        }
    }
}

void BrowserModel::band_update(std::size_t first, std::size_t last) {
    const std::size_t n = row_count();
    if (!band_active_ || band_base_.size() != n) {
        return; // the rows changed under the band (flatten results, a filtered watch change)
    }
    for (std::size_t i = 0; i < n; ++i) {
        flag(i) = band_base_[i];
    }
    if (first <= last) {
        for (std::size_t i = first; i <= last && i < n; ++i) {
            flag(i) = 1;
        }
    }
    recount();
}

std::size_t BrowserModel::select_by_names(const std::vector<std::string>& names) {
    std::size_t found = 0;
    for (const std::string& name : names) {
        const std::size_t row = find_row(name);
        if (row == kNoRow) {
            continue;
        }
        click_row(row, found == 0 ? Modifiers::None : Modifiers::Ctrl);
        ++found;
    }
    return found;
}

void BrowserModel::band_end() {
    band_active_ = false;
    band_base_.clear();
}

bool BrowserModel::execute(const Command& command) {
    const bool extend = (command.arg & CursorArg::Extend) != 0U;
    const bool keep_sel = !extend && (command.arg & CursorArg::KeepSelection) != 0U;
    const auto move_cursor_only = [&](std::ptrdiff_t delta, std::optional<std::size_t> abs_target) {
        const std::size_t n = row_count();
        if (n == 0) {
            return;
        }
        if (flat_) {
            flat_->cursor_follows_top = false;
        }
        std::size_t next = 0;
        if (abs_target) {
            next = std::min(*abs_target, n - 1);
        } else {
            const std::ptrdiff_t cur =
                cursor_ < n ? static_cast<std::ptrdiff_t>(cursor_) : std::ptrdiff_t{0};
            const std::ptrdiff_t clamped =
                std::clamp<std::ptrdiff_t>(cur + delta, 0, static_cast<std::ptrdiff_t>(n - 1));
            next = static_cast<std::size_t>(clamped);
        }
        cursor_ = next;
        ++cursor_serial_;
    };
    switch (command.id) {
    case CommandId::ToggleHidden: toggle_hidden(); return true;
    case CommandId::NavigateBack: back(); return true;
    case CommandId::NavigateForward: forward(); return true;
    case CommandId::NavigateUp: up(); return true;
    case CommandId::Refresh: refresh(); return true;
    case CommandId::Activate: activate(); return true;
    case CommandId::SelectAll: select_all(); return true;
    case CommandId::ClearSelection: escape(); return true;
    case CommandId::ToggleCursorSelected:
        if (cursor_ < row_count()) {
            click_row(cursor_, Modifiers::Ctrl);
        }
        return true;
    case CommandId::CursorUp:
        if (keep_sel) {
            move_cursor_only(-1, std::nullopt);
        } else {
            cursor_by(-1, extend);
        }
        return true;
    case CommandId::CursorDown:
        if (keep_sel) {
            move_cursor_only(1, std::nullopt);
        } else {
            cursor_by(1, extend);
        }
        return true;
    case CommandId::CursorPageUp:
        if (keep_sel) {
            const auto step = static_cast<std::ptrdiff_t>(page_rows_ > 1 ? page_rows_ - 1 : 1);
            move_cursor_only(-step, std::nullopt);
        } else {
            cursor_page(-1, extend);
        }
        return true;
    case CommandId::CursorPageDown:
        if (keep_sel) {
            const auto step = static_cast<std::ptrdiff_t>(page_rows_ > 1 ? page_rows_ - 1 : 1);
            move_cursor_only(step, std::nullopt);
        } else {
            cursor_page(1, extend);
        }
        return true;
    case CommandId::CursorHome:
        if (keep_sel) {
            move_cursor_only(0, std::size_t{0});
        } else {
            cursor_home(extend);
        }
        return true;
    case CommandId::CursorEnd:
        if (keep_sel) {
            if (const std::size_t n = row_count(); n > 0) {
                move_cursor_only(0, n - 1);
            }
        } else {
            cursor_end(extend);
        }
        return true;
    // Phase 5 (tab level). Focus and popups are the shell's (FocusFilter, ToggleTypesPopup);
    // the volume flatten and the index need the App's volume table and index service.
    case CommandId::ClearFilter: clear_filter(); return true;
    case CommandId::ToggleFlatten:
        if (flat_) {
            leave_flatten();
        } else {
            flatten(FlattenRequest{});
        }
        return true;
    case CommandId::ToggleFuzzy: toggle_match_mode(); return true;
    case CommandId::CancelSearch: cancel_search(); return true;
    case CommandId::FocusFilter:
    case CommandId::FlattenVolume:
    case CommandId::ToggleTypesPopup:
    case CommandId::RebuildIndex:
    // Phase 6: the Inspector (shell level; ui::App).
    case CommandId::Inspect:
    case CommandId::ToggleInspectorLock:
    case CommandId::InspectorZoomFit:
    case CommandId::InspectorZoom1to1:
    case CommandId::InspectorPlay:
    case CommandId::CycleInspectorPlacement:
    case CommandId::ToggleInspectorGrid:
    case CommandId::ToggleInspectorMinimap:
    case CommandId::None:
    case CommandId::Quit:
    case CommandId::ToggleFpsOverlay:
    case CommandId::ToggleTheme:
    // Phase 3: workspace level (tabs, panes, layouts; ui::Workspace).
    case CommandId::OpenInNewTab:
    case CommandId::NewTab:
    case CommandId::CloseTab:
    case CommandId::DuplicateTab:
    case CommandId::CloseLeftTabs:
    case CommandId::ReopenClosedTab:
    case CommandId::NextTab:
    case CommandId::PreviousTab:
    case CommandId::SearchTabs:
    case CommandId::MoveTabLeft:
    case CommandId::MoveTabRight:
    case CommandId::SplitRight:
    case CommandId::SplitDown:
    case CommandId::FocusNextPane:
    case CommandId::FocusPreviousPane:
    case CommandId::ClosePane:
    case CommandId::SaveLayout:
    case CommandId::LoadLayout:
    case CommandId::DeleteLayout:
    case CommandId::NewWindow:
    // Phase 4: shell level (sidebar, GoTo, address bar, bookmarks, clipboard).
    case CommandId::FocusAddressBar:
    case CommandId::GoTo:
    case CommandId::ToggleSidebar:
    case CommandId::ToggleViewLock:
    case CommandId::FocusSidebar:
    case CommandId::ToggleBookmark:
    case CommandId::RenameBookmark:
    case CommandId::MoveBookmarkUp:
    case CommandId::MoveBookmarkDown:
    case CommandId::ClearRecents:
    case CommandId::ToggleRecentsTracking:
    // Phase 7: file operations, trash and undo (shell level; ui::App and ui::FileOps).
    case CommandId::Copy:
    case CommandId::Cut:
    case CommandId::Paste:
    case CommandId::Undo:
    case CommandId::Redo:
    case CommandId::MoveToTrash:
    case CommandId::DeletePermanently:
    case CommandId::Rename:
    case CommandId::NewFolder:
    case CommandId::CopyToOtherPane:
    case CommandId::MoveToOtherPane:
    case CommandId::OpenTerminalHere:
    case CommandId::CancelOperation:
    case CommandId::TogglePauseOperation:
    case CommandId::ToggleOperationsPanel:
    case CommandId::OpenTrash:
    case CommandId::RestoreFromTrash:
    case CommandId::EmptyTrash:
    case CommandId::CopyPath:
    // Phase 8: palette, settings, keymap editor, context menu, Open With (shell level).
    case CommandId::OpenPalette:
    case CommandId::OpenSettings:
    case CommandId::EditKeymap:
    case CommandId::ResetKeymap:
    case CommandId::ExportKeymap:
    case CommandId::ShowContextMenu:
    case CommandId::OpenWith:
    // Phase 9A: view/zoom/group/lock commands are handled at shell level so ViewSettingsStore
    // persists them; tree commands run directly on this model.
    case CommandId::ViewModeDetails:
    case CommandId::ViewModeIcons:
    case CommandId::ViewModeList:
    case CommandId::ViewModeTree:
    case CommandId::ViewCycleMode:
    case CommandId::ViewZoomIn:
    case CommandId::ViewZoomOut:
    case CommandId::ViewZoomReset:
    case CommandId::GroupByNone:
    case CommandId::GroupByDate:
    case CommandId::GroupByType:
    case CommandId::GroupBySize:
    case CommandId::GroupCycle:
    case CommandId::ViewLockFolder:
    case CommandId::ViewLockHierarchy:
    case CommandId::ViewLockGlobal: return false;
    case CommandId::TreeExpand: tree_expand_cursor(); return true;
    case CommandId::TreeCollapse: tree_collapse_cursor(); return true;
    case CommandId::TreeCollapseAll: tree_collapse_all(); return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Watch updates
// ---------------------------------------------------------------------------------------------

bool BrowserModel::before(const fs::DirEntry& a, const fs::DirEntry& b) const noexcept {
    if (sort_.folders_first) {
        const bool da = fs::is_directory_like(a);
        const bool db = fs::is_directory_like(b);
        if (da != db) {
            return da;
        }
    }
    std::weak_ordering c = std::weak_ordering::equivalent;
    switch (sort_.key) {
    case fs::SortKey::Size: c = a.size <=> b.size; break;
    case fs::SortKey::Modified: c = a.mtime <=> b.mtime; break;
    case fs::SortKey::Extension:
        c = fs::compare_names(fs::extension(a.name), fs::extension(b.name), sort_);
        break;
    case fs::SortKey::Name:
    case fs::SortKey::Count: break;
    }
    if (c == 0) {
        c = fs::compare_names(a.name, b.name, sort_);
    }
    if (c == 0) {
        c = std::string_view(a.name) <=> std::string_view(b.name);
    }
    return sort_.descending ? c > 0 : c < 0;
}

std::size_t BrowserModel::insert_sorted(fs::DirEntry entry, bool selected) {
    std::size_t lo = 0;
    std::size_t hi = entries_.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) / 2);
        if (before(entries_[mid], entry)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    const auto at = static_cast<std::ptrdiff_t>(lo);
    entries_.insert(entries_.begin() + at, std::move(entry));
    selected_.insert(selected_.begin() + at, selected ? 1 : 0);
    ++listing_version_;
    if (!list_identity_) {
        listing_inserted(lo); // filtered rows shift; the new entry joins them if it matches
    } else if (listing_shown()) {
        if (band_active_) {
            band_base_.insert(band_base_.begin() + at, 0);
        }
        if (cursor_ != kNoRow && cursor_ >= lo) {
            ++cursor_;
        }
        if (anchor_ != kNoRow && anchor_ >= lo) {
            ++anchor_;
        }
    }
    return lo;
}

void BrowserModel::erase_row(std::size_t row) {
    const auto at = static_cast<std::ptrdiff_t>(row);
    entries_.erase(entries_.begin() + at);
    selected_.erase(selected_.begin() + at);
    ++listing_version_;
    if (!list_identity_) {
        listing_erased(row);
        return;
    }
    if (!listing_shown()) {
        return;
    }
    if (band_active_ && row < band_base_.size()) {
        band_base_.erase(band_base_.begin() + at);
    }
    const auto fix = [&](std::size_t& index) {
        if (index == kNoRow) {
            return;
        }
        if (index > row) {
            --index;
        } else if (index == row && index >= entries_.size()) {
            index = entries_.empty() ? kNoRow : entries_.size() - 1;
        }
    };
    fix(cursor_);
    fix(anchor_);
}

bool BrowserModel::apply_change(const WatchChange& change) {
    const auto visible = [this](std::string_view name) {
        return show_hidden_ || !fs::is_hidden_name(name);
    };
    const auto track_hidden = [this](const std::string& name, bool present) {
        if (fs::is_hidden_name(name)) {
            if (present) {
                hidden_names_.insert(name);
            } else {
                hidden_names_.erase(name);
            }
        }
    };
    // Removes `name` if listed; reports its selection/cursor/anchor state (row space).
    struct Taken {
        bool found = false;
        bool selected = false;
        bool cursor = false;
        bool anchor = false;
    };
    const auto take = [this](std::string_view name) {
        Taken t;
        const std::size_t src = find(name);
        if (src == kNoRow) {
            return t;
        }
        t.found = true;
        t.selected = selected_[src] != 0;
        if (listing_shown()) {
            const std::size_t row = list_row_of(src);
            t.cursor = row != kNoRow && cursor_ == row;
            t.anchor = row != kNoRow && anchor_ == row;
        }
        erase_row(src);
        return t;
    };
    const auto put = [this](fs::DirEntry entry, const Taken& t) {
        const std::size_t src = insert_sorted(std::move(entry), t.selected);
        const std::size_t row = list_row_of(src);
        if (row == kNoRow) {
            selected_[src] = 0; // hidden by the filter: never selected (invariant)
            return;
        }
        if (!listing_shown()) {
            return;
        }
        if (t.cursor) {
            cursor_ = row;
        }
        if (t.anchor) {
            anchor_ = row;
        }
        if (cursor_ == kNoRow) {
            cursor_ = row; // first entry of a folder that was empty
            anchor_ = row;
        }
    };

    switch (change.kind) {
    case WatchChange::Kind::Upsert: {
        if (!change.entry) {
            return false;
        }
        track_hidden(change.name, true);
        if (!visible(change.name)) {
            return true; // status bar count changed
        }
        fs::DirEntry entry = *change.entry;
        entry.name = change.name;
        put(std::move(entry), take(change.name));
        return true;
    }
    case WatchChange::Kind::Remove: {
        track_hidden(change.name, false);
        (void)take(change.name);
        return true;
    }
    case WatchChange::Kind::Rename: {
        track_hidden(change.old_name, false);
        const std::size_t old_src = find(change.old_name);
        std::optional<fs::DirEntry> entry = change.entry;
        if (!entry && old_src != kNoRow) {
            entry = entries_[old_src];
        }
        const Taken t = take(change.old_name);
        if (!entry) {
            return t.found;
        }
        entry->name = change.name;
        entry->hidden = fs::is_hidden_name(change.name);
        track_hidden(change.name, true);
        if (!visible(change.name)) {
            return true;
        }
        Taken existing = take(change.name); // renamed over an existing name
        existing.selected = t.selected || existing.selected;
        existing.cursor = t.cursor || existing.cursor;
        existing.anchor = t.anchor || existing.anchor;
        put(std::move(*entry), existing);
        return true;
    }
    }
    return false;
}

std::vector<WatchChange> BrowserModel::pair_moves(const std::vector<WatchChange>& changes) const {
    // A rename whose MovedFrom and MovedTo straddled a read() arrives as Remove(old) followed by
    // Upsert(new). When the Upsert's fresh stat has the FileId of the removed row, it is that
    // row renamed: turn the pair into one Rename so selection, cursor and anchor follow it.
    std::vector<WatchChange> out = changes;
    std::vector<bool> drop(out.size(), false);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i].kind != WatchChange::Kind::Remove) {
            continue;
        }
        const std::size_t row = find(out[i].name);
        if (row == kNoRow) {
            continue;
        }
        const FileId id = entries_[row].id;
        if (id.device == 0 && id.inode == 0) {
            continue;
        }
        for (std::size_t j = i + 1; j < out.size(); ++j) {
            if (out[j].name == out[i].name ||
                (out[j].kind == WatchChange::Kind::Rename && out[j].old_name == out[i].name)) {
                break; // the old name comes back first: not a plain rename
            }
            if (drop[j] || out[j].kind != WatchChange::Kind::Upsert) {
                continue;
            }
            const std::optional<fs::DirEntry>& fresh = out[j].entry;
            if (!fresh.has_value() || fresh->id != id) {
                continue;
            }
            out[j].kind = WatchChange::Kind::Rename;
            out[j].old_name = out[i].name;
            drop[i] = true;
            break;
        }
    }
    std::vector<WatchChange> kept;
    kept.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!drop[i]) {
            kept.push_back(std::move(out[i]));
        }
    }
    return kept;
}

bool BrowserModel::apply_watch_batch(const WatchBatch& batch) {
    if (batch.ready) {
        // The watch became active after this folder's listing began: changes in between were
        // seen by neither. Re-list once (the new listing begins after the watch is in place).
        if (batch.watching && batch.at > listed_at_ && !listing_error_ && !busy()) {
            request(NavKind::Relist, here());
        }
        return false;
    }
    bool changed = false;
    if (batch.overflow || batch.changes.size() > options_.max_incremental_changes) {
        if (!busy()) {
            request(NavKind::Relist, here());
        }
    } else {
        for (const WatchChange& change : pair_moves(batch.changes)) {
            changed = apply_change(change) || changed;
        }
        if (changed) {
            type_rows_valid_ = false;
            if (listing_shown()) {
                const std::size_t n = row_count();
                if (cursor_ >= n) {
                    cursor_ = n == 0 ? kNoRow : n - 1;
                }
                if (!list_identity_) {
                    band_active_ = false; // filtered rows moved under the band
                }
                recount();
            }
            ++content_serial_;
        }
    }
    if (batch.gone) {
        watch_gone_ = true; // the next request for this folder watches it again
        raise(NoticeKind::FolderGone, ErrorCode::NotFound, current_.dir);
        changed = true;
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------
// Phase 9A: view modes, zoom, grouping, column order, tree, hierarchical view lock
// ---------------------------------------------------------------------------------------------

view::ViewSettings BrowserModel::view_settings() const noexcept {
    view::ViewSettings s;
    s.mode = view_mode_;
    s.zoom_percent = zoom_percent_;
    s.group_by = group_by_;
    s.sort = sort_;
    s.show_hidden = show_hidden_;
    s.columns = column_order_;
    return s;
}

void BrowserModel::rebuild_tree() {
    auto entry_fn = [](const void* ctx, std::size_t row) -> const fs::DirEntry* {
        const auto* self = static_cast<const BrowserModel*>(ctx);
        if (row >= self->row_count()) {
            return nullptr;
        }
        return &self->row_entry(row);
    };
    const Path prev_path =
        tree_cursor_ < tree_.rows().size() ? tree_.rows()[tree_cursor_].full_path : Path{};
    tree_.rebuild(current_.dir, row_count(), entry_fn, this, sort_, show_hidden_);
    const auto& tr = tree_.rows();
    if (tr.empty()) {
        tree_cursor_ = kNoRow;
        return;
    }
    if (!prev_path.empty()) {
        if (const std::size_t idx = tree_.find_path(prev_path); idx != kTreeNoRootRow) {
            tree_cursor_ = idx;
            return;
        }
    }
    if (cursor_ < row_count()) {
        const Path cur_path = current_.dir / row_entry(cursor_).name;
        if (const std::size_t idx = tree_.find_path(cur_path); idx != kTreeNoRootRow) {
            tree_cursor_ = idx;
            return;
        }
    }
    tree_cursor_ = std::min(tree_cursor_ == kNoRow ? std::size_t{0} : tree_cursor_, tr.size() - 1);
}

void BrowserModel::apply_view_settings(const view::ViewSettings& raw, view::ViewScope scope,
                                       Path rule_path) {
    const view::ViewSettings s = view::normalize(raw);
    const bool sort_changed = s.sort != sort_;
    const bool hidden_changed = s.show_hidden != show_hidden_;
    const bool group_changed = s.group_by != group_by_;
    view_mode_ = s.mode;
    zoom_percent_ = s.zoom_percent;
    group_by_ = s.group_by;
    column_order_ = s.columns;
    view_scope_ = scope;
    view_rule_path_ = std::move(rule_path);
    if (group_changed) {
        collapsed_groups_.clear();
    }
    if (sort_changed) {
        sort_ = s.sort;
        tree_.resort_cached(sort_);
    }
    if (hidden_changed) {
        show_hidden_ = s.show_hidden;
        tree_.reload_expanded(sort_, show_hidden_);
    }
    if ((sort_changed || hidden_changed) && loaded_) {
        request(NavKind::Relist, here());
    }
    if (view_mode_ == view::ViewMode::Tree) {
        rebuild_tree();
    }
    ++content_serial_;
}

void BrowserModel::set_view_mode(view::ViewMode mode) {
    if (view_mode_ == mode) {
        return;
    }
    view_mode_ = mode;
    if (view_mode_ == view::ViewMode::Tree) {
        rebuild_tree();
    } else if (tree_cursor_ < tree_.rows().size()) {
        // Carry cursor back to the corresponding depth-0 row when leaving Tree.
        std::size_t idx = tree_cursor_;
        while (idx != kTreeNoRootRow && idx < tree_.rows().size() &&
               tree_.rows()[idx].root_row == kTreeNoRootRow) {
            idx = tree_.rows()[idx].parent_index;
        }
        if (idx != kTreeNoRootRow && idx < tree_.rows().size()) {
            const std::size_t r = tree_.rows()[idx].root_row;
            if (r < row_count()) {
                cursor_ = r;
                ++cursor_serial_;
            }
        }
    }
    ++content_serial_;
}

void BrowserModel::set_zoom_percent(int percent) {
    const int clamped = view::clamp_zoom(percent);
    if (zoom_percent_ == clamped) {
        return;
    }
    zoom_percent_ = clamped;
    ++content_serial_;
}

void BrowserModel::step_zoom(int delta_steps) {
    const auto [next_mode, next_zoom] = view::step_zoom(view_mode_, zoom_percent_, delta_steps);
    const bool changed = next_mode != view_mode_ || next_zoom != zoom_percent_;
    zoom_percent_ = next_zoom;
    if (next_mode != view_mode_) {
        set_view_mode(next_mode);
    } else if (changed) {
        ++content_serial_;
    }
}

void BrowserModel::set_group_by(view::GroupBy group) {
    if (group_by_ == group) {
        return;
    }
    group_by_ = group;
    collapsed_groups_.clear();
    ++content_serial_;
}

void BrowserModel::reorder_column(std::size_t from, std::size_t to) {
    const view::ColumnOrder next = view::reorder_column(column_order_, from, to);
    if (next == column_order_) {
        return;
    }
    column_order_ = next;
    ++content_serial_;
}

void BrowserModel::set_view_scope(view::ViewScope scope, Path rule_path) noexcept {
    view_scope_ = scope;
    view_rule_path_ = std::move(rule_path);
}

void BrowserModel::toggle_group_collapsed(std::uint8_t bucket) {
    if (collapsed_groups_.contains(bucket)) {
        collapsed_groups_.erase(bucket);
    } else {
        collapsed_groups_.insert(bucket);
    }
    ++content_serial_;
}

void BrowserModel::set_tree_cursor(std::size_t idx) {
    const auto& tr = tree_.rows();
    if (tr.empty()) {
        tree_cursor_ = kNoRow;
        return;
    }
    tree_cursor_ = std::min(idx, tr.size() - 1);
    if (tr[tree_cursor_].root_row != kTreeNoRootRow && tr[tree_cursor_].root_row < row_count()) {
        cursor_ = tr[tree_cursor_].root_row;
    }
    ++cursor_serial_;
}

void BrowserModel::tree_toggle(const Path& dir) {
    if (tree_.toggle(dir, sort_, show_hidden_)) {
        rebuild_tree();
        ++content_serial_;
    }
}

void BrowserModel::tree_expand_cursor() {
    if (view_mode_ != view::ViewMode::Tree) {
        return;
    }
    const auto& tr = tree_.rows();
    if (tree_cursor_ >= tr.size()) {
        return;
    }
    const TreeRow& row = tr[tree_cursor_];
    if (!row.expandable) {
        return;
    }
    if (!row.expanded) {
        if (tree_.expand(row.full_path, sort_, show_hidden_)) {
            rebuild_tree();
            ++content_serial_;
        }
    } else if (tree_cursor_ + 1 < tr.size() && tr[tree_cursor_ + 1].parent_index == tree_cursor_) {
        set_tree_cursor(tree_cursor_ + 1);
    }
}

void BrowserModel::tree_collapse_cursor() {
    if (view_mode_ != view::ViewMode::Tree) {
        return;
    }
    const auto& tr = tree_.rows();
    if (tree_cursor_ >= tr.size()) {
        return;
    }
    const TreeRow& row = tr[tree_cursor_];
    if (row.expanded) {
        if (tree_.collapse(row.full_path)) {
            rebuild_tree();
            ++content_serial_;
        }
    } else if (row.parent_index != kTreeNoRootRow && row.parent_index < tr.size()) {
        set_tree_cursor(row.parent_index);
    }
}

void BrowserModel::tree_collapse_all() {
    if (tree_.collapse_all()) {
        rebuild_tree();
        ++content_serial_;
    }
}

} // namespace navigator::ui
