// Shared helpers for tests/platform: RAII temporary directories and small file utilities.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <navigator/types.hpp>

namespace navigator::platform_test {

// mkdtemp() directory under $TMPDIR (or /tmp), removed recursively on destruction.
class TempDir {
public:
    TempDir() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "navigator-platform-XXXXXX").native();
        if (::mkdtemp(pattern.data()) == nullptr) {
            std::abort();
        }
        // Canonical, so paths compare equal to what the kernel and realpath() report.
        path_ = std::filesystem::canonical(pattern);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ec;
        // Restore permissions a test may have removed, then delete everything.
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        for (auto it = std::filesystem::recursive_directory_iterator(path_, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code inner;
            if (it->is_directory(inner) && !it->is_symlink(inner)) {
                std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::add, inner);
            }
        }
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const Path& path() const noexcept { return path_; }
    [[nodiscard]] Path operator/(std::string_view rel) const { return path_ / rel; }

private:
    Path path_;
};

inline void write_file(const Path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Sets or unsets one environment variable for a scope and restores the old value afterwards.
class ScopedEnv {
public:
    explicit ScopedEnv(const char* name) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_ = old;
        }
    }
    ScopedEnv(const char* name, const std::optional<std::string>& value) : ScopedEnv(name) {
        if (value) {
            set(*value);
        } else {
            unset();
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;
    ~ScopedEnv() {
        if (old_) {
            ::setenv(name_, old_->c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }
    void set(const std::string& value) const { ::setenv(name_, value.c_str(), 1); }
    void unset() const { ::unsetenv(name_); }

private:
    const char* name_;
    std::optional<std::string> old_;
};

} // namespace navigator::platform_test
