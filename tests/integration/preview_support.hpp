// tests/integration/preview_support.hpp: helpers for navigator_preview_integration_tests
// (owner: qa). Phase 6: the real preview service, the Freedesktop cache in TEMPORARY roots only
// (never ~/.cache/thumbnails), the real process runner, fixtures made at run time with system
// tools (ffmpeg, gdk-pixbuf-thumbnailer; a case SKIPs when a tool is missing), and an
// independent PNG chunk reader (signature, IHDR, tEXt, CRC) so spec checks do not go through
// Navigator's own PNG code.
//
// Thread-safety: RecordingRunner is internally synchronized (preview workers call run()
// concurrently); everything else is test-thread only.
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/mime.hpp>
#include <navigator/platform.hpp>
#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>
#include <navigator/thumbnail.hpp>
#include <navigator/types.hpp>

extern char** environ; // NOLINT(readability-redundant-declaration): posix_spawn needs it

namespace navigator::integration_test {

inline constexpr std::chrono::seconds kPreviewWait{30};

// ---- External tools ---------------------------------------------------------------------------

[[nodiscard]] inline bool have_program(std::string_view name) {
    return platform::find_program(name).has_value();
}

// Runs argv (no shell) with stdin/stdout/stderr on /dev/null; returns the exit status or -1.
[[nodiscard]] inline int run_tool(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& a : argv) {
        args.push_back(
            const_cast<char*>(a.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    ::posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    ::posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        return -1;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// One frame of ffmpeg's test pattern (`lavfi` source) written to `out` (format by extension).
[[nodiscard]] inline bool ffmpeg_image(const Path& out, std::uint32_t w, std::uint32_t h,
                                       std::string_view source = "testsrc") {
    return run_tool({"ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                     std::string(source) + "=size=" + std::to_string(w) + "x" + std::to_string(h),
                     "-frames:v", "1", out.string()}) == 0;
}

// A short clip (`seconds` at 10 fps) of the test pattern; codec chosen by the container.
[[nodiscard]] inline bool ffmpeg_clip(const Path& out, std::uint32_t w, std::uint32_t h,
                                      int seconds) {
    return run_tool({"ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                     "testsrc=size=" + std::to_string(w) + "x" + std::to_string(h) + ":rate=10",
                     "-t", std::to_string(seconds), "-pix_fmt", "yuv420p", out.string()}) == 0;
}

// ---- Files ------------------------------------------------------------------------------------

[[nodiscard]] inline std::vector<std::uint8_t> read_bytes(const Path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

inline void write_bytes(const Path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(
                bytes.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            static_cast<std::streamsize>(bytes.size()));
}

inline void write_text_file(const Path& p, std::string_view text, mode_t mode = 0644) {
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    (void)::chmod(p.c_str(), mode);
}

[[nodiscard]] inline mode_t mode_of(const Path& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
        return 0;
    }
    return st.st_mode & 07777U;
}

// Moves the file's mtime by `seconds` (whole seconds: Thumb::MTime granularity).
inline bool shift_mtime(const Path& p, int seconds) {
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) {
        return false;
    }
    std::array<timespec, 2> ts{};
    ts[0] = st.st_atim;
    ts[1].tv_sec = st.st_mtim.tv_sec + seconds;
    ts[1].tv_nsec = 0;
    return ::utimensat(AT_FDCWD, p.c_str(), ts.data(), 0) == 0;
}

// Regular files below `dir` (recursive), relative paths.
[[nodiscard]] inline std::vector<std::string> files_below(const Path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return out;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            out.push_back(std::filesystem::relative(it->path(), dir, ec).string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// md5 of `text` by the system md5sum (independent of thumbnail::md5_hex); "" when unavailable.
[[nodiscard]] inline std::string system_md5(const Path& scratch, std::string_view text) {
    const Path in = scratch / "md5-input";
    const Path out = scratch / "md5-output";
    write_text_file(in, text);
    const std::string cmd = "md5sum < '" + in.string() + "' > '" + out.string() + "'";
    if (run_tool({"sh", "-c", cmd}) != 0) {
        return {};
    }
    const std::vector<std::uint8_t> bytes = read_bytes(out);
    std::string hex(bytes.begin(), bytes.end());
    hex = hex.substr(0, std::min<std::size_t>(hex.find(' '), hex.size()));
    std::filesystem::remove(in);
    std::filesystem::remove(out);
    return hex.size() == 32 ? hex : std::string();
}

// ---- PNG chunks (independent reader) -----------------------------------------------------------

[[nodiscard]] inline std::uint32_t crc32_of(const std::uint8_t* data, std::size_t n,
                                            std::uint32_t crc = 0xFFFFFFFFU) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
            }
            t[i] = c;
        }
        return t;
    }();
    for (std::size_t i = 0; i < n; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc;
}

[[nodiscard]] inline std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24U) | (std::uint32_t{p[1]} << 16U) |
           (std::uint32_t{p[2]} << 8U) | std::uint32_t{p[3]};
}

