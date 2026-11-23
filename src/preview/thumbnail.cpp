// src/preview/thumbnail.cpp: Freedesktop Thumbnail Managing Standard: GLib-exact file URIs, MD5
// names, cache paths, policy, validated reads (tEXt Thumb::URI / Thumb::MTime / Thumb::Size),
// atomic writes (0700 directories, 0600 temporary file, rename) and failure markers.
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <new>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/thumbnail.hpp>

#include "internal.hpp"

namespace navigator::thumbnail {

namespace pd = preview::detail;

// =============================================================================================
// URI, MD5, paths
// =============================================================================================

namespace {

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
    Path n = p.lexically_normal();
    std::string s = n.native();
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return {s};
}

} // namespace

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

std::string md5_hex(std::string_view bytes) {
    // RFC 1321.
    static constexpr std::array<std::uint32_t, 64> kK = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static constexpr std::array<unsigned, 64> kS = {
        7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
        14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
        4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};
    std::uint32_t a0 = 0x67452301;
    std::uint32_t b0 = 0xefcdab89;
    std::uint32_t c0 = 0x98badcfe;
    std::uint32_t d0 = 0x10325476;
    const std::uint64_t bit_len = std::uint64_t{bytes.size()} * 8U;
    std::string msg(bytes);
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64U != 56U) {
        msg.push_back('\0');
    }
    for (unsigned i = 0; i < 8; ++i) {
        msg.push_back(static_cast<char>((bit_len >> (8U * i)) & 0xFFU));
    }
    const auto rotl = [](std::uint32_t x, unsigned c) { return (x << c) | (x >> (32U - c)); };
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto* q = reinterpret_cast<const unsigned char*>(msg.data() + chunk + i * 4U);
            m[i] = std::uint32_t{q[0]} | (std::uint32_t{q[1]} << 8U) |
                   (std::uint32_t{q[2]} << 16U) | (std::uint32_t{q[3]} << 24U);
        }
        std::uint32_t a = a0;
        std::uint32_t b = b0;
        std::uint32_t c = c0;
        std::uint32_t d = d0;
        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5U * i + 1U) % 16U;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3U * i + 5U) % 16U;
            } else {
                f = c ^ (b | ~d);
                g = (7U * i) % 16U;
            }
            f = f + a + kK[i] + m[g];
            a = d;
            d = c;
            c = b;
            b = b + rotl(f, kS[i]);
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }
    std::string out;
    out.reserve(32);
    constexpr char kHex[] = "0123456789abcdef";
    for (const std::uint32_t v : {a0, b0, c0, d0}) {
        for (unsigned i = 0; i < 4; ++i) {
            const auto byte = static_cast<unsigned>((v >> (8U * i)) & 0xFFU);
            out.push_back(kHex[byte >> 4U]);
            out.push_back(kHex[byte & 0x0FU]);
        }
    }
    return out;
}

std::string thumbnail_file_name(std::string_view uri) { return md5_hex(uri) + ".png"; }

Path thumbnail_path(const Path& cache_root, ThumbnailSize size, std::string_view uri) {
    return cache_root / std::string(dir_name(size)) / thumbnail_file_name(uri);
}

Path failure_path(const Path& cache_root, std::string_view uri) {
    return cache_root / std::string(kFailDirName) / std::string(kFailAppDirName) /
           thumbnail_file_name(uri);
}

bool is_inside_cache(const Path& path, const Path& cache_root) {
    if (path.empty() || cache_root.empty() || !path.is_absolute() || !cache_root.is_absolute()) {
        return false;
    }
    const Path p = normal_absolute(path);
    const Path r = normal_absolute(cache_root);
    auto pi = p.begin();
    for (auto ri = r.begin(); ri != r.end(); ++ri, ++pi) {
        if (ri->empty()) {
            continue;
        }
        if (pi == p.end() || *pi != *ri) {
            return false;
        }
    }
    return true;
}

