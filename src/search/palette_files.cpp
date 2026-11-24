// palette_history.json: JSON v1 (de)serialisation and file I/O (navigator/palette.hpp "History").
// The bounded read, atomic private write and ".bad" move-aside are the shared document rules of
// navigator_core (src/core/document_files.hpp, the same code session.json uses).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/palette.hpp>

#include "../core/document_files.hpp"
#include "../core/json_util.hpp"

namespace navigator::palette {
namespace {

constexpr std::size_t kMaxHistoryFileBytes = std::size_t{1} << 20U;
constexpr int kHistoryVersion = 1;

} // namespace

Result<History> history_from_json(std::string_view text) {
    try {
        if (text.size() > kMaxHistoryFileBytes) {
            return fail(ErrorCode::InvalidArgument, "palette history larger than 1 MiB");
        }
        Result<core::json::InJson> doc = core::json::parse_object(text);
        if (!doc) {
            return Unexpected<Error>(doc.error());
        }
        History history;
        const core::json::InJson* items = core::json::member(*doc, "items");
        if (items == nullptr || !items->is_array()) {
            return history;
        }
        std::vector<HistoryItem> parsed;
        for (const core::json::InJson& element : *items) {
            if (!element.is_object()) {
                continue;
            }
            const auto key = core::json::as_string(core::json::member(element, "command"));
            if (!key || key->empty()) {
                continue;
            }
            std::int64_t count = 1;
            if (const core::json::InJson* raw = core::json::member(element, "count")) {
                const auto value = core::json::as_integer(raw);
                if (!value || *value <= 0) {
                    continue;
                }
                count = *value;
            }
            const auto clamped = static_cast<std::uint32_t>(
                std::min<std::int64_t>(count, std::int64_t{kMaxHistoryCount}));
            parsed.push_back(HistoryItem{*key, clamped});
        }
        history.assign(std::move(parsed)); // dedupes (first wins) and applies the item limit
        return history;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while reading the palette history");
    }
}

std::string to_json(const History& history) {
    core::json::OutJson doc = core::json::OutJson::object();
    doc["version"] = kHistoryVersion;
    core::json::OutJson items = core::json::OutJson::array();
    for (const HistoryItem& item : history.items()) {
        core::json::OutJson entry = core::json::OutJson::object();
        entry["command"] = core::json::bytes_to_json(item.key);
        entry["count"] = item.count;
        items.push_back(std::move(entry));
    }
    doc["items"] = std::move(items);
    return core::json::dump(doc);
}

Result<History> load_history(const Path& file) {
    try {
        Result<std::string> text = core::read_document(file);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        Result<History> history = history_from_json(*text);
        if (!history && history.error().code == ErrorCode::InvalidArgument) {
            if (text->size() > kMaxHistoryFileBytes) {
                return history; // too large: left in place, not renamed
            }
            return Unexpected<Error>(core::move_aside_bad(file, history.error().message));
        }
        return history;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while loading the palette history");
    }
}

Result<void> save_history(const Path& file, const History& history) {
    try {
        if (file.empty()) {
            return fail(ErrorCode::InvalidArgument, "empty path");
        }
        return core::write_atomically(file, to_json(history));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while saving the palette history");
    }
}

} // namespace navigator::palette
