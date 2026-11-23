// Preview robustness through the whole service (owner: qa), Phase 6. Beyond the decoder fuzzing
// of tests/preview, every request here goes through IPreviewService: the worker pool, the
// Freedesktop cache lookup, the providers, the thumbnail and failure-marker writers (TEMPORARY
// cache roots) and the memory cache. Meant to run under ASan + UBSan and TSan as well.
//   - truncated, bit-flipped and huge-header PNG / JPEG / WebP / GIF / BMP / video / text
//     fixtures: every job completes (no hang), results respect their bounds, the cache holds only
//     spec-valid files, the next run serves known failures without decoding;
//   - cancellation storms: a scrolling simulation submits a moving window and cancel_older()s the
//     rest every frame while a second thread submits, cancels and invalidates; the last window is
//     delivered, Cancelled is never delivered, the service drains;
//   - the memory budget: many large full-resolution images stay within the budget; raising and
//     lowering it keeps / evicts at once; results the ui still holds are never freed under it.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>
#include <navigator/thumbnail.hpp>

#include "fixture.hpp"
#include "preview_support.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using preview::PixelSize;
using preview::PreviewPurpose;
using preview::RequestPriority;

namespace {

// Deterministic xorshift (seeded per format).
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13U;
        s ^= s >> 7U;
        s ^= s << 17U;
        return s;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
};

std::size_t find_marker(const std::vector<std::uint8_t>& d, std::uint8_t a, std::uint8_t b) {
    for (std::size_t i = 2; i + 1 < d.size(); ++i) {
        if (d[i] == a && d[i + 1] == b) {
            return i;
        }
    }
    return std::string::npos;
}

void put16be(std::vector<std::uint8_t>& d, std::size_t at, std::uint16_t v) {
    if (at + 1 < d.size()) {
        d[at] = static_cast<std::uint8_t>(v >> 8U);
        d[at + 1] = static_cast<std::uint8_t>(v);
    }
}
void put16le(std::vector<std::uint8_t>& d, std::size_t at, std::uint16_t v) {
    if (at + 1 < d.size()) {
        d[at] = static_cast<std::uint8_t>(v);
        d[at + 1] = static_cast<std::uint8_t>(v >> 8U);
    }
}
void put32le(std::vector<std::uint8_t>& d, std::size_t at, std::uint32_t v) {
    for (std::size_t k = 0; k < 4 && at + k < d.size(); ++k) {
        d[at + k] = static_cast<std::uint8_t>(v >> (8U * k));
    }
}

// Headers that claim enormous (or zero / negative) dimensions, per format.
std::vector<std::vector<std::uint8_t>> huge_headers(const std::string& ext,
                                                    const std::vector<std::uint8_t>& base) {
    std::vector<std::vector<std::uint8_t>> out;
    if (ext == "png") {
        for (const auto& [w, h] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
                 {60000, 60000}, {100000, 10}, {0, 0}, {0x7FFFFFFFU, 0x7FFFFFFFU}, {1, 1}}) {
            out.push_back(png_with_size(base, w, h));
        }
    } else if (ext == "jpg") {
        // SOF0 / SOF2: FF C0|C2, length(2), precision(1), height(2), width(2).
        std::size_t sof = find_marker(base, 0xFF, 0xC0);
        if (sof == std::string::npos) {
            sof = find_marker(base, 0xFF, 0xC2);
        }
        if (sof != std::string::npos) {
            for (const auto& [w, h] : std::vector<std::pair<std::uint16_t, std::uint16_t>>{
                     {65535, 65535}, {65000, 1}, {0, 0}}) {
                std::vector<std::uint8_t> d = base;
                put16be(d, sof + 5, h);
                put16be(d, sof + 7, w);
                out.push_back(std::move(d));
            }
        }
    } else if (ext == "gif") {
        // Logical screen (6..9), first image descriptor (after the 0x2C separator).
        std::vector<std::uint8_t> d = base;
        put16le(d, 6, 65535);
        put16le(d, 8, 65535);
        out.push_back(d);
        const std::size_t desc =
            static_cast<std::size_t>(std::find(d.begin() + 13, d.end(), 0x2C) - d.begin());
        if (desc + 9 < d.size()) {
            put16le(d, desc + 5, 65535);
            put16le(d, desc + 7, 65535);
            out.push_back(d);
        }
    } else if (ext == "bmp") {
        for (const auto& [w, h] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
                 {1000000, 1000000}, {30000, 30000}, {64, 0x80000000U}, {0xFFFFFFFFU, 1}}) {
            std::vector<std::uint8_t> d = base;
            put32le(d, 18, w);
            put32le(d, 22, h);
            out.push_back(std::move(d));
        }
    } else if (ext == "webp") {
        // Lossy VP8: 14-bit width / height at 26 / 28 (RIFF 12 + chunk 8 + frame tag 3 + start
        // code 3).
        std::vector<std::uint8_t> d = base;
        put16le(d, 26, 0x3FFF);
        put16le(d, 28, 0x3FFF);
        out.push_back(d);
        // RIFF size far larger than the file.
        d = base;
        put32le(d, 4, 0xFFFFFFF0U);
        out.push_back(d);
    }
    return out;
}

