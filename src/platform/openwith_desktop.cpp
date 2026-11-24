// Phase 8 of <navigator/openwith.hpp>, part 1: shared text helpers, Desktop Entry parsing, Exec
// tokenizing and expansion, terminal prefixes, launching and system_options(). No GIO.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/openwith.hpp>
#include <navigator/platform.hpp>
#include <navigator/transfer.hpp>

#include "openwith_internal.hpp"

namespace navigator::openwith_internal {

std::string_view trim(std::string_view text) noexcept {
    constexpr std::string_view kBlank = " \t\r";
    const std::size_t first = text.find_first_not_of(kBlank);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kBlank);
    return text.substr(first, last - first + 1);
}

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') {
            x = static_cast<char>(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = static_cast<char>(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    const std::size_t n = text.size();
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    while (i < n) {
        const unsigned char c = byte(i);
        if (c < 0x80U) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        char32_t cp = 0;
        char32_t min = 0;
        if (c >= 0xC2U && c <= 0xDFU) {
            len = 2;
            cp = c & 0x1FU;
            min = 0x80;
        } else if (c >= 0xE0U && c <= 0xEFU) {
            len = 3;
            cp = c & 0x0FU;
            min = 0x800;
        } else if (c >= 0xF0U && c <= 0xF4U) {
            len = 4;
            cp = c & 0x07U;
            min = 0x10000;
        } else {
            return false;
        }
        if (i + len > n) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = byte(i + k);
            if ((cc & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (cc & 0x3FU);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

std::optional<std::string_view> group_header(std::string_view line) noexcept {
    line = trim(line);
    if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
        return line.substr(1, line.size() - 2);
    }
    return std::nullopt;
}

std::optional<KeyValue> key_value(std::string_view line) noexcept {
    line = trim(line);
    if (line.empty() || line.front() == '#' || line.front() == '[') {
        return std::nullopt;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos || eq == 0) {
        return std::nullopt;
    }
    return KeyValue{trim(line.substr(0, eq)), trim(line.substr(eq + 1))};
}

std::optional<std::string> read_text_file(const Path& path, std::size_t max_bytes,
                                          bool* truncated) {
    if (truncated != nullptr) {
        *truncated = false;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        return std::nullopt;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return std::nullopt;
    }
    try {
        std::string out;
        const auto expected = static_cast<std::size_t>(std::max<off_t>(st.st_size, 0));
        out.reserve(std::min(expected, max_bytes) + 1);
        std::array<char, 65536> buffer{};
        while (out.size() < max_bytes) {
            const std::size_t want = std::min(buffer.size(), max_bytes - out.size());
            const ssize_t got = ::read(fd, buffer.data(), want);
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ::close(fd);
                return std::nullopt;
            }
            if (got == 0) {
                ::close(fd);
                return out;
            }
            out.append(buffer.data(), static_cast<std::size_t>(got));
        }
        // Exactly max_bytes read: is there more?
        char extra = 0;
        const ssize_t more = ::read(fd, &extra, 1);
        ::close(fd);
        if (more > 0 && truncated != nullptr) {
            *truncated = true;
        }
        return out;
    } catch (const std::bad_alloc&) {
        ::close(fd);
        return std::nullopt;
    }
}

bool valid_mime(std::string_view mime) noexcept {
    if (mime.empty() || mime.size() > 255) {
        return false;
    }
    const std::size_t slash = mime.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == mime.size() ||
        mime.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    return std::none_of(mime.begin(), mime.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u <= 0x20U || u == 0x7FU;
    });
}

std::vector<std::string> split_semicolons(std::string_view value) {
    std::vector<std::string> out;
    while (!value.empty()) {
        const std::size_t semi = value.find(';');
        const std::string_view part = trim(value.substr(0, semi));
        if (!part.empty()) {
            out.emplace_back(part);
        }
        if (semi == std::string_view::npos) {
            break;
        }
        value.remove_prefix(semi + 1);
    }
    return out;
}

ExecCodes scan_exec_codes(std::string_view exec) noexcept {
    ExecCodes codes;
    for (std::size_t i = 0; i + 1 < exec.size(); ++i) {
        if (exec[i] != '%') {
            continue;
        }
        switch (exec[i + 1]) {
        case 'f': codes.f = true; break;
        case 'F': codes.F = true; break;
        case 'u': codes.u = true; break;
        case 'U': codes.U = true; break;
        default: break;
        }
        ++i; // also skips "%%"
    }
    return codes;
}

} // namespace navigator::openwith_internal

namespace navigator::openwith {

namespace {

using namespace openwith_internal;

// One escape of a Desktop Entry string value: \s \n \t \r \\ and \; (unknown ones kept verbatim).
void append_escape(std::string& out, char next) {
    switch (next) {
    case 's': out.push_back(' '); break;
    case 'n': out.push_back('\n'); break;
    case 't': out.push_back('\t'); break;
    case 'r': out.push_back('\r'); break;
    case '\\': out.push_back('\\'); break;
    case ';': out.push_back(';'); break;
    default:
        out.push_back('\\');
        out.push_back(next);
        break;
    }
}

std::string unescape_string(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            append_escape(out, raw[++i]);
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

// List value: elements separated by unescaped ';', escapes resolved per element, empty dropped.
std::vector<std::string> unescape_list(std::string_view raw) {
    std::vector<std::string> out;
    std::string current;
    const auto flush = [&] {
        if (!current.empty()) {
            out.push_back(std::move(current));
        }
        current.clear();
    };
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            append_escape(current, raw[++i]);
        } else if (raw[i] == ';') {
            flush();
        } else {
            current.push_back(raw[i]);
        }
    }
    flush();
    return out;
}

// "tr_TR.UTF-8" -> {"tr_TR", "tr"}; "sr_RS@latin" -> {"sr_RS@latin", "sr_RS", "sr@latin", "sr"}.
std::vector<std::string> locale_candidates(std::string_view locale) {
    std::vector<std::string> out;
    locale = trim(locale);
    std::string modifier;
    if (const std::size_t at = locale.find('@'); at != std::string_view::npos) {
        modifier = std::string(locale.substr(at + 1));
        locale = locale.substr(0, at);
    }
    if (const std::size_t dot = locale.find('.'); dot != std::string_view::npos) {
        locale = locale.substr(0, dot);
    }
    std::string country;
    if (const std::size_t us = locale.find('_'); us != std::string_view::npos) {
        country = std::string(locale.substr(us + 1));
        locale = locale.substr(0, us);
    }
    if (locale.empty() || locale == "C" || locale == "POSIX") {
        return out;
    }
    const std::string lang(locale);
    if (!country.empty() && !modifier.empty()) {
        out.push_back(lang + "_" + country + "@" + modifier);
    }
    if (!country.empty()) {
        out.push_back(lang + "_" + country);
    }
    if (!modifier.empty()) {
        out.push_back(lang + "@" + modifier);
    }
    out.push_back(lang);
    return out;
}

using KeyMap = std::unordered_map<std::string, std::string>;

const std::string* find_key(const KeyMap& keys, const std::string& key) {
    const auto it = keys.find(key);
    return it == keys.end() ? nullptr : &it->second;
}

std::string localized_value(const KeyMap& keys, const std::string& base,
                            const std::vector<std::string>& candidates) {
    for (const std::string& candidate : candidates) {
        if (const std::string* value = find_key(keys, base + "[" + candidate + "]")) {
            return unescape_string(*value);
        }
    }
    if (const std::string* value = find_key(keys, base)) {
        return unescape_string(*value);
    }
    return {};
}

bool is_true(const KeyMap& keys, const char* key) {
    const std::string* value = find_key(keys, key);
    return value != nullptr && *value == "true";
}

std::string value_or_empty(const KeyMap& keys, const char* key) {
    const std::string* value = find_key(keys, key);
    return value == nullptr ? std::string() : unescape_string(*value);
}

std::vector<std::string> list_value(const KeyMap& keys, const char* key) {
    const std::string* value = find_key(keys, key);
    return value == nullptr ? std::vector<std::string>() : unescape_list(*value);
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

bool contains_nul(const Path& path) { return path.native().find('\0') != std::string::npos; }

} // namespace

// ---------------------------------------------------------------------------------------------
// Desktop Entry
// ---------------------------------------------------------------------------------------------

Result<Handler> parse_desktop_entry(std::string_view text, std::string_view desktop_id,
                                    const Path& desktop_file, std::string_view locale) {
    if (text.size() > kMaxDesktopFileBytes) {
        return fail(ErrorCode::InvalidArgument, "desktop entry: file too large");
    }
    try {
        KeyMap keys;
        bool seen = false;
        bool in_entry = false;
        for_each_line(text, [&](std::string_view line) {
            if (const auto header = group_header(line)) {
                if (in_entry) {
                    return false; // only the first [Desktop Entry] group is read
                }
                if (*header == "Desktop Entry" && !seen) {
                    in_entry = true;
                    seen = true;
                }
                return true;
            }
            if (!in_entry) {
                return true;
            }
            if (const auto kv = key_value(line)) {
                keys.emplace(std::string(kv->key), std::string(kv->value)); // first wins
            }
            return true;
        });
        if (!seen) {
            return fail(ErrorCode::InvalidArgument, "desktop entry: no [Desktop Entry] group");
        }
        const std::string* type = find_key(keys, "Type");
        if (type == nullptr || *type != "Application") {
            return fail(ErrorCode::InvalidArgument, "desktop entry: Type is not Application");
        }
        if (is_true(keys, "Hidden")) {
            return fail(ErrorCode::InvalidArgument, "desktop entry: Hidden=true");
        }
        Handler h;
        h.exec = value_or_empty(keys, "Exec");
        if (trim(h.exec).empty()) {
            return fail(ErrorCode::InvalidArgument, "desktop entry: Exec missing or empty");
        }
        if (!is_valid_utf8(h.exec)) {
            return fail(ErrorCode::InvalidArgument, "desktop entry: invalid UTF-8 in Exec");
        }
        const std::vector<std::string> candidates = locale_candidates(locale);
        h.name = localized_value(keys, "Name", candidates);
        if (!is_valid_utf8(h.name) || !is_valid_utf8(value_or_empty(keys, "Name"))) {
            return fail(ErrorCode::InvalidArgument, "desktop entry: invalid UTF-8 in Name");
        }
        if (h.name.empty()) {
            h.name = desktop_id.ends_with(".desktop")
                         ? std::string(desktop_id.substr(0, desktop_id.size() - 8))
                         : std::string(desktop_id);
        }
        h.generic_name = localized_value(keys, "GenericName", candidates);
        h.comment = localized_value(keys, "Comment", candidates);
        if (!is_valid_utf8(h.generic_name)) {
            h.generic_name.clear();
        }
        if (!is_valid_utf8(h.comment)) {
            h.comment.clear();
        }
        h.desktop_id = std::string(desktop_id);
        h.desktop_file = desktop_file;
        h.icon_name = value_or_empty(keys, "Icon");
        h.try_exec = value_or_empty(keys, "TryExec");
        h.working_dir = Path(value_or_empty(keys, "Path"));
        h.mime_types = list_value(keys, "MimeType");
        h.terminal = is_true(keys, "Terminal");
        h.no_display = is_true(keys, "NoDisplay");
        h.only_show_in = list_value(keys, "OnlyShowIn");
        h.not_show_in = list_value(keys, "NotShowIn");
        const ExecCodes codes = scan_exec_codes(h.exec);
        h.accepts_multiple = codes.F || codes.U;
        h.accepts_uris = codes.u || codes.U;
        return h;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "desktop entry: out of memory");
    }
}

bool shown_in(const Handler& handler, Span<const std::string> current_desktops) noexcept {
    const auto listed = [&](const std::vector<std::string>& names) {
        return std::any_of(
            current_desktops.begin(), current_desktops.end(), [&](const std::string& desktop) {
                return std::any_of(names.begin(), names.end(),
                                   [&](const std::string& name) { return iequals(name, desktop); });
            });
    };
    if (!handler.only_show_in.empty() && !listed(handler.only_show_in)) {
        return false;
    }
    return !listed(handler.not_show_in);
}

// ---------------------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<Path> absolute_entries(const std::string& list, std::string_view fallback) {
    std::vector<Path> out;
    const auto add = [&](std::string_view text) {
        while (!text.empty()) {
            const std::size_t colon = text.find(':');
            const std::string_view entry = text.substr(0, colon);
            if (!entry.empty() && entry.front() == '/') {
                out.emplace_back(std::string(entry));
            }
            if (colon == std::string_view::npos) {
                break;
            }
            text.remove_prefix(colon + 1);
        }
    };
    add(list);
    if (out.empty()) {
        add(fallback);
    }
    return out;
}

} // namespace

