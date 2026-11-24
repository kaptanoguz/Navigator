// Phase 10 cancellable content search (navigator/content_search.hpp).
// Skips binary files (NUL byte in first 8 KiB), enforces max_file_bytes, honours std::stop_token.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <navigator/content_search.hpp>
#include <navigator/fs.hpp>

namespace navigator::search {

namespace {

constexpr std::size_t kBinarySniffBytes = 8192;

std::string fold_ascii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char ch : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::size_t find_match(std::string_view haystack, std::string_view needle, bool case_sensitive,
                       const std::string& folded_needle) {
    if (needle.empty()) {
        return std::string_view::npos;
    }
    if (case_sensitive) {
        return haystack.find(needle);
    }
    const std::string folded_line = fold_ascii(haystack);
    return folded_line.find(folded_needle);
}

std::string trim_snippet(std::string_view line, std::size_t match_pos, std::size_t match_len,
                         std::size_t max_len, std::uint32_t& out_col) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    std::size_t lead = 0;
    while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t') && lead < match_pos) {
        ++lead;
    }
    line.remove_prefix(lead);
    match_pos -= lead;

    if (max_len == 0 || line.size() <= max_len) {
        out_col = static_cast<std::uint32_t>(match_pos);
        return std::string(line);
    }

    const std::size_t half = max_len / 2;
    std::size_t start = 0;
    if (match_pos > half) {
        start = match_pos - half;
    }
    if (start + max_len > line.size()) {
        start = line.size() > max_len ? (line.size() - max_len) : 0;
    }
    const std::size_t len = std::min(max_len, line.size() - start);
    (void)match_len;
    out_col = static_cast<std::uint32_t>(match_pos - start);
    return std::string(line.substr(start, len));
}

bool is_binary_prefix(std::ifstream& in) {
    std::array<char, kBinarySniffBytes> buf{};
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    in.clear();
    in.seekg(0, std::ios::beg);
    for (std::size_t i = 0; i < got; ++i) {
        if (buf[i] == '\0') {
            return true;
        }
    }
    return false;
}

} // namespace

Result<bool> search_file_content(const Path& file_path, std::string_view rel_path,
                                 const ContentSearchOptions& options,
                                 std::vector<ContentMatch>& out, std::stop_token stop) {
    try {
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "content_search: cancelled");
        }
        if (options.query.empty()) {
            return false;
        }
        std::error_code ec;
        const auto fsize = std::filesystem::file_size(file_path, ec);
        if (ec) {
            return false;
        }
        if (options.max_file_bytes > 0 && fsize > options.max_file_bytes) {
            return false;
        }
        std::ifstream in(file_path, std::ios::binary);
        if (!in.is_open()) {
            return false;
        }
        if (options.skip_binary && is_binary_prefix(in)) {
            return false;
        }

        const std::string folded_needle =
            options.case_sensitive ? std::string{} : fold_ascii(options.query);
        std::string line;
        std::uint32_t line_no = 0;
        std::size_t file_matches = 0;

        while (std::getline(in, line)) {
            ++line_no;
            if ((line_no & 0x3FFU) == 0U && stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "content_search: cancelled");
            }
            const std::size_t pos =
                find_match(line, options.query, options.case_sensitive, folded_needle);
            if (pos != std::string_view::npos) {
                ContentMatch m;
                m.file_path = file_path;
                m.rel_path = std::string(rel_path);
                m.line_number = line_no;
                m.match_length = static_cast<std::uint32_t>(options.query.size());
                m.line_snippet = trim_snippet(line, pos, options.query.size(),
                                              options.max_line_snippet, m.column);
                out.push_back(std::move(m));
                ++file_matches;
                if (options.max_matches_per_file > 0 &&
                    file_matches >= options.max_matches_per_file) {
                    break;
                }
            }
        }
        return true;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "search_file_content: out of memory");
    }
}

struct ContentSearchSession::Impl {
    mutable std::mutex mutex;
    std::vector<ContentMatch> pending;
    ContentSearchStats stats;
    std::atomic<bool> running{false};
    std::jthread worker;

