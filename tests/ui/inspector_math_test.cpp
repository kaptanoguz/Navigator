// Phase 6 Inspector pure math: zoom / pan (fit, 1:1, zoom about a point, clamping, minimap),
// decode target buckets, the GPU upload budget scheduler, text wrapping / virtualization, and
// the Space rule of text fields.
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>
#include <navigator/preview.hpp>

#include "inspector/text_layout.hpp"
#include "inspector/upload_queue.hpp"
#include "inspector/zoom.hpp"
#include "search/filter_rules.hpp"

using namespace navigator;
using namespace navigator::ui;
using namespace navigator::ui::inspector;
using Catch::Approx;

TEST_CASE("Fit never upscales and centres the image", "[inspector][zoom]") {
    ImageViewport v;
    v.set_view(400.0, 300.0);
    v.set_image(4000.0, 3000.0);
    CHECK(v.mode() == ZoomMode::Fit);
    CHECK(v.zoom() == Approx(0.1));
    const Box r = v.image_rect();
    CHECK(r.x == Approx(0.0));
    CHECK(r.w == Approx(400.0));
    CHECK_FALSE(v.pannable());

    // A small image stays at 1:1 in fit mode, centred.
    v.set_image(100.0, 50.0);
    CHECK(v.zoom() == Approx(1.0));
    CHECK(v.image_rect().x == Approx(150.0));
    CHECK(v.image_rect().y == Approx(125.0));

    // Fit follows the view size.
    v.set_image(800.0, 600.0);
    v.set_view(200.0, 600.0);
    CHECK(v.zoom() == Approx(0.25));
}

TEST_CASE("1:1 is one image pixel per device pixel and keeps the anchor", "[inspector][zoom]") {
    ImageViewport v;
    v.set_view(400.0, 300.0);
    v.set_image(4000.0, 3000.0);
    // The image point under (100, 75) in fit is (1000, 750).
    const Vec2 before = v.view_to_image(Vec2{100.0, 75.0});
    CHECK(before.x == Approx(1000.0));
    v.one_to_one(Vec2{100.0, 75.0});
    CHECK(v.mode() == ZoomMode::OneToOne);
    CHECK(v.zoom() == Approx(1.0));
    const Vec2 after = v.view_to_image(Vec2{100.0, 75.0});
    CHECK(after.x == Approx(1000.0));
    CHECK(after.y == Approx(750.0));
    CHECK(v.pannable());
    v.fit();
    CHECK(v.mode() == ZoomMode::Fit);
    v.toggle_fit_1to1(Vec2{200.0, 150.0});
    CHECK(v.mode() == ZoomMode::OneToOne);
    v.toggle_fit_1to1(Vec2{200.0, 150.0});
    CHECK(v.mode() == ZoomMode::Fit);
}

TEST_CASE("Zoom about a point is clamped to [fit, 16]", "[inspector][zoom]") {
    ImageViewport v;
    v.set_view(400.0, 300.0);
    v.set_image(1000.0, 1000.0);
    const double fit = v.fit_zoom();
    CHECK(fit == Approx(0.3));
    v.zoom_at(2.0, Vec2{50.0, 50.0});
    CHECK(v.zoom() == Approx(0.6));
    CHECK(v.mode() == ZoomMode::Free);
    v.zoom_at(1000.0, Vec2{50.0, 50.0});
    CHECK(v.zoom() == Approx(kMaxZoom));
    CHECK(v.grid_visible());
    v.zoom_at(1.0e-6, Vec2{50.0, 50.0});
    CHECK(v.zoom() == Approx(fit));
    CHECK(v.mode() == ZoomMode::Fit);
}

TEST_CASE("Smooth zoom settles on its target and snaps through 1:1", "[inspector][zoom]") {
    ImageViewport v;
    v.set_view(400.0, 300.0);
    v.set_image(2000.0, 1500.0); // fit 0.2
    v.zoom_smooth(4.0, Vec2{200.0, 150.0});
    CHECK(v.target_zoom() == Approx(0.8));
    int frames = 0;
    while (v.animate(16.0, true) && frames < 200) {
        ++frames;
    }
    CHECK(frames > 1);
    CHECK(v.zoom() == Approx(0.8));
    v.zoom_smooth(2.0, Vec2{200.0, 150.0}); // would pass 1.0 -> snaps to 1:1
    CHECK(v.target_zoom() == Approx(1.0));
    CHECK_FALSE(v.animate(16.0, false)); // animations off: jumps
    CHECK(v.zoom() == Approx(1.0));
    CHECK(v.mode() == ZoomMode::OneToOne);
}

