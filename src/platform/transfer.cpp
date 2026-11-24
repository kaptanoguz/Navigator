// <navigator/transfer.hpp>: file:// URIs, text/uri-list, the GNOME / Nautilus copied-files
// clipboard formats and the clipboard offer / read order. Pure code: no I/O, no GIO.
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/transfer.hpp>

#include "transfer_internal.hpp"

namespace navigator::transfer {

namespace ti = transfer_internal;

namespace {

// Bytes g_filename_to_uri() leaves unescaped in a path (GLib's UNSAFE_PATH table): RFC 3986
// unreserved plus the sub-delims, ':', '@' and '/'. Identical to src/preview/thumbnail.cpp's
// uri_safe() so both file_uri() functions agree byte for byte.
[[nodiscard]] bool uri_safe(unsigned char c) noexcept {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case '-':
    case '.':
    case '_':
    case '~':
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case '/':
    case ':':
    case '=':
    case '@': return true;
    default: return false;
    }
}

[[nodiscard]] Path normal_absolute(const Path& p) {
    std::string s = p.lexically_normal().native();
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return {s};
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

[[nodiscard]] char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_scheme_char(char c, bool first) noexcept {
    const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (first) {
        return alpha;
    }
    return alpha || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

[[nodiscard]] std::string_view trim_blanks(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Splits at '\n' (a preceding '\r' is dropped). The text after the last '\n' is a line too
// when it is not empty.
[[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        if (nl == std::string_view::npos) {
            break;
        }
        text.remove_prefix(nl + 1);
    }
    return lines;
}

[[nodiscard]] std::optional<ClipboardAction> parse_action(std::string_view line) noexcept {
    line = trim_blanks(line);
    if (line == "copy") {
        return ClipboardAction::Copy;
    }
    if (line == "cut") {
        return ClipboardAction::Cut;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<FileClipboard> clipboard_from_lines(ClipboardAction action,
                                                                std::string_view rest) {
    UriList list = decode_uri_list(rest);
    if (list.paths.empty()) {
        return std::nullopt;
    }
    return FileClipboard{action, std::move(list.paths)};
}

// Joins with '\n', no trailing newline.
[[nodiscard]] std::string join_lines(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out += parts[i];
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Internal seams
// ---------------------------------------------------------------------------------------------

} // namespace navigator::transfer

namespace navigator::transfer_internal {

using transfer::hex_value;
using transfer::iequals;
using transfer::is_scheme_char;
using transfer::normal_absolute;

bool is_valid_utf8(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80U) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0U) == 0xC0U) {
            len = 2;
            cp = c & 0x1FU;
        } else if ((c & 0xF0U) == 0xE0U) {
            len = 3;
            cp = c & 0x0FU;
        } else if ((c & 0xF8U) == 0xF0U) {
            len = 4;
            cp = c & 0x07U;
        } else {
            return false;
        }
        if (i + len > s.size()) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (cc & 0x3FU);
        }
        const bool overlong =
            (len == 2 && cp < 0x80U) || (len == 3 && cp < 0x800U) || (len == 4 && cp < 0x10000U);
        if (overlong || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
            return false;
        }
        i += len;
    }
    return true;
}

UriClass classify_uri(std::string_view uri, Path* out) {
    // Scheme.
    const std::size_t colon = uri.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return UriClass::Malformed;
    }
    for (std::size_t i = 0; i < colon; ++i) {
        if (!is_scheme_char(uri[i], i == 0)) {
            return UriClass::Malformed;
        }
    }
    for (const char c : uri) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20U || u == 0x7FU) {
            return UriClass::Malformed; // spaces and control bytes are never part of a URI
        }
    }
    const std::string_view scheme = uri.substr(0, colon);
    std::string_view rest = uri.substr(colon + 1);
    if (!iequals(scheme, "file")) {
        return rest.empty() ? UriClass::Malformed : UriClass::Remote;
    }
    if (rest.find('?') != std::string_view::npos || rest.find('#') != std::string_view::npos) {
        return UriClass::Malformed;
    }
    std::string_view path_part;
    if (rest.starts_with("//")) {
        rest.remove_prefix(2);
        const std::size_t slash = rest.find('/');
        const std::string_view host = rest.substr(0, slash);
        if (!host.empty() && !iequals(host, "localhost")) {
            return UriClass::Remote;
        }
        if (slash == std::string_view::npos) {
            return UriClass::Malformed; // "file://" or "file://localhost": no path
        }
        path_part = rest.substr(slash);
    } else if (rest.starts_with('/')) {
        path_part = rest; // RFC 8089 minimal form "file:/a" (no authority)
    } else {
        return UriClass::Malformed;
    }
    std::string decoded;
    decoded.reserve(path_part.size());
    for (std::size_t i = 0; i < path_part.size(); ++i) {
        const char c = path_part[i];
        if (c != '%') {
            decoded.push_back(c);
            continue;
        }
        if (i + 2 >= path_part.size()) {
            return UriClass::Malformed;
        }
        const int hi = hex_value(path_part[i + 1]);
        const int lo = hex_value(path_part[i + 2]);
        if (hi < 0 || lo < 0) {
            return UriClass::Malformed;
        }
        const auto byte = static_cast<char>((hi << 4) | lo);
        if (byte == '\0') {
            return UriClass::Malformed;
        }
        decoded.push_back(byte);
        i += 2;
    }
    if (decoded.empty() || decoded.front() != '/') {
        return UriClass::Malformed;
    }
    if (out != nullptr) {
        *out = normal_absolute(Path(decoded));
    }
    return UriClass::Local;
}

} // namespace navigator::transfer_internal

