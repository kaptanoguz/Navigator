// Read-only cached Git status overlays (Phase 10).
// Runs `git status --porcelain=v1 -z -b --ignored=matching` on a single background worker thread
// via platform::IProcessRunner. Never blocks the UI thread.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <navigator/git.hpp>

namespace navigator::git {

namespace {

GitFileState classify_xy(char x, char y) noexcept {
    // Unmerged / conflict states
    if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) {
        return GitFileState::Conflict;
    }
    if (x == '?' && y == '?') {
        return GitFileState::Untracked;
    }
    if (x == '!' && y == '!') {
        return GitFileState::Ignored;
    }
    if (x == 'R' || y == 'R') {
        return GitFileState::Renamed;
    }
    if (x == 'C' || y == 'C') {
        return GitFileState::Copied;
    }
    if (x == 'D' || y == 'D') {
        return GitFileState::Deleted;
    }
    if (x == 'M' || y == 'M') {
        return GitFileState::Modified;
    }
    if (x == 'A' || y == 'A') {
        return GitFileState::Added;
    }
    return GitFileState::None;
}

std::string parse_branch_header(std::string_view entry) {
    // Format: "## main...origin/main [ahead 1]" or "## No commits yet on main" or "## HEAD (no
    // branch)"
    if (entry.size() < 3 || entry.substr(0, 3) != "## ") {
        return {};
    }
    std::string_view rest = entry.substr(3);
    constexpr std::string_view kNoCommits = "No commits yet on ";
    constexpr std::string_view kInitial = "Initial commit on ";
    if (rest.starts_with(kNoCommits)) {
        rest.remove_prefix(kNoCommits.size());
    } else if (rest.starts_with(kInitial)) {
        rest.remove_prefix(kInitial.size());
    }
    const std::size_t dots = rest.find("...");
    if (dots != std::string_view::npos) {
        rest = rest.substr(0, dots);
    }
    const std::size_t space = rest.find(' ');
    if (space != std::string_view::npos && !rest.starts_with("HEAD")) {
        rest = rest.substr(0, space);
    }
    return std::string(rest);
}

void propagate_to_dirs(std::unordered_map<std::string, GitFileState>& dir_states,
                       std::string_view rel_path, GitFileState state) {
    if (state == GitFileState::None || state == GitFileState::Ignored) {
        return;
    }
    std::size_t slash = rel_path.rfind('/');
    while (slash != std::string_view::npos && slash > 0) {
        const std::string dir(rel_path.substr(0, slash));
        auto it = dir_states.find(dir);
        if (it == dir_states.end()) {
            dir_states.emplace(dir, state);
        } else {
            it->second = merge_dir_state(it->second, state);
        }
        slash = rel_path.rfind('/', slash - 1);
    }
}

std::optional<Path> find_git_root(const Path& dir) {
    std::error_code ec;
    Path cur = dir;
    if (!std::filesystem::is_directory(cur, ec)) {
        cur = cur.parent_path();
    }
    for (int depth = 0; depth < 64 && !cur.empty(); ++depth) {
        const Path git_entry = cur / ".git";
        if (std::filesystem::exists(git_entry, ec)) {
            return cur.lexically_normal();
        }
        const Path parent = cur.parent_path();
        if (parent == cur) {
            break;
        }
        cur = parent;
    }
    return std::nullopt;
}

} // namespace

GitRepoSnapshot parse_porcelain_v1_z(std::string_view stdout_bytes, const Path& repo_root) {
    GitRepoSnapshot snap;
    snap.repo_root = repo_root.lexically_normal();

    std::size_t pos = 0;
    while (pos < stdout_bytes.size()) {
        const std::size_t nul = stdout_bytes.find('\0', pos);
        const std::size_t end = (nul == std::string_view::npos) ? stdout_bytes.size() : nul;
        const std::string_view record = stdout_bytes.substr(pos, end - pos);
        pos = (nul == std::string_view::npos) ? stdout_bytes.size() : (nul + 1);

        if (record.size() < 3) {
            continue;
        }
        if (record.substr(0, 3) == "## ") {
            snap.branch = parse_branch_header(record);
            continue;
        }
        if (record.size() < 4 || record[2] != ' ') {
            continue;
        }
        const char x = record[0];
        const char y = record[1];
        std::string_view path_sv = record.substr(3);
        // For renames/copies in -z mode, porcelain emits the destination path first and then an
        // extra NUL-terminated source path; consume and skip the source path.
        if (x == 'R' || x == 'C' || y == 'R' || y == 'C') {
            const std::size_t next_nul = stdout_bytes.find('\0', pos);
            pos = (next_nul == std::string_view::npos) ? stdout_bytes.size() : (next_nul + 1);
        }
        while (!path_sv.empty() && path_sv.back() == '/') {
            path_sv.remove_suffix(1);
        }
        if (path_sv.empty()) {
            continue;
        }
        const GitFileState state = classify_xy(x, y);
        if (state == GitFileState::None) {
            continue;
        }
        const std::string rel(path_sv);
        auto it = snap.file_states.find(rel);
        if (it == snap.file_states.end()) {
            snap.file_states.emplace(rel, state);
        } else {
            it->second = merge_dir_state(it->second, state);
        }
        propagate_to_dirs(snap.dir_states, rel, state);
    }
    return snap;
}

