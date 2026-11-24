// src/core/view_settings_json.cpp — JSON (de)serialization and atomic persistence for
// view_settings.json (navigator/view_settings.hpp).
#include <new>
#include <string>
#include <string_view>

#include <navigator/view_settings.hpp>

#include "document_files.hpp"
#include "json_util.hpp"

namespace navigator::view {
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

std::string_view sort_key_to_string(fs::SortKey key) noexcept {
    switch (key) {
    case fs::SortKey::Name: return "name";
    case fs::SortKey::Size: return "size";
    case fs::SortKey::Modified: return "modified";
    case fs::SortKey::Extension: return "extension";
    case fs::SortKey::Count: return "count";
    }
    return "name";
}

fs::SortKey parse_sort_key(std::string_view name) noexcept {
    if (name == "size") {
        return fs::SortKey::Size;
    }
    if (name == "modified") {
        return fs::SortKey::Modified;
    }
    if (name == "extension") {
        return fs::SortKey::Extension;
    }
    if (name == "count") {
        return fs::SortKey::Count;
    }
    return fs::SortKey::Name;
}

OutJson settings_to_json(const ViewSettings& raw) {
    const ViewSettings s = normalize(raw);
    OutJson j = OutJson::object();
    j["mode"] = std::string(to_string(s.mode));
    j["zoom"] = s.zoom_percent;
    j["group_by"] = std::string(to_string(s.group_by));
    j["show_hidden"] = s.show_hidden;

    OutJson cols = OutJson::array();
    for (ColumnId col : s.columns) {
        cols.push_back(std::string(to_string(col)));
    }
    j["columns"] = std::move(cols);

    OutJson sort = OutJson::object();
    sort["key"] = std::string(sort_key_to_string(s.sort.key));
    sort["descending"] = s.sort.descending;
    sort["folders_first"] = s.sort.folders_first;
    sort["natural"] = s.sort.natural;
    sort["case_insensitive"] = s.sort.case_insensitive;
    j["sort"] = std::move(sort);
    return j;
}

ViewSettings settings_from_json(const InJson& obj) {
    ViewSettings s;
    if (!obj.is_object()) {
        return s;
    }
    if (const auto m = as_string(member(obj, "mode"))) {
        if (const auto parsed = parse_view_mode(*m)) {
            s.mode = *parsed;
        }
    }
    if (const auto z = as_integer(member(obj, "zoom"))) {
        if (*z < kMinZoomPercent) {
            s.zoom_percent = kMinZoomPercent;
        } else if (*z > kMaxZoomPercent) {
            s.zoom_percent = kMaxZoomPercent;
        } else {
            s.zoom_percent = static_cast<int>(*z);
        }
    }
    if (const auto g = as_string(member(obj, "group_by"))) {
        if (const auto parsed = parse_group_by(*g)) {
            s.group_by = *parsed;
        }
    }
    s.show_hidden = as_bool(member(obj, "show_hidden"), false);

    if (const InJson* cols = member(obj, "columns"); cols != nullptr && cols->is_array()) {
        ColumnOrder order = kDefaultColumnOrder;
        std::size_t idx = 0;
        for (const InJson& item : *cols) {
            if (idx >= kColumnCount) {
                break;
            }
            if (const auto name = as_string(&item)) {
                if (const auto col = parse_column_id(*name)) {
                    order[idx++] = *col;
                }
            }
        }
        if (idx > 0) {
            s.columns = normalize_columns(order);
        }
    }

    if (const InJson* sort = member(obj, "sort"); sort != nullptr && sort->is_object()) {
        if (const auto k = as_string(member(*sort, "key"))) {
            s.sort.key = parse_sort_key(*k);
        }
        s.sort.descending = as_bool(member(*sort, "descending"), false);
        s.sort.folders_first = as_bool(member(*sort, "folders_first"), true);
        s.sort.natural = as_bool(member(*sort, "natural"), true);
        s.sort.case_insensitive = as_bool(member(*sort, "case_insensitive"), true);
    }
    return normalize(s);
}

} // namespace

Result<std::string> to_json(const ViewSettingsStore& store) {
    try {
        OutJson doc = OutJson::object();
        doc["version"] = ViewSettingsStore::kVersion;
        doc["global"] = settings_to_json(store.global());

        OutJson rules = OutJson::array();
        for (const ViewRule& r : store.rules()) {
            const std::string& raw = r.path.native();
            if (r.scope == ViewScope::Global || raw.empty() || raw[0] != '/') {
                continue;
            }
            OutJson entry = OutJson::object();
            entry["path"] = bytes_to_json(raw);
            entry["scope"] = std::string(to_string(r.scope));
            entry["settings"] = settings_to_json(r.settings);
            rules.push_back(std::move(entry));
        }
        doc["rules"] = std::move(rules);
        return dump(doc);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "view_settings to_json: out of memory");
    }
}

Result<ViewSettingsStore> from_json(std::string_view text) {
    try {
        auto parsed = parse_object(text);
        if (!parsed) {
            return Unexpected<Error>(parsed.error());
        }
        const InJson& doc = *parsed;
        ViewSettingsStore store;
        if (const InJson* g = member(doc, "global"); g != nullptr && g->is_object()) {
            store.set_global(settings_from_json(*g));
        }
        if (const InJson* rules = member(doc, "rules"); rules != nullptr && rules->is_array()) {
            for (const InJson& item : *rules) {
                if (!item.is_object()) {
                    continue;
                }
                const auto path_bytes = as_bytes(member(item, "path"));
                const auto scope_str = as_string(member(item, "scope"));
                if (!path_bytes || path_bytes->empty() || (*path_bytes)[0] != '/' || !scope_str) {
                    continue;
                }
                const auto scope = parse_view_scope(*scope_str);
                if (!scope || *scope == ViewScope::Global) {
                    continue;
                }
                const InJson* settings_obj = member(item, "settings");
                const ViewSettings settings =
                    settings_obj != nullptr ? settings_from_json(*settings_obj) : ViewSettings{};
                if (*scope == ViewScope::Folder) {
                    store.set_folder(Path{*path_bytes}, settings);
                } else {
                    store.set_hierarchy(Path{*path_bytes}, settings);
                }
            }
        }
        return store;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "view_settings from_json: out of memory");
    }
}

Result<ViewSettingsStore> load_view_settings(const Path& file) {
    auto raw = core::read_document(file);
    if (!raw) {
        return Unexpected<Error>(raw.error());
    }
    auto parsed = from_json(*raw);
    if (!parsed) {
        return Unexpected<Error>(core::move_aside_bad(file, parsed.error().message));
    }
    return parsed;
}

Result<void> save_view_settings(const Path& file, const ViewSettingsStore& store) {
    auto json = to_json(store);
    if (!json) {
        return Unexpected<Error>(json.error());
    }
    return core::write_atomically(file, *json);
}

} // namespace navigator::view
