// <navigator/trash.hpp> pure helpers: DeletionDate, GIO-compatible Path escaping, .trashinfo
// text, topdir trash names and Path= values. No I/O.
#include <array>
#include <cstdio>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include <navigator/trash.hpp>

#include "trash_internal.hpp"

namespace navigator::trash {

namespace ti = trash_internal;

namespace {

[[nodiscard]] bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Exactly `n` digits at text[pos..], as an int; -1 when not.
[[nodiscard]] int fixed_number(std::string_view text, std::size_t pos, std::size_t n) noexcept {
    if (pos + n > text.size()) {
        return -1;
    }
    int value = 0;
    for (std::size_t i = pos; i < pos + n; ++i) {
        if (!is_digit(text[i])) {
            return -1;
        }
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

[[nodiscard]] int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] std::string_view trim_left(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    return s;
}

} // namespace

} // namespace navigator::trash

// ---------------------------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------------------------

namespace navigator::trash_internal {

Path normal_path(const Path& p) {
    std::string s = p.lexically_normal().native();
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return {s};
}

bool at_or_below(const Path& path, const Path& base) {
    const std::string& p = path.native();
    const std::string& b = base.native();
    if (b.empty() || p.size() < b.size() || p.compare(0, b.size(), b) != 0) {
        return false;
    }
    return p.size() == b.size() || b.back() == '/' || p[b.size()] == '/';
}

std::string_view cut_utf8(std::string_view s, std::size_t max_bytes) noexcept {
    if (s.size() <= max_bytes) {
        return s;
    }
    // s[max_bytes] is the first byte left out. When it continues a sequence, step back to that
    // sequence's lead byte (at most 3 steps; stray continuation bytes are cut byte-wise).
    std::size_t cut = max_bytes;
    std::size_t steps = 0;
    while (cut > 0 && steps < 3 && (static_cast<unsigned char>(s[cut]) & 0xC0U) == 0x80U) {
        --cut;
        ++steps;
    }
    if ((static_cast<unsigned char>(s[cut]) & 0xC0U) != 0xC0U) {
        cut = max_bytes; // no lead byte found: invalid UTF-8, cut byte-wise
    }
    return s.substr(0, cut);
}

std::string candidate_name(std::string_view base, unsigned n) {
    constexpr std::size_t kNameMax = 255;
    constexpr std::size_t kInfoSuffix = 10; // ".trashinfo"
    std::string suffix;
    if (n > 1) {
        suffix = "." + std::to_string(n);
    }
    const std::size_t budget = kNameMax - kInfoSuffix - suffix.size();
    std::string out(cut_utf8(base, budget));
    out += suffix;
    return out;
}

} // namespace navigator::trash_internal

namespace navigator::trash {

// ---------------------------------------------------------------------------------------------
// DeletionDate
// ---------------------------------------------------------------------------------------------

std::string format_deletion_date(const LocalDateTime& when) {
    std::array<char, 64> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02d", when.year,
                                when.month, when.day, when.hour, when.minute, when.second);
    if (n < 0) {
        return {};
    }
    return {buf.data(), static_cast<std::size_t>(n) < buf.size() ? static_cast<std::size_t>(n)
                                                                 : buf.size() - 1};
}

std::optional<LocalDateTime> parse_deletion_date(std::string_view text) {
    // YYYY-MM-DDThh:mm:ss
    if (text.size() < 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    LocalDateTime t;
    t.year = fixed_number(text, 0, 4);
    t.month = fixed_number(text, 5, 2);
    t.day = fixed_number(text, 8, 2);
    t.hour = fixed_number(text, 11, 2);
    t.minute = fixed_number(text, 14, 2);
    t.second = fixed_number(text, 17, 2);
    if (t.year < 0 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.hour < 0 ||
        t.hour > 23 || t.minute < 0 || t.minute > 59 || t.second < 0 || t.second > 60) {
        return std::nullopt;
    }
    std::string_view rest = text.substr(19);
    if (!rest.empty() && (rest.front() == '.' || rest.front() == ',')) {
        rest.remove_prefix(1);
        std::size_t digits = 0;
        while (digits < rest.size() && is_digit(rest[digits])) {
            ++digits;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        rest.remove_prefix(digits);
    }
    if (rest.empty()) {
        return t;
    }
    if (rest == "Z" || rest == "z") {
        return t;
    }
    if (rest.front() == '+' || rest.front() == '-') {
        rest.remove_prefix(1);
        // hh, hhmm or hh:mm
        const int hh = fixed_number(rest, 0, 2);
        if (hh < 0 || hh > 23) {
            return std::nullopt;
        }
        rest.remove_prefix(2);
        if (rest.empty()) {
            return t;
        }
        if (rest.front() == ':') {
            rest.remove_prefix(1);
        }
        const int mm = fixed_number(rest, 0, 2);
        if (mm < 0 || mm > 59 || rest.size() != 2) {
            return std::nullopt;
        }
        return t;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Path escaping
// ---------------------------------------------------------------------------------------------

std::string escape_path(std::string_view raw) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size());
    for (const char ch : raw) {
        const auto c = static_cast<unsigned char>(ch);
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' ||
                          c == '/';
        if (keep) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4U]);
            out.push_back(kHex[c & 0x0FU]);
        }
    }
    return out;
}

