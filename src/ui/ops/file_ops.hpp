// src/ui/ops/file_ops.hpp
//
// The ui side of file operations (Phase 7, docs/architecture.md "Phase 7"): one FileOps per
// window owns the ops::IOperationQueue, the ops::UndoStack, the clipboard offer, the pending
// sheets (permanent-delete / empty-trash confirmation, conflicts, quit while operations run),
// the inline rename / new-folder editor, the notices ("Moved 3 items to the trash. Undo") and
// the history shown by the operations panel. Headless: no SDL, no renderer. The App feeds it
// selections (FileTarget) from commands, polls it at frame start and drains its reconcile
// output (refresh, select, Inspector follow) afterwards; the views read it and record Intents.
//
// Safety (ops.hpp S1..S16, trash.hpp T1..T7) as the ui enforces them:
//   - Delete means trash (S2): file.trash submits OpKind::Trash and never deletes. Items that
//     cannot be trashed come back in OpResult::untrashable and open the permanent-delete sheet
//     for exactly those items, with the focus on Cancel.
//   - Permanent delete is requested only from a confirmation sheet whose default focus is
//     Cancel; `confirmed` is set here and nowhere else (S3).
//   - Without an ITrash (headless runs without --trash-dir) trash / restore / empty are
//     Unsupported and never fall back to anything.
// Thread-safety: UI thread only. The queue's workers only push events (wake) and are polled.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <navigator/layout.hpp>
#include <navigator/ops.hpp>
#include <navigator/transfer.hpp>
#include <navigator/trash.hpp>
#include <navigator/types.hpp>

#include "ops/clipboard.hpp"
#include "widgets/text_edit.hpp"