struct GitStatusService::Impl {
    std::shared_ptr<platform::IProcessRunner> runner;
    std::unordered_map<std::string, GitRepoSnapshot> cache; // UI-thread owned

    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Path> pending_roots;
    std::unordered_set<std::string> inflight_roots;
    std::vector<GitRepoSnapshot> completed;
    std::uint64_t next_gen = 1;
    std::jthread worker;

    explicit Impl(std::shared_ptr<platform::IProcessRunner> r) : runner(std::move(r)) {
        if (!runner) {
            if (auto created = platform::create_default_process_runner(); created) {
                runner = std::shared_ptr<platform::IProcessRunner>(std::move(*created));
            }
        }
        worker = std::jthread([this](const std::stop_token& stop) { run_worker(stop); });
    }

    ~Impl() {
        worker.request_stop();
        cv.notify_all();
    }

    void run_worker(const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            Path root;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, stop, [&] { return !pending_roots.empty(); });
                if (stop.stop_requested()) {
                    break;
                }
                root = std::move(pending_roots.front());
                pending_roots.pop_front();
            }

            if (runner) {
                platform::ProcessRequest req;
                req.argv = {"git", "-C", root.native(),       "status", "--porcelain=v1",
                            "-z",  "-b", "--ignored=matching"};
                req.timeout = std::chrono::milliseconds{3000};
                req.max_capture_bytes = std::size_t{2} * 1024U * 1024U;

                auto res = runner->run(req, stop);
                if (res && res->succeeded()) {
                    GitRepoSnapshot snap = parse_porcelain_v1_z(res->stdout_text, root);
                    std::scoped_lock lock(mutex);
                    snap.generation = next_gen++;
                    completed.push_back(std::move(snap));
                    inflight_roots.erase(root.native());
                } else {
                    std::scoped_lock lock(mutex);
                    inflight_roots.erase(root.native());
                }
            } else {
                std::scoped_lock lock(mutex);
                inflight_roots.erase(root.native());
            }
        }
    }
};

GitStatusService::GitStatusService(std::shared_ptr<platform::IProcessRunner> runner)
    : impl_(std::make_unique<Impl>(std::move(runner))) {}

GitStatusService::~GitStatusService() = default;
GitStatusService::GitStatusService(GitStatusService&&) noexcept = default;
GitStatusService& GitStatusService::operator=(GitStatusService&&) noexcept = default;

void GitStatusService::request_directory(const Path& dir) {
    if (!impl_ || dir.empty()) {
        return;
    }
    const auto root = find_git_root(dir);
    if (!root) {
        return;
    }
    const std::string key = root->native();
    if (impl_->cache.contains(key)) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    if (impl_->inflight_roots.insert(key).second) {
        impl_->pending_roots.push_back(*root);
        impl_->cv.notify_one();
    }
}

void GitStatusService::invalidate(const Path& dir) {
    if (!impl_ || dir.empty()) {
        return;
    }
    const auto root = find_git_root(dir);
    if (!root) {
        return;
    }
    const std::string key = root->native();
    impl_->cache.erase(key);
    std::scoped_lock lock(impl_->mutex);
    if (impl_->inflight_roots.insert(key).second) {
        impl_->pending_roots.push_back(*root);
        impl_->cv.notify_one();
    }
}

bool GitStatusService::poll() {
    if (!impl_) {
        return false;
    }
    std::vector<GitRepoSnapshot> ready;
    {
        std::scoped_lock lock(impl_->mutex);
        ready.swap(impl_->completed);
    }
    if (ready.empty()) {
        return false;
    }
    for (auto& snap : ready) {
        const std::string key = snap.repo_root.native();
        impl_->cache[key] = std::move(snap);
    }
    return true;
}

GitFileState GitStatusService::status_for(const Path& path) const {
    if (!impl_ || path.empty() || impl_->cache.empty()) {
        return GitFileState::None;
    }
    const std::string p = path.lexically_normal().native();
    for (const auto& [root_str, snap] : impl_->cache) {
        if (p.size() <= root_str.size() + 1) {
            continue;
        }
        if (p.starts_with(root_str) && p[root_str.size()] == '/') {
            const std::string rel = p.substr(root_str.size() + 1);
            if (auto it = snap.file_states.find(rel); it != snap.file_states.end()) {
                return it->second;
            }
            if (auto it = snap.dir_states.find(rel); it != snap.dir_states.end()) {
                return it->second;
            }
            return GitFileState::None;
        }
    }
    return GitFileState::None;
}

std::optional<std::string> GitStatusService::branch_for(const Path& dir) const {
    if (!impl_ || dir.empty() || impl_->cache.empty()) {
        return std::nullopt;
    }
    const std::string p = dir.lexically_normal().native();
    for (const auto& [root_str, snap] : impl_->cache) {
        if (p == root_str ||
            (p.size() > root_str.size() && p.starts_with(root_str) && p[root_str.size()] == '/')) {
            if (!snap.branch.empty()) {
                return snap.branch;
            }
        }
    }
    return std::nullopt;
}

} // namespace navigator::git