Decision decide(const Path& path, const fs::DirEntry& entry,
                const preview::SourceLocation& location, const Path& cache_root,
                const Policy& policy) {
    if (path.empty() || !path.is_absolute() || is_inside_cache(path, cache_root)) {
        return Decision::Skip;
    }
    const bool file = entry.kind == fs::EntryKind::File ||
                      (entry.kind == fs::EntryKind::Symlink && !entry.dangling &&
                       entry.target_kind == fs::EntryKind::File);
    if (!file) {
        return Decision::Skip;
    }
    if (location.network && !policy.generate_on_network) {
        return Decision::ReadOnly;
    }
    if (location.removable && !policy.generate_on_removable) {
        return Decision::ReadOnly;
    }
    return Decision::Generate;
}

// =============================================================================================
// Reading
// =============================================================================================

namespace {

constexpr std::uint32_t kMaxThumbnailSide = 4096;

[[nodiscard]] const std::string* find_text(const pd::PngHeader& h, std::string_view key) {
    for (const auto& [k, v] : h.text) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<std::int64_t> parse_int(const std::string* s) {
    if (s == nullptr || s->empty()) {
        return std::nullopt;
    }
    std::int64_t v = 0;
    const char* b = s->data();
    const char* e = s->data() + s->size();
    while (b < e && *b == ' ') {
        ++b;
    }
    const auto r = std::from_chars(b, e, v);
    if (r.ec != std::errc() || r.ptr == b) {
        return std::nullopt;
    }
    // Accept a fractional part ("123.0") as some writers emit floats; nothing else may follow.
    const char* rest = r.ptr;
    if (rest < e && *rest == '.') {
        ++rest;
        while (rest < e && *rest >= '0' && *rest <= '9') {
            ++rest;
        }
    }
    if (rest != e) {
        return std::nullopt;
    }
    return v;
}

enum class Verdict : std::uint8_t { Valid, Stale, Invalid };

[[nodiscard]] Verdict validate(const pd::PngHeader& h, const SourceInfo& expected,
                               bool check_size) {
    if (h.size.empty() || h.size.width > kMaxThumbnailSide || h.size.height > kMaxThumbnailSide) {
        return Verdict::Invalid;
    }
    const std::string* uri = find_text(h, "Thumb::URI");
    if (uri == nullptr || *uri != expected.uri) {
        return Verdict::Invalid;
    }
    const auto mtime = parse_int(find_text(h, "Thumb::MTime"));
    if (!mtime) {
        return Verdict::Invalid;
    }
    if (*mtime != mtime_seconds(expected.mtime)) {
        return Verdict::Stale;
    }
    if (check_size) {
        if (const std::string* size = find_text(h, "Thumb::Size"); size != nullptr) {
            const auto v = parse_int(size);
            if (!v || *v < 0 || static_cast<std::uint64_t>(*v) != expected.size) {
                return Verdict::Stale;
            }
        }
    }
    return Verdict::Valid;
}

preview::PixelSize keep_stored_size(const pd::PngHeader& h, void* /*user*/) {
    return h.size; // validated before pixels are wanted
}

struct ThumbReadState {
    const SourceInfo* expected = nullptr;
    bool check_size = true;
    Verdict verdict = Verdict::Invalid;
};

preview::PixelSize choose_if_valid(const pd::PngHeader& h, void* user) {
    auto* st = static_cast<ThumbReadState*>(user);
    st->verdict = validate(h, *st->expected, st->check_size);
    if (st->verdict != Verdict::Valid) {
        return {};
    }
    return keep_stored_size(h, nullptr);
}

// Reads one file; `decode_pixels` false only validates.
Result<ThumbnailRead> read_one(const Path& file, const SourceInfo& expected,
                               const std::stop_token& stop, bool decode_pixels, bool check_size) {
    ThumbnailRead out;
    out.file = file;
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "cancelled");
    }
    const int fd_raw = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd_raw < 0) {
        const int err = errno;
        if (err == ENOENT || err == ENOTDIR) {
            out.status = LookupStatus::Missing;
            return out;
        }
        if (err == ELOOP) {
            out.status = LookupStatus::Invalid;
            return out;
        }
        return Unexpected<Error>(pd::errno_error(err, "open thumbnail", file));
    }
    pd::Fd fd(fd_raw);
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        return Unexpected<Error>(pd::errno_error(errno, "fstat thumbnail", file));
    }
    if (!S_ISREG(st.st_mode)) {
        out.status = LookupStatus::Invalid;
        return out;
    }
    const int fl = ::fcntl(fd.get(), F_GETFL);
    if (fl >= 0) {
        (void)::fcntl(fd.get(), F_SETFL, fl & ~O_NONBLOCK);
    }
    pd::PngDecodeOptions options;
    options.max_dimension = kMaxThumbnailSide;
    options.full_decode_pixels = std::uint64_t{kMaxThumbnailSide} * kMaxThumbnailSide;
    options.header_only = !decode_pixels;
    ThumbReadState state;
    state.expected = &expected;
    state.check_size = check_size;
    auto decoded = pd::png_decode_fd(fd.get(), options, choose_if_valid, &state, stop);
    if (!decoded) {
        switch (decoded.error().code) {
        case ErrorCode::Cancelled:
        case ErrorCode::IoError:
        case ErrorCode::OutOfMemory: return Unexpected<Error>(decoded.error());
        default: out.status = LookupStatus::Invalid; return out;
        }
    }
    if (!decode_pixels) {
        state.verdict = validate(decoded->header, expected, check_size);
    }
    switch (state.verdict) {
    case Verdict::Stale: out.status = LookupStatus::Stale; return out;
    case Verdict::Invalid: out.status = LookupStatus::Invalid; return out;
    case Verdict::Valid: break;
    }
    if (decode_pixels && (decoded->image.empty() || decoded->partial)) {
        out.status = LookupStatus::Invalid;
        return out;
    }
    out.status = LookupStatus::Valid;
    out.image = std::move(decoded->image);
    const auto ow = parse_int(find_text(decoded->header, "Thumb::Image::Width"));
    const auto oh = parse_int(find_text(decoded->header, "Thumb::Image::Height"));
    if (ow && oh && *ow > 0 && *oh > 0 && *ow <= 0xFFFFFFFF && *oh <= 0xFFFFFFFF) {
        out.original =
            preview::PixelSize{static_cast<std::uint32_t>(*ow), static_cast<std::uint32_t>(*oh)};
    }
    out.movie_length_s = parse_int(find_text(decoded->header, "Thumb::Movie::Length"));
    return out;
}

} // namespace