struct Corpus {
    std::vector<Path> files;
};

// Base fixtures (ffmpeg), then 5 truncations, `flips` bit-flip variants and the huge headers of
// each format, plus text oddities.
Corpus make_corpus(const Path& dir, std::size_t flips) {
    Corpus c;
    std::filesystem::create_directories(dir);
    std::vector<std::pair<std::string, Path>> bases;
    for (const char* ext : {"png", "jpg", "webp", "gif", "bmp"}) {
        const Path p = dir / (std::string("base.") + ext);
        REQUIRE(ffmpeg_image(p, 160, 120));
        bases.emplace_back(ext, p);
    }
    const Path clip = dir / "base.mkv";
    if (ffmpeg_clip(clip, 160, 120, 1)) {
        bases.emplace_back("mkv", clip);
    }
    std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
    for (const auto& [ext, base] : bases) {
        c.files.push_back(base);
        const std::vector<std::uint8_t> bytes = read_bytes(base);
        REQUIRE(bytes.size() > 64);
        int n = 0;
        const auto add = [&](const std::vector<std::uint8_t>& d, const char* kind) {
            const Path p = dir / (std::string(kind) + "-" + std::to_string(n++) + "." + ext);
            write_bytes(p, d);
            c.files.push_back(p);
        };
        for (const double frac : {0.0, 0.1, 0.5, 0.9, 0.999}) {
            const auto cut = static_cast<std::size_t>(static_cast<double>(bytes.size()) * frac);
            add(std::vector<std::uint8_t>(bytes.begin(),
                                          bytes.begin() + static_cast<std::ptrdiff_t>(cut)),
                "trunc");
        }
        add(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 33), "header");
        Rng rng{seed += 0x1234567ULL};
        for (std::size_t i = 0; i < flips; ++i) {
            std::vector<std::uint8_t> d = bytes;
            const std::size_t count = 1 + rng.below(8);
            for (std::size_t k = 0; k < count; ++k) {
                // Half the flips land in the first 512 bytes (headers, tables).
                const std::size_t at = (k % 2 == 0)
                                           ? rng.below(std::min<std::size_t>(512, d.size()))
                                           : rng.below(d.size());
                d[at] ^= static_cast<std::uint8_t>(1U << rng.below(8));
            }
            add(d, "flip");
        }
        for (const std::vector<std::uint8_t>& d : huge_headers(ext, bytes)) {
            add(d, "huge");
        }
    }
    // Text oddities: invalid UTF-8, a UTF-16 BOM with an odd byte count, NULs, empty.
    write_text_file(dir / "bad-utf8.txt", std::string("ok \xC3\x28 \xF0\x28\x8C\x28 \xFF\xFE"));
    write_text_file(dir / "odd-utf16.txt", std::string("\xFF\xFE"
                                                       "a\0b\0c",
                                                       7));
    write_text_file(dir / "nul.txt", std::string(4096, '\0'));
    write_text_file(dir / "empty.txt", "");
    write_text_file(dir / "lines.txt", std::string(200000, '\n'));
    for (const char* n : {"bad-utf8.txt", "odd-utf16.txt", "nul.txt", "empty.txt", "lines.txt"}) {
        c.files.push_back(dir / n);
    }
    return c;
}

