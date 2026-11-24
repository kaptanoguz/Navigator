// Fixtures for the trash tests. SAFETY: every trash root, item and mount point the tests use lies
// in a temporary fixture directory; require_in_fixture() is asserted on each of them, so a bug
// can never reach the owner's real ~/.local/share/Trash or a real volume's .Trash directories.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/trash.hpp>

#include "fake_mounts.hpp"
#include "test_support.hpp"
#include "trash_internal.hpp"

namespace navigator::platform_test {

// Fixture roots created by this process (main temp fixtures and /dev/shm fixtures).
inline std::vector<Path>& fixture_roots() {
    static std::vector<Path> roots;
    return roots;
}

[[nodiscard]] inline bool in_fixture(const Path& p) {
    const Path n = trash_internal::normal_path(p);
    for (const Path& root : fixture_roots()) {
        if (trash_internal::at_or_below(n, root)) {
            return true;
        }
    }
    return false;
}

// The guard: a test path outside every fixture aborts the test run before anything happens.
inline void require_in_fixture(const Path& p) {
    INFO("path outside the test fixture: " << p.native());
    REQUIRE(in_fixture(p));
}

inline void require_in_fixture(const trash::TrashDir& d) {
    require_in_fixture(d.root);
    if (!d.topdir.empty()) {
        require_in_fixture(d.topdir);
    }
}

inline void require_in_fixture(const trash::TrashedItem& item) {
    require_in_fixture(item.dir);
    require_in_fixture(item.files_path());
    require_in_fixture(item.original_path);
}

inline void forget_fixture_root(const Path& p) {
    auto& roots = fixture_roots();
    std::erase(roots, p);
}

// A temporary fixture directory (under $TMPDIR) that registers itself with the guard.
class FixtureDir {
public:
    FixtureDir() { fixture_roots().push_back(tmp_.path()); }
    FixtureDir(const FixtureDir&) = delete;
    FixtureDir& operator=(const FixtureDir&) = delete;
    FixtureDir(FixtureDir&&) = delete;
    FixtureDir& operator=(FixtureDir&&) = delete;
    ~FixtureDir() { forget_fixture_root(tmp_.path()); }

    [[nodiscard]] const Path& path() const noexcept { return tmp_.path(); }
    [[nodiscard]] Path operator/(std::string_view rel) const { return tmp_.path() / rel; }

private:
    TempDir tmp_;
};

// A fixture directory on another filesystem (mkdtemp under `parent_dir`, normally /dev/shm),
// for the cross-filesystem and per-volume trash tests. usable() is false when `parent_dir` is
// missing, not writable or on the same filesystem as `same_as` (the test then SKIPs).
class OtherFsFixture {
public:
    OtherFsFixture(const Path& parent_dir, const Path& same_as) {
        std::string pattern = (parent_dir / "navigator-trash-XXXXXX").native();
        if (::mkdtemp(pattern.data()) == nullptr) {
            return;
        }
        std::error_code ec;
        path_ = std::filesystem::canonical(pattern, ec);
        if (ec) {
            path_ = pattern;
        }
        fixture_roots().push_back(path_);
        struct stat a {};
        struct stat b {};
        usable_ = ::stat(path_.c_str(), &a) == 0 && ::stat(same_as.c_str(), &b) == 0 &&
                  a.st_dev != b.st_dev;
    }
    OtherFsFixture(const OtherFsFixture&) = delete;
    OtherFsFixture& operator=(const OtherFsFixture&) = delete;
    OtherFsFixture(OtherFsFixture&&) = delete;
    OtherFsFixture& operator=(OtherFsFixture&&) = delete;
    ~OtherFsFixture() {
        if (path_.empty()) {
            return;
        }
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(path_, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code inner;
            if (it->is_directory(inner) && !it->is_symlink(inner)) {
                std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::add, inner);
            }
        }
        std::filesystem::remove_all(path_, ec);
        forget_fixture_root(path_);
    }

    [[nodiscard]] bool usable() const noexcept { return usable_; }
    [[nodiscard]] const Path& path() const noexcept { return path_; }
    [[nodiscard]] Path operator/(std::string_view rel) const { return path_ / rel; }

private:
    Path path_;
    bool usable_ = false;
};

[[nodiscard]] inline std::uint64_t device_of(const Path& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_dev) : 0U;
}

[[nodiscard]] inline std::uint32_t my_uid() { return static_cast<std::uint32_t>(::getuid()); }

// One mountinfo line for a fixture mount point (asserted to be inside a fixture).
[[nodiscard]] inline std::string mount_line(int id, const Path& mount_point,
                                            const std::string& fs_type = "ext4",
                                            const std::string& opts = "rw") {
    require_in_fixture(mount_point);
    return std::to_string(id) + " 1 8:" + std::to_string(id) + " / " + mount_point.native() + " " +
           opts + ",relatime - " + fs_type + " /dev/fake" + std::to_string(id) + " " + opts + "\n";
}

inline const trash::LocalDateTime kFixedDate{2026, 11, 23, 14, 5, 9};

// Trash options confined to `fixture`: home trash in <fixture>/data/Trash, a fake mount table
// (only fixture mount points), the real uid and a fixed clock.
[[nodiscard]] inline trash::TrashOptions fixture_options(const Path& fixture,
                                                         const std::string& mount_table = {}) {
    trash::TrashOptions o;
    o.home_trash = fixture / "data" / "Trash";
    require_in_fixture(o.home_trash);
    o.uid = my_uid();
    o.volumes.source = std::make_shared<FakeMountSource>(mount_table);
    o.volumes.sysfs_root = fixture / "no-sys";
    o.volumes.devfs_root = fixture / "no-dev";
    o.volumes.home_dir = fixture / "no-home";
    o.volumes.include_hidden = true; // /dev/shm fixtures would otherwise be filtered
    o.now = [] { return kFixedDate; };
    return o;
}

[[nodiscard]] inline std::shared_ptr<trash::ITrash> make_fixture_trash(trash::TrashOptions o) {
    require_in_fixture(o.home_trash);
    auto t = trash::create_trash(std::move(o));
    REQUIRE(t.has_value());
    return *t;
}

[[nodiscard]] inline std::string read_text(const Path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] inline unsigned mode_bits(const Path& p) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
        return 0xFFFFU;
    }
    return static_cast<unsigned>(st.st_mode) & 07777U;
}

[[nodiscard]] inline bool exists_nofollow(const Path& p) {
    struct stat st {};
    return ::lstat(p.c_str(), &st) == 0;
}

} // namespace navigator::platform_test