inline void put_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24U));
    out.push_back(static_cast<std::uint8_t>(v >> 16U));
    out.push_back(static_cast<std::uint8_t>(v >> 8U));
    out.push_back(static_cast<std::uint8_t>(v));
}

inline constexpr std::array<std::uint8_t, 8> kPngSignature{0x89, 'P',  'N',  'G',
                                                           0x0D, 0x0A, 0x1A, 0x0A};

struct PngChunks {
    bool signature = false;
    bool crc_ok = true;
    bool complete = false; // reached IEND
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t color_type = 0;
    std::vector<std::string> order;          // chunk types in file order
    std::map<std::string, std::string> text; // tEXt keyword -> value
    std::size_t text_before_idat = 0;        // tEXt chunks before the first IDAT

    // The tEXt value of `keyword`, "" when absent.
    [[nodiscard]] std::string key(const std::string& keyword) const {
        const auto it = text.find(keyword);
        return it == text.end() ? std::string() : it->second;
    }
};

[[nodiscard]] inline PngChunks parse_png(const std::vector<std::uint8_t>& d) {
    PngChunks out;
    if (d.size() < 8 || !std::equal(kPngSignature.begin(), kPngSignature.end(), d.begin())) {
        return out;
    }
    out.signature = true;
    bool idat = false;
    std::size_t i = 8;
    while (i + 12 <= d.size()) {
        const std::uint32_t n = be32(&d[i]);
        if (n > d.size() - i - 12) {
            out.crc_ok = false;
            return out;
        }
        const std::string type(d.begin() + static_cast<std::ptrdiff_t>(i + 4),
                               d.begin() + static_cast<std::ptrdiff_t>(i + 8));
        const std::uint8_t* body = &d[i + 8];
        if ((crc32_of(&d[i + 4], n + 4U) ^ 0xFFFFFFFFU) != be32(body + n)) {
            out.crc_ok = false;
        }
        out.order.push_back(type);
        if (type == "IHDR" && n >= 13) {
            out.width = be32(body);
            out.height = be32(body + 4);
            out.color_type = body[9];
        } else if (type == "tEXt") {
            const std::string chunk(body, body + n);
            const std::size_t nul = chunk.find('\0');
            if (nul != std::string::npos) {
                out.text[chunk.substr(0, nul)] = chunk.substr(nul + 1);
            }
            if (!idat) {
                ++out.text_before_idat;
            }
        } else if (type == "IDAT") {
            idat = true;
        } else if (type == "IEND") {
            out.complete = true;
            return out;
        }
        i += 12U + n;
    }
    return out;
}

[[nodiscard]] inline std::vector<std::uint8_t> png_chunk(std::string_view type,
                                                         const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> c;
    put_be32(c, static_cast<std::uint32_t>(body.size()));
    c.insert(c.end(), type.begin(), type.end());
    c.insert(c.end(), body.begin(), body.end());
    put_be32(c, crc32_of(&c[4], body.size() + 4U) ^ 0xFFFFFFFFU);
    return c;
}

// Inserts tEXt chunks right after IHDR, as GdkPixbuf's PNG saver does for the
// "tEXt::Thumb::URI" ... options gnome-desktop's thumbnail factory passes.
[[nodiscard]] inline std::vector<std::uint8_t>
add_text_chunks(const std::vector<std::uint8_t>& png,
                const std::vector<std::pair<std::string, std::string>>& keys) {
    if (png.size() < 33) {
        return png;
    }
    const std::size_t after_ihdr = 8U + 12U + be32(&png[8]);
    std::vector<std::uint8_t> out(png.begin(),
                                  png.begin() + static_cast<std::ptrdiff_t>(after_ihdr));
    for (const auto& [k, v] : keys) {
        std::vector<std::uint8_t> body(k.begin(), k.end());
        body.push_back(0);
        body.insert(body.end(), v.begin(), v.end());
        const std::vector<std::uint8_t> c = png_chunk("tEXt", body);
        out.insert(out.end(), c.begin(), c.end());
    }
    out.insert(out.end(), png.begin() + static_cast<std::ptrdiff_t>(after_ihdr), png.end());
    return out;
}

// Rewrites IHDR width / height (CRC fixed): a "huge header" fixture.
[[nodiscard]] inline std::vector<std::uint8_t> png_with_size(std::vector<std::uint8_t> png,
                                                             std::uint32_t w, std::uint32_t h) {
    if (png.size() < 33) {
        return png;
    }
    std::vector<std::uint8_t> v;
    put_be32(v, w);
    put_be32(v, h);
    std::copy(v.begin(), v.end(), png.begin() + 16);
    const std::uint32_t crc = crc32_of(&png[12], 17) ^ 0xFFFFFFFFU;
    std::vector<std::uint8_t> c;
    put_be32(c, crc);
    std::copy(c.begin(), c.end(), png.begin() + 29);
    return png;
}

