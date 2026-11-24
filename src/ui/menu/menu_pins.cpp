// Context menu pin list and menu_pins.json persistence (navigator/menu.hpp "Pins").
#include <algorithm>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/menu.hpp>

#include "../../core/document_files.hpp"
#include "../../core/json_util.hpp"

namespace navigator::menu {
namespace {

constexpr std::size_t kMaxPinsFileBytes = std::size_t{64} * 1024U;
constexpr int kPinsVersion = 1;

[[nodiscard]] bool is_non_pinnable_id(std::string_view id) noexcept {
    if (id.empty()) {
        return true;
    }
    if (id.starts_with("submenu.") || id.starts_with("open_with:")) {
        return true;
    }
    return id == "file.delete_permanently" || id == "trash.empty" || id == "layout.delete" ||
           id == "keymap.reset_defaults";
}

} // namespace

Result<void> Pins::pin(std::string_view id) {
    try {
        if (is_non_pinnable_id(id)) {
            return fail(ErrorCode::InvalidArgument, "menu item id is not pinnable");
        }
        if (contains(id)) {
            return fail(ErrorCode::AlreadyExists, "menu item is already pinned");
        }
        if (ids_.size() >= kMaxPins) {
            return fail(ErrorCode::InvalidArgument, "pin limit reached");
        }
        ids_.emplace_back(id);
        return {};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while pinning menu item");
    }
}

bool Pins::unpin(std::string_view id) { return std::erase(ids_, id) > 0; }

bool Pins::move(std::string_view id, std::size_t index) {
    const auto it = std::find(ids_.begin(), ids_.end(), id);
    if (it == ids_.end()) {
        return false;
    }
    std::string moved = std::move(*it);
    ids_.erase(it);
    const std::size_t clamped = std::min(index, ids_.size());
    ids_.insert(ids_.begin() + static_cast<std::ptrdiff_t>(clamped), std::move(moved));
    return true;
}

void Pins::clear() noexcept { ids_.clear(); }

bool Pins::contains(std::string_view id) const noexcept {
    return std::find(ids_.begin(), ids_.end(), id) != ids_.end();
}

Result<Pins> pins_from_json(std::string_view text) {
    try {
        if (text.size() > kMaxPinsFileBytes) {
            return fail(ErrorCode::InvalidArgument, "menu pins JSON larger than 64 KiB");
        }
        Result<core::json::InJson> doc = core::json::parse_object(text);
        if (!doc) {
            return Unexpected<Error>(doc.error());
        }
        Pins pins;
        const core::json::InJson* arr = core::json::member(*doc, "pins");
        if (arr == nullptr || !arr->is_array()) {
            return pins;
        }
        for (const core::json::InJson& elem : *arr) {
            const auto id = core::json::as_string(&elem);
            if (!id) {
                continue;
            }
            (void)pins.pin(*id); // drops empty, non-pinnable, duplicates, excess
        }
        return pins;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while parsing menu pins");
    }
}

std::string to_json(const Pins& pins) {
    core::json::OutJson doc = core::json::OutJson::object();
    doc["version"] = kPinsVersion;
    core::json::OutJson arr = core::json::OutJson::array();
    for (const std::string& id : pins.ids()) {
        arr.push_back(core::json::bytes_to_json(id));
    }
    doc["pins"] = std::move(arr);
    return core::json::dump(doc);
}

Result<Pins> load_pins(const Path& file) {
    try {
        Result<std::string> text = core::read_document(file);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        Result<Pins> pins = pins_from_json(*text);
        if (!pins && pins.error().code == ErrorCode::InvalidArgument) {
            if (text->size() > kMaxPinsFileBytes) {
                return pins;
            }
            return Unexpected<Error>(core::move_aside_bad(file, pins.error().message));
        }
        return pins;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while loading menu pins");
    }
}

Result<void> save_pins(const Path& file, const Pins& pins) {
    try {
        if (file.empty()) {
            return fail(ErrorCode::InvalidArgument, "empty path");
        }
        return core::write_atomically(file, to_json(pins));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while saving menu pins");
    }
}

} // namespace navigator::menu
