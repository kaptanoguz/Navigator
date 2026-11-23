// ops::UndoStack (navigator/ops.hpp): bounded undo/redo history, UI thread only.
#include <memory>
#include <utility>

#include <navigator/ops.hpp>

namespace navigator::ops {

UndoStack::UndoStack(std::size_t depth) noexcept : depth_(depth == 0 ? 1 : depth) {}

UndoId UndoStack::record_done(UndoRecord record, OpOrigin origin) {
    record.id = UndoId{next_id_++};
    const UndoId id = record.id;
    auto ptr = std::make_shared<const UndoRecord>(std::move(record));
    switch (origin) {
    case OpOrigin::User:
        undo_.push_back(std::move(ptr));
        redo_.clear();
        break;
    case OpOrigin::Undo: redo_.push_back(std::move(ptr)); break;
    case OpOrigin::Redo: undo_.push_back(std::move(ptr)); break;
    }
    while (undo_.size() > depth_) {
        undo_.erase(undo_.begin());
    }
    while (redo_.size() > depth_) {
        redo_.erase(redo_.begin());
    }
    return id;
}

bool UndoStack::can_undo() const noexcept { return !undo_.empty(); }

bool UndoStack::can_redo() const noexcept { return !redo_.empty(); }

const UndoRecord* UndoStack::peek_undo() const noexcept {
    return undo_.empty() ? nullptr : undo_.back().get();
}

const UndoRecord* UndoStack::peek_redo() const noexcept {
    return redo_.empty() ? nullptr : redo_.back().get();
}

std::shared_ptr<const UndoRecord> UndoStack::take_undo() {
    if (undo_.empty()) {
        return nullptr;
    }
    auto r = std::move(undo_.back());
    undo_.pop_back();
    return r;
}

std::shared_ptr<const UndoRecord> UndoStack::take_redo() {
    if (redo_.empty()) {
        return nullptr;
    }
    auto r = std::move(redo_.back());
    redo_.pop_back();
    return r;
}

void UndoStack::restore(std::shared_ptr<const UndoRecord> record, OpOrigin taken_for) {
    if (!record) {
        return;
    }
    if (taken_for == OpOrigin::Redo) {
        redo_.push_back(std::move(record));
        while (redo_.size() > depth_) {
            redo_.erase(redo_.begin());
        }
    } else {
        undo_.push_back(std::move(record));
        while (undo_.size() > depth_) {
            undo_.erase(undo_.begin());
        }
    }
}

void UndoStack::clear() noexcept {
    undo_.clear();
    redo_.clear();
}

std::size_t UndoStack::undo_size() const noexcept { return undo_.size(); }

std::size_t UndoStack::redo_size() const noexcept { return redo_.size(); }

std::size_t UndoStack::depth() const noexcept { return depth_; }

} // namespace navigator::ops