Options system_options() {
    Options options;
    try {
        std::string home = env_or_empty("HOME");
        if (home.empty() || home.front() != '/') {
            auto resolved = platform::user_dir(platform::UserDir::Home);
            home = resolved ? resolved->string() : std::string();
        }
        options.home = Path(home);
        const std::string config_home = env_or_empty("XDG_CONFIG_HOME");
        if (!config_home.empty() && config_home.front() == '/') {
            options.config_home = Path(config_home);
        } else if (!home.empty()) {
            options.config_home = options.home / ".config";
        }
        options.config_dirs = absolute_entries(env_or_empty("XDG_CONFIG_DIRS"), "/etc/xdg");
        const std::string data_home = env_or_empty("XDG_DATA_HOME");
        if (!data_home.empty() && data_home.front() == '/') {
            options.data_home = Path(data_home);
        } else if (!home.empty()) {
            options.data_home = options.home / ".local" / "share";
        }
        options.data_dirs =
            absolute_entries(env_or_empty("XDG_DATA_DIRS"), "/usr/local/share:/usr/share");
        std::string_view desktops = env_or_empty("XDG_CURRENT_DESKTOP");
        const std::string desktops_text(desktops);
        desktops = desktops_text;
        while (!desktops.empty()) {
            const std::size_t colon = desktops.find(':');
            const std::string_view entry = desktops.substr(0, colon);
            if (!entry.empty()) {
                options.current_desktops.emplace_back(entry);
            }
            if (colon == std::string_view::npos) {
                break;
            }
            desktops.remove_prefix(colon + 1);
        }
        for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
            std::string value = env_or_empty(name);
            if (!value.empty()) {
                options.locale = std::move(value);
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        // Defaults stay: an empty Options finds nothing, which is a safe degradation.
    }
    return options;
}

// ---------------------------------------------------------------------------------------------
// Exec
// ---------------------------------------------------------------------------------------------

Result<std::vector<std::string>> parse_exec_tokens(std::string_view exec) {
    try {
        std::vector<std::string> tokens;
        std::string current;
        bool has_token = false;
        bool quoted = false;
        for (std::size_t i = 0; i < exec.size(); ++i) {
            const char c = exec[i];
            if (quoted) {
                if (c == '"') {
                    quoted = false;
                } else if (c == '\\') {
                    if (i + 1 >= exec.size()) {
                        return fail(ErrorCode::InvalidArgument, "Exec: dangling backslash");
                    }
                    const char next = exec[++i];
                    if (next != '"' && next != '`' && next != '$' && next != '\\') {
                        return fail(ErrorCode::InvalidArgument, "Exec: invalid escape in quotes");
                    }
                    current.push_back(next);
                } else {
                    current.push_back(c);
                }
                continue;
            }
            if (c == '"') {
                quoted = true;
                has_token = true;
            } else if (c == ' ' || c == '\t') {
                if (has_token) {
                    tokens.push_back(std::move(current));
                    current.clear();
                    has_token = false;
                }
            } else {
                current.push_back(c);
                has_token = true;
            }
        }
        if (quoted) {
            return fail(ErrorCode::InvalidArgument, "Exec: unterminated quote");
        }
        if (has_token) {
            tokens.push_back(std::move(current));
        }
        if (tokens.empty()) {
            return fail(ErrorCode::InvalidArgument, "Exec: empty command");
        }
        return tokens;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "Exec: out of memory");
    }
}

