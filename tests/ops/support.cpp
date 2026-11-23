// tests/ops/support.cpp: see support.hpp.
#include "support.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <system_error>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace navigator::ops::test {
namespace {

constexpr std::string_view kPrefix = "navigator-ops-test-";

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}
std::vector<Path>& registry() {
    static std::vector<Path> roots;
    return roots;
}

[[noreturn]] void die(const std::string& why) {
    std::fprintf(stderr, "\n*** tests/ops SAFETY GUARD: %s\n", why.c_str());
    std::fflush(stderr);
    std::abort();
}

[[nodiscard]] bool lexically_inside(const Path& p, const Path& root) {
    // Reject ".." outright: lexical normalization could otherwise hide an escape.
    for (const auto& c : p) {
        if (c == "..") {
            return false;
        }
    }
    return is_same_or_inside(p, root);
}

detail::IoFaultHook& test_hook() {
    static detail::IoFaultHook h;
    return h;
}
std::mutex& hook_mutex() {
    static std::mutex m;
    return m;
}

// Installed once at startup: every path the engine writes, creates or unlinks is checked.
const bool g_guard_installed = [] {
    detail::set_io_fault_hook([](detail::IoSite site, const Path& p, std::uint64_t off) -> int {
        require_inside_fixture(p);
        detail::IoFaultHook h;
        {
            const std::lock_guard lock(hook_mutex());
            h = test_hook();
        }
        return h ? h(site, p, off) : 0;
    });
    return true;
}();

void make_parents(const Path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
}

void digest_into(const Path& dir, const std::string& rel, std::string& out) {
    std::vector<std::string> names;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
        out += "!unreadable " + rel + "\n";
        return;
    }
    while (const dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") {
            names.push_back(n);
        }
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        const Path p = dir / n;
        std::string r = rel;
        if (!r.empty()) {
            r += '/';
        }
        r += n;
        struct stat st {};
        if (::lstat(p.c_str(), &st) != 0) {
            continue;
        }
        char mode[16];
        std::snprintf(mode, sizeof mode, "%04o", static_cast<unsigned>(st.st_mode & 07777));
        if (S_ISDIR(st.st_mode)) {
            out.append("d ").append(r).append(" ").append(mode).append("\n");
            digest_into(p, r, out);
        } else if (S_ISLNK(st.st_mode)) {
            std::string target(4096, '\0');
            const ssize_t len = ::readlink(p.c_str(), target.data(), target.size());
            target.resize(len > 0 ? static_cast<std::size_t>(len) : 0);
            out.append("l ").append(r).append(" -> ").append(target).append("\n");
        } else if (S_ISREG(st.st_mode)) {
            const std::string data = read_file(p);
            std::uint64_t h = 1469598103934665603ULL;
            for (const char c : data) {
                h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
            }
            out += "f " + r + " " + mode + " " + std::to_string(data.size()) + " " +
                   std::to_string(h) + "\n";
        } else {
            out += "o " + r + " " + mode + "\n";
        }
    }
}

void collect_parts(const Path& dir, std::vector<Path>& out) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
        return;
    }
    std::vector<std::string> names;
    while (const dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") {
            names.push_back(n);
        }
    }
    ::closedir(d);
    for (const std::string& n : names) {
        const Path p = dir / n;
        if (n.find(".navigator-") != std::string::npos && n.size() > 5 &&
            n.compare(n.size() - 5, 5, ".part") == 0) {
            out.push_back(p);
        }
        struct stat st {};
        if (::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            collect_parts(p, out);
        }
    }
}

} // namespace

void require_inside_fixture(const Path& p) {
    (void)g_guard_installed;
    if (p.empty() || !p.is_absolute()) {
        die("relative or empty path handed to the engine: '" + p.native() + "'");
    }
    const std::lock_guard lock(registry_mutex());
    for (const Path& root : registry()) {
        if (lexically_inside(p, root)) {
            return;
        }
    }
    die("path outside every test fixture: '" + p.native() + "'");
}

// ---------------------------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------------------------

