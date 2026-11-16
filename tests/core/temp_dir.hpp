// tests/core/temp_dir.hpp: RAII temporary fixture directory for navigator_core tests.
//
// Creates <temp_directory_path()>/navigator-test-<random>; the destructor restores owner
// permissions on everything below it (tests chmod 000 on purpose) and removes the tree.
// All helpers take raw byte names (std::string) so invalid UTF-8 names can be created.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace navigator::test {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32U) ^ rd());
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 16; ++attempt) {
            const std::filesystem::path p =
                base / ("navigator-test-" + std::to_string(gen() & 0xFFFFFFFFFFFFULL));
            if (::mkdir(p.c_str(), 0700) == 0) {
                path_ = p;
                return;
            }
        }
        throw std::runtime_error("TempDir: cannot create a temporary directory");
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ec;
        restore_permissions(path_);
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path operator/(std::string_view name) const {
        return path_ / std::string(name);
    }

    // Creates (or truncates) a regular file with `size` bytes of 'x'.
    std::filesystem::path file(std::string_view name, std::size_t size = 0) const {
        const std::filesystem::path p = *this / name;
        const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            throw std::runtime_error("TempDir::file: open failed for " + p.native());
        }
        const std::string data(size, 'x');
        std::size_t done = 0;
        while (done < size) {
            const ssize_t n = ::write(fd, data.data() + done, size - done);
            if (n <= 0) {
                ::close(fd);
                throw std::runtime_error("TempDir::file: write failed");
            }
            done += static_cast<std::size_t>(n);
        }
        ::close(fd);
        return p;
    }

    std::filesystem::path dir(std::string_view name) const {
        const std::filesystem::path p = *this / name;
        if (::mkdir(p.c_str(), 0755) != 0) {
            throw std::runtime_error("TempDir::dir: mkdir failed for " + p.native());
        }
        return p;
    }

    // Creates `name` -> `target` (target text stored verbatim).
    std::filesystem::path link(std::string_view name, std::string_view target) const {
        const std::filesystem::path p = *this / name;
        if (::symlink(std::string(target).c_str(), p.c_str()) != 0) {
            throw std::runtime_error("TempDir::link: symlink failed for " + p.native());
        }
        return p;
    }

private:
    static void restore_permissions(const std::filesystem::path& root) noexcept {
        std::error_code ec;
        ::chmod(root.c_str(), 0700);
        std::filesystem::recursive_directory_iterator it(root, ec);
        const std::filesystem::recursive_directory_iterator end;
        while (!ec && it != end) {
            if (it->is_directory(ec) && !it->is_symlink(ec)) {
                ::chmod(it->path().c_str(), 0700);
            }
            it.increment(ec);
        }
    }

    std::filesystem::path path_;
};

[[nodiscard]] inline bool running_as_root() noexcept { return ::geteuid() == 0; }

} // namespace navigator::test
