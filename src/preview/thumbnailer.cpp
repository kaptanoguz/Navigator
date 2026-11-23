// src/preview/thumbnailer.cpp: *.thumbnailer files (parse, Exec splitting / expansion without a
// shell, XDG loading with TryExec), choice by MIME, and running one through the injected
// platform::IProcessRunner in a private temporary directory.
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>

#include <sys/stat.h>
#include <unistd.h>

#include <navigator/thumbnail.hpp>

#include "internal.hpp"

namespace navigator::thumbnail {

namespace pd = preview::detail;

namespace {

constexpr std::size_t kMaxThumbnailerFile = std::size_t{64} * 1024U;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Desktop-entry value escapes: \s \n \t \r \\ (and "\;" kept as "\;" for list splitting).
[[nodiscard]] std::string unescape(std::string_view v, bool keep_semicolon_escape) {
    std::string out;
    out.reserve(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\' || i + 1 >= v.size()) {
            out.push_back(v[i]);
            continue;
        }
        const char n = v[++i];
        switch (n) {
        case 's': out.push_back(' '); break;
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case '\\':
            out.push_back('\\');
            if (keep_semicolon_escape) {
                out.push_back('\\');
            }
            break;
        case ';':
            if (keep_semicolon_escape) {
                out.push_back('\\');
            }
            out.push_back(';');
            break;
        default:
            out.push_back('\\');
            out.push_back(n);
            break;
        }
    }
    return out;
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

} // namespace

Result<Thumbnailer> parse_thumbnailer(std::string_view text, std::string_view id,
                                      const Path& file) {
    if (text.size() > kMaxThumbnailerFile) {
        return fail(ErrorCode::InvalidArgument, "thumbnailer: file larger than 64 KiB");
    }
    try {
        Thumbnailer t;
        t.id = std::string(id);
        t.file = file;
        bool in_group = false;
        bool seen_group = false;
        std::string mime_raw;
        bool have_exec = false;
        bool have_mime = false;
        std::size_t pos = 0;
        while (pos <= text.size()) {
            std::size_t eol = text.find('\n', pos);
            if (eol == std::string_view::npos) {
                eol = text.size();
            }
            const std::string_view line = trim(text.substr(pos, eol - pos));
            pos = eol + 1;
            if (line.empty() || line.front() == '#') {
                continue;
            }
            if (line.front() == '[') {
                in_group = line == "[Thumbnailer Entry]";
                seen_group = seen_group || in_group;
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
            const std::string_view value = trim(line.substr(eq + 1));
            if (key.find('[') != std::string_view::npos) {
                continue; // localized key
            }
            if (key == "TryExec") {
                t.try_exec = unescape(value, false);
            } else if (key == "Exec") {
                t.exec = unescape(value, false);
                have_exec = true;
            } else if (key == "MimeType") {
                mime_raw = unescape(value, true);
                have_mime = true;
            }
        }
        if (!seen_group) {
            return fail(ErrorCode::InvalidArgument, "thumbnailer: no [Thumbnailer Entry] group");
        }
        if (!have_exec || trim(t.exec).empty()) {
            return fail(ErrorCode::InvalidArgument, "thumbnailer: missing Exec");
        }
        auto words = split_exec(t.exec);
        if (!words) {
            return Unexpected<Error>(words.error());
        }
        std::set<std::string> seen;
        std::string item;
        const auto flush = [&] {
            std::string m(trim(item));
            for (char& c : m) {
                c = ascii_lower(c);
            }
            if (!m.empty() && seen.insert(m).second) {
                t.mime_types.push_back(std::move(m));
            }
            item.clear();
        };
        for (std::size_t i = 0; i < mime_raw.size(); ++i) {
            const char c = mime_raw[i];
            if (c == '\\' && i + 1 < mime_raw.size()) {
                item.push_back(mime_raw[++i]);
            } else if (c == ';') {
                flush();
            } else {
                item.push_back(c);
            }
        }
        flush();
        if (!have_mime || t.mime_types.empty()) {
            return fail(ErrorCode::InvalidArgument, "thumbnailer: missing MimeType");
        }
        return t;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "thumbnailer: allocation failed");
    }
}

namespace {

[[nodiscard]] Result<void> check_field_codes(std::string_view word) {
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (word[i] != '%') {
            continue;
        }
        if (i + 1 >= word.size()) {
            return fail(ErrorCode::InvalidArgument, "Exec: trailing %");
        }
        const char c = word[++i];
        if (c != 'i' && c != 'o' && c != 's' && c != 'u' && c != '%') {
            return fail(ErrorCode::InvalidArgument, std::string("Exec: unknown field code %") + c);
        }
    }
    return ok();
}

} // namespace

