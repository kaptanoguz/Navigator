// tests/integration/ops_support.hpp: shared helpers of navigator_ops_integration_tests (owner: qa).
//
// SAFETY (CLAUDE.md rule 9, ops.hpp S16). These tests MUTATE files, so they are dangerous too:
//   - every path the engine, the trash or the queue sees lies in a Fixture (mkdtemp under
//     temp_directory_path(), or under /dev/shm for second-filesystem cases);
//   - the registry of live fixture roots is checked by require_inside(): a path outside every
//     root ABORTS the process (std::abort) before anything happens;
//   - Rig wraps the trash in GuardedTrash, installs an I/O hook (every path the engine writes,
//     renames or unlinks) and a fault_injector guard (every entry it processes), and every
//     request is checked before the engine sees it;
//   - "forbidden" roots (a symlink-trap sentinel tree) are the opposite guard: the engine must
//     never name a path inside one of them, which proves a link was not followed;
//   - the real $HOME, the real XDG trash and real volumes never appear: the trash is created
//     with TrashOptions::home_trash inside the fixture and a fake mount table.
//
// Catch2 v3 assertions are not thread-safe: worker threads record into plain structs.
#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <navigator/ops.hpp>
#include <navigator/trash.hpp>
#include <navigator/volume.hpp>

#include "sys.hpp" // src/ops: the documented I/O fault hook seam (tests/ops uses it too)

namespace navigator::opsit {

namespace stdfs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// Fixture registry and guard
// ---------------------------------------------------------------------------------------------

struct Registry {
    std::mutex mu;
    std::vector<Path> roots;
    std::vector<Path> forbidden;
};
inline Registry& registry() {
    static Registry r;
    return r;
}

[[noreturn]] inline void die(const std::string& why) {
    std::fprintf(stderr, "\n*** ops integration SAFETY GUARD: %s\n", why.c_str());
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] inline bool lexically_inside(const Path& p, const Path& root) {
    for (const auto& c : p) {
        if (c == "..") {
            return false; // never let normalization hide an escape
        }
    }
    return ops::is_same_or_inside(p, root);
}

// Aborts when `p` is empty-but-required, relative, not inside a live fixture, or inside a
// forbidden root.
inline void require_inside(const Path& p, const char* what = "path") {
    if (p.empty()) {
        return; // callers that allow "no path" pass nothing; an empty destination is unused
    }
    if (!p.is_absolute()) {
        die(std::string(what) + " is not absolute: " + p.native());
    }
    Registry& r = registry();
    const std::lock_guard<std::mutex> lock(r.mu);
    for (const Path& f : r.forbidden) {
        if (lexically_inside(p, f)) {
            die(std::string(what) + " inside a forbidden (sentinel) tree: " + p.native());
        }
    }
    for (const Path& root : r.roots) {
        if (lexically_inside(p, root)) {
            return;
        }
    }
    die(std::string(what) + " escapes every fixture: " + p.native());
}

inline void require_inside(const trash::TrashedItem& item) {
    require_inside(item.dir.root, "trash root");
    if (!item.dir.topdir.empty()) {
        require_inside(item.dir.topdir, "trash topdir");
    }
    require_inside(item.files_path(), "trashed files path");
    require_inside(item.original_path, "trashed original path");
}

// ---------------------------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------------------------

class Fixture {
public:
    enum class Where : std::uint8_t { Temp, Shm };