TEST_CASE("Panning is clamped so the image covers the view", "[inspector][zoom]") {
    ImageViewport v;
    v.set_view(400.0, 300.0);
    v.set_image(1000.0, 200.0);
    v.one_to_one();
    // Wider than the view: x pans; shorter than the view: y stays centred.
    v.pan(-10000.0, -500.0);
    CHECK(v.image_rect().right() == Approx(400.0));
    CHECK(v.image_rect().y == Approx(50.0));
    v.pan(10000.0, 0.0);
    CHECK(v.image_rect().x == Approx(0.0));
    v.pan_steps(1, 0);
    CHECK(v.image_rect().x == Approx(-50.0));
    const Box vis = v.visible_region();
    CHECK(vis.x == Approx(50.0));
    CHECK(vis.w == Approx(400.0));
}

TEST_CASE("Minimap maps the visible region and clicks back", "[inspector][zoom]") {
    ImageViewport v;
    v.set_view(400.0, 300.0);
    v.set_image(4000.0, 3000.0);
    v.one_to_one();
    const Box frame = minimap_frame(4000.0, 3000.0, 0.0, 0.0, 120.0, 120.0);
    CHECK(frame.w == Approx(120.0));
    CHECK(frame.h == Approx(90.0));
    CHECK(frame.y == Approx(30.0)); // bottom-anchored in its box
    const Box vis = minimap_visible(v, frame);
    CHECK(vis.w == Approx(12.0));
    CHECK(vis.h == Approx(9.0));
    v.center_on(minimap_to_image(v, frame, Vec2{frame.x + 1.0, frame.y + 1.0}));
    CHECK(v.image_rect().x == Approx(0.0)); // clamped to the left edge
    v.center_on(minimap_to_image(v, frame, Vec2{frame.right(), frame.bottom()}));
    CHECK(v.image_rect().right() == Approx(400.0));
}

TEST_CASE("Decode targets are bucketed and only grow", "[inspector][zoom]") {
    CHECK(bucket_target(300.0, 200.0) == DecodeTarget{512, 256});
    CHECK(bucket_target(513.0, 1.0) == DecodeTarget{768, 256});
    CHECK(covers(DecodeTarget{768, 512}, DecodeTarget{512, 512}));
    CHECK_FALSE(covers(DecodeTarget{512, 512}, DecodeTarget{768, 512}));
    CHECK(covers(DecodeTarget{0, 0}, DecodeTarget{4096, 4096})); // full resolution
}

TEST_CASE("Uploads respect the per-frame budget, Inspector first", "[inspector][upload]") {
    UploadQueue<int> q;
    const std::size_t mib = std::size_t{1024} * 1024U;
    q.push(1, UploadClass::Thumbnail, 128, 512, 1); // 64 KiB thumbnail
    q.push(2, UploadClass::Thumbnail, 128, 512, 2);
    q.push(3, UploadClass::Inspector, 4000, 16000, 3); // ~61 MiB image
    // Frame 1: the Inspector image takes the whole 8 MiB budget in bands.
    auto s1 = q.plan(8 * mib);
    REQUIRE(s1.size() == 1);
    CHECK(s1[0].id == 3);
    CHECK(s1[0].first_row == 0);
    CHECK(s1[0].row_count == (8 * mib) / 16000);
    CHECK_FALSE(s1[0].last);
    std::size_t frames = 1;
    std::vector<std::uint64_t> finished;
    while (!q.empty() && frames < 20) {
        for (const auto& sl : q.plan(8 * mib)) {
            if (sl.last) {
                finished.push_back(sl.id);
            }
        }
        ++frames;
    }
    CHECK(q.empty());
    CHECK(frames >= 8); // 61 MiB over 8 MiB frames
    // The Inspector image completes first; the thumbnails fill the budget left after it.
    CHECK(finished == std::vector<std::uint64_t>{3, 1, 2});

    // Progress guarantee: one band even when a single row exceeds the budget.
    q.push(9, UploadClass::Thumbnail, 2, 4 * mib, 9);
    auto big = q.plan(1024);
    REQUIRE(big.size() == 1);
    CHECK(big[0].row_count == 1);
    // Re-pushing an id restarts it, never duplicates it.
    q.push(9, UploadClass::Thumbnail, 2, 4 * mib, 9);
    CHECK(q.size() == 1);
    CHECK(q.pending_bytes() == 8 * mib);
}

namespace {

preview::TextPreview make_text(const std::string& s, std::uint32_t row_bytes = 1024) {
    preview::TextOptions o;
    o.max_row_bytes = row_bytes;
    // Built by hand like decode_text: rows of <= row_bytes, split at '\n'.
    preview::TextPreview t;
    t.text = s;
    std::uint32_t line = 0;
    std::uint32_t start = 0;
    const auto n = static_cast<std::uint32_t>(s.size());
    for (std::uint32_t i = 0; i <= n; ++i) {
        if (i == n || s[i] == '\n') {
            std::uint32_t off = start;
            do {
                const std::uint32_t len = std::min(row_bytes, i - off);
                t.rows.push_back(preview::TextRow{off, len, line});
                off += len;
            } while (off < i);
            ++line;
            start = i + 1;
        }
    }
    t.line_count = line;
    return t;
}

} // namespace

