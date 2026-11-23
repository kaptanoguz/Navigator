// src/ui/trash/trash_model.cpp — see trash_model.hpp.
#include "trash/trash_model.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace navigator::ui {
namespace {
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
} // namespace

TrashModel::TrashModel(Options options) : options_(std::move(options)) {}

TrashModel::~TrashModel() {
    worker_.request_stop();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void TrashModel::refresh() {
    if (!options_.trash) {
        loaded_ = true;
        error_ = Error{ErrorCode::Unsupported, "no trash"};
        ++serial_;
        return;
    }
    worker_.request_stop();
    if (worker_.joinable()) {
        worker_.join();
    }
    const std::uint64_t gen = ++generation_;
    loading_ = true;
    worker_ = std::jthread([this, gen, trash = options_.trash](const std::stop_token& stop) {
        Result<trash::TrashListing> r = trash->list(stop);
        if (stop.stop_requested()) {
            return;
        }
        {
            const std::scoped_lock lock(mu_);
            posted_ = Posted{gen, std::move(r)};
        }
        if (options_.wake) {
            options_.wake();
        }
    });
}

bool TrashModel::frame_start() {
    std::optional<Posted> got;
    {
        const std::scoped_lock lock(mu_);
        got = std::move(posted_);
        posted_.reset();
    }
    if (!got || got->generation != generation_) {
        return false;
    }
    loading_ = false;
    loaded_ = true;
    ++serial_;
    // Keep the selection by identity across a refresh.
    std::vector<trash::TrashedItem> keep = selected_items();
    const trash::TrashedItem* cur = cursor_ < items_.size() ? &items_[cursor_] : nullptr;
    const trash::TrashedItem cursor_item = cur != nullptr ? *cur : trash::TrashedItem{};
    const bool had_cursor = cur != nullptr;
    if (!got->listing) {
        error_ = got->listing.error();
        items_.clear();
        selected_.clear();
        cursor_ = anchor_ = kNone;
        recount();
        return true;
    }
    error_.reset();
    items_ = std::move(got->listing->items);
    orphans_ = got->listing->orphans;
    dir_errors_ = got->listing->dir_errors.size();
    selected_.assign(items_.size(), 0);
    for (std::size_t i = 0; i < items_.size(); ++i) {
        for (const trash::TrashedItem& k : keep) {
            if (items_[i].files_path() == k.files_path() && items_[i].id == k.id) {
                selected_[i] = 1;
            }
        }
    }
    cursor_ = anchor_ = kNone;
    if (had_cursor) {
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].files_path() == cursor_item.files_path()) {
                cursor_ = anchor_ = i;
            }
        }
    }
    if (cursor_ == kNone && !items_.empty()) {
        cursor_ = anchor_ = 0;
    }
    recount();
    return true;
}

bool TrashModel::wait_listing(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)frame_start();
        if (!loading_) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void TrashModel::recount() {
    selected_count_ = 0;
    for (const std::uint8_t f : selected_) {
        selected_count_ += f != 0 ? 1U : 0U;
    }
}

void TrashModel::click_row(std::size_t row, Modifiers mods) {
    if (row >= items_.size()) {
        return;
    }
    const bool ctrl = has(mods, Modifiers::Ctrl);
    const bool shift = has(mods, Modifiers::Shift);
    if (shift && anchor_ < items_.size()) {
        const std::size_t a = std::min(anchor_, row);
        const std::size_t b = std::max(anchor_, row);
        if (!ctrl) {
            std::ranges::fill(selected_, std::uint8_t{0});
        }
        for (std::size_t i = a; i <= b; ++i) {
            selected_[i] = 1;
        }
        cursor_ = row;
    } else if (ctrl) {
        selected_[row] = selected_[row] != 0 ? 0 : 1;
        cursor_ = anchor_ = row;
    } else {
        std::ranges::fill(selected_, std::uint8_t{0});
        selected_[row] = 1;
        cursor_ = anchor_ = row;
    }
    recount();
    ++serial_;
}

void TrashModel::select_all() {
    std::ranges::fill(selected_, std::uint8_t{1});
    recount();
    ++serial_;
}

void TrashModel::clear_selection() {
    std::ranges::fill(selected_, std::uint8_t{0});
    recount();
    ++serial_;
}

void TrashModel::cursor_by(std::ptrdiff_t delta, bool extend) {
    if (items_.empty()) {
        return;
    }
    const auto n = static_cast<std::ptrdiff_t>(items_.size());
    std::ptrdiff_t at =
        cursor_ == kNone ? (delta > 0 ? -1 : n) : static_cast<std::ptrdiff_t>(cursor_);
    at = std::clamp<std::ptrdiff_t>(at + delta, 0, n - 1);
    const auto row = static_cast<std::size_t>(at);
    if (extend) {
        click_row(row, Modifiers::Shift);
    } else {
        click_row(row, Modifiers::None);
    }
}

void TrashModel::cursor_page(int direction, bool extend, std::size_t page_rows) {
    cursor_by(static_cast<std::ptrdiff_t>(direction) *
                  static_cast<std::ptrdiff_t>(std::max<std::size_t>(page_rows, 1)),
              extend);
}

void TrashModel::cursor_home(bool extend) {
    if (!items_.empty()) {
        click_row(0, extend ? Modifiers::Shift : Modifiers::None);
    }
}

void TrashModel::cursor_end(bool extend) {
    if (!items_.empty()) {
        click_row(items_.size() - 1, extend ? Modifiers::Shift : Modifiers::None);
    }
}

bool TrashModel::is_selected(std::size_t row) const noexcept {
    return row < selected_.size() && selected_[row] != 0;
}

std::vector<trash::TrashedItem> TrashModel::selected_items() const {
    std::vector<trash::TrashedItem> out;
    for (std::size_t i = 0; i < items_.size() && i < selected_.size(); ++i) {
        if (selected_[i] != 0) {
            out.push_back(items_[i]);
        }
    }
    return out;
}

std::vector<trash::TrashedItem> TrashModel::selected_or_cursor() const {
    std::vector<trash::TrashedItem> out = selected_items();
    if (out.empty() && cursor_ < items_.size()) {
        out.push_back(items_[cursor_]);
    }
    return out;
}

std::string TrashModel::display_name_of(const trash::TrashedItem& item) {
    std::string name = item.original_path.filename().string();
    if (name.empty()) {
        name = item.name;
    }
    return fs::to_display_name(name);
}

fs::DirEntry TrashModel::entry_of(const trash::TrashedItem& item) {
    fs::DirEntry e;
    e.name = item.original_path.filename().string();
    if (e.name.empty()) {
        e.name = item.name;
    }
    e.kind = item.kind;
    e.size = item.size;
    e.mtime = item.mtime;
    e.hidden = fs::is_hidden_name(e.name);
    e.readable = true;
    return e;
}

Timestamp TrashModel::deleted_at(const trash::TrashedItem& item) {
    if (!item.deletion_date) {
        return Timestamp{};
    }
    const trash::LocalDateTime& d = *item.deletion_date;
    std::tm tm{};
    tm.tm_year = d.year - 1900;
    tm.tm_mon = d.month - 1;
    tm.tm_mday = d.day;
    tm.tm_hour = d.hour;
    tm.tm_min = d.minute;
    tm.tm_sec = d.second;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) {
        return Timestamp{};
    }
    return Timestamp{std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(t))};
}

} // namespace navigator::ui