Result<ThumbnailRead> read_thumbnail(
    const Path& file, const SourceInfo& expected,
    std::stop_token stop) { // NOLINT(performance-unnecessary-value-param): header signature
    try {
        return read_one(file, expected, stop, true, true);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "read_thumbnail: allocation failed");
    }
}

Result<ThumbnailRead>
lookup_thumbnail(const Path& cache_root, ThumbnailSize wanted, const SourceInfo& expected,
                 std::stop_token stop) { // NOLINT(performance-unnecessary-value-param)
    constexpr ThumbnailSize kAll[] = {ThumbnailSize::Normal, ThumbnailSize::Large,
                                      ThumbnailSize::XLarge, ThumbnailSize::XXLarge};
    bool any_stale = false;
    bool any_invalid = false;
    for (const ThumbnailSize s : kAll) {
        if (pixels(s) < pixels(wanted)) {
            continue;
        }
        auto r = read_thumbnail(thumbnail_path(cache_root, s, expected.uri), expected, stop);
        if (!r) {
            return r;
        }
        r->size_class = s;
        if (r->status == LookupStatus::Valid) {
            return r;
        }
        any_stale = any_stale || r->status == LookupStatus::Stale;
        any_invalid = any_invalid || r->status == LookupStatus::Invalid;
    }
    ThumbnailRead out;
    out.size_class = wanted;
    out.file = thumbnail_path(cache_root, wanted, expected.uri);
    out.status = any_stale ? LookupStatus::Stale
                           : (any_invalid ? LookupStatus::Invalid : LookupStatus::Missing);
    return out;
}

// =============================================================================================
// Writing
// =============================================================================================

namespace {

Result<void> make_dirs_0700(const Path& dir) {
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) {
        return ok();
    }
    const Path parent = dir.parent_path();
    if (!parent.empty() && parent != dir) {
        auto r = make_dirs_0700(parent);
        if (!r) {
            return r;
        }
    }
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        const int err = errno;
        Error e = pd::errno_error(err, "mkdir", dir);
        if (e.code == ErrorCode::NotFound) {
            e.code = ErrorCode::IoError;
        }
        return Unexpected<Error>(std::move(e));
    }
    return ok();
}