    explicit Fixture(Where where = Where::Temp) {
        const Path parent = where == Where::Shm ? Path("/dev/shm") : stdfs::temp_directory_path();
        std::string pattern = (parent / "navigator-opsit-XXXXXX").native();
        if (::mkdtemp(pattern.data()) == nullptr) {
            die("mkdtemp failed under " + parent.native());
        }
        std::error_code ec;
        root_ = stdfs::canonical(pattern, ec);
        if (ec) {
            root_ = pattern;
        }
        const std::lock_guard<std::mutex> lock(registry().mu);
        registry().roots.push_back(root_);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    ~Fixture() {
        make_removable(root_);
        std::error_code ec;
        if (root_.filename().native().starts_with("navigator-opsit-")) { // only what we made
            stdfs::remove_all(root_, ec);
        }
        const std::lock_guard<std::mutex> lock(registry().mu);
        std::erase(registry().roots, root_);
    }

    // True when /dev/shm is writable and on another filesystem than `other`.
    [[nodiscard]] static bool shm_is_other_filesystem(const Path& other) {
        struct stat a {};
        struct stat b {};
        if (::stat("/dev/shm", &a) != 0 || ::stat(other.c_str(), &b) != 0 ||
            ::access("/dev/shm", W_OK) != 0) {
            return false;
        }
        return a.st_dev != b.st_dev;
    }

    [[nodiscard]] const Path& root() const noexcept { return root_; }
    [[nodiscard]] Path operator/(std::string_view rel) const { return root_ / std::string(rel); }

    Path file(std::string_view rel, std::string_view content = {}) const {
        const Path p = root_ / std::string(rel);
        require_inside(p);
        stdfs::create_directories(p.parent_path());
        write_raw(p, content);
        return p;
    }
    Path dir(std::string_view rel) const {
        const Path p = root_ / std::string(rel);
        require_inside(p);
        stdfs::create_directories(p);
        return p;
    }
    Path link(std::string_view rel, const std::string& target) const {
        const Path p = root_ / std::string(rel);
        require_inside(p);
        stdfs::create_directories(p.parent_path());
        if (::symlink(target.c_str(), p.c_str()) != 0) {
            die("symlink failed: " + p.native());
        }
        return p;
    }
    Path fifo(std::string_view rel) const {
        const Path p = root_ / std::string(rel);
        require_inside(p);
        stdfs::create_directories(p.parent_path());
        if (::mkfifo(p.c_str(), 0644) != 0) {
            die("mkfifo failed: " + p.native());
        }
        return p;
    }

    static void write_raw(const Path& p, std::string_view content) {
        const int fd =
            ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) {
            die("open for write failed: " + p.native());
        }
        std::size_t off = 0;
        while (off < content.size()) {
            const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
            if (n <= 0) {
                ::close(fd);
                die("write failed: " + p.native());
            }
            off += static_cast<std::size_t>(n);
        }
        ::close(fd);
    }

private:
    // Read-only folders must become deletable again before remove_all.
    static void make_removable(const Path& root) {
        std::error_code ec;
        for (auto it = stdfs::recursive_directory_iterator(
                 root, stdfs::directory_options::skip_permission_denied, ec);
             !ec && it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code inner;
            if (it->is_directory(inner) && !it->is_symlink(inner)) {
                stdfs::permissions(it->path(), stdfs::perms::owner_all, stdfs::perm_options::add,
                                   inner);
            }
        }
        std::error_code top;
        stdfs::permissions(root, stdfs::perms::owner_all, stdfs::perm_options::add, top);
    }

    Path root_;
};

// A sentinel tree that the engine must never name (a symlink in another fixture points at it).
class Forbidden {
public:
    explicit Forbidden(const Path& root) : root_(root) {
        const std::lock_guard<std::mutex> lock(registry().mu);
        registry().forbidden.push_back(root_);
    }
    Forbidden(const Forbidden&) = delete;
    Forbidden& operator=(const Forbidden&) = delete;
    Forbidden(Forbidden&&) = delete;
    Forbidden& operator=(Forbidden&&) = delete;
    ~Forbidden() {
        const std::lock_guard<std::mutex> lock(registry().mu);
        std::erase(registry().forbidden, root_);
    }

private:
    Path root_;
};

// ---------------------------------------------------------------------------------------------
// File helpers (raw syscalls, never following links)
// ---------------------------------------------------------------------------------------------

[[nodiscard]] inline bool present(const Path& p) {
    struct stat st {};
    return ::lstat(p.c_str(), &st) == 0;
}
[[nodiscard]] inline struct stat lstat_of(const Path& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
        st.st_mode = 0;
    }
    return st;
}
[[nodiscard]] inline std::string read_file(const Path& p) {
    std::string out;
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return out;
    }
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

// 64-bit FNV-1a of a regular file's bytes (0 when unreadable): the "checksum" of the brief.
[[nodiscard]] inline std::uint64_t hash_file(const Path& p) {
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return 0;
    }
    std::uint64_t h = 1469598103934665603ULL;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
        for (ssize_t i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;
        }
    }
    ::close(fd);
    return h;
}