namespace navigator::ui {

// What a file command acts on: the active tab of the active pane.
struct FileTarget {
    layout::TabId tab;
    Path dir;                   // the folder shown (the paste / new-folder destination)
    std::vector<Path> selected; // selected rows, list order (absolute)
    Path cursor;                // cursor row (absolute); empty without one
    bool cursor_is_dir = false; // the cursor row is a folder (or a link to one)
};

enum class NoticeAction : std::uint8_t { None, Undo, Redo, ShowInFolder, ShowPanel };

struct OpNotice {
    std::string text;
    bool error = false;
    NoticeAction action = NoticeAction::None;
    ops::OpId op;
    Path folder; // ShowInFolder
    std::uint64_t serial = 0;
    double until_ms = 0.0;
};

// One finished operation in the panel's history (newest last).
struct FinishedOp {
    ops::OpId id;
    ops::OpKind kind = ops::OpKind::Copy;
    ops::OpOrigin origin = ops::OpOrigin::User;
    std::string title;                // "Moved 3 items to Documents" / "2 of 3 items moved"
    std::vector<std::string> details; // failing items, one plain-language sentence each
    bool ok = true;                   // nothing failed or was left unchanged
    bool cancelled = false;
    bool undoable = false;
    Path folder; // "Show in folder"
    double finished_ms = 0.0;
};

// What the App must do to its views after operations finished (drain with take_reconcile()).
struct Reconcile {
    std::vector<Path> refresh;                // folders whose listing needs a re-list
    std::vector<std::pair<Path, Path>> moved; // source -> destination (empty: gone), in order
    struct Select {
        Path dir;                       // select these names when `dir` is shown
        std::vector<std::string> names; // raw bytes
    };
    std::vector<Select> select;
    // Ctrl+Shift+N made a folder: open the inline editor on it once its row exists.
    struct Rename {
        layout::TabId tab;
        Path dir;
        std::string name;
    };
    std::optional<Rename> rename_after_create;
    bool trash_changed = false; // refresh the Trash view / count
    [[nodiscard]] bool empty() const noexcept {
        return refresh.empty() && moved.empty() && select.empty() && !trash_changed &&
               !rename_after_create;
    }
};

enum class SheetKind : std::uint8_t {
    None,
    Conflict,
    DeletePermanently, // Shift+Delete
    DeleteUntrashable, // Delete on items without a usable trash
    EmptyTrash,
    DeleteTrashed, // Delete in the Trash view
    QuitRunning,
};

// Totals of a tree, counted off the UI thread (sheet body: "3 items, 12.4 MB").
struct TreeStats {
    std::uint64_t files = 0;
    std::uint64_t folders = 0;
    std::uint64_t bytes = 0;
    bool done = false;
    bool partial = false; // a limit was hit: "at least"
};

class TreeProbe; // below

struct ConfirmSheet {
    SheetKind kind = SheetKind::None;
    std::vector<Path> paths;                 // DeletePermanently, DeleteUntrashable
    std::vector<trash::TrashedItem> trashed; // EmptyTrash, DeleteTrashed
    std::size_t count = 0;
    std::vector<std::string> names; // first few display names
    std::size_t focus = 0;          // button index; 0 is always Cancel
    std::shared_ptr<TreeProbe> probe;
    static constexpr std::size_t kButtons = 2;
};

struct ConflictSheet {
    ops::Conflict conflict;
    std::size_t focus = 0; // index into buttons()
    bool apply_all = false;
    bool editing_name = false;
    TextEdit name{ops::kNameMaxBytes}; // the Keep both name (suggested, editable)
    std::string name_error;            // validate_new_name feedback
    std::string suggested;
};

// One button of the conflict sheet.
enum class ConflictButton : std::uint8_t { Replace, Skip, KeepBoth, Merge, Cancel };
[[nodiscard]] std::vector<ConflictButton> conflict_buttons(const ops::Conflict& c);

// The inline editor of F2 / Ctrl+Shift+N (a Phase 4 text field: Enter commits, Escape cancels).
struct InlineEdit {
    layout::TabId tab;
    Path dir;
    std::string original; // raw name before editing
    bool is_dir = false;
    bool after_create = false; // started right after Ctrl+Shift+N made the folder
    TextEdit edit{ops::kNameMaxBytes};
    std::string message; // inline feedback (translated); empty: none
    bool message_error = false;
    float scroll = 0.0F; // field scroll (view state)
};

enum class QuitState : std::uint8_t {
    Running,       // nothing asked
    Asking,        // the quit sheet is open
    WaitingToQuit, // "Wait, then quit": quit once the queue is idle
    Quit,          // quit now (nothing running, or cancelled)
};

struct FileOpsOptions {
    // Injected trash (never the real XDG trash unless the App says so). nullptr: trash,
    // restore and "empty" are Unsupported.
    std::shared_ptr<trash::ITrash> trash;
    ops::ProtectedPaths protected_paths;
    std::function<void()> wake;      // thread-safe; wakes the frame loop
    IClipboard* clipboard = nullptr; // not owned; nullptr: copy / paste are no-ops
    // Undo checks that the volumes a record touched are mounted; empty: always.
    std::function<bool(const Path&)> is_mounted;
    // Name of the "New folder" default (translated by the caller; Turkish "Yeni klasör").
    std::function<std::string()> new_folder_name;
    std::size_t max_concurrent = 2;
    // Test / debug seams (EngineOptions).
    std::function<bool(const Path&)> fault_injector;
    std::chrono::milliseconds progress_interval{100};
    std::size_t copy_chunk_bytes = std::size_t{1} << 20U;
    double notice_ms = 8000.0;
};

// While the inline rename / new-folder editor is open, no file command may fire or restart it
// (BUG-P7-1: F2 re-ran file.rename and discarded the typed text; Ctrl+Shift+N, F7 and Shift+F7
// would create or copy things mid-edit). Only the operations panel commands stay available.
[[nodiscard]] constexpr bool file_command_blocked_while_editing(CommandId id) noexcept {
    switch (id) {
    case CommandId::CancelOperation:
    case CommandId::TogglePauseOperation:
    case CommandId::ToggleOperationsPanel: return false;
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
    case CommandId::OpenTrash:
    case CommandId::RestoreFromTrash:
    case CommandId::EmptyTrash: return true;
    default: return false;
    }
}

class FileOps {
public:
    explicit FileOps(FileOpsOptions options);
    ~FileOps();
    FileOps(const FileOps&) = delete;
    FileOps& operator=(const FileOps&) = delete;
    FileOps(FileOps&&) = delete;
    FileOps& operator=(FileOps&&) = delete;