namespace {

enum class Mode { None, Single, Multi };

bool is_known_code(char c) {
    switch (c) {
    case 'f':
    case 'F':
    case 'u':
    case 'U':
    case 'i':
    case 'c':
    case 'k':
    case '%':
    case 'd':
    case 'D':
    case 'n':
    case 'N':
    case 'v':
    case 'm': return true;
    default: return false;
    }
}

// Everything one invocation needs to substitute.
struct Substitution {
    const Handler* handler = nullptr;
    // Paths / URIs of this invocation (one for Single mode, a chunk for Multi mode).
    std::vector<std::string> paths;
    std::vector<std::string> uris;
};

// Expands the field codes embedded in one longer argument (single path / URL, first of the chunk).
// Returns false when the argument is to be dropped (only deprecated codes, nothing left).
bool expand_embedded(std::string_view token, const Substitution& sub, std::string& out) {
    bool deprecated = false;
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] != '%' || i + 1 >= token.size()) {
            out.push_back(token[i]);
            continue;
        }
        const char code = token[++i];
        switch (code) {
        case '%': out.push_back('%'); break;
        case 'f':
        case 'F':
            if (!sub.paths.empty()) {
                out += sub.paths.front();
            }
            break;
        case 'u':
        case 'U':
            if (!sub.uris.empty()) {
                out += sub.uris.front();
            }
            break;
        case 'c': out += sub.handler->name; break;
        case 'k': out += sub.handler->desktop_file.native(); break;
        case 'i': break; // %i is only meaningful as a whole argument
        default: deprecated = true; break;
        }
    }
    return !(deprecated && out.empty());
}