// ---- Requests and the service -------------------------------------------------------------------

[[nodiscard]] inline std::optional<fs::DirEntry> entry_of(const Path& p) {
    Result<fs::DirEntry> e = fs::stat(p, false);
    if (!e) {
        return std::nullopt;
    }
    return *e;
}

[[nodiscard]] inline preview::PreviewRequest
request_for(const Path& p, preview::PreviewPurpose purpose, preview::PixelSize target) {
    preview::PreviewRequest r;
    r.path = p;
    if (std::optional<fs::DirEntry> e = entry_of(p); e) {
        r.entry = *e;
    }
    r.mime = std::string(mime::guess_for_entry(r.entry));
    r.purpose = purpose;
    r.target = target;
    return r;
}

[[nodiscard]] inline thumbnail::SourceInfo source_of(const Path& p) {
    thumbnail::SourceInfo s;
    s.path = p;
    if (Result<std::string> uri = thumbnail::file_uri(p); uri) {
        s.uri = *uri;
    }
    if (std::optional<fs::DirEntry> e = entry_of(p); e) {
        s.mtime = e->mtime;
        s.size = e->size;
        s.mime = std::string(mime::guess_for_entry(*e));
    }
    return s;
}

struct ServiceSetup {
    Path root; // empty: no disk cache
    std::size_t workers = 2;
    std::uint64_t budget = preview::kDefaultCacheBudgetBytes;
    std::vector<thumbnail::Thumbnailer> thumbnailers;
    std::shared_ptr<platform::IProcessRunner> runner;
    thumbnail::ThumbnailerLimits limits;

    // Builders (designated initializers trip -Wmissing-field-initializers on GCC 13).
    [[nodiscard]] ServiceSetup&& with_workers(std::size_t n) && {
        workers = n;
        return std::move(*this);
    }
    [[nodiscard]] ServiceSetup&& with_budget(std::uint64_t bytes) && {
        budget = bytes;
        return std::move(*this);
    }
    [[nodiscard]] ServiceSetup&& with_thumbnailers(std::vector<thumbnail::Thumbnailer> t,
                                                   std::shared_ptr<platform::IProcessRunner> r,
                                                   thumbnail::ThumbnailerLimits l = {}) && {
        thumbnailers = std::move(t);
        runner = std::move(r);
        limits = l;
        return std::move(*this);
    }
};

// A setup with the cache at `root` (empty: no disk cache).
[[nodiscard]] inline ServiceSetup cache_at(Path root) {
    ServiceSetup s;
    s.root = std::move(root);
    return s;
}

[[nodiscard]] inline std::unique_ptr<preview::IPreviewService>
make_service(const ServiceSetup& setup) {
    auto registry = std::make_shared<preview::ProviderRegistry>();
    preview::BuiltinProviderOptions bo;
    bo.thumbnailers = setup.thumbnailers;
    bo.runner = setup.runner;
    bo.thumbnailer_limits = setup.limits;
    if (!preview::register_builtin_providers(*registry, bo)) {
        return nullptr;
    }
    preview::PreviewServiceOptions so;
    so.registry = registry;
    so.worker_count = setup.workers;
    so.cache_budget_bytes = setup.budget;
    so.thumbnails.root = setup.root;
    Result<std::unique_ptr<preview::IPreviewService>> s =
        preview::create_preview_service(std::move(so));
    return s ? std::move(*s) : nullptr;
}