// Deterministic digest of a tree: relative names, kinds, permission bits, sizes, file content
// hashes, link targets; never follows links, never opens FIFOs. Equal digests = equal trees.
// `ignore_part` skips engine temp files (".*.navigator-*.part").
[[nodiscard]] inline std::string tree_digest(const Path& root, bool ignore_part = false) {
    std::vector<std::string> lines;
    std::vector<Path> stack{root};
    while (!stack.empty()) {
        const Path dir = std::move(stack.back());
        stack.pop_back();
        std::error_code ec;
        std::vector<Path> children;
        for (auto it = stdfs::directory_iterator(dir, ec); !ec && it != stdfs::directory_iterator();
             it.increment(ec)) {
            children.push_back(it->path());
        }
        for (const Path& c : children) {
            const std::string name = c.filename().native();
            if (ignore_part && name.starts_with(".") && name.ends_with(".part") &&
                name.find(".navigator-") != std::string::npos) {
                continue;
            }
            const struct stat st = lstat_of(c);
            std::string line = c.lexically_relative(root).native();
            char meta[96];
            if (S_ISDIR(st.st_mode)) {
                std::snprintf(meta, sizeof meta, " D %04o",
                              static_cast<unsigned>(st.st_mode & 07777));
                line += meta;
                stack.push_back(c);
            } else if (S_ISLNK(st.st_mode)) {
                char buf[4096];
                const ssize_t n = ::readlink(c.c_str(), buf, sizeof buf);
                line += " L " + std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0U);
            } else if (S_ISREG(st.st_mode)) {
                std::snprintf(meta, sizeof meta, " F %04o %llu %016llx",
                              static_cast<unsigned>(st.st_mode & 07777),
                              static_cast<unsigned long long>(st.st_size),
                              static_cast<unsigned long long>(hash_file(c)));
                line += meta;
            } else {
                std::snprintf(meta, sizeof meta, " O %07o", static_cast<unsigned>(st.st_mode));
                line += meta;
            }
            lines.push_back(std::move(line));
        }
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const std::string& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

// One top-level item's digest: tree digest for a folder, content hash + mode for a file, the
// target for a link. Empty string when absent.
[[nodiscard]] inline std::string item_digest(const Path& p) {
    const struct stat st = lstat_of(p);
    if (S_ISDIR(st.st_mode)) {
        return "D\n" + tree_digest(p);
    }
    if (S_ISLNK(st.st_mode)) {
        char buf[4096];
        const ssize_t n = ::readlink(p.c_str(), buf, sizeof buf);
        return "L " + std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0U);
    }
    if (S_ISREG(st.st_mode)) {
        char meta[96];
        std::snprintf(meta, sizeof meta, "F %04o %llu %016llx",
                      static_cast<unsigned>(st.st_mode & 07777),
                      static_cast<unsigned long long>(st.st_size),
                      static_cast<unsigned long long>(hash_file(p)));
        return meta;
    }
    return st.st_mode == 0 ? std::string() : "O";
}

// Paths below `root` that look like engine temp files (".<name>.navigator-<random>.part").
[[nodiscard]] inline std::vector<Path> part_files(const Path& root) {
    std::vector<Path> out;
    std::error_code ec;
    for (auto it = stdfs::recursive_directory_iterator(
             root, stdfs::directory_options::skip_permission_denied, ec);
         !ec && it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().native();
        if (name.starts_with(".") && name.ends_with(".part") &&
            name.find(".navigator-") != std::string::npos) {
            out.push_back(it->path());
        }
    }
    return out;
}

