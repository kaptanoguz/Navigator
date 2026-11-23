// Internal (src/platform): building blocks of <navigator/transfer.hpp>, exposed to tests.
#pragma once

#include <string_view>

#include <navigator/types.hpp>

namespace navigator::transfer_internal {

// Strict UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
[[nodiscard]] bool is_valid_utf8(std::string_view s) noexcept;

enum class UriClass : unsigned char {
    Local = 0,     // file URI with an empty authority or "localhost" (or "file:/path")
    Remote = 1,    // well-formed URI of another scheme, or a file URI on another host
    Malformed = 2, // not a URI, bad escape, %00, query/fragment on a file URI, no path
};

// Classifies one URI; for Local, `*out` (when not null) receives the decoded, lexically
// normal absolute path.
[[nodiscard]] UriClass classify_uri(std::string_view uri, Path* out);

} // namespace navigator::transfer_internal