namespace navigator::transfer {

// ---------------------------------------------------------------------------------------------
// file:// URIs
// ---------------------------------------------------------------------------------------------

Result<std::string> file_uri(const Path& absolute_path) {
    if (absolute_path.empty() || !absolute_path.is_absolute()) {
        return fail(ErrorCode::InvalidArgument, "file_uri: empty or relative path");
    }
    try {
        const std::string s = normal_absolute(absolute_path).native();
        std::string out = "file://";
        out.reserve(out.size() + s.size() * 3U);
        constexpr char kHex[] = "0123456789ABCDEF";
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            if (uri_safe(c)) {
                out.push_back(ch);
            } else {
                out.push_back('%');
                out.push_back(kHex[c >> 4U]);
                out.push_back(kHex[c & 0x0FU]);
            }
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "file_uri: allocation failed");
    }
}

Result<Path> path_from_file_uri(std::string_view uri) {
    try {
        Path out;
        switch (ti::classify_uri(uri, &out)) {
        case ti::UriClass::Local: return out;
        case ti::UriClass::Remote:
            if (uri.size() >= 5 && iequals(uri.substr(0, 5), "file:")) {
                return fail(ErrorCode::Unsupported,
                            "path_from_file_uri: file URI on another host: " + std::string(uri));
            }
            return fail(ErrorCode::InvalidArgument,
                        "path_from_file_uri: not a file URI: " + std::string(uri));
        case ti::UriClass::Malformed: break;
        }
        return fail(ErrorCode::InvalidArgument,
                    "path_from_file_uri: malformed file URI: " + std::string(uri));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "path_from_file_uri: allocation failed");
    }
}

// ---------------------------------------------------------------------------------------------
// text/uri-list
// ---------------------------------------------------------------------------------------------

std::string encode_uri_list(const std::vector<std::string>& uris) {
    std::string out;
    for (const std::string& uri : uris) {
        out += uri;
        out += "\r\n";
    }
    return out;
}

Result<std::string> encode_uri_list(const std::vector<Path>& paths) {
    try {
        std::vector<std::string> uris;
        uris.reserve(paths.size());
        for (const Path& p : paths) {
            auto uri = file_uri(p);
            if (!uri) {
                return fail(uri.error().code, uri.error().message);
            }
            uris.push_back(std::move(*uri));
        }
        return encode_uri_list(uris);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "encode_uri_list: allocation failed");
    }
}

