// src/core/json_util.hpp (internal to navigator_core)
//
// Purpose: the lenient JSON reading and deterministic writing helpers shared by the session /
//          layout documents (layout_json.cpp) and bookmarks.json / recents.json
//          (places_json.cpp): the <bytes> encoding of raw paths, typed member access that never
//          throws on a wrong type, and the "must be a JSON object" parse.
//          nlohmann_json stays private to navigator_core (never in a public header).
// Thread-safety: pure functions, no state.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <navigator/types.hpp>

namespace navigator::core::json {

using OutJson = nlohmann::ordered_json; // writing: keys in insertion (documented) order
using InJson = nlohmann::json;          // reading

// <bytes>: a JSON string when `raw` is valid UTF-8, else { "hex": "<lowercase hex>" }.
[[nodiscard]] OutJson bytes_to_json(std::string_view raw);

// 2-space indent, strict UTF-8, trailing newline.
[[nodiscard]] std::string dump(const OutJson& doc);

// Member `key` of an object, nullptr when absent or `object` is not an object.
[[nodiscard]] const InJson* member(const InJson& object, const char* key);

// Integer value saturated to the int64 range; floats (even 1.0) are not integers.
[[nodiscard]] std::optional<std::int64_t> as_integer(const InJson* value);

[[nodiscard]] bool as_bool(const InJson* value, bool fallback);

// <bytes> in either form; nullopt for anything else (or bad hex).
[[nodiscard]] std::optional<std::string> as_bytes(const InJson* value);

// A UTF-8 JSON string value, nullopt for anything else.
[[nodiscard]] std::optional<std::string> as_string(const InJson* value);

// Errors: InvalidArgument (not JSON, or the top level is not an object).
[[nodiscard]] Result<InJson> parse_object(std::string_view text);

} // namespace navigator::core::json
