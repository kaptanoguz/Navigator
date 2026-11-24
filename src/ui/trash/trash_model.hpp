// src/ui/trash/trash_model.hpp
//
// Headless state of the Trash view (Phase 7, docs/architecture.md "Trash"): the items of every
// trash directory (trash::ITrash::list on a worker, newest deletion first), a cursor and a
// selection. A tab in Trash mode shows this instead of its folder (a runtime mode like the
// flattened view: not persisted). No SDL; the view draws it and records Intents, the App
// applies them at frame end.
//
// Safety: nothing here changes a file. Restore and delete go through FileOps (OpKind::Restore,
// confirmed DeletePermanently); there is no default destructive action on the cursor row:
// delete acts on the selection only.
// Thread-safety: UI thread only; the listing worker posts one result through a mutex and wakes
// the frame loop.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/trash.hpp>
#include <navigator/types.hpp>

namespace navigator::ui {

class TrashModel {
public:
    struct Options {
        std::shared_ptr<trash::ITrash> trash; // nullptr: unavailable (Unsupported)
        std::function<void()> wake;           // thread-safe
    };

    explicit TrashModel(Options options);
    ~TrashModel();
    TrashModel(const TrashModel&) = delete;
    TrashModel& operator=(const TrashModel&) = delete;
    TrashModel(TrashModel&&) = delete;
    TrashModel& operator=(TrashModel&&) = delete;

    // Starts a listing (replaces one in flight: the newest result wins).
    void refresh();
    // Applies a finished listing. True when anything visible changed.
    bool frame_start();
    // Tests: blocks until the outstanding listing finished, then applies it.
    [[nodiscard]] bool wait_listing(std::chrono::milliseconds timeout);

    [[nodiscard]] bool available() const noexcept { return options_.trash != nullptr; }
    [[nodiscard]] bool loading() const noexcept { return loading_; }
    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t orphans() const noexcept { return orphans_; }
    [[nodiscard]] std::size_t unreadable_dirs() const noexcept { return dir_errors_; }
    [[nodiscard]] const std::vector<trash::TrashedItem>& items() const noexcept { return items_; }
    [[nodiscard]] std::size_t count() const noexcept { return items_.size(); }
    [[nodiscard]] std::uint64_t serial() const noexcept { return serial_; }

    // Selection: click semantics of the folder list (plain: only this row; Ctrl: toggle; Shift:
    // range from the anchor).
    void click_row(std::size_t row, Modifiers mods);
    void select_all();
    void clear_selection();
    void cursor_by(std::ptrdiff_t delta, bool extend);
    void cursor_page(int direction, bool extend, std::size_t page_rows);
    void cursor_home(bool extend);
    void cursor_end(bool extend);
    [[nodiscard]] bool is_selected(std::size_t row) const noexcept;
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::size_t selected_count() const noexcept { return selected_count_; }
    [[nodiscard]] std::vector<trash::TrashedItem> selected_items() const;
    // The selection, else the cursor row (Restore is never destructive).
    [[nodiscard]] std::vector<trash::TrashedItem> selected_or_cursor() const;

    // The row's name as shown (file name of the original path, display form).
    [[nodiscard]] static std::string display_name_of(const trash::TrashedItem& item);
    // A DirEntry describing the item (glyph and type text).
    [[nodiscard]] static fs::DirEntry entry_of(const trash::TrashedItem& item);
    // Deletion time as a Timestamp (local time of the info file), for format_date().
    [[nodiscard]] static Timestamp deleted_at(const trash::TrashedItem& item);

private:
    struct Posted {
        std::uint64_t generation = 0;
        Result<trash::TrashListing> listing = trash::TrashListing{};
    };
    void recount();

    Options options_;
    std::mutex mu_;
    std::optional<Posted> posted_;
    std::jthread worker_;
    std::uint64_t generation_ = 0;
    bool loading_ = false;
    bool loaded_ = false;
    std::optional<Error> error_;
    std::size_t orphans_ = 0;
    std::size_t dir_errors_ = 0;
    std::vector<trash::TrashedItem> items_;
    std::vector<std::uint8_t> selected_;
    std::size_t cursor_ = static_cast<std::size_t>(-1);
    std::size_t anchor_ = static_cast<std::size_t>(-1);
    std::size_t selected_count_ = 0;
    std::uint64_t serial_ = 0;
};

} // namespace navigator::ui
