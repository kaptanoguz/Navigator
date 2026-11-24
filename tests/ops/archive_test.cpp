#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/archive.hpp>

#include "support.hpp"

namespace {

using navigator::Path;

// Writes a minimal POSIX USTAR tar archive containing (name, content) pairs.
void write_ustar_archive(const Path& tar_path,
                         const std::vector<std::pair<std::string, std::string>>& files) {
    std::ofstream out(tar_path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());

    for (const auto& [name, content] : files) {
        std::array<char, 512> hdr{};
        std::snprintf(hdr.data(), 100, "%s", name.c_str());
        std::snprintf(hdr.data() + 100, 8, "%07o", 0644U);
        std::snprintf(hdr.data() + 108, 8, "%07o", 0U);
        std::snprintf(hdr.data() + 116, 8, "%07o", 0U);
        std::snprintf(hdr.data() + 124, 12, "%011lo",
                      static_cast<unsigned long>(content.size()));
        std::snprintf(hdr.data() + 136, 12, "%011lo", 1700000000UL);
        std::memset(hdr.data() + 148, ' ', 8);
        hdr[156] = '0'; // regular file
        std::memcpy(hdr.data() + 257, "ustar", 6);
        std::memcpy(hdr.data() + 263, "00", 2);

        unsigned int sum = 0;
        for (char c : hdr) {
            sum += static_cast<unsigned char>(c);
        }
        std::snprintf(hdr.data() + 148, 7, "%06o", sum);
        hdr[155] = ' ';

        out.write(hdr.data(), 512);
        if (!content.empty()) {
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            const std::size_t rem = content.size() % 512;
            if (rem != 0) {
                std::array<char, 512> pad{};
                out.write(pad.data(), static_cast<std::streamsize>(512 - rem));
            }
        }
    }
    // Two 512-byte zero blocks terminate a tar archive.
    std::array<char, 1024> eof{};
    out.write(eof.data(), 1024);
}

} // namespace

TEST_CASE("archive path helpers parse and format virtual archive paths", "[ops][p10][archive]") {
    CHECK(navigator::archive::is_archive_name("backup.tar.gz"));
    CHECK(navigator::archive::is_archive_name("PHOTOS.ZIP"));
    CHECK(navigator::archive::is_archive_name("code.tar.xz"));
    CHECK_FALSE(navigator::archive::is_archive_name("readme.txt"));
    CHECK_FALSE(navigator::archive::is_archive_name(".zip"));

    const Path p = navigator::archive::make_archive_path("/tmp/a.zip", "/sub/dir/");
    CHECK(p == Path("/tmp/a.zip::sub/dir"));

    const auto parsed = navigator::archive::parse_archive_path(p.native());
    REQUIRE(parsed.has_value());
    CHECK(parsed->archive_file == Path("/tmp/a.zip"));
    CHECK(parsed->inner_path == "sub/dir");
    CHECK_FALSE(navigator::archive::parse_archive_path("/tmp/plain/dir").has_value());
}

TEST_CASE("archive lists virtual directories, extracts entries, and blocks Zip Slip",
          "[ops][p10][archive]") {
    if (!navigator::archive::libarchive_available()) {
        SKIP("Built without libarchive");
    }

    navigator::ops::test::Fixture fx;
    const Path tar_path = fx.root() / "sample.tar";
    write_ustar_archive(tar_path, {
                                      {"readme.txt", "hello archive\n"},
                                      {"src/app/main.cpp", "int main() {}\n"},
                                      {"../evil.txt", "should never be extracted"},
                                  });

    // Root listing synthesizes implicit "src" folder and excludes "../evil.txt".
    auto root_entries = navigator::archive::list_archive_directory(tar_path, "");
    REQUIRE(root_entries.has_value());
    REQUIRE(root_entries->size() == 2);

    // Nested listing inside "src/app"
    auto nested = navigator::archive::list_archive_directory(tar_path, "src/app");
    REQUIRE(nested.has_value());
    REQUIRE(nested->size() == 1);
    CHECK((*nested)[0].name == "main.cpp");
    CHECK((*nested)[0].kind == navigator::fs::EntryKind::File);

    // Single-entry extract
    const Path single_out = fx.root() / "out_readme.txt";
    auto ext_one = navigator::archive::extract_entry_to_file(tar_path, "readme.txt", single_out);
    REQUIRE(ext_one.has_value());
    std::ifstream in(single_out);
    std::string text;
    std::getline(in, text);
    CHECK(text == "hello archive");

    // Full extract into subdir: evil.txt must NOT escape to fx.root() / "evil.txt".
    const Path dest = fx.root() / "extracted";
    auto ext_all = navigator::archive::extract_entries_to_dir(tar_path, {}, dest);
    REQUIRE(ext_all.has_value());
    CHECK(std::filesystem::exists(dest / "readme.txt"));
    CHECK(std::filesystem::exists(dest / "src/app/main.cpp"));
    CHECK_FALSE(std::filesystem::exists(fx.root() / "evil.txt"));
    CHECK_FALSE(std::filesystem::exists(dest / "evil.txt"));
}