std::optional<std::string> unescape_path(std::string_view escaped) {
    std::string out;
    out.reserve(escaped.size());
    for (std::size_t i = 0; i < escaped.size(); ++i) {
        const char c = escaped[i];
        if (c == '\0') {
            return std::nullopt; // a raw NUL cannot be part of a path either
        }
        if (c != '%') {
            out.push_back(c);
            continue;
        }
        if (i + 2 >= escaped.size()) {
            return std::nullopt;
        }
        const int hi = hex_value(escaped[i + 1]);
        const int lo = hex_value(escaped[i + 2]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        const auto byte = static_cast<char>((hi << 4) | lo);
        if (byte == '\0') {
            return std::nullopt;
        }
        out.push_back(byte);
        i += 2;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// .trashinfo
// ---------------------------------------------------------------------------------------------

std::string format_trashinfo(const TrashInfo& info) {
    std::string out = "[Trash Info]\nPath=";
    out += escape_path(info.path);
    out += '\n';
    if (info.deletion_date) {
        out += "DeletionDate=";
        out += format_deletion_date(*info.deletion_date);
        out += '\n';
    }
    return out;
}

Result<TrashInfo> parse_trashinfo(std::string_view text) {
    try {
        bool in_group = false;
        bool seen_group = false;
        std::optional<std::string_view> path_value;
        std::optional<std::string_view> date_value;
        while (!text.empty()) {
            const std::size_t nl = text.find('\n');
            std::string_view line = text.substr(0, nl);
            text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            const std::string_view t = trim(line);
            if (t.empty() || t.front() == '#') {
                continue;
            }
            if (t.front() == '[') {
                if (in_group) {
                    break; // only the first [Trash Info] group counts
                }
                if (t == "[Trash Info]") {
                    in_group = true;
                    seen_group = true;
                }
                continue;
            }
            if (!in_group) {
                continue;
            }
            const std::size_t eq = line.find('=');
            if (eq == std::string_view::npos) {
                continue;
            }
            const std::string_view key = trim(line.substr(0, eq));
            const std::string_view value = trim_left(line.substr(eq + 1));
            if (key == "Path" && !path_value) {
                path_value = value;
            } else if (key == "DeletionDate" && !date_value) {
                date_value = value;
            }
        }
        if (!seen_group) {
            return fail(ErrorCode::InvalidArgument, "parse_trashinfo: no [Trash Info] group");
        }
        if (!path_value || path_value->empty()) {
            return fail(ErrorCode::InvalidArgument, "parse_trashinfo: Path missing or empty");
        }
        auto decoded = unescape_path(*path_value);
        if (!decoded || decoded->empty()) {
            return fail(ErrorCode::InvalidArgument, "parse_trashinfo: Path is not unescapable");
        }
        TrashInfo info;
        info.path = std::move(*decoded);
        if (date_value) {
            info.deletion_date = parse_deletion_date(*date_value);
        }
        return info;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "parse_trashinfo: allocation failed");
    }
}

// ---------------------------------------------------------------------------------------------
// Topdir trash names and Path= values
// ---------------------------------------------------------------------------------------------

Path shared_topdir_trash(const Path& topdir, std::uint32_t uid) {
    return topdir / ".Trash" / std::to_string(uid);
}

Path user_topdir_trash(const Path& topdir, std::uint32_t uid) {
    return topdir / (".Trash-" + std::to_string(uid));
}

Result<std::string> info_path_for(const Path& original, const Path& topdir) {
    try {
        if (original.empty() || !original.is_absolute()) {
            return fail(ErrorCode::InvalidArgument, "info_path_for: original is not absolute");
        }
        const Path orig = ti::normal_path(original);
        if (topdir.empty()) {
            return orig.native();
        }
        if (!topdir.is_absolute()) {
            return fail(ErrorCode::InvalidArgument, "info_path_for: topdir is not absolute");
        }
        const Path top = ti::normal_path(topdir);
        if (orig == top || !ti::at_or_below(orig, top)) {
            return fail(ErrorCode::InvalidArgument, "info_path_for: '" + orig.native() +
                                                        "' is not below '" + top.native() + "'");
        }
        std::string rel = orig.native().substr(top.native().size());
        while (!rel.empty() && rel.front() == '/') {
            rel.erase(0, 1);
        }
        return rel;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "info_path_for: allocation failed");
    }
}

Result<Path> original_path_from(std::string_view info_path, const Path& topdir) {
    try {
        if (info_path.empty()) {
            return fail(ErrorCode::InvalidArgument, "original_path_from: empty Path");
        }
        if (info_path.find('\0') != std::string_view::npos) {
            return fail(ErrorCode::InvalidArgument, "original_path_from: NUL in Path");
        }
        if (info_path.front() == '/') {
            return ti::normal_path(Path(std::string(info_path)));
        }
        if (topdir.empty() || !topdir.is_absolute()) {
            return fail(ErrorCode::InvalidArgument,
                        "original_path_from: relative Path in the home trash");
        }
        const Path top = ti::normal_path(topdir);
        Path joined = ti::normal_path(top / std::string(info_path));
        if (joined == top || !ti::at_or_below(joined, top)) {
            return fail(ErrorCode::InvalidArgument,
                        "original_path_from: Path escapes its topdir: " + std::string(info_path));
        }
        return joined;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "original_path_from: allocation failed");
    }
}

} // namespace navigator::trash