Result<std::vector<std::string>> split_exec(std::string_view exec) {
    try {
        std::vector<std::string> words;
        std::string cur;
        bool in_word = false;
        std::size_t i = 0;
        while (i < exec.size()) {
            const char c = exec[i];
            if (c == ' ' || c == '\t' || c == '\n') {
                if (in_word) {
                    words.push_back(std::move(cur));
                    cur.clear();
                    in_word = false;
                }
                ++i;
                continue;
            }
            in_word = true;
            if (c == '\'') {
                const std::size_t end = exec.find('\'', i + 1);
                if (end == std::string_view::npos) {
                    return fail(ErrorCode::InvalidArgument, "Exec: unterminated single quote");
                }
                cur.append(exec.substr(i + 1, end - i - 1));
                i = end + 1;
                continue;
            }
            if (c == '"') {
                ++i;
                bool closed = false;
                while (i < exec.size()) {
                    const char d = exec[i];
                    if (d == '"') {
                        closed = true;
                        ++i;
                        break;
                    }
                    if (d == '\\' && i + 1 < exec.size()) {
                        const char n = exec[i + 1];
                        if (n == '\\' || n == '"' || n == '$' || n == '`') {
                            cur.push_back(n);
                            i += 2;
                            continue;
                        }
                        if (n == '\n') {
                            i += 2;
                            continue;
                        }
                    }
                    cur.push_back(d);
                    ++i;
                }
                if (!closed) {
                    return fail(ErrorCode::InvalidArgument, "Exec: unterminated double quote");
                }
                continue;
            }
            if (c == '\\') {
                if (i + 1 >= exec.size()) {
                    return fail(ErrorCode::InvalidArgument, "Exec: trailing backslash");
                }
                if (exec[i + 1] != '\n') {
                    cur.push_back(exec[i + 1]);
                }
                i += 2;
                continue;
            }
            cur.push_back(c);
            ++i;
        }
        if (in_word) {
            words.push_back(std::move(cur));
        }
        if (words.empty()) {
            return fail(ErrorCode::InvalidArgument, "Exec: no words");
        }
        for (const std::string& w : words) {
            auto ok_codes = check_field_codes(w);
            if (!ok_codes) {
                return Unexpected<Error>(ok_codes.error());
            }
        }
        return words;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "Exec: allocation failed");
    }
}

Result<std::vector<std::string>> expand_exec(std::string_view exec, const ExecInputs& inputs) {
    auto words = split_exec(exec);
    if (!words) {
        return words;
    }
    try {
        const std::string size = inputs.size == 0 ? std::string() : std::to_string(inputs.size);
        std::vector<std::string> out;
        out.reserve(words->size());
        for (const std::string& w : *words) {
            std::string e;
            for (std::size_t i = 0; i < w.size(); ++i) {
                if (w[i] != '%') {
                    e.push_back(w[i]);
                    continue;
                }
                const char c = w[++i]; // validated by split_exec
                std::string_view value;
                switch (c) {
                case '%': e.push_back('%'); continue;
                case 'i': value = inputs.input_path; break;
                case 'u': value = inputs.input_uri; break;
                case 'o': value = inputs.output_path; break;
                case 's': value = size; break;
                default: break;
                }
                if (value.empty()) {
                    return fail(ErrorCode::InvalidArgument,
                                std::string("Exec: empty value for %") + c);
                }
                e.append(value);
            }
            out.push_back(std::move(e));
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "Exec: allocation failed");
    }
}

std::vector<Path> thumbnailer_dirs(Span<const Path> data_dirs) {
    std::vector<Path> out;
    out.reserve(data_dirs.size());
    for (const Path& d : data_dirs) {
        out.push_back(d / "thumbnailers");
    }
    return out;
}

Result<std::vector<Thumbnailer>>
load_thumbnailers(Span<const Path> dirs, const ProgramExists& exists,
                  std::stop_token stop) { // NOLINT(performance-unnecessary-value-param)
    try {
        std::vector<Thumbnailer> out;
        std::set<std::string> seen_ids;
        for (const Path& dir : dirs) {
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            std::error_code ec;
            std::filesystem::directory_iterator it(dir, ec);
            if (ec) {
                continue;
            }
            std::vector<std::pair<std::string, Path>> files;
            for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
                if (ec) {
                    break;
                }
                const std::string name = it->path().filename().native();
                constexpr std::string_view kSuffix = ".thumbnailer";
                if (name.size() <= kSuffix.size() ||
                    name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
                    continue;
                }
                files.emplace_back(name.substr(0, name.size() - kSuffix.size()), it->path());
            }
            std::sort(files.begin(), files.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [id, file] : files) {
                if (stop.stop_requested()) {
                    return fail(ErrorCode::Cancelled, "cancelled");
                }
                if (seen_ids.count(id) != 0) {
                    continue;
                }
                std::uint64_t size = 0;
                auto fd = pd::open_regular(file, &size);
                if (!fd || size > kMaxThumbnailerFile) {
                    continue;
                }
                auto bytes = pd::read_prefix(fd->get(), kMaxThumbnailerFile + 1U, stop, file);
                if (!bytes) {
                    if (bytes.error().code == ErrorCode::Cancelled) {
                        return Unexpected<Error>(bytes.error());
                    }
                    continue;
                }
                auto parsed = parse_thumbnailer(
                    std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()),
                    id, file);
                if (!parsed) {
                    if (parsed.error().code == ErrorCode::OutOfMemory) {
                        return Unexpected<Error>(parsed.error());
                    }
                    continue;
                }
                seen_ids.insert(id); // a valid earlier entry shadows later ones
                if (!parsed->try_exec.empty() && !(exists && exists(parsed->try_exec))) {
                    continue;
                }
                auto words = split_exec(parsed->exec);
                if (!words || !(exists && exists(words->front()))) {
                    continue;
                }
                out.push_back(std::move(*parsed));
            }
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "load_thumbnailers: allocation failed");
    }
}