bool Fixture::shm_is_other_filesystem() {
    struct stat a {};
    struct stat b {};
    const Path tmp = std::filesystem::temp_directory_path();
    if (::stat(tmp.c_str(), &a) != 0 || ::stat("/dev/shm", &b) != 0 || !S_ISDIR(b.st_mode) ||
        ::access("/dev/shm", W_OK) != 0) {
        return false;
    }
    return a.st_dev != b.st_dev;
}

Fixture::Fixture(Where where) {
    const Path base =
        where == Where::Shm ? Path("/dev/shm") : std::filesystem::temp_directory_path();
    std::string tmpl = (base / (std::string(kPrefix) + "XXXXXX")).native();
    if (::mkdtemp(tmpl.data()) == nullptr) {
        throw std::runtime_error("mkdtemp failed under " + base.native());
    }
    std::unique_ptr<char, decltype(&std::free)> real(::realpath(tmpl.c_str(), nullptr), &std::free);
    if (!real) {
        throw std::runtime_error("realpath failed");
    }
    root_ = Path(real.get());
    // Paranoia: the root must be a fresh, private directory named like a fixture, directly
    // inside the base, never $HOME or the repository.
    struct stat st {};
    if (::lstat(root_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() ||
        !root_.filename().native().starts_with(kPrefix)) {
        die("fixture root is not a private temp directory: " + root_.native());
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        if (is_same_or_inside(Path(home), root_)) {
            die("fixture root contains $HOME: " + root_.native());
        }
    }
    const std::lock_guard lock(registry_mutex());
    registry().push_back(root_);
}

Fixture::~Fixture() {
    {
        const std::lock_guard lock(registry_mutex());
        std::erase(registry(), root_);
    }
    std::error_code ec;
    // Owner permissions back on (tests chmod 000 / 0500 on purpose), then remove. remove_all
    // never follows symlinks.
    ::chmod(root_.c_str(), 0700);
    for (auto it = std::filesystem::recursive_directory_iterator(root_, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) {
            ::chmod(it->path().c_str(), 0700);
        }
    }
    std::filesystem::remove_all(root_, ec);
}

Path Fixture::file(std::string_view rel, std::string_view content) const {
    const Path p = *this / rel;
    require_inside_fixture(p);
    make_parents(p);
    write_file(p, content);
    return p;
}

Path Fixture::dir(std::string_view rel) const {
    const Path p = *this / rel;
    require_inside_fixture(p);
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    if (ec) {
        throw std::runtime_error("create_directories failed: " + p.native());
    }
    return p;
}

Path Fixture::link(std::string_view rel, std::string_view target) const {
    const Path p = *this / rel;
    require_inside_fixture(p);
    make_parents(p);
    if (::symlink(std::string(target).c_str(), p.c_str()) != 0) {
        throw std::runtime_error("symlink failed: " + p.native());
    }
    return p;
}

std::string read_file(const Path& p) {
    std::string out;
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return "<unreadable>";
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

void write_file(const Path& p, std::string_view content) {
    require_inside_fixture(p);
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        throw std::runtime_error("open failed: " + p.native() + ": " + std::strerror(errno));
    }
    std::size_t done = 0;
    while (done < content.size()) {
        const ssize_t n = ::write(fd, content.data() + done, content.size() - done);
        if (n <= 0) {
            ::close(fd);
            throw std::runtime_error("write failed: " + p.native());
        }
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
}

bool present(const Path& p) {
    struct stat st {};
    return ::lstat(p.c_str(), &st) == 0;
}

struct stat lstat_of(const Path& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
        throw std::runtime_error("lstat failed: " + p.native());
    }
    return st;
}

std::string tree_digest(const Path& root) {
    std::string out;
    digest_into(root, "", out);
    return out;
}

std::vector<Path> part_files(const Path& root) {
    std::vector<Path> out;
    collect_parts(root, out);
    return out;
}

std::size_t count_entries(const Path& root) {
    std::size_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        ++n;
    }
    return n;
}

