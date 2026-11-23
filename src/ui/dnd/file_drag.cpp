// src/ui/dnd/file_drag.cpp — see file_drag.hpp.
#include "dnd/file_drag.hpp"

#include <algorithm>

#include <sys/stat.h>
#include <unistd.h>

#include <navigator/ops.hpp>

namespace navigator::ui {
namespace {

int priority(SlotKind kind) noexcept {
    switch (kind) {
    case SlotKind::Pane: return 10;
    case SlotKind::Tab:
    case SlotKind::Crumb:
    case SlotKind::Sidebar: return 40;
    case SlotKind::Trash: return 45;
    case SlotKind::Row: return 50;
    }
    return 0;
}

class DefaultProbe final : public DropProbe {
public:
    [[nodiscard]] std::optional<std::uint64_t> device(const Path& path) override {
        struct stat st {};
        if (::lstat(path.c_str(), &st) != 0) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(st.st_dev);
    }
    [[nodiscard]] bool writable(const Path& folder) override {
        return ::access(folder.c_str(), W_OK | X_OK) == 0;
    }
};

} // namespace

DropProbe& default_drop_probe() {
    static DefaultProbe probe;
    return probe;
}

const DropSlot* pick_slot(const std::vector<DropSlot>& slots, float x, float y) {
    const DropSlot* best = nullptr;
    for (const DropSlot& s : slots) {
        if (!contains(s.rect, x, y)) {
            continue;
        }
        if (best == nullptr || priority(s.kind) >= priority(best->kind)) {
            best = &s;
        }
    }
    return best;
}

DropEval evaluate_drop(const std::vector<Path>& sources, const DropSlot* slot, bool internal,
                       Modifiers mods, DropProbe& probe) {
    DropEval out;
    if (slot == nullptr || sources.empty()) {
        return out;
    }
    out.kind = slot->kind;
    out.label = slot->label;
    out.rect = slot->rect;
    if (slot->kind == SlotKind::Trash) {
        out.trash = internal; // only Navigator's own items go to the trash by dropping
        return out;
    }
    out.folder = slot->folder;
    if (slot->folder.empty()) {
        return out;
    }
    transfer::DropContext c;
    c.internal = internal;
    c.target_writable = probe.writable(slot->folder);
    c.ctrl = has(mods, Modifiers::Ctrl);
    c.shift = has(mods, Modifiers::Shift);
    c.alt = has(mods, Modifiers::Alt);
    c.into_source_folder = true;
    c.same_filesystem = true;
    const std::optional<std::uint64_t> target_dev = probe.device(slot->folder);
    for (const Path& s : sources) {
        if (s.parent_path() != slot->folder) {
            c.into_source_folder = false;
        }
        if (ops::is_same_or_inside(slot->folder, s)) {
            c.into_itself = true;
        }
        const std::optional<std::uint64_t> dev = probe.device(s);
        if (!dev || !target_dev || *dev != *target_dev) {
            c.same_filesystem = false;
        }
    }
    out.action = transfer::choose_drop_action(c);
    return out;
}

// ---------------------------------------------------------------------------------------------

void FileDrag::begin(std::vector<Path> items, std::uint32_t origin_tab, double now_ms) {
    active_ = !items.empty();
    items_ = std::move(items);
    origin_tab_ = origin_tab;
    hover_.reset();
    eval_ = DropEval{};
    hover_since_ms_ = now_ms;
    spring_fired_ = false;
}

void FileDrag::update(float x, float y, Modifiers mods, const std::vector<DropSlot>& slots,
                      double now_ms, DropProbe& probe) {
    if (!active_) {
        return;
    }
    x_ = x;
    y_ = y;
    mods_ = mods;
    const DropSlot* slot = pick_slot(slots, x, y);
    const bool same = slot != nullptr && hover_ && hover_->kind == slot->kind &&
                      hover_->folder == slot->folder && hover_->tab == slot->tab;
    if (!same) {
        hover_since_ms_ = now_ms;
        spring_fired_ = false;
    }
    if (slot != nullptr) {
        hover_ = *slot;
    } else {
        hover_.reset();
    }
    eval_ = evaluate_drop(items_, slot, true, mods, probe);
}

FileDrag::Outcome FileDrag::finish() {
    Outcome out;
    if (active_ && hover_ && eval_.allowed()) {
        out.kind = Outcome::Kind::Drop;
        out.items = items_;
        out.eval = eval_;
    }
    cancel();
    return out;
}

void FileDrag::cancel() noexcept {
    active_ = false;
    items_.clear();
    hover_.reset();
    eval_ = DropEval{};
    spring_fired_ = false;
}

std::optional<DropSlot> FileDrag::take_spring(double now_ms) {
    if (!active_ || !hover_ || spring_fired_ || hover_->kind != SlotKind::Row ||
        now_ms - hover_since_ms_ < kSpringLoadMs || !eval_.allowed()) {
        return std::nullopt;
    }
    spring_fired_ = true;
    return hover_;
}

// ---------------------------------------------------------------------------------------------

void IncomingDrop::begin() {
    active_ = true;
    positioned_ = false;
    paths_.clear();
}

void IncomingDrop::position(float x, float y) {
    if (!active_) {
        begin();
    }
    x_ = x;
    y_ = y;
    positioned_ = true;
}

void IncomingDrop::add_path(const Path& path) {
    if (!active_) {
        begin();
    }
    if (path.empty() || !path.is_absolute()) {
        return;
    }
    const Path norm = path.lexically_normal();
    if (std::ranges::find(paths_, norm) == paths_.end()) {
        paths_.push_back(norm);
    }
}

std::size_t IncomingDrop::add_text(std::string_view text) {
    const transfer::UriList list = transfer::decode_uri_list(text);
    std::size_t added = 0;
    for (const Path& p : list.paths) {
        const std::size_t before = paths_.size();
        add_path(p);
        added += paths_.size() - before;
    }
    return added;
}

std::vector<Path> IncomingDrop::complete() {
    std::vector<Path> out = std::move(paths_);
    cancel();
    return out;
}

void IncomingDrop::cancel() noexcept {
    active_ = false;
    positioned_ = false;
    paths_.clear();
}

} // namespace navigator::ui
