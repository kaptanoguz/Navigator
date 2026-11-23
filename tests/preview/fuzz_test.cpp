// tests/preview/fuzz_test.cpp: deterministic mutation fuzzing of every in-tree and library
// decoder (bit flips, byte overwrites, insertions, truncation) on tiny fixtures. Run under
// ASan/UBSan in the sanitizer gate. NAVIGATOR_FUZZ_ITERATIONS overrides the per-format count.
#include <cstdlib>

#include <catch2/catch_test_macros.hpp>

#include <navigator/thumbnail.hpp>

#include "internal.hpp"
#include "test_util.hpp"

using namespace navigator;
using namespace navigator::preview;
namespace pd = navigator::preview::detail;

namespace {

int iterations() {
    if (const char* env = std::getenv("NAVIGATOR_FUZZ_ITERATIONS")) {
        const int v = std::atoi(env);
        if (v > 0) {
            return v;
        }
    }
    return 600;
}

std::vector<std::uint8_t> mutate(std::vector<std::uint8_t> d, std::mt19937& rng) {
    const int ops = 1 + static_cast<int>(rng() % 4U);
    for (int k = 0; k < ops && !d.empty(); ++k) {
        const std::size_t pos = rng() % d.size();
        switch (rng() % 6U) {
        case 0: d[pos] = static_cast<std::uint8_t>(d[pos] ^ (1U << (rng() % 8U))); break;
        case 1: d[pos] = static_cast<std::uint8_t>(rng()); break;
        case 2: d[pos] = (rng() % 2U) != 0U ? 0xFF : 0x00; break;
        case 3: d.resize(pos + 1); break;
        case 4:
            d.insert(d.begin() + static_cast<std::ptrdiff_t>(pos),
                     static_cast<std::uint8_t>(rng()));
            break;
        default: {
            const std::size_t len = std::min<std::size_t>(d.size() - pos, 1 + rng() % 8U);
            d.erase(d.begin() + static_cast<std::ptrdiff_t>(pos),
                    d.begin() + static_cast<std::ptrdiff_t>(pos + len));
            break;
        }
        }
    }
    return d;
}

} // namespace

TEST_CASE("Mutated fixtures never crash any decoder", "[fuzz]") {
    test::TempDir dir;
    test::Rgba img = test::quadrants(16, 12);
    img.fill_rect(0, 0, 3, 3, {10, 20, 30, 100});
    test::JpegOptions jo;
    jo.app1 = test::make_exif_app1({});
    test::JpegOptions prog;
    prog.progressive = true;
    test::PngOptions pal;
    pal.color = 4;
    pal.text = {{"Thumb::URI", "file:///x"}, {"Thumb::MTime", "1"}};
    test::PngOptions inter;
    inter.interlaced = true;
    inter.bit_depth = 16;
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>> seeds = {
        {"jpeg", test::encode_jpeg(img, jo)},       {"pjpeg", test::encode_jpeg(img, prog)},
        {"png", test::encode_png(img, pal)},        {"ipng", test::encode_png(img, inter)},
        {"webp", test::encode_webp(img, true)},     {"lwebp", test::encode_webp(img, false)},
        {"gif", test::encode_gif(img, true, 1, 1)}, {"bmp", test::encode_bmp(img, 32)},
        {"bmp8", test::encode_bmp(img, 8)},
    };
    const int n = iterations();
    std::mt19937 rng(0x6E617669U);
    std::size_t ok_count = 0;
    std::size_t err_count = 0;
    for (const auto& [name, seed] : seeds) {
        const auto file = dir / ("fuzz-" + name);
        for (int i = 0; i < n; ++i) {
            test::write_file(file, mutate(seed, rng));
            pd::DecodeParams p;
            p.target = (i % 2 == 0) ? PixelSize{8, 8} : PixelSize{};
            auto r = pd::decode_image_file(file, p);
            if (r) {
                REQUIRE(r->image.valid());
                ++ok_count;
            } else {
                ++err_count;
            }
            // The thumbnail reader parses the same bytes as a cache file.
            if (name == "png" || name == "ipng") {
                thumbnail::SourceInfo src;
                src.uri = "file:///x";
                src.mtime = Timestamp{std::chrono::seconds(1)};
                auto t = thumbnail::read_thumbnail(file, src, {});
                REQUIRE((t || t.error().code != ErrorCode::Unknown));
            }
        }
    }
    CHECK(ok_count + err_count == seeds.size() * static_cast<std::size_t>(n));
    // Mutated EXIF blocks straight into the parser.
    const auto exif = test::make_exif_app1({});
    for (int i = 0; i < n * 4; ++i) {
        const auto m = mutate(exif, rng);
        (void)pd::parse_exif(m.data(), m.size());
    }
    // Mutated text.
    for (int i = 0; i < n; ++i) {
        std::vector<std::uint8_t> t = {0xFF, 0xFE, 'a', 0, 0x00, 0xD8, '\n', 0, 0xC3, 0xA9, 'x'};
        t = mutate(t, rng);
        auto r = decode_text(Span<const std::uint8_t>(t.data(), t.size()), t.size(), {});
        if (r) {
            for (std::size_t k = 0; k < r->rows.size(); ++k) {
                REQUIRE(std::size_t{r->rows[k].offset} + r->rows[k].length <= r->text.size());
            }
        }
    }
}