ScopedIoHook::ScopedIoHook(detail::IoFaultHook hook) {
    const std::lock_guard lock(hook_mutex());
    test_hook() = std::move(hook);
}

ScopedIoHook::~ScopedIoHook() {
    const std::lock_guard lock(hook_mutex());
    test_hook() = nullptr;
}

ScopedFreeSpace::ScopedFreeSpace(std::uint64_t bytes) {
    detail::set_free_space_hook(
        [bytes](const Path&) -> std::optional<std::uint64_t> { return bytes; });
}

ScopedFreeSpace::~ScopedFreeSpace() { detail::set_free_space_hook(nullptr); }

// ---------------------------------------------------------------------------------------------
// FakeTrash
// ---------------------------------------------------------------------------------------------

FakeTrash::FakeTrash(Path root) {
    require_inside_fixture(root);
    std::error_code ec;
    std::filesystem::create_directories(root / "files", ec);
    std::filesystem::create_directories(root / "info", ec);
    dir_.kind = trash::TrashDirKind::Home;
    dir_.root = std::move(root);
    struct stat st {};
    if (::stat(dir_.root.c_str(), &st) == 0) {
        dir_.device = st.st_dev;
    }
}

void FakeTrash::set_unsupported(std::vector<Path> prefixes) {
    const std::lock_guard lock(mu_);
    unsupported_ = std::move(prefixes);
}

bool FakeTrash::unsupported(const Path& p) const {
    for (const Path& u : unsupported_) {
        if (is_same_or_inside(p, u)) {
            return true;
        }
    }
    return false;
}

std::size_t FakeTrash::item_count() const {
    std::size_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir_.root / "files", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++n;
    }
    return n;
}

Result<trash::TrashDir> FakeTrash::trash_dir_for(const Path& path) {
    require_inside_fixture(path);
    const std::lock_guard lock(mu_);
    if (unsupported(path)) {
        return fail(ErrorCode::Unsupported, "no trash on this filesystem (fake)");
    }
    struct stat st {};
    const Path probe = ::lstat(path.c_str(), &st) == 0 ? path : path.parent_path();
    if (::lstat(probe.c_str(), &st) == 0 && st.st_dev != dir_.device) {
        return fail(ErrorCode::Unsupported, "other filesystem (fake trash is single-device)");
    }
    return dir_;
}

Result<trash::TrashedItem> FakeTrash::trash(const Path& path) {
    require_inside_fixture(path);
    ++trash_calls;
    const std::lock_guard lock(mu_);
    if (unsupported(path)) {
        return fail(ErrorCode::Unsupported, "no trash on this filesystem (fake)");
    }
    if (is_same_or_inside(path, dir_.root)) {
        return fail(ErrorCode::InvalidArgument, "inside the trash");
    }
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) {
        return fail(ErrorCode::NotFound, "no such item: " + path.native());
    }
    if (st.st_dev != dir_.device) {
        return fail(ErrorCode::Unsupported, "other filesystem (T1)");
    }
    const std::string base = path.filename().native();
    for (int n = 1; n < 10000; ++n) {
        const std::string name = n == 1 ? base : base + "." + std::to_string(n);
        const Path info = dir_.info_dir() / (name + ".trashinfo");
        const int fd = ::open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            return fail(ErrorCode::IoError, "cannot reserve info file");
        }
        const std::string text = "[Trash Info]\nPath=" + path.native() + "\n";
        (void)!::write(fd, text.data(), text.size());
        ::close(fd);
        const Path files = dir_.files_dir() / name;
        if (::renameat2(AT_FDCWD, path.c_str(), AT_FDCWD, files.c_str(), RENAME_NOREPLACE) != 0) {
            const int err = errno;
            ::unlink(info.c_str()); // T5
            if (err == EEXIST) {
                continue;
            }
            return fail(detail::code_from_errno(err), "rename into trash failed");
        }
        trash::TrashedItem item;
        item.dir = dir_;
        item.name = name;
        item.original_path = path;
        struct stat ts {};
        ::lstat(files.c_str(), &ts);
        item.kind = detail::kind_of(ts.st_mode);
        item.size = S_ISDIR(ts.st_mode) ? 0 : static_cast<std::uint64_t>(ts.st_size);
        item.mtime = detail::to_timestamp(ts.st_mtim);
        item.id = detail::id_of(ts);
        return item;
    }
    return fail(ErrorCode::IoError, "no free trash name");
}

