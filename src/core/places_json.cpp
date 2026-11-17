// navigator/places.hpp "JSON format (version 1)" and "Files": bookmarks.json and recents.json.
// Writing is deterministic (ordered keys, json_util dump); reading is lenient and never drops an
// entry for being offline or missing. The file rules (size cap, atomic save, ".bad") are the
// session's, shared through document_files.hpp.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/path_input.hpp>
#include <navigator/places.hpp>

#include "document_files.hpp"
#include "json_util.hpp"
#include "places_internal.hpp"

namespace navigator::places {
namespace {

using core::json::as_bool;
using core::json::as_bytes;
using core::json::as_integer;
using core::json::as_string;
using core::json::bytes_to_json;
using core::json::dump;
using core::json::InJson;
using core::json::member;
using core::json::OutJson;
using core::json::parse_object;

constexpr int kFormatVersion = 1;

// ---- writing -------------------------------------------------------------------------------

std::int64_t to_seconds(Timestamp t) noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

OutJson volume_to_json(const VolumeIdentity& volume) {
    OutJson out = OutJson::object();
    if (!volume.uuid.empty()) {
        out["uuid"] = bytes_to_json(volume.uuid);
    }
    out["root"] = bytes_to_json(volume.root);
    out["device"] = bytes_to_json(volume.device);
    out["mount_point"] = bytes_to_json(volume.mount_point.native());
    return out;
}

// ---- reading -------------------------------------------------------------------------------

// Seconds since the epoch -> Timestamp, saturated to what Timestamp (int64 ns) can hold.
Timestamp timestamp_from(const InJson* value) {
    const std::optional<std::int64_t> seconds = as_integer(value);
    if (!seconds) {
        return Timestamp{};
    }
    constexpr std::int64_t kLimit = std::numeric_limits<std::int64_t>::max() / 1'000'000'000;
    const std::int64_t clamped = std::clamp(*seconds, -kLimit, kLimit);
    return Timestamp{std::chrono::seconds{clamped}};
}

std::optional<VolumeIdentity> volume_from(const InJson* value) {
    if (value == nullptr || !value->is_object()) {
        return std::nullopt;
    }
    VolumeIdentity volume;
    volume.uuid = as_bytes(member(*value, "uuid")).value_or(std::string{});
    volume.root = as_bytes(member(*value, "root")).value_or(std::string{});
    volume.device = as_bytes(member(*value, "device")).value_or(std::string{});
    if (std::optional<std::string> mount = as_bytes(member(*value, "mount_point"))) {
        volume.mount_point = Path(std::move(*mount));
    }
    if (volume.uuid.empty() && volume.device.empty()) {
        return std::nullopt; // matches nothing: counts as absent
    }
    return volume;
}

// The entry's normalized path, or an empty Path when it must be dropped.
Path path_from(const InJson& entry) {
    if (!entry.is_object()) {
        return {};
    }
    std::optional<std::string> raw = as_bytes(member(entry, "path"));
    if (!raw || raw->empty()) {
        return {};
    }
    return path_input::normalize_absolute(Path(std::move(*raw)));
}

const InJson* array_member(const InJson& doc, const char* key) {
    const InJson* value = member(doc, key);
    return value != nullptr && value->is_array() ? value : nullptr;
}

template <class Store, class FromJson>
Result<Store> load_file(const Path& file, FromJson from_json, const char* what) {
    try {
        Result<std::string> text = core::read_document(file);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        Result<Store> store = from_json(*text);
        if (!store && store.error().code == ErrorCode::InvalidArgument) {
            return Unexpected<Error>(core::move_aside_bad(file, store.error().message));
        }
        return store;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, std::string("out of memory while loading ") + what);
    }
}

template <class Store, class ToJson>
Result<void> save_file(const Path& file, const Store& store, ToJson to_json, const char* what) {
    try {
        if (file.empty()) {
            return fail(ErrorCode::InvalidArgument, "empty path");
        }
        Result<std::string> text = to_json(store);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        return core::write_atomically(file, *text);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, std::string("out of memory while saving ") + what);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Bookmarks
// ---------------------------------------------------------------------------------------------

Result<std::string> bookmarks_to_json(const BookmarkStore& store) {
    try {
        OutJson doc = OutJson::object();
        doc["version"] = kFormatVersion;
        OutJson list = OutJson::array();
        for (const Bookmark& bookmark : store.list()) {
            OutJson entry = OutJson::object();
            entry["path"] = bytes_to_json(bookmark.path.native());
            if (!bookmark.name.empty()) {
                entry["name"] = bytes_to_json(bookmark.name);
            }
            entry["added"] = to_seconds(bookmark.added);
            if (bookmark.volume) {
                entry["volume"] = volume_to_json(*bookmark.volume);
            }
            list.push_back(std::move(entry));
        }
        doc["bookmarks"] = std::move(list);
        return dump(doc);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while serializing bookmarks");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Unknown, std::string("bookmark serialization failed: ") + e.what());
    }
}

Result<BookmarkStore> bookmarks_from_json(std::string_view text) {
    try {
        Result<InJson> doc = parse_object(text);
        if (!doc) {
            return Unexpected<Error>(doc.error());
        }
        BookmarkStore store;
        const InJson* list = array_member(*doc, "bookmarks");
        if (list == nullptr) {
            return store;
        }
        for (const InJson& entry : *list) {
            if (store.size() >= kMaxBookmarks) {
                break;
            }
            Path path = path_from(entry);
            if (path.empty()) {
                continue;
            }
            std::string name = as_string(member(entry, "name")).value_or(std::string{});
            if (!internal::is_valid_bookmark_name(name)) {
                name.clear();
            }
            Result<BookmarkId> added =
                store.add(path, volume_from(member(entry, "volume")),
                          timestamp_from(member(entry, "added")), std::move(name));
            if (!added && added.error().code == ErrorCode::OutOfMemory) {
                return Unexpected<Error>(added.error());
            }
            // AlreadyExists: a later duplicate of an earlier path is dropped.
        }
        return store;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while reading bookmarks");
    } catch (const std::exception& e) {
        return fail(ErrorCode::InvalidArgument,
                    std::string("bookmarks JSON rejected: ") + e.what());
    }
}

// ---------------------------------------------------------------------------------------------
// Recents
// ---------------------------------------------------------------------------------------------

Result<std::string> recents_to_json(const RecentsStore& store) {
    try {
        OutJson doc = OutJson::object();
        doc["version"] = kFormatVersion;
        OutJson options = OutJson::object();
        options["enabled"] = store.options().enabled;
        options["highlight"] = store.options().highlight;
        options["sort_on_top"] = store.options().sort_on_top;
        doc["options"] = std::move(options);
        OutJson list = OutJson::array();
        for (const RecentEntry& recent : store.entries()) {
            OutJson entry = OutJson::object();
            entry["path"] = bytes_to_json(recent.path.native());
            entry["last_visit"] = to_seconds(recent.last_visit);
            entry["visits"] = recent.visit_count;
            if (recent.volume) {
                entry["volume"] = volume_to_json(*recent.volume);
            }
            list.push_back(std::move(entry));
        }
        doc["recents"] = std::move(list);
        return dump(doc);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while serializing recents");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Unknown, std::string("recents serialization failed: ") + e.what());
    }
}

Result<RecentsStore> recents_from_json(std::string_view text, std::size_t capacity) {
    try {
        Result<InJson> doc = parse_object(text);
        if (!doc) {
            return Unexpected<Error>(doc.error());
        }
        RecentsStore store(capacity);
        RecentsOptions options;
        if (const InJson* opts = member(*doc, "options"); opts != nullptr && opts->is_object()) {
            options.enabled = as_bool(member(*opts, "enabled"), options.enabled);
            options.highlight = as_bool(member(*opts, "highlight"), options.highlight);
            options.sort_on_top = as_bool(member(*opts, "sort_on_top"), options.sort_on_top);
        }
        store.set_options(options);
        const InJson* list = array_member(*doc, "recents");
        if (list == nullptr) {
            return store;
        }
        // Dedupe, normalization and the capacity cut are restore()'s; only the per-field
        // leniency is done here.
        std::vector<RecentEntry> loaded;
        loaded.reserve(std::min(list->size(), store.capacity()));
        for (const InJson& entry : *list) {
            Path path = path_from(entry);
            if (path.empty()) {
                continue;
            }
            RecentEntry recent;
            recent.path = std::move(path);
            recent.volume = volume_from(member(entry, "volume"));
            recent.last_visit = timestamp_from(member(entry, "last_visit"));
            const std::optional<std::int64_t> visits = as_integer(member(entry, "visits"));
            constexpr auto kMaxVisits =
                static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
            recent.visit_count = !visits || *visits < 1
                                     ? 1U
                                     : static_cast<std::uint32_t>(std::min(*visits, kMaxVisits));
            loaded.push_back(std::move(recent));
        }
        store.restore(std::move(loaded));
        return store;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while reading recents");
    } catch (const std::exception& e) {
        return fail(ErrorCode::InvalidArgument, std::string("recents JSON rejected: ") + e.what());
    }
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

Result<BookmarkStore> load_bookmarks(const Path& file) {
    return load_file<BookmarkStore>(
        file, [](std::string_view text) { return bookmarks_from_json(text); }, "bookmarks");
}

Result<void> save_bookmarks(const Path& file, const BookmarkStore& store) {
    return save_file(
        file, store, [](const BookmarkStore& s) { return bookmarks_to_json(s); }, "bookmarks");
}

Result<RecentsStore> load_recents(const Path& file, std::size_t capacity) {
    return load_file<RecentsStore>(
        file, [capacity](std::string_view text) { return recents_from_json(text, capacity); },
        "recents");
}

Result<void> save_recents(const Path& file, const RecentsStore& store) {
    return save_file(
        file, store, [](const RecentsStore& s) { return recents_to_json(s); }, "recents");
}

} // namespace navigator::places
