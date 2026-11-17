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

BrowserModel::BrowserModel(Options options) : options_(std::move(options)) {
    if (options_.start_suspended) {
        suspended_ = true;
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
    suspended_ = true;
}

void BrowserModel::resume() {
    if (!suspended_) {
        return;
    }
    suspended_ = false;
    start_workers();
    if (deferred_) {
        Deferred d = std::move(*deferred_);
        deferred_.reset();
        request(d.kind, std::move(d.target), /*fold=*/false);
    } else if (loaded_) {
        request(NavKind::Relist, here());
    }
}

void BrowserModel::restore(const layout::TabState& state, float row_dp) {
    sort_ = state.sort;
    show_hidden_ = state.show_hidden;
    Location target;
    target.dir = normalize(state.path);
    target.cursor_name = state.cursor_name;
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
        if (unloaded->scroll > 0.0F && row_dp > 0.0F) {
            s.scroll_rows = static_cast<double>(unloaded->scroll / row_dp);
        }
        return s;
    }
    s.path = current_.dir;
    if (cursor_ < entries_.size()) {
        s.cursor_name = entries_[cursor_].name;
    }
    if (row_dp > 0.0F) {
        s.scroll_rows = static_cast<double>(std::max(scroll_, 0.0F) / row_dp);
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
    if (cursor_ < entries_.size()) {
        loc.cursor_name = entries_[cursor_].name;
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

void BrowserModel::navigate_to(const Path& dir) {
    const Path target = normalize(dir);
    if (loaded_ && target == current_.dir) {
        // Stay here. This must not fold into a pending navigation elsewhere: the newest request
        // wins.
        request(NavKind::Refresh, here(), /*fold=*/false);
        return;
    }
    Location loc;
    loc.dir = target;
    request(NavKind::Push, std::move(loc));
}

void BrowserModel::back() {
    if (!back_.empty()) {
        request(NavKind::Back, back_.back());
    }
}

void BrowserModel::forward() {
    if (!forward_.empty()) {
        request(NavKind::Forward, forward_.back());
    }
}

void BrowserModel::up() {
    if (!can_up()) {
        return;
    }
    Location target;
    target.dir = current_.dir.parent_path();
    target.cursor_name = current_.dir.filename().string();
    request(NavKind::Up, std::move(target));
}

void BrowserModel::refresh() {
    if (loaded_) {
        request(NavKind::Refresh, here());
    }
}

void BrowserModel::toggle_hidden() {
    show_hidden_ = !show_hidden_;
    if (loaded_) {
        request(NavKind::Relist, here());
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
}

void BrowserModel::activate() {
    if (cursor_ >= entries_.size()) {
        return;
    }
    const fs::DirEntry& entry = entries_[cursor_];
    const Path path = current_.dir / entry.name;
    if (fs::is_directory_like(entry)) {
        navigate_to(path);
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

    // Names to carry over (re-list of the same directory only).
    std::string cursor_name;
    std::string anchor_name;
    std::unordered_set<std::string> selected_names;
    const std::size_t old_cursor = cursor_;
    if (relist) {
        if (cursor_ < entries_.size()) {
            cursor_name = entries_[cursor_].name;
        }
        if (anchor_ < entries_.size()) {
            anchor_name = entries_[anchor_].name;
        }
        if (selected_count_ > 0) {
            for (std::size_t i = 0; i < entries_.size(); ++i) {
                if (selected_[i] != 0) {
                    selected_names.insert(entries_[i].name);
                }
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
    entries_ = std::move(*result.entries);
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

    if (relist) {
        if (!selected_names.empty()) {
            for (std::size_t i = 0; i < entries_.size(); ++i) {
                if (selected_names.contains(entries_[i].name)) {
                    selected_[i] = 1;
                }
            }
        }
        cursor_ = cursor_name.empty() ? kNoRow : find(cursor_name);
        if (cursor_ == kNoRow && !entries_.empty()) {
            cursor_ = old_cursor == kNoRow ? 0 : std::min(old_cursor, entries_.size() - 1);
        }
        anchor_ = anchor_name.empty() ? kNoRow : find(anchor_name);
        if (anchor_ == kNoRow) {
            anchor_ = cursor_;
        }
    } else {
        cursor_ = pending.target.cursor_name.empty() ? kNoRow : find(pending.target.cursor_name);
        if (cursor_ == kNoRow && !entries_.empty()) {
            cursor_ = 0;
        }
        anchor_ = cursor_;
        restored_scroll_ = pending.target.scroll;
        scroll_ = pending.target.scroll;
        notice_.reset(); // a successful navigation retires the previous notice
        ++location_serial_;
    }
    if (entries_.empty()) {
        cursor_ = kNoRow;
        anchor_ = kNoRow;
    }
    for (const WatchBatch& batch : pending.replay) {
        (void)apply_watch_batch(batch);
    }
    recount();
    ++content_serial_;
    ++cursor_serial_;
}

void BrowserModel::fail_request(Pending& pending, ListingResult& result) {
    const Error error = result.entries.error();
    switch (pending.kind) {
    case NavKind::Start:
        current_.dir = pending.target.dir;
        entries_.clear();
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
    if (row >= entries_.size()) {
        return;
    }
    cursor_ = row;
    anchor_ = row;
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
    if (notice_shown()) {
        dismiss_notice();
        return;
    }
    clear_selection();
}

// ---------------------------------------------------------------------------------------------
// Cursor and selection
// ---------------------------------------------------------------------------------------------

std::size_t BrowserModel::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) {
            return i;
        }
    }
    return kNoRow;
}

void BrowserModel::recount() {
    selected_count_ = 0;
    selected_bytes_ = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (selected_[i] != 0) {
            ++selected_count_;
            if (!fs::is_directory_like(entries_[i])) {
                selected_bytes_ += entries_[i].size;
            }
        }
    }
}

void BrowserModel::select_only(std::size_t row) {
    std::fill(selected_.begin(), selected_.end(), std::uint8_t{0});
    if (row < selected_.size()) {
        selected_[row] = 1;
    }
    recount();
}

void BrowserModel::select_range(std::size_t a, std::size_t b, bool add) {
    if (!add) {
        std::fill(selected_.begin(), selected_.end(), std::uint8_t{0});
    }
    const std::size_t lo = std::min(a, b);
    const std::size_t hi = std::min(std::max(a, b), selected_.size() - 1);
    for (std::size_t i = lo; i <= hi && i < selected_.size(); ++i) {
        selected_[i] = 1;
    }
    recount();
}

void BrowserModel::set_cursor(std::size_t row, bool extend) {
    if (entries_.empty()) {
        return;
    }
    row = std::min(row, entries_.size() - 1);
    cursor_ = row;
    if (extend) {
        if (anchor_ >= entries_.size()) {
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
    if (entries_.empty()) {
        return;
    }
    const auto last = static_cast<std::ptrdiff_t>(entries_.size() - 1);
    std::ptrdiff_t next = 0;
    if (cursor_ >= entries_.size()) {
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
    if (!entries_.empty()) {
        set_cursor(entries_.size() - 1, extend);
    }
}

void BrowserModel::select_all() {
    std::fill(selected_.begin(), selected_.end(), std::uint8_t{1});
    recount();
}

void BrowserModel::clear_selection() {
    std::fill(selected_.begin(), selected_.end(), std::uint8_t{0});
    recount();
}

void BrowserModel::click_row(std::size_t row, Modifiers mods) {
    if (row >= entries_.size()) {
        return;
    }
    const bool ctrl = has(mods, Modifiers::Ctrl);
    const bool shift = has(mods, Modifiers::Shift);
    if (anchor_ >= entries_.size()) {
        anchor_ = row;
    }
    if (shift) {
        select_range(anchor_, row, ctrl); // Ctrl+Shift adds the range
    } else if (ctrl) {
        selected_[row] = selected_[row] != 0 ? 0 : 1;
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
    band_active_ = true;
    band_additive_ = additive;
    band_base_ = additive ? selected_ : std::vector<std::uint8_t>(selected_.size(), 0);
}

void BrowserModel::band_update(std::size_t first, std::size_t last) {
    if (!band_active_) {
        return;
    }
    selected_ = band_base_;
    if (first <= last) {
        for (std::size_t i = first; i <= last && i < selected_.size(); ++i) {
            selected_[i] = 1;
        }
    }
    recount();
}

void BrowserModel::band_end() {
    band_active_ = false;
    band_base_.clear();
}

bool BrowserModel::execute(const Command& command) {
    const bool extend = (command.arg & CursorArg::Extend) != 0U;
    switch (command.id) {
    case CommandId::ToggleHidden: toggle_hidden(); return true;
    case CommandId::NavigateBack: back(); return true;
    case CommandId::NavigateForward: forward(); return true;
    case CommandId::NavigateUp: up(); return true;
    case CommandId::Refresh: refresh(); return true;
    case CommandId::Activate: activate(); return true;
    case CommandId::SelectAll: select_all(); return true;
    case CommandId::ClearSelection: escape(); return true;
    case CommandId::CursorUp: cursor_by(-1, extend); return true;
    case CommandId::CursorDown: cursor_by(1, extend); return true;
    case CommandId::CursorPageUp: cursor_page(-1, extend); return true;
    case CommandId::CursorPageDown: cursor_page(1, extend); return true;
    case CommandId::CursorHome: cursor_home(extend); return true;
    case CommandId::CursorEnd: cursor_end(extend); return true;
    case CommandId::FocusAddressBar: return true; // Phase 4; registered no-op
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
    case CommandId::NewWindow: return false;
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
    if (band_active_) {
        band_base_.insert(band_base_.begin() + at, 0);
    }
    if (cursor_ != kNoRow && cursor_ >= lo) {
        ++cursor_;
    }
    if (anchor_ != kNoRow && anchor_ >= lo) {
        ++anchor_;
    }
    return lo;
}

void BrowserModel::erase_row(std::size_t row) {
    const auto at = static_cast<std::ptrdiff_t>(row);
    entries_.erase(entries_.begin() + at);
    selected_.erase(selected_.begin() + at);
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
    // Removes `name` if listed; reports its selection/cursor/anchor state.
    struct Taken {
        bool found = false;
        bool selected = false;
        bool cursor = false;
        bool anchor = false;
    };
    const auto take = [this](std::string_view name) {
        Taken t;
        const std::size_t row = find(name);
        if (row == kNoRow) {
            return t;
        }
        t.found = true;
        t.selected = selected_[row] != 0;
        t.cursor = cursor_ == row;
        t.anchor = anchor_ == row;
        erase_row(row);
        return t;
    };
    const auto put = [this](fs::DirEntry entry, const Taken& t) {
        const std::size_t row = insert_sorted(std::move(entry), t.selected);
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
        const std::size_t old_row = find(change.old_name);
        std::optional<fs::DirEntry> entry = change.entry;
        if (!entry && old_row != kNoRow) {
            entry = entries_[old_row];
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
            if (cursor_ >= entries_.size()) {
                cursor_ = entries_.empty() ? kNoRow : entries_.size() - 1;
            }
            recount();
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

} // namespace navigator::ui