Result<std::vector<trash::TrashDir>> FakeTrash::trash_dirs() {
    return std::vector<trash::TrashDir>{dir_};
}

Result<trash::TrashListing> FakeTrash::list(std::stop_token /*stop*/) {
    const std::lock_guard lock(mu_);
    trash::TrashListing out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir_.files_dir(), ec)) {
        trash::TrashedItem item;
        item.dir = dir_;
        item.name = e.path().filename().native();
        struct stat st {};
        ::lstat(e.path().c_str(), &st);
        item.kind = detail::kind_of(st.st_mode);
        item.size = S_ISDIR(st.st_mode) ? 0 : static_cast<std::uint64_t>(st.st_size);
        item.mtime = detail::to_timestamp(st.st_mtim);
        item.id = detail::id_of(st);
        const std::string info = read_file(item.info_path());
        if (const auto pos = info.find("Path="); pos != std::string::npos) {
            const auto end = info.find('\n', pos);
            item.original_path =
                info.substr(pos + 5, end == std::string::npos ? end : end - pos - 5);
        }
        out.items.push_back(std::move(item));
    }
    return out;
}

Result<void> FakeTrash::restore(const trash::TrashedItem& item, const Path& destination) {
    require_inside_fixture(destination);
    ++restore_calls;
    const std::lock_guard lock(mu_);
    const Path files = item.files_path();
    require_inside_fixture(files);
    struct stat st {};
    if (::lstat(files.c_str(), &st) != 0) {
        return fail(ErrorCode::NotFound, "trashed item gone");
    }
    if (!(detail::id_of(st) == item.id)) {
        return fail(ErrorCode::InvalidArgument, "identity changed since listing");
    }
    struct stat pst {};
    if (::stat(destination.parent_path().c_str(), &pst) != 0) {
        return fail(ErrorCode::NotFound, "destination folder missing");
    }
    if (::renameat2(AT_FDCWD, files.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) !=
        0) {
        const int err = errno;
        if (err == EEXIST) {
            return fail(ErrorCode::AlreadyExists, "destination exists");
        }
        if (err == EXDEV) {
            return fail(ErrorCode::CrossDevice, "other filesystem");
        }
        return fail(detail::code_from_errno(err), "restore failed");
    }
    ::unlink(item.info_path().c_str());
    return ok();
}

Result<void> FakeTrash::forget(const trash::TrashedItem& item) {
    const std::lock_guard lock(mu_);
    require_inside_fixture(item.files_path());
    struct stat st {};
    if (::lstat(item.files_path().c_str(), &st) == 0) {
        return fail(ErrorCode::InvalidArgument, "files entry still exists");
    }
    if (::unlink(item.info_path().c_str()) != 0) {
        return fail(ErrorCode::NotFound, "info already gone");
    }
    return ok();
}

bool FakeTrash::is_inside_trash(const Path& path) const {
    return is_same_or_inside(path, dir_.root);
}

// ---------------------------------------------------------------------------------------------
// Env / run
// ---------------------------------------------------------------------------------------------

Env::Env(Fixture& fixture, bool with_trash) : fx(fixture) {
    if (with_trash) {
        trash = std::make_shared<FakeTrash>(fixture / "trash");
        engine.trash = trash;
        engine.protected_paths.trash_roots.push_back(trash->root());
    }
    engine.copy_chunk_bytes = std::size_t{64} * 1024;
}