// The result honours the request's bounds.
bool within_bounds(const preview::PreviewResult& r, const preview::PreviewRequest& req) {
    const auto fits = [&](const preview::ImageBuffer& img) {
        if (!img.valid()) {
            return img.size().width == 0 && img.size().height == 0;
        }
        if (req.target.width == 0) {
            return std::uint64_t{img.size().width} * img.size().height <= preview::kMaxOutputPixels;
        }
        return img.size().width <= req.target.width && img.size().height <= req.target.height;
    };
    if (const auto* i = std::get_if<preview::ImagePreview>(&r.content)) {
        return fits(i->image);
    }
    if (const auto* v = std::get_if<preview::VideoPreview>(&r.content)) {
        return fits(v->poster);
    }
    return true;
}

} // namespace

TEST_CASE("Corrupt, truncated and huge-header files through service, cache and writer",
          "[preview][robustness][service]") {
    if (!have_program("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    TempDir t;
    const Path root = t / "thumbnails";
    const Corpus corpus = make_corpus(t / "data", 30);
    REQUIRE(corpus.files.size() > 150);
    const std::vector<PixelSize> targets{{128, 128}, {1024, 768}, {0, 0}};

    std::vector<std::pair<preview::Ticket, preview::PreviewRequest>> jobs;
    std::unique_ptr<preview::IPreviewService> s = make_service(cache_at(root).with_workers(4));
    REQUIRE(s);
    for (const Path& f : corpus.files) {
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const PreviewPurpose purpose =
                i == 0 ? PreviewPurpose::Thumbnail : PreviewPurpose::Inspector;
            preview::PreviewRequest r = request_for(f, purpose, targets[i]);
            const preview::Ticket tk =
                s->submit(r, i == 0 ? RequestPriority::Visible : RequestPriority::Inspector, 1);
            REQUIRE(tk.valid());
            jobs.emplace_back(tk, std::move(r));
        }
    }
    std::vector<preview::Ticket> tickets;
    tickets.reserve(jobs.size());
    for (const auto& j : jobs) {
        tickets.push_back(j.first);
    }
    const std::map<std::uint64_t, preview::Completion> got =
        wait_for(*s, tickets, std::chrono::seconds(120));
    CHECK(got.size() == jobs.size()); // nothing hangs, nothing is lost
    std::size_t ok = 0;
    std::size_t failed = 0;
    for (const auto& [ticket, req] : jobs) {
        const auto it = got.find(ticket.value);
        if (it == got.end()) {
            FAIL_CHECK("no completion for " << req.path << " target " << req.target.width);
            continue;
        }
        const preview::Completion& c = it->second;
        CHECK(c.key == preview::make_key(req));
        if (c.result) {
            REQUIRE(*c.result);
            INFO(req.path << " target " << req.target.width << "x" << req.target.height);
            CHECK(within_bounds(**c.result, req));
            ++ok;
        } else {
            CHECK(c.result.error().code != ErrorCode::Cancelled);
            ++failed;
        }
    }
    CHECK(ok > 0);
    CHECK(failed > 0);
    REQUIRE(wait_idle(*s));
    const preview::ServiceStats st = s->stats();
    CHECK(st.cache_bytes <= st.cache_budget_bytes);
    CHECK(st.failures_written > 0);
    s->shutdown();

    // Every file in the cache is a complete PNG that names one of the corpus files; no
    // temporaries were left behind.
    std::set<std::string> uris;
    for (const Path& f : corpus.files) {
        uris.insert(thumbnail::file_uri(f).value_or(""));
    }
    std::size_t markers = 0;
    for (const std::string& rel : files_below(root)) {
        INFO(rel);
        CHECK(rel.ends_with(".png"));
        const PngChunks png = parse_png(read_bytes(root / rel));
        CHECK(png.complete);
        CHECK(png.crc_ok);
        CHECK(uris.contains(png.key("Thumb::URI")));
        CHECK(mode_of(root / rel) == 0600);
        if (rel.starts_with("fail/")) {
            CHECK(rel.starts_with("fail/navigator-1/"));
            ++markers;
        } else {
            CHECK(std::max(png.width, png.height) <= 128U);
        }
    }
    CHECK(markers == st.failures_written);

    // A second run on the same cache: thumbnails come from disk or known failures, nothing is
    // decoded again for the Thumbnail purpose.
    std::unique_ptr<preview::IPreviewService> s2 = make_service(cache_at(root).with_workers(4));
    REQUIRE(s2);
    std::vector<preview::Ticket> again;
    again.reserve(corpus.files.size());
    for (const Path& f : corpus.files) {
        again.push_back(s2->submit(request_for(f, PreviewPurpose::Thumbnail, {128, 128}),
                                   RequestPriority::Visible, 1));
    }
    const std::map<std::uint64_t, preview::Completion> got2 =
        wait_for(*s2, again, std::chrono::seconds(120));
    CHECK(got2.size() == again.size());
    const preview::ServiceStats st2 = s2->stats();
    INFO("disk hits " << st2.disk_hits << " produced " << st2.produced);
    CHECK(st2.thumbnails_written == 0);
    CHECK(st2.failures_written == 0);
    // Files that neither decoded nor failed "corrupt" (Unsupported, NoProvider: text, video
    // without a poster) are produced again; images never are.
    CHECK(st2.disk_hits + markers > 0);
    s2->shutdown();
}

TEST_CASE("Cancellation storm while scrolling: the last window arrives, the service drains",
          "[preview][robustness][cancel]") {
    if (!have_program("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    TempDir t;
    std::filesystem::create_directories(t / "data");
    const Path base = t / "base.jpg";
    REQUIRE(ffmpeg_image(base, 640, 480));
    const std::vector<std::uint8_t> bytes = read_bytes(base);
    std::vector<Path> files;
    for (int i = 0; i < 240; ++i) {
        const Path p = t / "data" / ("img" + std::to_string(1000 + i) + ".jpg");
        write_bytes(p, bytes);
        files.push_back(p);
    }
    const Path root = t / "thumbnails";
    std::unique_ptr<preview::IPreviewService> s =
        make_service(cache_at(root).with_workers(4).with_budget(preview::kMinCacheBudgetBytes));
    REQUIRE(s);

    std::set<std::uint64_t> issued;
    std::size_t delivered = 0;
    bool cancelled_delivered = false;
    bool foreign = false;
    std::set<std::string> names;
    for (const Path& f : files) {
        names.insert(f.string());
    }
    std::vector<preview::Completion> batch;
    std::map<std::uint64_t, bool> seen; // ticket -> had an image
    const auto drain = [&] {
        batch.clear();
        (void)s->poll(batch);
        for (const preview::Completion& c : batch) {
            ++delivered;
            seen[c.ticket.value] = image_of(c) != nullptr;
            if (!c.result && c.result.error().code == ErrorCode::Cancelled) {
                cancelled_delivered = true;
            }
            if (!names.contains(c.key.path.string())) {
                foreign = true;
            }
        }
    };

    // A second thread hammers the same service (the ui is the only poller; any thread may
    // submit / cancel).
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> side_submits{0};
    std::thread side([&] {
        std::uint64_t epoch = 1'000'000;
        std::size_t i = 0;
        while (!stop.load()) {
            const preview::Ticket tk = s->submit(
                request_for(files[(i * 7) % files.size()], PreviewPurpose::Inspector, {800, 600}),
                RequestPriority::Prefetch, ++epoch);
            side_submits.fetch_add(1);
            if (i % 3 == 0) {
                s->cancel(tk);
            }
            if (i % 50 == 0) {
                s->invalidate(t / "data", preview::InvalidateMode::Memory);
            }
            if (i % 17 == 0) {
                s->cancel_older(RequestPriority::Prefetch, epoch);
            }
            (void)s->stats();
            ++i;
            std::this_thread::yield();
        }
    });

    constexpr std::size_t kWindow = 20;
    std::uint64_t epoch = 0;
    std::vector<preview::Ticket> last;
    for (std::size_t frame = 0; frame < 400; ++frame) {
        if (frame == 380) {
            // The side thread stops before the last frames: its invalidate() would drop the
            // final window's pending jobs (documented: the ui resubmits after an invalidate).
            stop.store(true);
            side.join();
        }
        ++epoch;
        // Scroll down and back up again (a fling and a return).
        const std::size_t top = frame < 200 ? (frame * 3) % (files.size() - kWindow)
                                            : ((399 - frame) * 2) % (files.size() - kWindow);
        last.clear();
        for (std::size_t r = top; r < top + kWindow; ++r) {
            const preview::Ticket tk =
                s->submit(request_for(files[r], PreviewPurpose::Thumbnail, {96, 96}),
                          RequestPriority::Visible, epoch);
            REQUIRE(tk.valid());
            issued.insert(tk.value);
            last.push_back(tk);
        }
        for (std::size_t r = top + kWindow; r < std::min(files.size(), top + kWindow + 6); ++r) {
            issued.insert(s->submit(request_for(files[r], PreviewPurpose::Thumbnail, {96, 96}),
                                    RequestPriority::Prefetch, epoch)
                              .value);
        }
        s->cancel_older(RequestPriority::Visible, epoch);
        s->cancel_older(RequestPriority::Prefetch, epoch);
        if (frame % 9 == 0) {
            s->cancel(last[frame % kWindow]); // a row scrolled past in the same frame
            last.erase(last.begin() + static_cast<std::ptrdiff_t>(frame % kWindow));
        }
        drain();
    }
    // The final window is delivered (some of it was already polled in the last frames).
    std::vector<preview::Ticket> outstanding;
    for (const preview::Ticket& tk : last) {
        if (!seen.contains(tk.value)) {
            outstanding.push_back(tk);
        }
    }
    const std::map<std::uint64_t, preview::Completion> got =
        wait_for(*s, outstanding, std::chrono::seconds(60));
    delivered += got.size();
    for (const auto& [ticket, c] : got) {
        seen[ticket] = image_of(c) != nullptr;
    }
    for (const preview::Ticket& tk : last) {
        INFO("ticket " << tk.value);
        const auto it = seen.find(tk.value);
        REQUIRE(it != seen.end());
        CHECK(it->second);
    }
    REQUIRE(wait_idle(*s, std::chrono::seconds(60)));
    drain();
    CHECK_FALSE(cancelled_delivered);
    CHECK_FALSE(foreign);
    const preview::ServiceStats st = s->stats();
    INFO("produced " << st.produced << " written " << st.thumbnails_written << " delivered "
                     << delivered << " side " << side_submits.load());
    CHECK(st.pending == 0);
    CHECK(st.running == 0);
    // Most of the storm was cancelled, not decoded.
    CHECK(st.produced < issued.size() + side_submits.load());
    // Nothing half-written in the cache.
    for (const std::string& rel : files_below(root)) {
        INFO(rel);
        CHECK(rel.ends_with(".png"));
        CHECK(parse_png(read_bytes(root / rel)).complete);
    }
    s->shutdown();
    CHECK(s->submit(request_for(files[0], PreviewPurpose::Thumbnail, {96, 96}),
                    RequestPriority::Visible, epoch + 1)
              .value == 0); // after shutdown: no ticket
}

TEST_CASE("Memory budget with many large full-resolution images", "[preview][robustness][memory]") {
    if (!have_program("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    TempDir t;
    std::filesystem::create_directories(t / "data");
    const Path base = t / "base.png";
    REQUIRE(ffmpeg_image(base, 1600, 1200)); // 7.3 MiB of RGBA at 1:1
    const std::vector<std::uint8_t> bytes = read_bytes(base);
    std::vector<Path> files;
    for (int i = 0; i < 24; ++i) {
        const Path p = t / "data" / ("big" + std::to_string(i) + ".png");
        write_bytes(p, bytes);
        files.push_back(p);
    }
    constexpr std::uint64_t kBudget = preview::kMinCacheBudgetBytes; // 16 MiB: two entries
    std::unique_ptr<preview::IPreviewService> s =
        make_service(cache_at({}).with_workers(4).with_budget(1)); // below the minimum: raised
    REQUIRE(s);
    CHECK(s->stats().cache_budget_bytes == kBudget);

    std::vector<preview::Ticket> tickets;
    std::vector<preview::PreviewKey> keys;
    for (const Path& f : files) {
        preview::PreviewRequest r = request_for(f, PreviewPurpose::Inspector, {0, 0});
        keys.push_back(preview::make_key(r));
        tickets.push_back(s->submit(std::move(r), RequestPriority::Inspector, 1));
    }
    // Poll as the ui does, dropping every result at once; the cache never exceeds its budget.
    std::uint64_t peak = 0;
    std::size_t got = 0;
    std::vector<preview::Completion> batch;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (got < tickets.size() && std::chrono::steady_clock::now() < deadline) {
        batch.clear();
        got += s->poll(batch);
        for (const preview::Completion& c : batch) {
            REQUIRE(c.result.has_value());
            const preview::ImagePreview* img = image_of(c);
            REQUIRE(img != nullptr);
            CHECK(img->image.size() == PixelSize{1600, 1200});
        }
        batch.clear(); // the ui drops them
        peak = std::max(peak, s->stats().cache_bytes);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(got == tickets.size());
    REQUIRE(wait_idle(*s));
    const preview::ServiceStats st = s->stats();
    INFO("peak " << peak << " entries " << st.cache_entries);
    // Completions waiting for poll() hold their results (never evicted early, counted against
    // the budget): at most one per worker between two polls here.
    constexpr std::uint64_t kEntry = std::uint64_t{1600} * 1200 * 4;
    CHECK(peak <= kBudget + 4 * (kEntry + 4096));
    CHECK(st.cache_bytes <= kBudget);
    CHECK(st.cache_entries <= 2);
    CHECK(st.cache_entries >= 1);

    // Results the ui holds stay alive and count against the budget; they are never freed early.
    std::vector<std::shared_ptr<const preview::PreviewResult>> held;
    for (std::size_t i = 0; i < 4; ++i) {
        batch.clear();
        s->invalidate(files[i], preview::InvalidateMode::Memory);
        const preview::Ticket tk =
            s->submit(request_for(files[i], PreviewPurpose::Inspector, {0, 0}),
                      RequestPriority::Inspector, 2);
        std::map<std::uint64_t, preview::Completion> one = wait_for(*s, {tk});
        REQUIRE(one.contains(tk.value));
        REQUIRE(one.at(tk.value).result.has_value());
        held.push_back(*one.at(tk.value).result);
    }
    for (const auto& h : held) {
        const auto* img = std::get_if<preview::ImagePreview>(&h->content);
        REQUIRE(img != nullptr);
        CHECK(img->image.pixels.size() >= std::size_t{1600} * 1200 * 4); // still intact
    }
    held.clear();

    // Raising the budget keeps more; lowering it evicts at once.
    s->set_cache_budget(std::uint64_t{128} * 1024 * 1024);
    s->clear_cache();
    std::vector<preview::Ticket> more;
    for (std::size_t i = 0; i < 12; ++i) {
        more.push_back(s->submit(request_for(files[i], PreviewPurpose::Inspector, {0, 0}),
                                 RequestPriority::Inspector, 3));
    }
    CHECK(wait_for(*s, more, std::chrono::seconds(120)).size() == more.size());
    REQUIRE(wait_idle(*s));
    CHECK(s->stats().cache_entries >= 10);
    CHECK(s->lookup(keys[0]) != nullptr); // a memory hit
    CHECK(s->stats().memory_hits >= 1);
    s->set_cache_budget(kBudget);
    CHECK(s->stats().cache_bytes <= kBudget);
    CHECK(s->stats().cache_entries <= 2);
    s->shutdown();
}
