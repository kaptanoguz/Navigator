// src/app/bench_fixture.cpp — see bench_fixture.hpp.
#include "bench_fixture.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace navigator::app {
namespace {

constexpr std::array kStems{
    "Project report",
    "IMG_2024",
    "invoice",
    "Çalışma notları",
    "meeting-notes",
    "screenshot",
    "Ödev",
    "budget",
    "backup",
    "Şirket sunumu",
    "track",
    "a rather long file name that "
    "will need an ellipsis in the list",
    "data",
    "Ağustos fotoğrafları",
    "build-output",
    "README",
};
constexpr std::array kExts{"pdf", "jpg", "txt", "md",   "png",  "zip", "tar.gz", "cpp",
                           "mp3", "mkv", "odt", "xlsx", "json", "deb", "",       "log"};

} // namespace

std::vector<std::string> bench_fixture_names(int count) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(count > 0 ? count : 0));
    std::array<char, 32> num{};
    for (int i = 0; i < count; ++i) {
        std::snprintf(num.data(), num.size(), "%05d", i);
        std::string name;
        if (i % 20 == 0) {
            name = std::string("Folder ") + num.data();
        } else {
            name =
                std::string(kStems[static_cast<std::size_t>(i) % kStems.size()]) + " " + num.data();
            const char* ext = kExts[static_cast<std::size_t>(i / 3) % kExts.size()];
            if (*ext != '\0') {
                name += '.';
                name += ext;
            }
        }
        out.push_back(std::move(name));
    }
    return out;
}

namespace {

Result<BenchFixture> materialize(const std::vector<std::string>& names,
                                 const std::vector<bool>& is_dir) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded at startup.
    const char* tmp = std::getenv("TMPDIR");
    std::string templ =
        std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") + "/navigator-bench-XXXXXX";
    if (mkdtemp(templ.data()) == nullptr) {
        return fail(ErrorCode::IoError, std::string("mkdtemp failed: ") + std::strerror(errno));
    }
    BenchFixture fx;
    fx.root = templ;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string path = templ + "/" + names[i];
        int rc = 0;
        if (is_dir[i]) {
            rc = ::mkdir(path.c_str(), 0755);
        } else {
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            rc = fd < 0 ? -1 : ::close(fd);
        }
        if (rc != 0) {
            const std::string why = std::strerror(errno);
            remove_bench_fixture(fx);
            std::string message = "creating '";
            message += path;
            message += "' failed: ";
            message += why;
            return fail(ErrorCode::IoError, std::move(message));
        }
        fx.names.push_back(names[i]);
        fx.is_dir.push_back(is_dir[i]);
    }
    return fx;
}

} // namespace

Result<BenchFixture> create_bench_fixture(int count) {
    const std::vector<std::string> names = bench_fixture_names(count);
    std::vector<bool> is_dir(names.size(), false);
    for (std::size_t i = 0; i < names.size(); i += 20) {
        is_dir[i] = true;
    }
    return materialize(names, is_dir);
}

std::vector<std::string> bench_tree_names(int count, std::vector<bool>& is_dir) {
    constexpr int kPerFolder = 64;
    constexpr int kSubfolders = 8;
    is_dir.clear();
    std::vector<std::string> out;
    std::vector<std::string> folders{std::string()}; // relative, with a trailing '/'
    int left = count;
    for (std::size_t f = 0; f < folders.size() && left > 0; ++f) {
        const std::string parent = folders[f]; // a copy: `folders` grows below
        for (int k = 0; k < kPerFolder && left > 0; ++k, --left) {
            const bool dir = k < kSubfolders;
            std::string name = parent;
            if (dir) {
                name += "d" + std::to_string(k);
                folders.push_back(name + "/");
            } else {
                const auto at = static_cast<std::size_t>(k);
                name += kStems[at % kStems.size()];
                name += ' ';
                name += std::to_string(k);
                if (const char* ext = kExts[at % kExts.size()]; *ext != '\0') {
                    name += '.';
                    name += ext;
                }
            }
            out.push_back(std::move(name));
            is_dir.push_back(dir);
        }
    }
    return out;
}

Result<BenchFixture> create_bench_tree(int count) {
    std::vector<bool> is_dir;
    const std::vector<std::string> names = bench_tree_names(count, is_dir);
    return materialize(names, is_dir);
}

void remove_bench_fixture(const BenchFixture& fixture) noexcept {
    if (fixture.root.empty()) {
        return;
    }
    const std::string root = fixture.root.string();
    // Children before their folders; only names this run created.
    for (std::size_t i = fixture.names.size(); i-- > 0;) {
        const std::string path = root + "/" + fixture.names[i];
        if (fixture.is_dir[i]) {
            (void)::rmdir(path.c_str());
        } else {
            (void)::unlink(path.c_str());
        }
    }
    (void)::rmdir(root.c_str());
}

} // namespace navigator::app