void expand_token(const std::string& token, const Substitution& sub,
                  std::vector<std::string>& argv) {
    if (token == "%f") {
        argv.push_back(sub.paths.front());
    } else if (token == "%F") {
        argv.insert(argv.end(), sub.paths.begin(), sub.paths.end());
    } else if (token == "%u") {
        argv.push_back(sub.uris.front());
    } else if (token == "%U") {
        argv.insert(argv.end(), sub.uris.begin(), sub.uris.end());
    } else if (token == "%i") {
        if (!sub.handler->icon_name.empty()) {
            argv.emplace_back("--icon");
            argv.push_back(sub.handler->icon_name);
        }
    } else if (token.find('%') == std::string::npos) {
        argv.push_back(token);
    } else {
        std::string out;
        if (expand_embedded(token, sub, out)) {
            argv.push_back(std::move(out));
        }
    }
}

} // namespace

Result<std::vector<std::vector<std::string>>>
expand_exec(const Handler& handler, Span<const Path> paths, const ExpandOptions& options) {
    if (paths.empty()) {
        return fail(ErrorCode::InvalidArgument, "expand_exec: no paths");
    }
    for (const Path& path : paths) {
        if (path.empty() || !path.is_absolute() || contains_nul(path)) {
            return fail(ErrorCode::InvalidArgument, "expand_exec: path must be absolute, no NUL");
        }
    }
    try {
        auto parsed = parse_exec_tokens(handler.exec);
        if (!parsed) {
            return Unexpected<Error>(std::move(parsed.error()));
        }
        const std::vector<std::string>& tokens = *parsed;

        Mode mode = Mode::None;
        bool need_paths = false;
        bool need_uris = false;
        bool multi_paths = false;
        bool multi_uris = false;
        for (const std::string& token : tokens) {
            for (std::size_t i = 0; i < token.size(); ++i) {
                if (token[i] != '%') {
                    continue;
                }
                if (i + 1 >= token.size() || !is_known_code(token[i + 1])) {
                    return fail(ErrorCode::InvalidArgument, "expand_exec: unknown field code");
                }
                const char code = token[++i];
                const bool whole = token.size() == 2;
                if (code == 'f' || code == 'F') {
                    need_paths = true;
                    if (code == 'F' && whole) {
                        multi_paths = true;
                    }
                } else if (code == 'u' || code == 'U') {
                    need_uris = true;
                    if (code == 'U' && whole) {
                        multi_uris = true;
                    }
                }
            }
        }
        if (multi_paths || multi_uris) {
            mode = Mode::Multi;
        } else if (need_paths || need_uris) {
            mode = Mode::Single;
        }

        std::vector<std::string> path_strings;
        std::vector<std::string> uri_strings;
        path_strings.reserve(paths.size());
        for (const Path& path : paths) {
            path_strings.push_back(path.native());
            if (need_uris) {
                auto uri = transfer::file_uri(path);
                if (!uri) {
                    return Unexpected<Error>(std::move(uri.error()));
                }
                uri_strings.push_back(std::move(*uri));
            }
        }

        // Chunks of path indices (begin, end).
        std::vector<std::pair<std::size_t, std::size_t>> chunks;
        if (mode == Mode::None) {
            chunks.emplace_back(0, 1);
        } else if (mode == Mode::Single) {
            for (std::size_t i = 0; i < paths.size(); ++i) {
                chunks.emplace_back(i, i + 1);
            }
        } else {
            std::size_t fixed = 0;
            for (const std::string& token : tokens) {
                fixed += token.size() + 1;
            }
            std::size_t begin = 0;
            std::size_t used = fixed;
            for (std::size_t i = 0; i < paths.size(); ++i) {
                std::size_t cost = 0;
                if (multi_paths) {
                    cost += path_strings[i].size() + 1;
                }
                if (multi_uris) {
                    cost += uri_strings[i].size() + 1;
                }
                if (i > begin && used + cost > kMaxArgvBytes) {
                    chunks.emplace_back(begin, i);
                    begin = i;
                    used = fixed;
                }
                used += cost;
            }
            chunks.emplace_back(begin, paths.size());
        }

        if (handler.terminal && options.terminal_prefix.empty()) {
            return fail(ErrorCode::Unsupported, "expand_exec: Terminal=true needs a terminal");
        }

        std::vector<std::vector<std::string>> result;
        result.reserve(chunks.size());
        for (const auto& [begin, end] : chunks) {
            Substitution sub;
            sub.handler = &handler;
            sub.paths.assign(path_strings.begin() + static_cast<std::ptrdiff_t>(begin),
                             path_strings.begin() + static_cast<std::ptrdiff_t>(end));
            if (need_uris) {
                sub.uris.assign(uri_strings.begin() + static_cast<std::ptrdiff_t>(begin),
                                uri_strings.begin() + static_cast<std::ptrdiff_t>(end));
            }
            std::vector<std::string> argv;
            for (const std::string& token : tokens) {
                expand_token(token, sub, argv);
            }
            if (argv.empty() || argv.front().empty()) {
                return fail(ErrorCode::InvalidArgument, "expand_exec: empty program");
            }
            if (handler.terminal) {
                argv.insert(argv.begin(), options.terminal_prefix.begin(),
                            options.terminal_prefix.end());
            }
            result.push_back(std::move(argv));
        }
        return result;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "expand_exec: out of memory");
    }
}