void guard_request(const OpRequest& r) {
    for (const Path& s : r.sources) {
        require_inside_fixture(s);
    }
    if (!r.destination.empty()) {
        require_inside_fixture(r.destination);
    }
    for (const trash::TrashedItem& t : r.trashed) {
        require_inside_fixture(t.files_path());
        if (!t.original_path.empty()) {
            require_inside_fixture(t.original_path);
        }
    }
    if (r.record) {
        for (const UndoStep& s : r.record->steps) {
            if (!s.before.empty()) {
                require_inside_fixture(s.before);
            }
            if (!s.after.path.empty()) {
                require_inside_fixture(s.after.path);
            }
            if (s.trashed) {
                require_inside_fixture(s.trashed->files_path());
            }
            if (s.replaced) {
                require_inside_fixture(s.replaced->files_path());
            }
        }
    }
}

Result<OpResult> run(Env& env, const OpRequest& request, const ConflictResolver& resolve,
                     std::stop_token stop, std::function<bool(const Path&)> inject,
                     const ProgressSink& progress) {
    guard_request(request);
    EngineOptions eo = env.engine;
    eo.fault_injector = [inject = std::move(inject)](const Path& p) {
        require_inside_fixture(p);
        return inject ? inject(p) : true;
    };
    return run_operation(request, eo, resolve, progress, std::move(stop));
}

OpRequest copy_req(std::vector<Path> sources, Path dest, ConflictPolicy policy) {
    OpRequest r;
    r.kind = OpKind::Copy;
    r.sources = std::move(sources);
    r.destination = std::move(dest);
    r.conflict = policy;
    return r;
}

OpRequest move_req(std::vector<Path> sources, Path dest, ConflictPolicy policy) {
    OpRequest r = copy_req(std::move(sources), std::move(dest), policy);
    r.kind = OpKind::Move;
    return r;
}

OpRequest rename_req(Path source, std::string new_name, ConflictPolicy policy) {
    OpRequest r;
    r.kind = OpKind::Rename;
    r.sources = {std::move(source)};
    r.new_name = std::move(new_name);
    r.conflict = policy;
    return r;
}

OpRequest mkdir_req(Path parent, std::string name, ConflictPolicy policy) {
    OpRequest r;
    r.kind = OpKind::MakeDirectory;
    r.destination = std::move(parent);
    r.new_name = std::move(name);
    r.conflict = policy;
    return r;
}

OpRequest trash_req(std::vector<Path> sources) {
    OpRequest r;
    r.kind = OpKind::Trash;
    r.sources = std::move(sources);
    return r;
}

OpRequest delete_req(std::vector<Path> sources) {
    OpRequest r;
    r.kind = OpKind::DeletePermanently;
    r.sources = std::move(sources);
    r.confirmed = true;
    return r;
}

OpRequest undo_req(const UndoRecord& record, ConflictPolicy policy) {
    OpRequest r;
    r.kind = OpKind::Undo;
    r.origin = OpOrigin::Undo;
    r.record = std::make_shared<const UndoRecord>(record);
    r.conflict = policy;
    return r;
}

OpRequest redo_req(const UndoRecord& record, ConflictPolicy policy) {
    OpRequest r = undo_req(record, policy);
    r.kind = OpKind::Redo;
    r.origin = OpOrigin::Redo;
    return r;
}

ErrorCode code_of(const ItemOutcome& o) { return o.error ? o.error->code : ErrorCode::Unknown; }

const Error& error_of(const ItemOutcome& o) {
    if (!o.error) {
        throw std::runtime_error("outcome has no error");
    }
    return *o.error;
}

const UndoRecord& record_of(const OpResult& r) {
    if (!r.record) {
        throw std::runtime_error("result has no undo record");
    }
    return *r.record;
}

const trash::TrashedItem& trashed_of(const UndoStep& s) {
    if (!s.trashed) {
        throw std::runtime_error("step has no trashed item");
    }
    return *s.trashed;
}

const trash::TrashedItem& replaced_of(const UndoStep& s) {
    if (!s.replaced) {
        throw std::runtime_error("step has no replaced item");
    }
    return *s.replaced;
}

ConflictResolver Answer::resolver() {
    return [this](const Conflict& c) {
        seen.push_back(c);
        return decision;
    };
}

} // namespace navigator::ops::test
