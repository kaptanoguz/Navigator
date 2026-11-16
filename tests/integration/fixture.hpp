// tests/integration/fixture.hpp: shared helpers for navigator_integration_tests (owner: qa).
//
// Every test works in a fresh mkdtemp() directory under temp_directory_path(), canonicalized,
// and removes it on destruction (CLAUDE.md rule 9). Helpers use raw syscalls and ignore races on
// purpose: these tests mutate directories from several threads at once.
//
// Catch2 v3 assertions are not thread-safe: worker threads only record into plain structs; the
// test thread asserts after join().
#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>
#include <navigator/watch.hpp>

namespace navigator::integration_test {

class TempDir {
public:
    TempDir() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "navigator-integration-XXXXXX").native();
        if (::mkdtemp(pattern.data()) == nullptr) {
            std::abort();
        }
        path_ = std::filesystem::canonical(pattern);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const Path& path() const noexcept { return path_; }
    [[nodiscard]] Path operator/(std::string_view rel) const { return path_ / rel; }

private:
    Path path_;
};

// Creates an empty regular file (O_WRONLY, so inotify also reports IN_CLOSE_WRITE). Returns
// false on any error (the directory may be gone in a race).
inline bool touch(const Path& path) noexcept {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    return true;
}

// Error codes list_directory() documents for the directory itself (fs.hpp).
[[nodiscard]] constexpr bool is_documented_list_error(ErrorCode code) noexcept {
    return code == ErrorCode::InvalidArgument || code == ErrorCode::NotFound ||
           code == ErrorCode::PermissionDenied || code == ErrorCode::OutOfMemory ||
           code == ErrorCode::IoError;
}

// Codes a per-entry metadata_error may carry (same mapping as fs::stat()).
[[nodiscard]] constexpr bool is_documented_metadata_error(ErrorCode code) noexcept {
    return code == ErrorCode::InvalidArgument || code == ErrorCode::NotFound ||
           code == ErrorCode::PermissionDenied || code == ErrorCode::OutOfMemory ||
           code == ErrorCode::IoError;
}

// A valid listed name: non-empty, not "." / "..", no '/' or NUL.
[[nodiscard]] inline bool is_sane_name(std::string_view name) noexcept {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

// Names of a fresh listing (hidden included), or nullopt on error.
[[nodiscard]] inline std::optional<std::set<std::string>> list_names(const Path& dir) {
    fs::ListOptions options;
    options.show_hidden = true;
    Result<std::vector<fs::DirEntry>> listed = fs::list_directory(dir, options);
    if (!listed) {
        return std::nullopt;
    }
    std::set<std::string> names;
    for (const fs::DirEntry& entry : *listed) {
        names.insert(entry.name);
    }
    return names;
}

// Name model of one watched directory, kept in sync from WatchEvents. Events that were already
// reflected by the listing are replayed harmlessly: each name's state is decided by its last
// event, and the watch is added before the first listing, so no change can fall in between.
class DirModel {
public:
    explicit DirModel(Path dir) : dir_(std::move(dir)) {}

    // Re-lists the directory (initial state and after Overflow). False if listing failed.
    bool relist() {
        std::optional<std::set<std::string>> names = list_names(dir_);
        if (!names) {
            return false;
        }
        names_ = std::move(*names);
        ++relists_;
        return true;
    }

    // Applies one event for `handle`; events of other handles are ignored. Returns false for an
    // event that this model cannot apply (relist failed, or the watched dir is gone).
    bool apply(const WatchEvent& event, WatchHandle handle) {
        if (event.kind == WatchEventKind::Overflow) {
            ++overflows_;
            return relist();
        }
        if (event.handle != handle) {
            return true;
        }
        switch (event.kind) {
        case WatchEventKind::Created:
        case WatchEventKind::MovedTo: names_.insert(event.name); break;
        case WatchEventKind::Deleted:
        case WatchEventKind::MovedFrom: names_.erase(event.name); break;
        case WatchEventKind::Renamed:
            names_.erase(event.old_name);
            names_.insert(event.name);
            break;
        case WatchEventKind::WatchedDirGone: gone_ = true; return false;
        case WatchEventKind::Modified:
        case WatchEventKind::Attrib:
        case WatchEventKind::Overflow: break;
        }
        return true;
    }

    [[nodiscard]] const std::set<std::string>& names() const noexcept { return names_; }
    [[nodiscard]] int overflows() const noexcept { return overflows_; }
    [[nodiscard]] int relists() const noexcept { return relists_; }
    [[nodiscard]] bool gone() const noexcept { return gone_; }

private:
    Path dir_;
    std::set<std::string> names_;
    int overflows_ = 0;
    int relists_ = 0;
    bool gone_ = false;
};

} // namespace navigator::integration_test