    // ---- Frame start -------------------------------------------------------------------------
    // Polls the queue (progress, conflicts, finished operations), expires notices. True when
    // anything visible changed.
    bool frame_start(double now_ms);
    // Tests: frame_start() in a loop until the queue is idle and no operation runs.
    [[nodiscard]] bool wait_idle(std::chrono::milliseconds timeout);

    // ---- Commands (frame end) ----------------------------------------------------------------
    void copy(const FileTarget& target, bool cut);
    void paste(const FileTarget& target, bool into_cursor_folder);
    void trash(const FileTarget& target);
    void trash_paths(std::vector<Path> paths);
    void request_delete_permanently(const FileTarget& target);
    void copy_to_folder(const FileTarget& source, const Path& folder, bool move);
    // Drag and drop / "other pane": copy, move or link `sources` into `folder`.
    void transfer(std::vector<Path> sources, const Path& folder, transfer::DropAction action);
    void new_folder(const FileTarget& target);
    // Phase 9B: submit a validated OpKind::BatchRename request from BatchRenameOverlay.
    void submit_batch_rename(ops::OpRequest request);
    void undo();
    void redo();
    void cancel(std::uint64_t op); // 0: the most recently started unfinished operation
    void toggle_pause(std::uint64_t op);
    void cancel_all();
    // Trash view.
    void restore(std::vector<trash::TrashedItem> items);
    void request_empty_trash(std::vector<trash::TrashedItem> items);
    void request_delete_trashed(std::vector<trash::TrashedItem> items);

    // ---- Inline rename ----------------------------------------------------------------------
    // Starts the editor on `name` (raw bytes) in `dir` for tab `tab`, stem selected. `after_create`
    // selects the whole name (a brand-new folder).
    void begin_rename(layout::TabId tab, const Path& dir, const std::string& name, bool is_dir,
                      bool after_create = false);
    [[nodiscard]] InlineEdit* inline_edit() noexcept { return inline_ ? &*inline_ : nullptr; }
    [[nodiscard]] const InlineEdit* inline_edit() const noexcept {
        return inline_ ? &*inline_ : nullptr;
    }
    // Re-validates the shown text (call after every edit). `taken(name)`: the name exists in the
    // folder (case-sensitive listing lookup).
    void inline_edited(const std::function<bool(std::string_view)>& taken);
    // Enter / focus loss: renames when the text is valid and changed; an invalid name keeps the
    // editor open (message set). Returns true when the editor closed.
    bool inline_commit(const std::function<bool(std::string_view)>& taken);
    void inline_cancel() noexcept { inline_.reset(); }

    // ---- Sheets (keyboard-driven; Enter / Escape / Tab / Space routed by the App) -----------
    [[nodiscard]] SheetKind sheet_kind() const noexcept;
    [[nodiscard]] bool sheet_open() const noexcept { return sheet_kind() != SheetKind::None; }
    [[nodiscard]] const ConfirmSheet* confirm_sheet() const noexcept {
        return confirm_ ? &*confirm_ : nullptr;
    }
    [[nodiscard]] const ConflictSheet* conflict_sheet() const noexcept {
        return conflict_ ? &*conflict_ : nullptr;
    }
    [[nodiscard]] ConflictSheet* conflict_sheet() noexcept {
        return conflict_ ? &*conflict_ : nullptr;
    }
    [[nodiscard]] std::size_t quit_focus() const noexcept { return quit_focus_; }
    void sheet_focus(int delta);
    void sheet_focus_to(std::size_t index);
    void sheet_activate();                  // Enter: the focused button
    void sheet_activate(std::size_t index); // a click on button `index`
    void sheet_escape();                    // Escape
    void sheet_toggle_apply_all();          // Space (conflict)
    void conflict_begin_name_edit();
    void conflict_name_edited();
    // The confirm sheet shows the TreeStats of its probe (nullopt: none).
    [[nodiscard]] std::optional<TreeStats> confirm_stats() const;

    // ---- Quit --------------------------------------------------------------------------------
    // Quit requested (window close, app.quit). Returns true when the app may quit now.
    bool request_quit();
    [[nodiscard]] QuitState quit_state() const noexcept { return quit_state_; }
    // The window is closing: cancels what runs and joins the queue (bounded).
    void shutdown() noexcept;