const Thumbnailer* choose_thumbnailer(Span<const Thumbnailer> thumbnailers,
                                      std::string_view mime) noexcept {
    for (const Thumbnailer& t : thumbnailers) {
        for (const std::string& m : t.mime_types) {
            if (iequals(m, mime)) {
                return &t;
            }
        }
    }
    return nullptr;
}

namespace {

struct TempDir {
    Path path;
    TempDir() = default;
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};

} // namespace

Result<preview::ImageBuffer>
run_thumbnailer(const Thumbnailer& thumbnailer, const SourceInfo& source, ThumbnailSize size,
                platform::IProcessRunner& runner, const ThumbnailerLimits& limits,
                std::stop_token stop) { // NOLINT(performance-unnecessary-value-param)
    try {
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        std::error_code ec;
        Path base = std::filesystem::temp_directory_path(ec);
        if (ec || base.empty() || !base.is_absolute()) {
            base = "/tmp";
        }
        TempDir tmp;
        std::string tmpl = (base / "navigator-thumb-XXXXXX").native();
        if (::mkdtemp(tmpl.data()) == nullptr) {
            return Unexpected<Error>(pd::errno_error(errno, "mkdtemp", Path(tmpl)));
        }
        tmp.path = Path(tmpl);
        (void)::chmod(tmp.path.c_str(), 0700);
        const Path out_png = tmp.path / "out.png";
        ExecInputs inputs;
        inputs.input_path = source.path.native();
        inputs.input_uri = source.uri;
        inputs.output_path = out_png.native();
        inputs.size = pixels(size);
        auto argv = expand_exec(thumbnailer.exec, inputs);
        if (!argv) {
            return Unexpected<Error>(argv.error());
        }
        platform::ProcessRequest req;
        req.argv = std::move(*argv);
        req.working_dir = tmp.path;
        req.timeout = limits.timeout;
        req.address_space_bytes = limits.address_space_bytes;
        req.cpu_time = limits.cpu_time;
        req.max_file_bytes = limits.max_output_bytes;
        req.sandbox = limits.sandbox;
        req.read_only_paths = {source.path};
        req.writable_paths = {tmp.path};
        auto result = runner.run(req, stop);
        if (!result) {
            return Unexpected<Error>(result.error());
        }
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        if (!result->succeeded()) {
            std::string why;
            if (result->timed_out) {
                why = "timed out";
            } else if (result->term_signal != 0) {
                why = "killed by signal " + std::to_string(result->term_signal);
            } else {
                why = "exit code " + std::to_string(result->exit_code);
            }
            return fail(ErrorCode::IoError, "thumbnailer " + thumbnailer.id + ": " + why);
        }
        struct stat st {};
        if (::lstat(out_png.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            return fail(ErrorCode::IoError, "thumbnailer " + thumbnailer.id + ": no output image");
        }
        if (st.st_size <= 0 || static_cast<std::uint64_t>(st.st_size) > limits.max_output_bytes) {
            return fail(ErrorCode::IoError,
                        "thumbnailer " + thumbnailer.id + ": output empty or too large");
        }
        pd::DecodeParams params;
        params.target = {pixels(size), pixels(size)};
        params.purpose = preview::PreviewPurpose::Thumbnail;
        params.stop = stop;
        params.full_decode_pixels = preview::kMaxFullDecodePixels;
        auto decoded = pd::decode_image_file(out_png, params, true);
        if (!decoded) {
            if (decoded.error().code == ErrorCode::Cancelled ||
                decoded.error().code == ErrorCode::OutOfMemory) {
                return Unexpected<Error>(decoded.error());
            }
            return fail(ErrorCode::InvalidArgument,
                        "thumbnailer " + thumbnailer.id +
                            ": output is not a decodable image: " + decoded.error().message);
        }
        return std::move(decoded->image);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "run_thumbnailer: allocation failed");
    }
}

} // namespace navigator::thumbnail