Result<Path> write_atomic(const Path& final_path, const preview::ImageBuffer& image,
                          const std::vector<std::pair<std::string, std::string>>& text) {
    auto dirs = make_dirs_0700(final_path.parent_path());
    if (!dirs) {
        return Unexpected<Error>(dirs.error());
    }
    std::string tmpl = final_path.native() + ".XXXXXX";
    const int fd_raw = ::mkostemp(tmpl.data(), O_CLOEXEC);
    if (fd_raw < 0) {
        return Unexpected<Error>(pd::errno_error(errno, "create temporary thumbnail", final_path));
    }
    pd::Fd fd(fd_raw);
    (void)::fchmod(fd.get(), 0600);
    const Path tmp(tmpl);
    auto written = pd::png_encode_fd(fd.get(), image, text);
    if (written && ::close(fd.release()) != 0) {
        written = Unexpected<Error>(pd::errno_error(errno, "close", tmp));
    }
    if (!written) {
        (void)::unlink(tmp.c_str());
        return Unexpected<Error>(written.error());
    }
    if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
        const int err = errno;
        (void)::unlink(tmp.c_str());
        return Unexpected<Error>(pd::errno_error(err, "rename", final_path));
    }
    return final_path;
}

[[nodiscard]] std::string software_value() { return std::string(kSoftware); }

} // namespace

Result<Path> write_thumbnail(const Path& cache_root, ThumbnailSize size, const SourceInfo& source,
                             const preview::ImageBuffer& image, const WriteOptions& options) {
    if (!image.valid() || image.width > pixels(size) || image.height > pixels(size) ||
        source.uri.empty() || is_inside_cache(source.path, cache_root) || cache_root.empty()) {
        return fail(ErrorCode::InvalidArgument, "write_thumbnail: invalid arguments");
    }
    try {
        std::vector<std::pair<std::string, std::string>> text;
        text.emplace_back("Thumb::URI", source.uri);
        text.emplace_back("Thumb::MTime", std::to_string(mtime_seconds(source.mtime)));
        text.emplace_back("Thumb::Size", std::to_string(source.size));
        if (!source.mime.empty()) {
            text.emplace_back("Thumb::Mimetype", source.mime);
        }
        if (!options.original.empty()) {
            text.emplace_back("Thumb::Image::Width", std::to_string(options.original.width));
            text.emplace_back("Thumb::Image::Height", std::to_string(options.original.height));
        }
        if (options.movie_length_s) {
            text.emplace_back("Thumb::Movie::Length", std::to_string(*options.movie_length_s));
        }
        text.emplace_back("Software", software_value());
        return write_atomic(thumbnail_path(cache_root, size, source.uri), image, text);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "write_thumbnail: allocation failed");
    }
}

Result<void> write_failure(const Path& cache_root, const SourceInfo& source) {
    if (source.uri.empty() || cache_root.empty() || is_inside_cache(source.path, cache_root)) {
        return fail(ErrorCode::InvalidArgument, "write_failure: invalid arguments");
    }
    try {
        auto img = preview::make_image({1, 1});
        if (!img) {
            return Unexpected<Error>(img.error());
        }
        std::vector<std::pair<std::string, std::string>> text;
        text.emplace_back("Thumb::URI", source.uri);
        text.emplace_back("Thumb::MTime", std::to_string(mtime_seconds(source.mtime)));
        text.emplace_back("Software", software_value());
        auto r = write_atomic(failure_path(cache_root, source.uri), *img, text);
        if (!r) {
            return Unexpected<Error>(r.error());
        }
        return ok();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "write_failure: allocation failed");
    }
}

Result<bool> has_failure(const Path& cache_root, const SourceInfo& source) {
    try {
        auto r =
            read_one(failure_path(cache_root, source.uri), source, std::stop_token{}, false, false);
        if (!r) {
            if (r.error().code == ErrorCode::PermissionDenied ||
                r.error().code == ErrorCode::IoError) {
                return Unexpected<Error>(r.error());
            }
            return false;
        }
        return r->status == LookupStatus::Valid;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "has_failure: allocation failed");
    }
}

} // namespace navigator::thumbnail
