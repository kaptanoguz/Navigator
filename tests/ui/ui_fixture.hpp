// Fixture directories for browser tests: a fresh mkdtemp() directory under $TMPDIR (or /tmp),
// removed recursively when the fixture goes out of scope. Tests create, rename and delete files
// only inside it.
#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

namespace navigator::test {

class TempDir {
public:
    TempDir() {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): tests read TMPDIR on one thread.
        const char* tmp = std::getenv("TMPDIR");
        std::string templ =
            std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") + "/navigator-ui-XXXXXX";
        if (mkdtemp(templ.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = templ;
    }
    ~TempDir() {
        std::error_code ec;
        // Restore permissions so removal can descend into locked test folders.
        for (auto it = std::filesystem::recursive_directory_iterator(
                 path_, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec) && !it->is_symlink(ec)) {
                ::chmod(it->path().c_str(), 0755);
            }
        }
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    std::filesystem::path dir(std::string_view rel) const {
        const auto p = path_ / rel;
        std::filesystem::create_directories(p);
        return p;
    }
    std::filesystem::path file(std::string_view rel, std::string_view content = {}) const {
        const auto p = path_ / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << content;
        return p;
    }
    std::filesystem::path link(std::string_view rel, const std::filesystem::path& target) const {
        const auto p = path_ / rel;
        std::filesystem::create_symlink(target, p);
        return p;
    }

private:
    std::filesystem::path path_;
};

} // namespace navigator::test