    // ---- State for the views -----------------------------------------------------------------
    [[nodiscard]] const std::vector<ops::OpProgress>& running() const noexcept { return running_; }
    // An operation was submitted and its Finished event has not been handled yet.
    [[nodiscard]] bool busy() const noexcept {
        return submitted_ > completed_ || !running_.empty();
    }
    [[nodiscard]] std::string title_of(const ops::OpProgress& p) const;
    [[nodiscard]] const std::deque<FinishedOp>& history() const noexcept { return history_; }
    [[nodiscard]] const std::optional<OpNotice>& notice() const noexcept { return notice_; }
    void dismiss_notice() noexcept { notice_.reset(); }
    [[nodiscard]] bool panel_open() const noexcept { return panel_open_; }
    void toggle_panel() noexcept { panel_open_ = !panel_open_; }
    void close_panel() noexcept { panel_open_ = false; }
    // A cut row is drawn dimmed until pasted or until another program takes the clipboard.
    [[nodiscard]] bool is_cut(const Path& path) const;
    [[nodiscard]] bool has_cut() const noexcept { return !cut_.empty(); }
    [[nodiscard]] const ops::UndoStack& undo_stack() const noexcept { return undo_; }
    // "move 3 items to Photos" of the next undo / redo, for the palette; nullopt when empty.
    [[nodiscard]] std::optional<std::string> undo_text() const;
    [[nodiscard]] std::optional<std::string> redo_text() const;
    // Items finished and not yet reported to the views.
    [[nodiscard]] Reconcile take_reconcile();
    // The conflict sheet needs previews of both files.
    [[nodiscard]] bool has_trash() const noexcept { return options_.trash != nullptr; }
    [[nodiscard]] trash::ITrash* trash_service() const noexcept { return options_.trash.get(); }
    [[nodiscard]] ops::IOperationQueue* queue() noexcept { return queue_.get(); }
    void set_protected_paths(ops::ProtectedPaths paths);
    // Debug / script: the queue's latest bytes_done of the newest operation (screenshots).
    [[nodiscard]] std::uint64_t completed_ops() const noexcept { return completed_; }

    // Posts a notice from outside (the App: "Nothing selected", errors of its own).
    void notify(std::string text, bool error = false, NoticeAction action = NoticeAction::None);

private:
    struct Meta {
        ops::OpKind kind = ops::OpKind::Copy;
        std::size_t count = 0;
        std::string name;
        std::string folder;
        std::string label;
        Path destination;
        std::vector<Path> sources;
        bool cut_paste = false;
        bool create_folder = false;
        std::string new_name;
        layout::TabId tab;
    };

    void submit(ops::OpRequest request, const Meta& meta);
    void handle_event(const ops::OpEvent& event);
    void handle_finished(const ops::OpResult& result);
    void sync_conflict();
    void open_confirm(ConfirmSheet sheet);
    void close_sheet() noexcept;
    void confirm_accept();
    void conflict_decide(ConflictButton button);
    [[nodiscard]] Meta meta_for(const ops::OpRequest& request) const;
    void post_notice(std::string text, bool error, NoticeAction action, ops::OpId op,
                     Path folder = {});
    void clear_cut_if_stale();
    [[nodiscard]] std::vector<Path> paths_of_selection(const FileTarget& target) const;

    FileOpsOptions options_;
    std::unique_ptr<ops::IOperationQueue> queue_;
    ops::UndoStack undo_;
    std::map<std::uint64_t, Meta> meta_;
    std::vector<ops::OpProgress> running_;
    std::deque<FinishedOp> history_;
    std::optional<OpNotice> notice_;
    std::uint64_t notice_serial_ = 0;
    double now_ms_ = 0.0;
    bool panel_open_ = false;
    std::unordered_set<std::string> cut_;
    Reconcile reconcile_;
    std::optional<InlineEdit> inline_;
    std::optional<ConfirmSheet> confirm_;
    std::optional<ConflictSheet> conflict_;
    QuitState quit_state_ = QuitState::Running;
    std::size_t quit_focus_ = 0;
    std::uint64_t completed_ = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t last_started_ = 0;
    std::unordered_set<std::uint64_t> answered_; // conflicts resolved, not yet withdrawn
};

} // namespace navigator::ui