[[nodiscard]] inline std::size_t count_entries(const Path& root) {
    std::size_t n = 0;
    std::error_code ec;
    for (auto it = stdfs::recursive_directory_iterator(root, ec);
         !ec && it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------
// Fake mount table (only fixture mount points) and the guarded real trash
// ---------------------------------------------------------------------------------------------

class MountTable final : public IMountSource {
public:
    explicit MountTable(std::string text = {}) : text_(std::move(text)) {}
    Result<std::string> read_mountinfo() override { return text_; }
    Result<SpaceInfo> query_space(const Path&) override {
        return fail(ErrorCode::NotFound, "no fake space");
    }

private:
    std::string text_;
};

[[nodiscard]] inline std::string mount_line(int id, const Path& mount_point,
                                            const std::string& opts = "rw") {
    require_inside(mount_point, "mount point");
    return std::to_string(id) + " 1 8:" + std::to_string(id) + " / " + mount_point.native() + " " +
           opts + ",relatime - ext4 /dev/fake" + std::to_string(id) + " " + opts + "\n";
}

// Delegates to the real trash after checking every path it is given or returns.
class GuardedTrash final : public trash::ITrash {
public:
    explicit GuardedTrash(std::shared_ptr<trash::ITrash> inner) : inner_(std::move(inner)) {}

    Result<trash::TrashDir> trash_dir_for(const Path& path) override {
        require_inside(path, "trash_dir_for");
        return inner_->trash_dir_for(path);
    }
    Result<trash::TrashedItem> trash(const Path& path) override {
        require_inside(path, "trash()");
        ++trash_calls;
        auto r = inner_->trash(path);
        if (r) {
            require_inside(*r);
        }
        return r;
    }
    Result<std::vector<trash::TrashDir>> trash_dirs() override { return inner_->trash_dirs(); }
    Result<trash::TrashListing> list(std::stop_token stop) override {
        auto r = inner_->list(stop);
        if (r) {
            for (const trash::TrashedItem& i : r->items) {
                require_inside(i);
            }
        }
        return r;
    }
    Result<void> restore(const trash::TrashedItem& item, const Path& destination) override {
        require_inside(item);
        require_inside(destination, "restore destination");
        ++restore_calls;
        return inner_->restore(item, destination);
    }
    Result<void> forget(const trash::TrashedItem& item) override {
        require_inside(item);
        ++forget_calls;
        return inner_->forget(item);
    }
    [[nodiscard]] bool is_inside_trash(const Path& path) const override {
        return inner_->is_inside_trash(path);
    }

    std::atomic<int> trash_calls{0};
    std::atomic<int> restore_calls{0};
    std::atomic<int> forget_calls{0};

private:
    std::shared_ptr<trash::ITrash> inner_;
};

// ---------------------------------------------------------------------------------------------
// Rig: engine options over one fixture with the real trash
// ---------------------------------------------------------------------------------------------

struct Rig {
    // `mount_table`: mountinfo text of the fake volumes (fixture mount points only).
    // `topdir`: per-volume trashes allowed (T4); false: the home trash only.
    explicit Rig(Fixture& fixture, std::string mount_table = {}, bool with_trash = true,
                 bool topdir = true)
        : fx(fixture), home_trash(fixture.root() / "data" / "Trash") {
        require_inside(home_trash, "home trash");
        if (with_trash) {
            trash::TrashOptions o;
            o.home_trash = home_trash;
            o.uid = static_cast<std::uint32_t>(::getuid());
            o.volumes.source = std::make_shared<MountTable>(std::move(mount_table));
            o.volumes.sysfs_root = fx.root() / "no-sys";
            o.volumes.devfs_root = fx.root() / "no-dev";
            o.volumes.home_dir = fx.root() / "no-home";
            o.volumes.query_space = false;
            o.volumes.include_hidden = true;
            o.use_topdir_trashes = topdir;
            auto made = trash::create_trash(std::move(o));
            if (!made) {
                die("create_trash failed: " + made.error().message);
            }
            trash = std::make_shared<GuardedTrash>(*made);
            engine.trash = trash;
        }
        engine.protected_paths.home = fx.root() / "home";
        engine.protected_paths.trash_roots.push_back(home_trash);
        engine.copy_chunk_bytes = 64U * 1024U; // fine cancel granularity
        engine.fault_injector = [this](const Path& p) {
            require_inside(p, "fault_injector entry");
            return !user_inject || user_inject(p);
        };
        ops::detail::set_io_fault_hook(
            [this](ops::detail::IoSite site, const Path& p, std::uint64_t off) -> int {
                require_inside(p, "io hook path");
                return user_io ? user_io(site, p, off) : 0;
            });
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;
    ~Rig() {
        ops::detail::set_io_fault_hook({});
        ops::detail::set_free_space_hook({});
    }

    Fixture& fx;
    Path home_trash;
    std::shared_ptr<GuardedTrash> trash;
    ops::EngineOptions engine;
    // Set BEFORE running; called from engine threads (make them thread-safe).
    std::function<bool(const Path&)> user_inject;
    std::function<int(ops::detail::IoSite, const Path&, std::uint64_t)> user_io;

    // Items currently in the trash (names in files/ of the home trash).
    [[nodiscard]] std::vector<std::string> trash_names() const {
        std::vector<std::string> names;
        std::error_code ec;
        for (auto it = stdfs::directory_iterator(home_trash / "files", ec);
             !ec && it != stdfs::directory_iterator(); it.increment(ec)) {
            names.push_back(it->path().filename().native());
        }
        std::sort(names.begin(), names.end());
        return names;
    }
    [[nodiscard]] std::vector<std::string> info_names() const {
        std::vector<std::string> names;
        std::error_code ec;
        for (auto it = stdfs::directory_iterator(home_trash / "info", ec);
             !ec && it != stdfs::directory_iterator(); it.increment(ec)) {
            names.push_back(it->path().filename().native());
        }
        std::sort(names.begin(), names.end());
        return names;
    }
};

// Checks every path of a request (sources, destination, trashed items, undo record).
inline void guard_request(const ops::OpRequest& r) {
    for (const Path& s : r.sources) {
        require_inside(s, "request source");
    }
    require_inside(r.destination, "request destination");
    for (const trash::TrashedItem& t : r.trashed) {
        require_inside(t);
    }
    if (r.record) {
        for (const ops::UndoStep& s : r.record->steps) {
            require_inside(s.before, "record before");
            require_inside(s.after.path, "record after");
            if (s.trashed) {
                require_inside(*s.trashed);
            }
            if (s.replaced) {
                require_inside(*s.replaced);
            }
            for (const ops::ItemGuard& g : s.created_parents) {
                require_inside(g.path, "record created parent");
            }
        }
    }
}

[[nodiscard]] inline Result<ops::OpResult> run(Rig& rig, const ops::OpRequest& request,
                                               const ops::ConflictResolver& resolve = {},
                                               std::stop_token stop = {},
                                               const ops::ProgressSink& progress = {}) {
    guard_request(request);
    return ops::run_operation(request, rig.engine, resolve, progress, stop);
}

// ---- request builders -----------------------------------------------------------------------

[[nodiscard]] inline ops::OpRequest copy_req(std::vector<Path> sources, Path dest,
                                             ops::ConflictPolicy p = ops::ConflictPolicy::Ask) {
    ops::OpRequest r;
    r.kind = ops::OpKind::Copy;
    r.sources = std::move(sources);
    r.destination = std::move(dest);
    r.conflict = p;
    return r;
}
[[nodiscard]] inline ops::OpRequest move_req(std::vector<Path> sources, Path dest,
                                             ops::ConflictPolicy p = ops::ConflictPolicy::Ask) {
    ops::OpRequest r = copy_req(std::move(sources), std::move(dest), p);
    r.kind = ops::OpKind::Move;
    return r;
}
[[nodiscard]] inline ops::OpRequest trash_req(std::vector<Path> sources) {
    ops::OpRequest r;
    r.kind = ops::OpKind::Trash;
    r.sources = std::move(sources);
    return r;
}
[[nodiscard]] inline ops::OpRequest delete_req(std::vector<Path> sources, bool confirmed = true) {
    ops::OpRequest r;
    r.kind = ops::OpKind::DeletePermanently;
    r.sources = std::move(sources);
    r.confirmed = confirmed;
    return r;
}
[[nodiscard]] inline ops::OpRequest rename_req(Path source, std::string name) {
    ops::OpRequest r;
    r.kind = ops::OpKind::Rename;
    r.sources = {std::move(source)};
    r.new_name = std::move(name);
    return r;
}
[[nodiscard]] inline ops::OpRequest restore_req(std::vector<trash::TrashedItem> items,
                                                ops::ConflictPolicy p = ops::ConflictPolicy::Ask) {
    ops::OpRequest r;
    r.kind = ops::OpKind::Restore;
    r.trashed = std::move(items);
    r.conflict = p;
    return r;
}
[[nodiscard]] inline ops::OpRequest delete_trashed_req(std::vector<trash::TrashedItem> items,
                                                       bool confirmed = true) {
    ops::OpRequest r;
    r.kind = ops::OpKind::DeletePermanently;
    r.trashed = std::move(items);
    r.confirmed = confirmed;
    return r;
}
[[nodiscard]] inline ops::OpRequest undo_req(const ops::UndoRecord& rec,
                                             ops::ConflictPolicy p = ops::ConflictPolicy::Ask) {
    ops::OpRequest r;
    r.kind = ops::OpKind::Undo;
    r.origin = ops::OpOrigin::Undo;
    r.record = std::make_shared<const ops::UndoRecord>(rec);
    r.conflict = p;
    return r;
}
[[nodiscard]] inline ops::OpRequest redo_req(const ops::UndoRecord& rec,
                                             ops::ConflictPolicy p = ops::ConflictPolicy::Ask) {
    ops::OpRequest r = undo_req(rec, p);
    r.kind = ops::OpKind::Redo;
    r.origin = ops::OpOrigin::Redo;
    return r;
}

// The undo record of a result; a missing one fails the test (exception) instead of a bad access.
[[nodiscard]] inline const ops::UndoRecord& rec(const ops::OpResult& r) {
    if (!r.record.has_value()) {
        throw std::runtime_error("the operation recorded nothing undoable");
    }
    return *r.record;
}

[[nodiscard]] inline ErrorCode code_of(const ops::ItemOutcome& o) {
    return o.error ? o.error->code : ErrorCode::Unknown;
}

// Every trashed item of the real trash, via the service (for building restore requests).
[[nodiscard]] inline std::vector<trash::TrashedItem> list_trash(Rig& rig) {
    auto l = rig.trash->list(std::stop_token{});
    return l ? l->items : std::vector<trash::TrashedItem>{};
}

// Runs a bounded busy wait for `pred` (the tests poll queues and clocks; never unbounded).
template <typename Pred> [[nodiscard]] bool wait_until(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace navigator::opsit