UriList decode_uri_list(std::string_view text) {
    UriList out;
    try {
        std::set<std::string> seen;
        for (std::string_view line : split_lines(text)) {
            line = trim_blanks(line);
            if (line.empty() || line.front() == '#') {
                continue;
            }
            Path path;
            switch (ti::classify_uri(line, &path)) {
            case ti::UriClass::Local:
                if (seen.insert(path.native()).second) {
                    out.paths.push_back(std::move(path));
                }
                break;
            case ti::UriClass::Remote: out.remote.emplace_back(line); break;
            case ti::UriClass::Malformed: ++out.malformed; break;
            }
        }
    } catch (const std::bad_alloc&) {
        ++out.malformed; // "never fails": keep what was decoded so far, count the rest as lost
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// File clipboard
// ---------------------------------------------------------------------------------------------

Result<std::string> encode_gnome_copied_files(const FileClipboard& clipboard) {
    if (clipboard.paths.empty()) {
        return fail(ErrorCode::InvalidArgument, "encode_gnome_copied_files: no paths");
    }
    try {
        std::string out = clipboard.action == ClipboardAction::Cut ? "cut" : "copy";
        for (const Path& p : clipboard.paths) {
            auto uri = file_uri(p);
            if (!uri) {
                return fail(uri.error().code, uri.error().message);
            }
            out.push_back('\n');
            out += *uri;
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "encode_gnome_copied_files: allocation failed");
    }
}

std::optional<FileClipboard> decode_gnome_copied_files(std::string_view text) {
    try {
        const std::size_t nl = text.find('\n');
        const auto action = parse_action(text.substr(0, nl));
        if (!action) {
            return std::nullopt;
        }
        return clipboard_from_lines(*action, nl == std::string_view::npos ? std::string_view{}
                                                                          : text.substr(nl + 1));
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

std::optional<FileClipboard> decode_nautilus_text(std::string_view text) {
    try {
        const std::size_t nl = text.find('\n');
        if (nl == std::string_view::npos ||
            trim_blanks(text.substr(0, nl)) != kNautilusTextHeader) {
            return std::nullopt;
        }
        return decode_gnome_copied_files(text.substr(nl + 1));
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

Result<std::vector<std::pair<std::string, std::string>>>
make_clipboard_offer(const FileClipboard& clipboard) {
    if (clipboard.paths.empty()) {
        return fail(ErrorCode::InvalidArgument, "make_clipboard_offer: no paths");
    }
    try {
        std::vector<std::string> uris;
        uris.reserve(clipboard.paths.size());
        bool plain_paths = true;
        std::vector<std::string> paths;
        paths.reserve(clipboard.paths.size());
        for (const Path& p : clipboard.paths) {
            auto uri = file_uri(p);
            if (!uri) {
                return fail(uri.error().code, uri.error().message);
            }
            uris.push_back(std::move(*uri));
            std::string native = normal_absolute(p).native();
            // A newline inside a name would split it into two "paths" in the text form.
            if (!ti::is_valid_utf8(native) || native.find('\n') != std::string::npos) {
                plain_paths = false;
            }
            paths.push_back(std::move(native));
        }
        const bool cut = clipboard.action == ClipboardAction::Cut;
        std::string gnome = cut ? "cut" : "copy";
        for (const std::string& uri : uris) {
            gnome.push_back('\n');
            gnome += uri;
        }
        const std::string text = plain_paths ? join_lines(paths) : join_lines(uris);

        std::vector<std::pair<std::string, std::string>> offer;
        offer.emplace_back(std::string(kMimeGnomeCopiedFiles), std::move(gnome));
        offer.emplace_back(std::string(kMimeUriList), encode_uri_list(uris));
        if (cut) {
            offer.emplace_back(std::string(kMimeKdeCutSelection), "1");
        }
        offer.emplace_back(std::string(kMimeTextUtf8), text);
        offer.emplace_back(std::string(kMimeText), text);
        offer.emplace_back(std::string(kMimeUtf8String), text);
        return offer;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "make_clipboard_offer: allocation failed");
    }
}

std::optional<FileClipboard> read_clipboard(const ClipboardGetter& get) {
    if (!get) {
        return std::nullopt;
    }
    try {
        if (auto gnome = get(kMimeGnomeCopiedFiles)) {
            if (auto clip = decode_gnome_copied_files(*gnome)) {
                return clip;
            }
        }
        if (auto list = get(kMimeUriList)) {
            UriList decoded = decode_uri_list(*list);
            if (!decoded.paths.empty()) {
                ClipboardAction action = ClipboardAction::Copy;
                if (auto marker = get(kMimeKdeCutSelection)) {
                    if (!marker->empty() && marker->front() == '1') {
                        action = ClipboardAction::Cut;
                    }
                }
                return FileClipboard{action, std::move(decoded.paths)};
            }
        }
        for (const std::string_view mime : {kMimeTextUtf8, kMimeText, kMimeUtf8String}) {
            if (auto text = get(mime)) {
                if (auto clip = decode_nautilus_text(*text)) {
                    return clip;
                }
            }
        }
    } catch (...) { // a throwing getter must not escape (no exceptions cross the API)
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace navigator::transfer
