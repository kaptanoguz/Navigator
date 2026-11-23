// src/ui/ops/file_ops.cpp — see file_ops.hpp.
#include "ops/file_ops.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

#include <navigator/fs.hpp>
#include <navigator/i18n.hpp>

#include "browser/format.hpp"
#include "i18n/catalog.hpp"
#include "ops/op_text.hpp"

namespace navigator::ui {
namespace {

constexpr std::size_t kHistoryLimit = 20;
constexpr std::size_t kDetailLimit = 8;
constexpr std::size_t kSheetNames = 4;
constexpr std::uint64_t kProbeEntryLimit = 400'000;

std::string name_of(const Path& p) { return p.filename().string(); }

std::string display_of(const Path& p) { return fs::to_display_name(name_of(p)); }

bool undoable_kind(ops::OpKind kind) noexcept {
    return kind != ops::OpKind::DeletePermanently && kind != ops::OpKind::Undo &&
           kind != ops::OpKind::Redo;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Tree probe: totals for the permanent-delete sheet, off the UI thread, bounded.
// ---------------------------------------------------------------------------------------------

class TreeProbe {
public:
    explicit TreeProbe(std::vector<Path> roots) {
        thread_ = std::jthread(
            [this, roots = std::move(roots)](const std::stop_token& stop) { run(roots, stop); });
    }
    ~TreeProbe() = default; // jthread requests stop and joins
    TreeProbe(const TreeProbe&) = delete;
    TreeProbe& operator=(const TreeProbe&) = delete;
    TreeProbe(TreeProbe&&) = delete;
    TreeProbe& operator=(TreeProbe&&) = delete;

    [[nodiscard]] TreeStats stats() const {
        TreeStats s;
        s.files = files_.load(std::memory_order_relaxed);
        s.folders = folders_.load(std::memory_order_relaxed);
        s.bytes = bytes_.load(std::memory_order_relaxed);
        s.done = done_.load(std::memory_order_acquire);
        s.partial = partial_.load(std::memory_order_relaxed);
        return s;
    }

private:
    void run(const std::vector<Path>& roots, const std::stop_token& stop) {
        std::uint64_t seen = 0;
        for (const Path& root : roots) {
            const Result<fs::DirEntry> top = fs::stat(root, false);
            if (!top) {
                continue;
            }
            if (top->kind != fs::EntryKind::Directory) {
                files_.fetch_add(1, std::memory_order_relaxed);
                bytes_.fetch_add(top->size, std::memory_order_relaxed);
                continue;
            }
            folders_.fetch_add(1, std::memory_order_relaxed);
            std::vector<Path> stack{root};
            while (!stack.empty()) {
                if (stop.stop_requested()) {
                    return;
                }
                const Path dir = std::move(stack.back());
                stack.pop_back();
                const fs::ListOptions lo{.show_hidden = true, .follow_symlinks = false};
                Result<std::vector<fs::DirEntry>> list = fs::list_directory(dir, lo, stop);
                if (!list) {
                    continue;
                }
                for (const fs::DirEntry& e : *list) {
                    if (++seen > kProbeEntryLimit) {
                        partial_.store(true, std::memory_order_relaxed);
                        done_.store(true, std::memory_order_release);
                        return;
                    }
                    if (e.kind == fs::EntryKind::Directory) {
                        // One filesystem per tree (S5): a mount point inside is left alone.
                        if (e.id.device == top->id.device) {
                            folders_.fetch_add(1, std::memory_order_relaxed);
                            stack.push_back(dir / e.name);
                        }
                    } else {
                        files_.fetch_add(1, std::memory_order_relaxed);
                        bytes_.fetch_add(e.size, std::memory_order_relaxed);
                    }
                }
            }
        }
        done_.store(true, std::memory_order_release);
    }

    std::atomic<std::uint64_t> files_{0};
    std::atomic<std::uint64_t> folders_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<bool> done_{false};
    std::atomic<bool> partial_{false};
    std::jthread thread_;
};

std::vector<ConflictButton> conflict_buttons(const ops::Conflict& c) {
    std::vector<ConflictButton> out;
    if (c.replace_allowed && !c.same_file) {
        out.push_back(ConflictButton::Replace);
    }
    out.push_back(ConflictButton::Skip);
    out.push_back(ConflictButton::KeepBoth);
    if (c.merge_allowed && !c.same_file) {
        out.push_back(ConflictButton::Merge);
    }
    out.push_back(ConflictButton::Cancel);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------

FileOps::FileOps(FileOpsOptions options) : options_(std::move(options)) {
    ops::QueueOptions qo;
    qo.engine.trash = options_.trash;
    qo.engine.protected_paths = options_.protected_paths;
    qo.engine.fault_injector = options_.fault_injector;
    qo.engine.progress_interval = options_.progress_interval;
    qo.engine.copy_chunk_bytes = options_.copy_chunk_bytes;
    qo.max_concurrent = options_.max_concurrent;
    qo.on_event = options_.wake;
    Result<std::unique_ptr<ops::IOperationQueue>> q = ops::create_operation_queue(std::move(qo));
    if (q) {
        queue_ = std::move(*q);
    }
}

FileOps::~FileOps() { shutdown(); }

void FileOps::shutdown() noexcept {
    if (queue_) {
        queue_->shutdown();
        std::vector<ops::OpEvent> events;
        queue_->poll(events);
        for (const ops::OpEvent& e : events) {
            if (e.type == ops::OpEvent::Type::Finished && e.result) {
                if (e.result->record) {
                    undo_.record_done(*e.result->record, e.result->origin);
                }
            }
        }
    }
    close_sheet();
}

void FileOps::set_protected_paths(ops::ProtectedPaths paths) {
    options_.protected_paths = paths;
    if (queue_) {
        queue_->set_protected_paths(std::move(paths));
    }
}

// ---------------------------------------------------------------------------------------------
// Frame start
// ---------------------------------------------------------------------------------------------

bool FileOps::frame_start(double now_ms) {
    now_ms_ = now_ms;
    bool changed = false;
    if (queue_) {
        std::vector<ops::OpEvent> events;
        if (queue_->poll(events) > 0) {
            changed = true;
            for (const ops::OpEvent& e : events) {
                handle_event(e);
            }
        }
        std::vector<ops::OpProgress> snap = queue_->snapshot();
        if (snap.size() != running_.size()) {
            changed = true;
        } else {
            for (std::size_t i = 0; i < snap.size(); ++i) {
                if (snap[i].bytes_done != running_[i].bytes_done ||
                    snap[i].phase != running_[i].phase ||
                    snap[i].items_done != running_[i].items_done || snap[i].id != running_[i].id) {
                    changed = true;
                }
            }
        }
        running_ = std::move(snap);
        const std::size_t before = (conflict_ ? 1U : 0U) + (confirm_ ? 1U : 0U);
        sync_conflict();
        if (((conflict_ ? 1U : 0U) + (confirm_ ? 1U : 0U)) != before) {
            changed = true;
        }
    }
    if (notice_ && now_ms >= notice_->until_ms) {
        notice_.reset();
        changed = true;
    }
    const bool had_cut = !cut_.empty();
    clear_cut_if_stale();
    changed = changed || had_cut != !cut_.empty();
    if (quit_state_ == QuitState::WaitingToQuit && running_.empty()) {
        quit_state_ = QuitState::Quit;
        changed = true;
    }
    return changed;
}

bool FileOps::wait_idle(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)frame_start(now_ms_ + 10.0);
        if (!busy() && (!queue_ || queue_->snapshot().empty())) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void FileOps::clear_cut_if_stale() {
    if (!cut_.empty() && (options_.clipboard == nullptr || !options_.clipboard->owns_offer())) {
        cut_.clear();
    }
}

bool FileOps::is_cut(const Path& path) const {
    return !cut_.empty() && cut_.contains(path.string());
}

// ---------------------------------------------------------------------------------------------
// Submitting
// ---------------------------------------------------------------------------------------------

FileOps::Meta FileOps::meta_for(const ops::OpRequest& r) const {
    Meta m;
    m.kind = r.kind;
    m.destination = r.destination;
    m.sources = r.sources;
    if (!r.trashed.empty()) {
        m.count = r.trashed.size();
        m.name = fs::to_display_name(name_of(r.trashed.front().original_path));
    } else {
        m.count = r.sources.size();
        if (!r.sources.empty()) {
            m.name = display_of(r.sources.front());
        }
    }
    if (r.kind == ops::OpKind::Rename || r.kind == ops::OpKind::MakeDirectory) {
        m.name = fs::to_display_name(r.new_name);
        m.count = 1;
    }
    if (r.kind == ops::OpKind::Trash || r.kind == ops::OpKind::DeletePermanently) {
        m.folder = {};
    } else {
        m.folder = folder_display(r.destination);
    }
    if (r.record) {
        m.label = undo_label_text(ops::make_label(*r.record));
    }
    return m;
}

void FileOps::submit(ops::OpRequest request, Meta meta) {
    if (!queue_) {
        post_notice(error_sentence(ErrorCode::Unsupported, meta.name), true, NoticeAction::None,
                    ops::OpId{});
        return;
    }
    Meta base = meta_for(request);
    base.cut_paste = meta.cut_paste;
    base.create_folder = meta.create_folder;
    base.new_name = meta.new_name;
    base.tab = meta.tab;
    Result<ops::OpId> id = queue_->submit(std::move(request));
    if (!id) {
        const ErrorCode code = id.error().code;
        std::string text = code == ErrorCode::PermissionDenied
                               ? substitute(std::string(tr("ops.err.protected")), "name", base.name)
                               : error_sentence(code, base.name);
        post_notice(std::move(text), true, NoticeAction::None, ops::OpId{});
        return;
    }
    meta_[id->value] = std::move(base);
    last_started_ = id->value;
    ++submitted_;
}

std::vector<Path> FileOps::paths_of_selection(const FileTarget& t) const {
    if (!t.selected.empty()) {
        return t.selected;
    }
    if (!t.cursor.empty()) {
        return {t.cursor};
    }
    return {};
}

void FileOps::copy(const FileTarget& target, bool cut) {
    std::vector<Path> paths = paths_of_selection(target);
    if (paths.empty()) {
        notify(std::string(tr("notice.nothing_selected")));
        return;
    }
    if (options_.clipboard == nullptr) {
        return;
    }
    transfer::FileClipboard clip;
    clip.action = cut ? transfer::ClipboardAction::Cut : transfer::ClipboardAction::Copy;
    clip.paths = paths;
    Result<std::vector<std::pair<std::string, std::string>>> offer =
        transfer::make_clipboard_offer(clip);
    if (!offer) {
        notify(error_sentence(offer.error().code, display_of(paths.front())), true);
        return;
    }
    options_.clipboard->offer(std::move(*offer));
    cut_.clear();
    if (cut) {
        for (const Path& p : paths) {
            cut_.insert(p.string());
        }
    }
    post_notice(substitute(tr_count(cut ? "ops.clip.cut" : "ops.clip.copied", paths.size()), "name",
                           display_of(paths.front())),
                false, NoticeAction::None, ops::OpId{});
    notice_->until_ms = now_ms_ + 2500.0;
}

void FileOps::paste(const FileTarget& target, bool into_cursor_folder) {
    if (options_.clipboard == nullptr) {
        return;
    }
    const std::optional<transfer::FileClipboard> clip = transfer::read_clipboard(
        [this](std::string_view mime) { return options_.clipboard->read(mime); });
    if (!clip || clip->paths.empty()) {
        notify(std::string(tr("notice.clipboard_no_files")));
        return;
    }
    const Path dest = (into_cursor_folder && target.cursor_is_dir && !target.cursor.empty())
                          ? target.cursor
                          : target.dir;
    if (dest.empty()) {
        return;
    }
    const bool cut = clip->action == transfer::ClipboardAction::Cut;
    ops::OpRequest r;
    r.kind = cut ? ops::OpKind::Move : ops::OpKind::Copy;
    r.sources = clip->paths;
    r.destination = dest;
    r.conflict = ops::ConflictPolicy::Ask;
    Meta m;
    m.cut_paste = cut;
    m.tab = target.tab;
    submit(std::move(r), std::move(m));
}

void FileOps::trash(const FileTarget& target) { trash_paths(paths_of_selection(target)); }

void FileOps::trash_paths(std::vector<Path> paths) {
    if (paths.empty()) {
        notify(std::string(tr("notice.nothing_selected")));
        return;
    }
    ops::OpRequest r;
    r.kind = ops::OpKind::Trash;
    r.sources = std::move(paths);
    submit(std::move(r), Meta{});
}

void FileOps::transfer(std::vector<Path> sources, const Path& folder, transfer::DropAction action) {
    if (sources.empty() || folder.empty()) {
        return;
    }
    ops::OpRequest r;
    switch (action) {
    case transfer::DropAction::Copy: r.kind = ops::OpKind::Copy; break;
    case transfer::DropAction::Move: r.kind = ops::OpKind::Move; break;
    case transfer::DropAction::Link: r.kind = ops::OpKind::CreateSymlink; break;
    case transfer::DropAction::None:
    case transfer::DropAction::Ask: return;
    }
    r.sources = std::move(sources);
    r.destination = folder;
    r.conflict = ops::ConflictPolicy::Ask;
    submit(std::move(r), Meta{});
}

void FileOps::copy_to_folder(const FileTarget& source, const Path& folder, bool move) {
    std::vector<Path> paths = paths_of_selection(source);
    if (paths.empty()) {
        notify(std::string(tr("notice.nothing_selected")));
        return;
    }
    transfer(std::move(paths), folder,
             move ? transfer::DropAction::Move : transfer::DropAction::Copy);
}

void FileOps::new_folder(const FileTarget& target) {
    if (target.dir.empty()) {
        return;
    }
    const std::string base =
        options_.new_folder_name ? options_.new_folder_name() : std::string("New folder");
    std::string name = base;
    for (unsigned n = 2; n < 2000; ++n) {
        const Result<fs::DirEntry> existing = fs::stat(target.dir / name, false);
        if (!existing && existing.error().code == ErrorCode::NotFound) {
            break;
        }
        name = ops::keep_both_name(base, n, true);
    }
    ops::OpRequest r;
    r.kind = ops::OpKind::MakeDirectory;
    r.destination = target.dir;
    r.new_name = name;
    Meta m;
    m.create_folder = true;
    m.tab = target.tab;
    m.new_name = name;
    submit(std::move(r), std::move(m));
}

void FileOps::undo() {
    if (!queue_) {
        return;
    }
    if (!undo_.can_undo()) {
        notify(std::string(tr("notice.undo_empty")));
        return;
    }
    std::shared_ptr<const ops::UndoRecord> record = undo_.take_undo();
    for (const Path& volume : record->volumes) {
        if (options_.is_mounted && !options_.is_mounted(volume)) {
            const std::string name = display_of(volume);
            undo_.restore(record, ops::OpOrigin::Undo);
            notify(substitute(std::string(tr("notice.undo_volume")), "name", name), true);
            return;
        }
    }
    ops::OpRequest r;
    r.kind = ops::OpKind::Undo;
    r.origin = ops::OpOrigin::Undo;
    r.record = record;
    r.conflict = ops::ConflictPolicy::Ask;
    const std::uint64_t before = last_started_;
    submit(std::move(r), Meta{});
    if (last_started_ == before) {
        undo_.restore(record, ops::OpOrigin::Undo); // refused: the entry stays
    }
}

void FileOps::redo() {
    if (!queue_) {
        return;
    }
    if (!undo_.can_redo()) {
        notify(std::string(tr("notice.redo_empty")));
        return;
    }
    std::shared_ptr<const ops::UndoRecord> record = undo_.take_redo();
    for (const Path& volume : record->volumes) {
        if (options_.is_mounted && !options_.is_mounted(volume)) {
            const std::string name = display_of(volume);
            undo_.restore(record, ops::OpOrigin::Redo);
            notify(substitute(std::string(tr("notice.undo_volume")), "name", name), true);
            return;
        }
    }
    ops::OpRequest r;
    r.kind = ops::OpKind::Redo;
    r.origin = ops::OpOrigin::Redo;
    r.record = record;
    r.conflict = ops::ConflictPolicy::Ask;
    const std::uint64_t before = last_started_;
    submit(std::move(r), Meta{});
    if (last_started_ == before) {
        undo_.restore(record, ops::OpOrigin::Redo);
    }
}

void FileOps::cancel(std::uint64_t op) {
    if (!queue_) {
        return;
    }
    if (op == 0) {
        op = running_.empty() ? last_started_ : running_.back().id.value;
    }
    queue_->cancel(ops::OpId{op});
}

void FileOps::toggle_pause(std::uint64_t op) {
    if (!queue_) {
        return;
    }
    if (op == 0 && !running_.empty()) {
        op = running_.back().id.value;
    }
    for (const ops::OpProgress& p : running_) {
        if (p.id.value == op) {
            if (p.phase == ops::OpPhase::Paused) {
                queue_->resume(p.id);
            } else {
                queue_->pause(p.id);
            }
            return;
        }
    }
}

void FileOps::cancel_all() {
    if (queue_) {
        queue_->cancel_all();
    }
}

void FileOps::restore(std::vector<trash::TrashedItem> items) {
    if (items.empty()) {
        notify(std::string(tr("notice.nothing_selected")));
        return;
    }
    ops::OpRequest r;
    r.kind = ops::OpKind::Restore;
    r.trashed = std::move(items);
    r.conflict = ops::ConflictPolicy::Ask;
    submit(std::move(r), Meta{});
}

// ---------------------------------------------------------------------------------------------
// Sheets
// ---------------------------------------------------------------------------------------------

void FileOps::open_confirm(ConfirmSheet sheet) {
    sheet.focus = 0; // Cancel: a destructive action is never the default
    if (sheet.kind == SheetKind::DeletePermanently || sheet.kind == SheetKind::DeleteUntrashable) {
        sheet.probe = std::make_shared<TreeProbe>(sheet.paths);
    }
    confirm_ = std::move(sheet);
    // Conflicts wait behind it.
    conflict_.reset();
}

void FileOps::close_sheet() noexcept {
    confirm_.reset();
    if (quit_state_ == QuitState::Asking) {
        quit_state_ = QuitState::Running;
    }
}

void FileOps::request_delete_permanently(const FileTarget& target) {
    std::vector<Path> paths = paths_of_selection(target);
    if (paths.empty()) {
        notify(std::string(tr("notice.nothing_selected")));
        return;
    }
    ConfirmSheet s;
    s.kind = SheetKind::DeletePermanently;
    s.count = paths.size();
    for (std::size_t i = 0; i < paths.size() && i < kSheetNames; ++i) {
        s.names.push_back(display_of(paths[i]));
    }
    s.paths = std::move(paths);
    open_confirm(std::move(s));
}

void FileOps::request_empty_trash(std::vector<trash::TrashedItem> items) {
    if (items.empty()) {
        notify(std::string(tr("trash.already_empty")));
        return;
    }
    ConfirmSheet s;
    s.kind = SheetKind::EmptyTrash;
    s.count = items.size();
    for (std::size_t i = 0; i < items.size() && i < kSheetNames; ++i) {
        s.names.push_back(fs::to_display_name(name_of(items[i].original_path)));
    }
    s.trashed = std::move(items);
    open_confirm(std::move(s));
}

void FileOps::request_delete_trashed(std::vector<trash::TrashedItem> items) {
    if (items.empty()) {
        notify(std::string(tr("notice.nothing_selected")));
        return;
    }
    ConfirmSheet s;
    s.kind = SheetKind::DeleteTrashed;
    s.count = items.size();
    for (std::size_t i = 0; i < items.size() && i < kSheetNames; ++i) {
        s.names.push_back(fs::to_display_name(name_of(items[i].original_path)));
    }
    s.trashed = std::move(items);
    open_confirm(std::move(s));
}

SheetKind FileOps::sheet_kind() const noexcept {
    if (confirm_) {
        return confirm_->kind;
    }
    if (quit_state_ == QuitState::Asking) {
        return SheetKind::QuitRunning;
    }
    if (conflict_) {
        return SheetKind::Conflict;
    }
    return SheetKind::None;
}

std::optional<TreeStats> FileOps::confirm_stats() const {
    if (!confirm_ || !confirm_->probe) {
        return std::nullopt;
    }
    return confirm_->probe->stats();
}

void FileOps::sync_conflict() {
    if (!queue_) {
        return;
    }
    std::vector<ops::Conflict> pending = queue_->pending_conflicts();
    for (auto it = answered_.begin(); it != answered_.end();) {
        const bool still =
            std::ranges::any_of(pending, [&](const ops::Conflict& c) { return c.id.value == *it; });
        it = still ? std::next(it) : answered_.erase(it);
    }
    std::erase_if(pending, [&](const ops::Conflict& c) { return answered_.contains(c.id.value); });
    if (conflict_) {
        const bool alive = std::ranges::any_of(
            pending, [&](const ops::Conflict& c) { return c.id == conflict_->conflict.id; });
        if (!alive) {
            conflict_.reset();
        }
    }
    if (!conflict_ && !confirm_ && !pending.empty()) {
        ConflictSheet s;
        s.conflict = pending.front();
        const std::vector<ConflictButton> buttons = conflict_buttons(s.conflict);
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            if (buttons[i] == ConflictButton::KeepBoth) {
                s.focus = i; // Enter = Keep both, the safe default
            }
        }
        s.suggested = s.conflict.keep_both_name;
        s.name.set(s.suggested);
        conflict_ = std::move(s);
    }
}

void FileOps::sheet_focus(int delta) {
    if (confirm_) {
        confirm_->focus = (confirm_->focus + (delta > 0 ? 1U : ConfirmSheet::kButtons - 1U)) %
                          ConfirmSheet::kButtons;
    } else if (quit_state_ == QuitState::Asking) {
        quit_focus_ = (quit_focus_ + (delta > 0 ? 1U : 2U)) % 3U;
    } else if (conflict_) {
        const std::size_t n = conflict_buttons(conflict_->conflict).size();
        conflict_->focus = (conflict_->focus + (delta > 0 ? 1U : n - 1U)) % n;
    }
}

void FileOps::sheet_focus_to(std::size_t index) {
    if (confirm_) {
        confirm_->focus = std::min(index, ConfirmSheet::kButtons - 1U);
    } else if (quit_state_ == QuitState::Asking) {
        quit_focus_ = std::min<std::size_t>(index, 2);
    } else if (conflict_) {
        conflict_->focus = std::min(index, conflict_buttons(conflict_->conflict).size() - 1U);
    }
}

void FileOps::sheet_activate() {
    if (confirm_) {
        sheet_activate(confirm_->focus);
    } else if (quit_state_ == QuitState::Asking) {
        sheet_activate(quit_focus_);
    } else if (conflict_) {
        if (conflict_->editing_name) {
            conflict_decide(ConflictButton::KeepBoth);
        } else {
            sheet_activate(conflict_->focus);
        }
    }
}

void FileOps::sheet_activate(std::size_t index) {
    if (confirm_) {
        if (index == 0) {
            close_sheet();
        } else {
            confirm_accept();
        }
        return;
    }
    if (quit_state_ == QuitState::Asking) {
        if (index == 0) {
            quit_state_ = QuitState::Running; // keep working
        } else if (index == 1) {
            quit_state_ = QuitState::WaitingToQuit;
        } else {
            cancel_all();
            quit_state_ = QuitState::Quit;
        }
        return;
    }
    if (conflict_) {
        const std::vector<ConflictButton> buttons = conflict_buttons(conflict_->conflict);
        if (index < buttons.size()) {
            conflict_decide(buttons[index]);
        }
    }
}

void FileOps::sheet_escape() {
    if (confirm_) {
        close_sheet();
    } else if (quit_state_ == QuitState::Asking) {
        quit_state_ = QuitState::Running;
    } else if (conflict_) {
        if (conflict_->editing_name) {
            conflict_->editing_name = false;
            conflict_->name.set(conflict_->suggested);
            conflict_->name_error.clear();
        } else {
            conflict_decide(ConflictButton::Cancel); // Escape cancels the operation
        }
    }
}

void FileOps::sheet_toggle_apply_all() {
    if (conflict_ && !conflict_->editing_name) {
        conflict_->apply_all = !conflict_->apply_all;
    }
}

void FileOps::conflict_begin_name_edit() {
    if (conflict_) {
        conflict_->editing_name = true;
        conflict_->name.select_all();
    }
}

void FileOps::conflict_name_edited() {
    if (!conflict_) {
        return;
    }
    const ops::NameError e = ops::validate_new_name(conflict_->name.text());
    std::string msg;
    switch (e) {
    case ops::NameError::None: break;
    case ops::NameError::Empty: msg = std::string(tr("rename.err.empty")); break;
    case ops::NameError::Dot:
    case ops::NameError::DotDot: msg = std::string(tr("rename.err.dot")); break;
    case ops::NameError::ContainsSlash:
    case ops::NameError::ContainsNul: msg = std::string(tr("rename.err.slash")); break;
    case ops::NameError::TooLong: msg = std::string(tr("rename.err.long")); break;
    }
    conflict_->name_error = std::move(msg);
}

void FileOps::conflict_decide(ConflictButton button) {
    if (!conflict_ || !queue_) {
        return;
    }
    ops::ConflictDecision d;
    d.apply_to_all = conflict_->apply_all;
    switch (button) {
    case ConflictButton::Replace: d.action = ops::ConflictAction::Replace; break;
    case ConflictButton::Skip: d.action = ops::ConflictAction::Skip; break;
    case ConflictButton::Merge: d.action = ops::ConflictAction::Merge; break;
    case ConflictButton::Cancel: d.action = ops::ConflictAction::Cancel; break;
    case ConflictButton::KeepBoth:
        d.action = ops::ConflictAction::KeepBoth;
        if (conflict_->name.text() != conflict_->suggested) {
            conflict_name_edited();
            if (!conflict_->name_error.empty()) {
                return; // keep the sheet open with the message
            }
            d.new_name = conflict_->name.text();
        }
        break;
    }
    const ops::ConflictId id = conflict_->conflict.id;
    const Result<void> r = queue_->resolve(id, std::move(d));
    if (!r && r.error().code == ErrorCode::InvalidArgument) {
        conflict_->name_error = std::string(tr("rename.err.empty"));
        return;
    }
    answered_.insert(id.value);
    conflict_.reset(); // NotFound: the operation was cancelled meanwhile
    sync_conflict();
}

void FileOps::confirm_accept() {
    if (!confirm_) {
        return;
    }
    ConfirmSheet s = std::move(*confirm_);
    confirm_.reset();
    ops::OpRequest r;
    r.kind = ops::OpKind::DeletePermanently;
    r.confirmed = true; // S3: set here, after the sheet, and nowhere else
    switch (s.kind) {
    case SheetKind::DeletePermanently:
    case SheetKind::DeleteUntrashable: r.sources = std::move(s.paths); break;
    case SheetKind::EmptyTrash:
    case SheetKind::DeleteTrashed: r.trashed = std::move(s.trashed); break;
    case SheetKind::None:
    case SheetKind::Conflict:
    case SheetKind::QuitRunning: return;
    }
    submit(std::move(r), Meta{});
    sync_conflict();
}

bool FileOps::request_quit() {
    if (!queue_ || queue_->snapshot().empty()) {
        quit_state_ = QuitState::Quit;
        return true;
    }
    if (quit_state_ == QuitState::Quit) {
        return true;
    }
    if (quit_state_ != QuitState::WaitingToQuit) {
        quit_state_ = QuitState::Asking;
        quit_focus_ = 0;
        confirm_.reset();
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Inline editor
// ---------------------------------------------------------------------------------------------

void FileOps::begin_rename(layout::TabId tab, const Path& dir, std::string name, bool is_dir,
                           bool after_create) {
    InlineEdit e;
    e.tab = tab;
    e.dir = dir;
    e.original = name;
    e.is_dir = is_dir;
    e.after_create = after_create;
    const std::string shown = fs::to_display_name(name);
    e.edit.set(shown);
    if (shown == name && !after_create) {
        const std::size_t stem = ops::stem_length(name, is_dir);
        e.edit.select_range(0, stem);
    } else {
        e.edit.select_all();
    }
    inline_ = std::move(e);
}

void FileOps::inline_edited(const std::function<bool(std::string_view)>& taken) {
    if (!inline_) {
        return;
    }
    const std::string& text = inline_->edit.text();
    inline_->message.clear();
    inline_->message_error = false;
    switch (ops::validate_new_name(text)) {
    case ops::NameError::None: break;
    case ops::NameError::Empty:
        inline_->message = std::string(tr("rename.err.empty"));
        inline_->message_error = true;
        return;
    case ops::NameError::Dot:
    case ops::NameError::DotDot:
        inline_->message = std::string(tr("rename.err.dot"));
        inline_->message_error = true;
        return;
    case ops::NameError::ContainsSlash:
    case ops::NameError::ContainsNul:
        inline_->message = std::string(tr("rename.err.slash"));
        inline_->message_error = true;
        return;
    case ops::NameError::TooLong:
        inline_->message = std::string(tr("rename.err.long"));
        inline_->message_error = true;
        return;
    }
    const bool unchanged = text == fs::to_display_name(inline_->original);
    if (!unchanged && taken && taken(text)) {
        inline_->message = substitute(std::string(tr("rename.err.exists")), "name", text);
        inline_->message_error = true;
        return;
    }
    if (!unchanged) {
        if (text.front() == ' ' || text.back() == ' ') {
            inline_->message = std::string(tr("rename.warn.space"));
        } else if (text.front() == '.' && !inline_->original.starts_with('.')) {
            inline_->message = std::string(tr("rename.warn.hidden"));
        }
    }
}

bool FileOps::inline_commit(const std::function<bool(std::string_view)>& taken) {
    if (!inline_) {
        return true;
    }
    inline_edited(taken);
    if (inline_->message_error) {
        return false;
    }
    const std::string text = inline_->edit.text();
    if (text == fs::to_display_name(inline_->original)) {
        inline_.reset();
        return true;
    }
    ops::OpRequest r;
    r.kind = ops::OpKind::Rename;
    r.sources = {inline_->dir / inline_->original};
    r.new_name = text;
    r.conflict = ops::ConflictPolicy::Ask;
    Meta m;
    m.tab = inline_->tab;
    inline_.reset();
    submit(std::move(r), std::move(m));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

void FileOps::handle_event(const ops::OpEvent& event) {
    using Type = ops::OpEvent::Type;
    switch (event.type) {
    case Type::Started:
    case Type::Progress:
    case Type::ConflictRaised: break;
    case Type::ItemFinished:
        if (event.item && event.item->status == ops::ItemStatus::Done) {
            const auto it = meta_.find(event.op.value);
            const ops::OpKind kind = it != meta_.end() ? it->second.kind : ops::OpKind::Move;
            const ops::ItemOutcome& item = *event.item;
            if (kind == ops::OpKind::Move || kind == ops::OpKind::Rename ||
                kind == ops::OpKind::Undo || kind == ops::OpKind::Redo) {
                if (!item.destination.empty()) {
                    reconcile_.moved.emplace_back(item.source, item.destination);
                }
            } else if (kind == ops::OpKind::Trash) {
                reconcile_.moved.emplace_back(item.source, Path{});
            }
        }
        break;
    case Type::Finished:
        if (event.result) {
            handle_finished(*event.result);
        }
        break;
    }
}

void FileOps::post_notice(std::string text, bool error, NoticeAction action, ops::OpId op,
                          Path folder) {
    OpNotice n;
    n.text = std::move(text);
    n.error = error;
    n.action = action;
    n.op = op;
    n.folder = std::move(folder);
    n.serial = ++notice_serial_;
    n.until_ms = now_ms_ + (error ? options_.notice_ms * 1.5 : options_.notice_ms);
    notice_ = std::move(n);
}

void FileOps::notify(std::string text, bool error, NoticeAction action) {
    post_notice(std::move(text), error, action, ops::OpId{});
}

void FileOps::handle_finished(const ops::OpResult& r) {
    ++completed_;
    Meta meta;
    if (const auto it = meta_.find(r.id.value); it != meta_.end()) {
        meta = std::move(it->second);
        meta_.erase(it);
    }
    const std::size_t total = r.items.size();

    FinishedOp f;
    f.id = r.id;
    f.kind = r.kind;
    f.origin = r.origin;
    f.cancelled = r.cancelled;
    f.finished_ms = now_ms_;
    f.undoable = r.record.has_value() && !r.record->steps.empty();
    f.folder = meta.destination;
    if (f.folder.empty() && !r.items.empty()) {
        f.folder = r.items.front().source.parent_path();
    }

    // Per-item sentences.
    for (const ops::ItemOutcome& item : r.items) {
        if (f.details.size() >= kDetailLimit) {
            break;
        }
        const std::string name = fs::to_display_name(item.source.filename().string());
        if (item.status == ops::ItemStatus::Failed) {
            const ErrorCode code = item.error ? item.error->code : ErrorCode::Unknown;
            if (r.kind == ops::OpKind::Trash && code == ErrorCode::Unsupported) {
                f.details.push_back(substitute(std::string(tr("ops.err.no_trash")), "name", name));
            } else {
                f.details.push_back(error_sentence(code, name));
            }
        } else if (item.status == ops::ItemStatus::Changed) {
            f.details.push_back(changed_sentence(name));
        }
    }
    const bool problems = r.failed > 0 || r.changed > 0;
    f.ok = !problems && !r.cancelled;

    const std::string label = meta.label;
    if (r.cancelled) {
        f.title = op_partial_text(r.kind, r.done, total, true);
    } else if (r.done == 0 && r.skipped > 0 && !problems) {
        f.title = std::string(tr("ops.nothing_to_do"));
    } else if (r.done >= total && !problems) {
        f.title = op_done_text(r.kind, r.done, meta.name, meta.folder, label);
    } else if (r.done == 0 && r.failed > 0 && total == 1 && !f.details.empty()) {
        f.title = f.details.front();
    } else {
        f.title = op_partial_text(r.kind, r.done, total, false);
    }

    // Undo stack: every record (S12), with the operation's origin.
    if (r.record && !r.record->steps.empty()) {
        undo_.record_done(*r.record, r.origin);
    }

    // The notice: trash and undo/redo carry an action; quiet operations stay quiet.
    const bool quiet =
        (r.kind == ops::OpKind::MakeDirectory || r.kind == ops::OpKind::Rename) && f.ok;
    if (!quiet) {
        NoticeAction action = NoticeAction::None;
        if (f.undoable && undoable_kind(r.kind) && !problems && !r.cancelled) {
            action = NoticeAction::Undo;
        } else if (r.kind == ops::OpKind::Undo && f.undoable && !problems) {
            action = NoticeAction::Redo;
        } else if (!f.details.empty() || r.cancelled) {
            action = NoticeAction::ShowPanel;
        }
        std::string text = f.title;
        if (problems && !f.details.empty() && f.details.front() != f.title) {
            text += " ";
            text += f.details.front();
        }
        post_notice(std::move(text), problems, action, r.id, f.folder);
    }

    // Items that cannot be trashed: offer the permanent-delete sheet for exactly those (S2).
    if (r.kind == ops::OpKind::Trash && !r.untrashable.empty()) {
        ConfirmSheet s;
        s.kind = SheetKind::DeleteUntrashable;
        s.count = r.untrashable.size();
        for (std::size_t i = 0; i < r.untrashable.size() && i < kSheetNames; ++i) {
            s.names.push_back(display_of(r.untrashable[i]));
        }
        s.paths = r.untrashable;
        open_confirm(std::move(s));
    }

    // Reconcile: folders to re-list, names to select, a created folder to rename.
    for (const Path& p : r.touched_folders) {
        reconcile_.refresh.push_back(p);
    }
    reconcile_.trash_changed = true;
    if (r.kind == ops::OpKind::Copy || r.kind == ops::OpKind::Move ||
        r.kind == ops::OpKind::Restore || r.kind == ops::OpKind::CreateSymlink ||
        r.kind == ops::OpKind::Rename || r.kind == ops::OpKind::MakeDirectory) {
        std::map<Path, std::vector<std::string>> by_dir;
        for (const ops::ItemOutcome& item : r.items) {
            if (item.status == ops::ItemStatus::Done && !item.destination.empty()) {
                by_dir[item.destination.parent_path()].push_back(
                    item.destination.filename().string());
            }
        }
        for (auto& [dir, names] : by_dir) {
            reconcile_.select.push_back(Reconcile::Select{dir, std::move(names)});
        }
    }
    if (meta.create_folder && r.done == 1) {
        reconcile_.rename_after_create =
            Reconcile::Rename{meta.tab, meta.destination, meta.new_name};
    }

    // A cut that was pasted: the clipboard no longer offers it (while we still own it).
    if (meta.cut_paste && r.done > 0 && options_.clipboard != nullptr) {
        options_.clipboard->clear_own();
        cut_.clear();
    }

    history_.push_back(std::move(f));
    while (history_.size() > kHistoryLimit) {
        history_.pop_front();
    }
}

Reconcile FileOps::take_reconcile() {
    Reconcile out = std::move(reconcile_);
    reconcile_ = Reconcile{};
    return out;
}

std::string FileOps::title_of(const ops::OpProgress& p) const {
    const auto it = meta_.find(p.id.value);
    if (it == meta_.end()) {
        return op_title(p.kind, p.top_level_total, {}, folder_display(p.destination));
    }
    const Meta& m = it->second;
    return op_title(m.kind, m.count, m.name, m.folder, m.label);
}

std::optional<std::string> FileOps::undo_text() const {
    const ops::UndoRecord* r = undo_.peek_undo();
    if (r == nullptr) {
        return std::nullopt;
    }
    return undo_label_text(ops::make_label(*r));
}

std::optional<std::string> FileOps::redo_text() const {
    const ops::UndoRecord* r = undo_.peek_redo();
    if (r == nullptr) {
        return std::nullopt;
    }
    return undo_label_text(ops::make_label(*r));
}

} // namespace navigator::ui
