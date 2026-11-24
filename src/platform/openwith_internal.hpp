// Internal (src/platform): helpers shared by openwith_desktop.cpp (Desktop Entry, Exec) and
// openwith_assoc.cpp (MIME Applications, shared-mime-info, set_default_handler). No GIO.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/types.hpp>

namespace navigator::openwith_internal {

// Space, tab, CR removed from both ends.
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;

// ASCII lowercase copy / case-insensitive equality.
[[nodiscard]] std::string ascii_lower(std::string_view text);
[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;

// Strict UTF-8 (no overlongs, surrogates or code points above U+10FFFF).
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// Calls f(line) for every '\n' separated line (a trailing '\r' removed; a leading UTF-8 BOM
// skipped). f returns false to stop.
template <class F> void for_each_line(std::string_view text, F&& f) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!f(line)) {
            return;
        }
        if (nl == std::string_view::npos) {
            break;
        }
        text.remove_prefix(nl + 1);
    }
}

// "[Group]" header: the group name, else nullopt.
[[nodiscard]] std::optional<std::string_view> group_header(std::string_view line) noexcept;

// "key=value" with both sides trimmed; nullopt for blanks, comments, headers and lines without '='.
struct KeyValue {
    std::string_view key;
    std::string_view value;
};
[[nodiscard]] std::optional<KeyValue> key_value(std::string_view line) noexcept;

// Regular file read, at most `max_bytes` bytes (a longer file is cut; *truncated says so). Never
// blocks on FIFOs or devices (non-regular files read as missing). nullopt: cannot open / read.
[[nodiscard]] std::optional<std::string> read_text_file(const Path& path, std::size_t max_bytes,
                                                        bool* truncated = nullptr);

// "type/subtype": exactly one '/', both parts non-empty, no whitespace or control characters, at
// most 255 bytes.
[[nodiscard]] bool valid_mime(std::string_view mime) noexcept;

// Splits at unescaped ';' (plain; no escapes processed), dropping empty elements.
[[nodiscard]] std::vector<std::string> split_semicolons(std::string_view value);

// Which file field codes an Exec value contains (%% is skipped).
struct ExecCodes {
    bool f = false;
    bool F = false;
    bool u = false;
    bool U = false;
};
[[nodiscard]] ExecCodes scan_exec_codes(std::string_view exec) noexcept;

} // namespace navigator::openwith_internal