    ~Impl() { stop_worker(); }

    void stop_worker() noexcept {
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
        running.store(false, std::memory_order_release);
    }

    void run(ContentSearchOptions opts, std::stop_token stop) {
        std::error_code ec;
        std::vector<Path> stack;
        stack.push_back(opts.root);

        while (!stack.empty() && !stop.stop_requested()) {
            const Path dir = std::move(stack.back());
            stack.pop_back();

            fs::ListOptions lopt;
            lopt.show_hidden = opts.include_hidden;
            lopt.follow_symlinks = false;
            auto entries = fs::list_directory(dir, lopt, stop);
            if (!entries) {
                continue;
            }
            for (const auto& entry : *entries) {
                if (stop.stop_requested()) {
                    break;
                }
                const Path full = dir / entry.name;
                if (entry.kind == fs::EntryKind::Directory) {
                    if (!opts.include_hidden && entry.name == ".git") {
                        continue;
                    }
                    stack.push_back(full);
                    continue;
                }
                if (entry.kind != fs::EntryKind::File) {
                    continue;
                }
                if (opts.max_file_bytes > 0 && entry.size > opts.max_file_bytes) {
                    std::scoped_lock lock(mutex);
                    ++stats.files_skipped_large;
                    continue;
                }

                std::string rel = full.native();
                const std::string& root_str = opts.root.native();
                if (rel.size() > root_str.size() && rel.starts_with(root_str)) {
                    rel.erase(0, root_str.size());
                    if (!rel.empty() && rel.front() == '/') {
                        rel.erase(0, 1);
                    }
                }

                std::vector<ContentMatch> file_hits;
                auto scanned = search_file_content(full, rel, opts, file_hits, stop);
                if (!scanned) {
                    if (scanned.error().code == ErrorCode::Cancelled) {
                        break;
                    }
                    continue;
                }

                std::scoped_lock lock(mutex);
                if (*scanned) {
                    ++stats.files_scanned;
                } else {
                    ++stats.files_skipped_binary;
                }
                for (auto& hit : file_hits) {
                    if (opts.max_matches > 0 && stats.matches_found >= opts.max_matches) {
                        stats.truncated = true;
                        break;
                    }
                    pending.push_back(std::move(hit));
                    ++stats.matches_found;
                }
                if (stats.truncated) {
                    break;
                }
            }
            {
                std::scoped_lock lock(mutex);
                if (stats.truncated) {
                    break;
                }
            }
        }

        std::scoped_lock lock(mutex);
        stats.cancelled = stop.stop_requested();
        stats.done = true;
        running.store(false, std::memory_order_release);
    }
};

ContentSearchSession::ContentSearchSession() : impl_(std::make_unique<Impl>()) {}
ContentSearchSession::~ContentSearchSession() = default;
ContentSearchSession::ContentSearchSession(ContentSearchSession&&) noexcept = default;
ContentSearchSession& ContentSearchSession::operator=(ContentSearchSession&&) noexcept = default;

void ContentSearchSession::start(ContentSearchOptions options) {
    if (!impl_) {
        return;
    }
    impl_->stop_worker();
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->pending.clear();
        impl_->stats = ContentSearchStats{};
        if (options.query.empty() || options.root.empty()) {
            impl_->stats.done = true;
            return;
        }
        impl_->stats.done = false;
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->worker = std::jthread([impl = impl_.get(), opts = std::move(options)](
                                     std::stop_token stop) mutable {
        impl->run(std::move(opts), std::move(stop));
    });
}

void ContentSearchSession::cancel() noexcept {
    if (!impl_) {
        return;
    }
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
    }
}

ContentSearchPoll ContentSearchSession::poll() {
    ContentSearchPoll out;
    if (!impl_) {
        return out;
    }
    std::scoped_lock lock(impl_->mutex);
    out.new_matches.swap(impl_->pending);
    out.stats = impl_->stats;
    return out;
}

bool ContentSearchSession::active() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

} // namespace navigator::search