// Polls until every ticket in `want` completed (or the deadline). Returns completions by ticket.
[[nodiscard]] inline std::map<std::uint64_t, preview::Completion>
wait_for(preview::IPreviewService& service, const std::vector<preview::Ticket>& want,
         std::chrono::milliseconds timeout = kPreviewWait) {
    std::map<std::uint64_t, preview::Completion> got;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<preview::Completion> batch;
    while (std::chrono::steady_clock::now() < deadline) {
        batch.clear();
        (void)service.poll(batch);
        for (preview::Completion& c : batch) {
            got.insert_or_assign(c.ticket.value, std::move(c));
        }
        const bool all = std::all_of(want.begin(), want.end(), [&](const preview::Ticket& t) {
            return got.contains(t.value);
        });
        if (all) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return got;
}

// A completion that may not have arrived (a deadline passed). Like std::optional, but the
// test's REQUIRE(c.has_value()) is enough for clang-tidy's optional-access check, which does
// not see through REQUIRE.
class Delivery {
public:
    Delivery() = default;
    explicit Delivery(preview::Completion c) : c_(std::move(c)), has_(true) {}
    [[nodiscard]] bool has_value() const noexcept { return has_; }
    [[nodiscard]] const preview::Completion& operator*() const noexcept { return c_; }
    [[nodiscard]] const preview::Completion* operator->() const noexcept { return &c_; }

private:
    preview::Completion c_{{}, {}, std::shared_ptr<const preview::PreviewResult>{}};
    bool has_ = false;
};

// One request through the service; an empty Delivery on timeout.
[[nodiscard]] inline Delivery
produce_one(preview::IPreviewService& service, preview::PreviewRequest r,
            preview::RequestPriority prio = preview::RequestPriority::Inspector) {
    const preview::Ticket t = service.submit(std::move(r), prio, 1);
    if (!t.valid()) {
        return {};
    }
    std::map<std::uint64_t, preview::Completion> got = wait_for(service, {t});
    const auto it = got.find(t.value);
    if (it == got.end()) {
        return {};
    }
    return Delivery(std::move(it->second));
}

// Waits until nothing is pending or running.
[[nodiscard]] inline bool wait_idle(const preview::IPreviewService& service,
                                    std::chrono::milliseconds timeout = kPreviewWait) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const preview::ServiceStats st = service.stats();
        if (st.pending == 0 && st.running == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

[[nodiscard]] inline const preview::ImagePreview* image_of(const preview::Completion& c) {
    if (!c.result || !*c.result) {
        return nullptr;
    }
    return std::get_if<preview::ImagePreview>(&(*c.result)->content);
}

[[nodiscard]] inline const preview::PlaceholderPreview*
placeholder_of(const preview::Completion& c) {
    if (!c.result || !*c.result) {
        return nullptr;
    }
    return std::get_if<preview::PlaceholderPreview>(&(*c.result)->content);
}

// The real process runner, recording every request and result (thread-safe).
class RecordingRunner final : public platform::IProcessRunner {
public:
    explicit RecordingRunner(std::unique_ptr<platform::IProcessRunner> real)
        : real_(std::move(real)) {}

    Result<platform::ProcessResult> run(const platform::ProcessRequest& request,
                                        std::stop_token stop) override {
        Result<platform::ProcessResult> r = real_->run(request, std::move(stop));
        const std::lock_guard lock(mu_);
        requests_.push_back(request);
        results_.push_back(r);
        return r;
    }
    [[nodiscard]] std::vector<Result<platform::ProcessResult>> results() const {
        const std::lock_guard lock(mu_);
        return results_;
    }
    [[nodiscard]] std::vector<platform::ProcessRequest> requests() const {
        const std::lock_guard lock(mu_);
        return requests_;
    }

private:
    std::unique_ptr<platform::IProcessRunner> real_;
    mutable std::mutex mu_;
    std::vector<platform::ProcessRequest> requests_;
    std::vector<Result<platform::ProcessResult>> results_;
};

[[nodiscard]] inline std::shared_ptr<RecordingRunner> recording_runner() {
    Result<std::unique_ptr<platform::IProcessRunner>> real =
        platform::create_default_process_runner();
    if (!real) {
        return nullptr;
    }
    return std::make_shared<RecordingRunner>(std::move(*real));
}

// The installed thumbnailers (XDG data dirs, TryExec / Exec on PATH), as the app loads them.
[[nodiscard]] inline std::vector<thumbnail::Thumbnailer> system_thumbnailers() {
    Result<std::vector<Path>> dirs = platform::data_dirs();
    if (!dirs) {
        return {};
    }
    const std::vector<Path> tdirs =
        thumbnail::thumbnailer_dirs(Span<const Path>(dirs->data(), dirs->size()));
    Result<std::vector<thumbnail::Thumbnailer>> t = thumbnail::load_thumbnailers(
        Span<const Path>(tdirs.data(), tdirs.size()),
        [](std::string_view program) { return have_program(program); }, std::stop_token{});
    return t ? std::move(*t) : std::vector<thumbnail::Thumbnailer>{};
}

// Can bubblewrap create the namespaces the runner's probe needs on this machine?
[[nodiscard]] inline bool bwrap_works() {
    if (!have_program("bwrap")) {
        return false;
    }
    return run_tool({"bwrap",         "--unshare-all", "--die-with-parent",
                     "--new-session", "--ro-bind",     "/usr",
                     "/usr",          "--symlink",     "usr/lib",
                     "/lib",          "--symlink",     "usr/lib64",
                     "/lib64",        "--symlink",     "usr/bin",
                     "/bin",          "--proc",        "/proc",
                     "--dev",         "/dev",          "true"}) == 0;
}

// No child of this process is left (running or zombie).
[[nodiscard]] inline bool no_children() {
    int status = 0;
    errno = 0;
    return ::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD;
}

} // namespace navigator::integration_test
