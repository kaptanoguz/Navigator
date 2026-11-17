// navigator/path_input.hpp: typed / pasted path text -> absolute folder, case correction,
// directory-name completion and Copy as Path text. Everything is byte-oriented: values from the
// environment and on-disk names are copied verbatim, never re-encoded or normalized.
#include <algorithm>
#include <compare>
#include <cstddef>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/path_input.hpp>

#include "utf8.hpp"

namespace navigator::path_input {
namespace {

constexpr std::string_view kFileScheme = "file://";

[[nodiscard]] constexpr bool is_ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return ascii_lower(x) == ascii_lower(y);
           });
}

[[nodiscard]] constexpr bool is_name_start(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

[[nodiscard]] constexpr bool is_name_char(char c) noexcept {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

[[nodiscard]] constexpr bool is_quote(char c) noexcept { return c == '\'' || c == '"'; }

[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept {
    while (!text.empty() && is_ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// Step 1: trim, then remove one pair of matching wrapping quotes.
[[nodiscard]] std::string_view strip_input(std::string_view text) noexcept {
    text = trim_ascii(text);
    if (text.size() >= 2 && is_quote(text.front()) && text.back() == text.front()) {
        text = text.substr(1, text.size() - 2);
    }
    return text;
}

[[nodiscard]] int hex_digit(char c) noexcept {
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

// %HH -> byte; a malformed escape stays literal.
[[nodiscard]] std::string percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex_digit(text[i + 1]);
            const int lo = hex_digit(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

// Step 4: one left-to-right pass over `text`, appending to `out`.
void expand_variables(std::string_view text, const ExpandContext& ctx, std::string& out,
                      std::vector<std::string>& unresolved) {
    const auto lookup = [&](std::string_view name, std::string_view token) {
        std::optional<std::string> value;
        if (ctx.env) {
            value = ctx.env(name);
        }
        if (value) {
            out.append(*value);
        } else {
            out.append(token);
            unresolved.emplace_back(token);
        }
    };
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '$' && i + 1 < text.size()) {
            if (is_name_start(text[i + 1])) {
                std::size_t end = i + 2;
                while (end < text.size() && is_name_char(text[end])) {
                    ++end;
                }
                lookup(text.substr(i + 1, end - i - 1), text.substr(i, end - i));
                i = end;
                continue;
            }
            if (text[i + 1] == '{' && i + 2 < text.size() && is_name_start(text[i + 2])) {
                std::size_t end = i + 3;
                while (end < text.size() && is_name_char(text[end])) {
                    ++end;
                }
                if (end < text.size() && text[end] == '}') {
                    lookup(text.substr(i + 2, end - i - 2), text.substr(i, end - i + 1));
                    i = end + 1;
                    continue;
                }
            }
        } else if (c == '%' && ctx.percent_variables && i + 1 < text.size() &&
                   is_name_start(text[i + 1])) {
            std::size_t end = i + 2;
            while (end < text.size() && is_name_char(text[end])) {
                ++end;
            }
            if (end < text.size() && text[end] == '%') {
                lookup(text.substr(i + 1, end - i - 1), text.substr(i, end - i + 1));
                i = end + 1;
                continue;
            }
        }
        out.push_back(c);
        ++i;
    }
}

// Steps 2-4 on stripped, non-empty text; the result may still be relative.
Result<std::string> expand_text(std::string_view text, const ExpandContext& ctx,
                                std::vector<std::string>& unresolved) {
    if (text.size() >= kFileScheme.size() &&
        iequals_ascii(text.substr(0, kFileScheme.size()), kFileScheme)) {
        const std::string_view rest = text.substr(kFileScheme.size());
        const std::size_t slash = rest.find('/');
        const std::string_view host = rest.substr(0, slash);
        if (!host.empty() && !iequals_ascii(host, "localhost")) {
            return fail(ErrorCode::InvalidArgument,
                        "file:// URI with a remote host: '" + std::string(host) + "'");
        }
        if (slash == std::string_view::npos) {
            return std::string("/");
        }
        return percent_decode(rest.substr(slash));
    }

    std::string out;
    out.reserve(text.size());
    if (text.front() == '~') {
        const std::size_t slash = text.find('/');
        const std::string_view token = text.substr(0, slash);
        const std::string_view name = token.substr(1);
        std::optional<Path> home;
        if (name.empty()) {
            if (!ctx.home.empty()) {
                home = ctx.home;
            }
        } else if (ctx.user_home) {
            home = ctx.user_home(name);
        }
        if (home && !home->empty()) {
            out.append(home->native());
        } else {
            out.append(token);
            unresolved.emplace_back(token);
        }
        text = slash == std::string_view::npos ? std::string_view{} : text.substr(slash);
    }
    expand_variables(text, ctx, out, unresolved);
    return out;
}

// `path` (absolute or not) made absolute against ctx.cwd and normalized.
Result<Path> absolutize(std::string text, const ExpandContext& ctx) {
    Path path(std::move(text));
    if (!path.is_absolute()) {
        if (ctx.cwd.empty() || !ctx.cwd.is_absolute()) {
            return fail(ErrorCode::InvalidArgument,
                        "relative path without an absolute current folder");
        }
        path = ctx.cwd / path;
    }
    Path normal = normalize_absolute(path);
    if (normal.empty()) {
        return fail(ErrorCode::InvalidArgument, "path is not absolute");
    }
    return normal;
}

[[nodiscard]] bool exists_follow(const Path& path) { return fs::stat(path, true).has_value(); }

} // namespace

// ---------------------------------------------------------------------------------------------
// normalize_absolute
// ---------------------------------------------------------------------------------------------

Path normalize_absolute(const Path& path) {
    const std::string& raw = path.native();
    if (raw.empty() || raw.front() != '/') {
        return {};
    }
    std::vector<std::string_view> parts;
    std::string_view rest(raw);
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
            continue;
        }
        parts.push_back(part);
    }
    if (parts.empty()) {
        return {"/"};
    }
    std::string out;
    out.reserve(raw.size());
    for (const std::string_view part : parts) {
        out.push_back('/');
        out.append(part);
    }
    return {std::move(out)};
}

// ---------------------------------------------------------------------------------------------
// expand_user_path
// ---------------------------------------------------------------------------------------------

Result<ExpandedPath> expand_user_path(std::string_view input, const ExpandContext& ctx) {
    try {
        const std::string_view text = strip_input(input);
        if (text.empty()) {
            return fail(ErrorCode::InvalidArgument, "empty path");
        }
        ExpandedPath result;
        Result<std::string> expanded = expand_text(text, ctx, result.unresolved);
        if (!expanded) {
            return Unexpected<Error>(expanded.error());
        }
        Result<Path> path = absolutize(std::move(*expanded), ctx);
        if (!path) {
            return Unexpected<Error>(path.error());
        }
        result.path = std::move(*path);
        return result;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while expanding a path");
    }
}

// ---------------------------------------------------------------------------------------------
// fix_path_case / resolve_input
// ---------------------------------------------------------------------------------------------

// The by-value std::stop_token parameters are fixed by path_input.hpp.
// NOLINTBEGIN(performance-unnecessary-value-param)
Result<CaseFix> fix_path_case(const Path& path, std::stop_token stop) {
    // NOLINTEND(performance-unnecessary-value-param)
    try {
        const Path normal = normalize_absolute(path);
        if (normal.empty()) {
            return fail(ErrorCode::InvalidArgument, "fix_path_case needs an absolute path");
        }
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        CaseFix fix;
        if (exists_follow(normal)) {
            fix.path = normal;
            fix.exists = true;
            return fix;
        }
        std::vector<std::string> parts;
        for (const Path& part : normal.relative_path()) {
            parts.push_back(part.native());
        }
        Path current("/");
        std::size_t i = 0;
        for (; i < parts.size(); ++i) {
            const Path candidate = current / parts[i];
            if (exists_follow(candidate)) {
                current = candidate;
                continue;
            }
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            fs::ListOptions options;
            options.show_hidden = true;
            Result<std::vector<fs::DirEntry>> listing = fs::list_directory(current, options, stop);
            if (!listing) {
                if (listing.error().code == ErrorCode::Cancelled ||
                    listing.error().code == ErrorCode::OutOfMemory) {
                    return Unexpected<Error>(listing.error());
                }
                break;
            }
            const bool last = i + 1 == parts.size();
            const std::string wanted = fs::fold_name(parts[i]);
            const fs::DirEntry* match = nullptr;
            std::size_t matches = 0;
            for (const fs::DirEntry& entry : *listing) {
                if (!last && !fs::is_directory_like(entry)) {
                    continue;
                }
                if (fs::fold_name(entry.name) == wanted) {
                    match = &entry;
                    if (++matches > 1) {
                        break;
                    }
                }
            }
            if (matches != 1) {
                break;
            }
            if (match->name != parts[i]) {
                fix.changed = true;
            }
            current /= match->name;
        }
        for (; i < parts.size(); ++i) {
            current /= parts[i];
        }
        fix.path = std::move(current);
        fix.exists = exists_follow(fix.path);
        return fix;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while correcting path case");
    }
}

// NOLINTBEGIN(performance-unnecessary-value-param)
Result<ResolvedInput> resolve_input(std::string_view input, const ExpandContext& ctx,
                                    std::stop_token stop) {
    // NOLINTEND(performance-unnecessary-value-param)
    try {
        Result<ExpandedPath> expanded = expand_user_path(input, ctx);
        if (!expanded) {
            return Unexpected<Error>(expanded.error());
        }
        Result<CaseFix> fix = fix_path_case(expanded->path, stop);
        if (!fix) {
            return Unexpected<Error>(fix.error());
        }
        ResolvedInput result;
        result.path = std::move(fix->path);
        result.case_corrected = fix->changed;
        result.unresolved = std::move(expanded->unresolved);
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        if (fix->exists) {
            Result<fs::DirEntry> entry = fs::stat(result.path, true);
            if (entry) {
                result.entry = std::move(*entry);
            } else if (entry.error().code == ErrorCode::OutOfMemory) {
                return Unexpected<Error>(entry.error());
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while resolving a path");
    }
}

// ---------------------------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------------------------

Result<CompletionQuery> parse_completion_input(std::string_view input, const ExpandContext& ctx) {
    try {
        std::string_view text = input;
        while (!text.empty() && is_ascii_space(text.front())) {
            text.remove_prefix(1);
        }
        if (!text.empty() && is_quote(text.front())) {
            const char quote = text.front();
            text.remove_prefix(1);
            // A balanced pair (a pasted, quoted path) loses its closing quote too.
            if (!text.empty() && text.back() == quote) {
                text.remove_suffix(1);
            }
        }
        CompletionQuery query;
        const std::size_t slash = text.rfind('/');
        if (slash == std::string_view::npos) {
            if (!text.empty() && text.front() == '~') {
                query.typed_dir = std::string(text) + "/";
            } else {
                query.prefix = std::string(text);
                query.dir = normalize_absolute(ctx.cwd);
                if (query.dir.empty()) {
                    return fail(ErrorCode::InvalidArgument,
                                "relative input without an absolute current folder");
                }
                return query;
            }
        } else {
            query.typed_dir = std::string(text.substr(0, slash + 1));
            query.prefix = std::string(text.substr(slash + 1));
        }
        Result<ExpandedPath> dir = expand_user_path(query.typed_dir, ctx);
        if (!dir) {
            return Unexpected<Error>(dir.error());
        }
        query.dir = std::move(dir->path);
        return query;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while parsing completion input");
    }
}

std::vector<Completion> match_completions(const CompletionQuery& query,
                                          Span<const fs::DirEntry> entries,
                                          const CompletionOptions& options) {
    const std::string folded_prefix = fs::fold_name(query.prefix);
    const bool want_hidden = options.include_hidden || fs::is_hidden_name(query.prefix);
    std::vector<Completion> out;
    for (const fs::DirEntry& entry : entries) {
        if (entry.name.empty() || !core::is_valid_utf8(entry.name)) {
            continue;
        }
        const bool is_dir = fs::is_directory_like(entry);
        if (!is_dir && !options.include_files) {
            continue;
        }
        if (fs::is_hidden_name(entry.name) && !want_hidden) {
            continue;
        }
        CompletionMatch match = CompletionMatch::Prefix;
        if (!folded_prefix.empty()) {
            const std::string folded = fs::fold_name(entry.name);
            if (folded.starts_with(folded_prefix)) {
                match = CompletionMatch::Prefix;
            } else if (options.substring_matches &&
                       folded.find(folded_prefix) != std::string::npos) {
                match = CompletionMatch::Substring;
            } else {
                continue;
            }
        }
        Completion completion;
        completion.name = entry.name;
        completion.path = query.dir / entry.name;
        completion.text = query.typed_dir + entry.name;
        if (is_dir) {
            completion.text.push_back('/');
        }
        completion.match = match;
        completion.is_symlink = entry.kind == fs::EntryKind::Symlink;
        completion.is_directory = is_dir;
        out.push_back(std::move(completion));
    }
    fs::SortOptions order;
    order.natural = true;
    order.case_insensitive = true;
    std::sort(out.begin(), out.end(), [&](const Completion& a, const Completion& b) {
        if (a.match != b.match) {
            return a.match < b.match;
        }
        const std::weak_ordering c = fs::compare_names(a.name, b.name, order);
        if (c != 0) {
            return c < 0;
        }
        return a.name < b.name;
    });
    const std::size_t limit = std::max<std::size_t>(options.max_results, 1);
    if (out.size() > limit) {
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(limit), out.end());
    }
    return out;
}

// NOLINTBEGIN(performance-unnecessary-value-param)
Result<std::vector<Completion>> complete_path(std::string_view input, const ExpandContext& ctx,
                                              const CompletionOptions& options,
                                              std::stop_token stop) {
    // NOLINTEND(performance-unnecessary-value-param)
    try {
        Result<CompletionQuery> query = parse_completion_input(input, ctx);
        if (!query) {
            return Unexpected<Error>(query.error());
        }
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        if (!exists_follow(query->dir)) {
            Result<CaseFix> fix = fix_path_case(query->dir, stop);
            if (!fix) {
                return Unexpected<Error>(fix.error());
            }
            if (fix->changed && fix->exists) {
                query->dir = std::move(fix->path);
                query->typed_dir = query->dir.native();
                if (query->typed_dir.back() != '/') {
                    query->typed_dir.push_back('/');
                }
            }
        }
        fs::ListOptions list_options;
        list_options.show_hidden = true;
        Result<std::vector<fs::DirEntry>> listing =
            fs::list_directory(query->dir, list_options, stop);
        if (!listing) {
            if (listing.error().code == ErrorCode::Cancelled ||
                listing.error().code == ErrorCode::OutOfMemory) {
                return Unexpected<Error>(listing.error());
            }
            return std::vector<Completion>{};
        }
        return match_completions(*query, *listing, options);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while completing a path");
    }
}

// ---------------------------------------------------------------------------------------------
// Copy as Path
// ---------------------------------------------------------------------------------------------

std::string quote_for_shell(std::string_view text) {
    const auto safe = [](char c) {
        const auto b = static_cast<unsigned char>(c);
        if (b >= 0x80 || is_name_char(c)) {
            return true;
        }
        switch (c) {
        case '@':
        case '%':
        case '+':
        case '=':
        case ':':
        case ',':
        case '.':
        case '/':
        case '-': return true;
        default: return false;
        }
    };
    if (!text.empty() && std::all_of(text.begin(), text.end(), safe)) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('\'');
    for (const char c : text) {
        if (c == '\'') {
            out.append("'\\''");
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string format_paths_for_copy(Span<const Path> paths, PathQuoting quoting) {
    std::string out;
    if (paths.empty()) {
        return out;
    }
    const bool raw =
        quoting == PathQuoting::Never || (quoting == PathQuoting::Auto && paths.size() == 1);
    const char separator = quoting == PathQuoting::Never ? '\n' : ' ';
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) {
            out.push_back(separator);
        }
        if (raw) {
            out.append(paths[i].native());
        } else {
            out.append(quote_for_shell(paths[i].native()));
        }
    }
    return out;
}

} // namespace navigator::path_input