std::vector<std::string> default_terminal_prefix(const ProgramExists& exists) {
    struct Terminal {
        const char* program;
        const char* flag; // nullptr: none
    };
    static constexpr std::array<Terminal, 10> kTerminals{{
        {"x-terminal-emulator", "-e"},
        {"gnome-terminal", "--"},
        {"kgx", "-e"},
        {"konsole", "-e"},
        {"xfce4-terminal", "-x"},
        {"mate-terminal", "-x"},
        {"tilix", "-e"},
        {"alacritty", "-e"},
        {"kitty", nullptr},
        {"xterm", "-e"},
    }};
    if (!exists) {
        return {};
    }
    try {
        for (const Terminal& terminal : kTerminals) {
            if (exists(terminal.program)) {
                std::vector<std::string> prefix{terminal.program};
                if (terminal.flag != nullptr) {
                    prefix.emplace_back(terminal.flag);
                }
                return prefix;
            }
        }
    } catch (const std::bad_alloc&) {
        return {};
    }
    return {};
}

Result<void> launch_with(const Handler& handler, Span<const Path> paths,
                         platform::IAppLauncher& launcher, const LaunchOptions& options) {
    auto invocations = expand_exec(handler, paths, options.expand);
    if (!invocations) {
        return Unexpected<Error>(std::move(invocations.error()));
    }
    try {
        for (std::vector<std::string>& argv : *invocations) {
            platform::LaunchRequest request;
            request.argv = std::move(argv);
            request.working_dir = handler.working_dir;
            auto started = launcher.launch(request);
            if (!started) {
                return started;
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "launch_with: out of memory");
    }
    return ok();
}

} // namespace navigator::openwith