TEST_CASE("Text rows wrap at the column count into visual lines", "[inspector][text]") {
    const preview::TextPreview t = make_text("short\n" + std::string(25, 'x') + "\n\nend");
    TextWrap w;
    w.rebuild(&t, 10);
    // "short" 1, 25 x's -> 3, "" 1, "end" 1.
    CHECK(w.visual_lines() == 6);
    const VisualLine l0 = w.line(0);
    CHECK(l0.first_of_line);
    CHECK(l0.line == 0);
    const VisualLine l2 = w.line(2);
    CHECK(l2.row == 1);
    CHECK(l2.byte_begin == 10);
    CHECK(l2.byte_end == 20);
    CHECK_FALSE(l2.first_of_line);
    CHECK(w.line(4).first_of_line);
    CHECK(w.line(4).byte_end == 0); // empty line
    CHECK(w.line(5).line == 3);
}

TEST_CASE("A huge single line stays cheap per visual line", "[inspector][text]") {
    // 4 MiB on one line = 4096 rows of 1 KiB; each wraps into ceil(1024 / 100) = 11 lines.
    const preview::TextPreview t = make_text(std::string(std::size_t{4} * 1024 * 1024, 'a'));
    REQUIRE(t.rows.size() == 4096);
    TextWrap w;
    w.rebuild(&t, 100);
    CHECK(w.visual_lines() == std::size_t{4096} * 11U);
    // Continuation rows of the same logical line never get a line number.
    CHECK(w.line(0).first_of_line);
    CHECK_FALSE(w.line(11).first_of_line);
    const VisualLine last = w.line(w.visual_lines() - 1);
    CHECK(last.row == 4095);
    CHECK(last.byte_end - last.byte_begin == 24);
    // Only the visible range is touched.
    const LineRange r = visible_lines(5.0e5, 600.0, 20.0, w.visual_lines());
    CHECK(r.first == 25000);
    CHECK(r.last == 25030);
    CHECK(visible_lines(1.0e9, 600.0, 20.0, w.visual_lines()).first == w.visual_lines());
    CHECK(max_scroll(600.0, 20.0, 10) == Approx(0.0));
    CHECK(max_scroll(100.0, 20.0, 10) == Approx(100.0));
}

TEST_CASE("Words wrap after the last blank that fits", "[inspector][text]") {
    const preview::TextPreview t = make_text("alpha beta gamma delta");
    TextWrap w;
    w.rebuild(&t, 12);
    REQUIRE(w.visual_lines() == 2);
    CHECK(w.line(0).byte_end == 11); // "alpha beta " | "gamma delta"
    CHECK(w.line(1).byte_begin == 11);
    CHECK(w.line(1).cell_begin == 11);
    // A word longer than the line breaks at the column.
    const preview::TextPreview u = make_text("ab " + std::string(20, 'x'));
    w.rebuild(&u, 10);
    CHECK(w.visual_lines() == 3);
    CHECK(w.line(0).byte_end == 3);
    CHECK(w.line(1).byte_end == 13);
}

TEST_CASE("Tabs, UTF-8 and controls in text rows", "[inspector][text]") {
    CHECK(count_cells("a\tb") == 5); // tab to column 4
    CHECK(count_cells("ğüş") == 3);  // code points, not bytes
    CHECK(display_text("a\tb", 0) == "a   b");
    CHECK(display_text("x\x01y\r", 0) == "x\xC2\xB7y");
    const preview::TextPreview t = make_text("ğğğğğğğğğğğğ"); // 12 two-byte code points
    TextWrap w;
    w.rebuild(&t, 8);
    CHECK(w.visual_lines() == 2);
    CHECK(w.line(1).byte_begin == 16); // never inside a UTF-8 sequence
}

TEST_CASE("Text fields consume Space and Shift+Space as text", "[inspector][keys]") {
    CHECK(field_consumes_as_text(KeyChord{Key::Space, Modifiers::None}));
    CHECK(field_consumes_as_text(KeyChord{Key::Space, Modifiers::Shift}));
    CHECK_FALSE(field_consumes_as_text(KeyChord{Key::Space, Modifiers::Ctrl})); // reserved
    CHECK_FALSE(field_consumes_as_text(KeyChord{Key::KeypadEnter, Modifiers::Alt}));
    CHECK_FALSE(field_consumes_as_text(KeyChord{Key::A, Modifiers::None}));
    // With the list focused, a leading Space never starts type-to-filter (Phase 5 rule), so it
    // reaches the keymap as view.inspect.
    CHECK_FALSE(starts_type_to_filter(KeyboardOwner::List, Modifiers::None, " "));
}
