// Phase 6 Inspector logic against a fake IPreviewService: following the cursor with debounce,
// lock, refresh in place, view targets, prefetch, dock placement / clamps, persistence through
// the Workspace; list thumbnail scheduling (visible only, epochs, cancel_older, completions);
// metadata formatting (EN / TR); thumbnail root rules.
#include <chrono>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <navigator/i18n.hpp>
#include <navigator/layout.hpp>
#include <navigator/preview.hpp>

#include "fake_preview_service.hpp"
#include "inspector/inspector_controller.hpp"
#include "inspector/metadata.hpp"
#include "inspector/preview_backend.hpp"
#include "inspector/thumbnail_scheduler.hpp"
#include "ui_fixture.hpp"
#include "workspace/workspace.hpp"

using namespace navigator;
using namespace navigator::ui;
using namespace navigator::ui::inspector;
using Catch::Approx;
using navigator::ui::test::FakePreviewService;
using navigator::ui::test::file_entry;
using navigator::ui::test::image_result;

namespace {

Item item(const std::string& name) {
    Item it;
    it.path = Path("/data/photos") / name;
    it.entry = file_entry(name);
    return it;
}

Subject subject(const std::string& name) {
    Subject s;
    s.cursor = item(name);
    s.selected_count = 0;
    return s;
}

struct LanguageGuard {
    explicit LanguageGuard(Language l) { set_language(l); }
    ~LanguageGuard() { set_language(Language::English); }
    LanguageGuard(const LanguageGuard&) = delete;
    LanguageGuard& operator=(const LanguageGuard&) = delete;
    LanguageGuard(LanguageGuard&&) = delete;
    LanguageGuard& operator=(LanguageGuard&&) = delete;
};

} // namespace

TEST_CASE("The Inspector follows the cursor with a debounce", "[inspector][controller]") {
    FakePreviewService svc;
    Controller c(&svc);
    c.set_visible(true, 0.0);
    CHECK(c.status() == Status::Empty);

    // First change after a pause: requested at once.
    c.follow(subject("a.jpg"), 1000.0);
    c.tick(1000.0, 1);
    CHECK(svc.submits == 1);
    CHECK(c.status() == Status::Loading);
    CHECK(svc.pending.front().priority == preview::RequestPriority::Inspector);
    CHECK(svc.pending.front().request.purpose == preview::PreviewPurpose::Inspector);

    // A held arrow key: changes 30 ms apart wait until they settle.
    c.follow(subject("b.jpg"), 1030.0);
    c.tick(1030.0, 1);
    c.follow(subject("c.jpg"), 1060.0);
    c.tick(1060.0, 1);
    CHECK(svc.submits == 1);
    CHECK(svc.pending.empty()); // a.jpg's job was cancelled when the cursor moved on
    c.tick(1060.0 + kFollowDebounceMs + 1.0, 1);
    CHECK(svc.submits == 2);
    REQUIRE(svc.pending.size() == 1);
    CHECK(svc.pending.front().request.path.filename() == "c.jpg");

    // Completion: ready.
    const preview::PreviewKey key = svc.pending.front().key;
    REQUIRE(svc.finish(key, image_result(64, 48)));
    std::vector<preview::Completion> done;
    svc.poll(done);
    REQUIRE(done.size() == 1);
    CHECK(c.on_completion(done.front()));
    CHECK(c.status() == Status::Ready);
    CHECK(c.result_is_current());

    // Back to a cached item: shown at once from the memory cache, no job.
    c.follow(subject("d.jpg"), 3000.0);
    c.tick(3000.0, 1);
    c.follow(subject("c.jpg"), 4000.0);
    CHECK(c.status() == Status::Ready);
    CHECK(svc.submits == 3);
}

TEST_CASE("Lock keeps the item while the cursor moves", "[inspector][controller]") {
    FakePreviewService svc;
    Controller c(&svc);
    c.set_visible(true, 0.0);
    c.follow(subject("a.png"), 1000.0);
    c.tick(1000.0, 1);
    c.toggle_lock();
    CHECK(c.locked());
    c.follow(subject("b.png"), 2000.0);
    c.tick(3000.0, 1);
    REQUIRE(c.item() != nullptr);
    CHECK(c.item()->path.filename() == "a.png");
    CHECK(svc.submits == 1);
    // A multi-selection does not replace the locked item either.
    Subject multi = subject("b.png");
    multi.selected_count = 3;
    c.follow(multi, 3100.0);
    CHECK(c.status() != Status::Multi);
    // Gone: unlocks and follows again.
    CHECK(c.unlock_if_gone(false));
    CHECK_FALSE(c.locked());
    c.follow(subject("b.png"), 5000.0);
    CHECK(c.item()->path.filename() == "b.png");
    c.toggle_lock();
    c.toggle_lock();
    CHECK_FALSE(c.locked());
}

TEST_CASE("Multi-selection shows a summary without a request", "[inspector][controller]") {
    FakePreviewService svc;
    Controller c(&svc);
    c.set_visible(true, 0.0);
    Subject s = subject("a.png");
    s.selected_count = 4;
    s.selected_bytes = 4096;
    c.follow(s, 1000.0);
    CHECK(c.status() == Status::Multi);
    CHECK(c.selected_count() == 4);
    CHECK(c.selected_bytes() == 4096);
}

TEST_CASE("Hidden Inspectors request nothing; showing requests at once",
          "[inspector][controller]") {
    FakePreviewService svc;
    Controller c(&svc);
    c.follow(subject("a.png"), 1000.0);
    c.tick(2000.0, 1);
    CHECK(svc.submits == 0);
    c.set_visible(true, 2100.0);
    c.tick(2100.0, 1);
    CHECK(svc.submits == 1);
    c.set_visible(false, 2200.0);
    CHECK(svc.pending.empty()); // cancelled
}

TEST_CASE("View targets: fit grows, 1:1 asks for full resolution, refresh in place",
          "[inspector][controller]") {
    FakePreviewService svc;
    Controller c(&svc);
    c.set_visible(true, 0.0);
    c.follow(subject("a.jpg"), 1000.0);
    c.tick(1000.0, 1);
    REQUIRE(svc.pending.size() == 1);
    CHECK(svc.pending.front().request.target == preview::PixelSize{1024, 1024});
    REQUIRE(svc.finish(svc.pending.front().key, image_result(1024, 768)));
    std::vector<preview::Completion> done;
    svc.poll(done);
    REQUIRE(c.on_completion(done.front()));

    // Shrinking never re-decodes.
    c.set_view_target(preview::PixelSize{512, 512}, false, 1100.0);
    c.tick(1100.0, 1);
    CHECK(svc.submits == 1);
    // 1:1: full resolution; the fit result stays on screen meanwhile.
    c.set_view_target(preview::PixelSize{512, 512}, true, 1200.0);
    c.tick(1200.0, 1);
    CHECK(svc.submits == 2);
    CHECK(svc.pending.back().request.target.empty());
    CHECK(c.result() != nullptr);
    CHECK(c.status() == Status::Loading);

    // Text and folders ignore the view size (one key).
    Controller t(&svc);
    t.set_visible(true, 0.0);
    t.follow(subject("notes.txt"), 1000.0);
    t.tick(1000.0, 1);
    CHECK(svc.pending.back().request.target.empty());
    const std::uint64_t before = svc.submits;
    t.set_view_target(preview::PixelSize{4096, 4096}, false, 1100.0);
    t.tick(1100.0, 1);
    CHECK(svc.submits == before);

    // The file changed on disk (same path, new mtime): the old result stays until the new lands.
    Item moved = item("a.jpg");
    moved.entry.mtime += std::chrono::seconds{5};
    Subject changed;
    changed.cursor = moved;
    c.follow(changed, 5000.0);
    CHECK(c.result() != nullptr);
    CHECK_FALSE(c.result_is_current());
}

TEST_CASE("Errors are reported, and neighbours are prefetched", "[inspector][controller]") {
    FakePreviewService svc;
    Controller c(&svc);
    c.set_visible(true, 0.0);
    Subject s = subject("a.jpg");
    s.neighbours = {item("b.jpg"), item("z.txt"), item("c.jpg")};
    c.follow(s, 1000.0);
    c.tick(1000.0, 7);
    REQUIRE(svc.finish(svc.pending.front().key, image_result(32, 32)));
    std::vector<preview::Completion> done;
    svc.poll(done);
    REQUIRE(c.on_completion(done.front()));
    c.tick(1001.0, 7);
    // Images only, Prefetch priority, with the ui's thumbnail epoch.
    REQUIRE(svc.pending.size() == 2);
    for (const auto& j : svc.pending) {
        CHECK(j.priority == preview::RequestPriority::Prefetch);
        CHECK(j.epoch == 7);
    }

    Controller e(&svc);
    e.set_visible(true, 0.0);
    e.follow(subject("broken.png"), 1000.0);
    e.tick(1000.0, 1);
    REQUIRE(svc.fail(svc.pending.back().key, ErrorCode::InvalidArgument));
    done.clear();
    svc.poll(done);
    REQUIRE(e.on_completion(done.back()));
    CHECK(e.status() == Status::Failed);
    const std::optional<Error> err = e.error();
    CHECK((err && err->code == ErrorCode::InvalidArgument));

    // Without a service (build without src/preview): a clean "unsupported" failure.
    Controller none(nullptr);
    none.set_visible(true, 0.0);
    none.follow(subject("a.jpg"), 1000.0);
    none.tick(1000.0, 1);
    CHECK(none.status() == Status::Failed);
}

TEST_CASE("Dock placement, size clamps and the list minimum", "[inspector][dock]") {
    layout::InspectorState st;
    st.visible = true;
    const RectF content{200.0F, 0.0F, 1080.0F, 800.0F};
    // Auto: right on a wide window.
    DockLayout d = dock_inspector(st, content, 1.0F, 1280.0, 800.0);
    CHECK(d.placement == layout::InspectorPlacement::Right);
    CHECK(d.inspector.w == Approx(360.0F));
    CHECK(d.list.w == Approx(720.0F));
    CHECK(d.inspector.x == Approx(920.0F));
    // Auto: bottom on a narrow window.
    d = dock_inspector(st, RectF{0.0F, 0.0F, 700.0F, 900.0F}, 1.0F, 700.0, 900.0);
    CHECK(d.placement == layout::InspectorPlacement::Bottom);
    CHECK(d.inspector.h == Approx(280.0F));
    CHECK(d.list.h == Approx(620.0F));
    // Forced right, scale 2: device pixels.
    st.placement = layout::InspectorPlacement::Right;
    st.width = 500.0;
    d = dock_inspector(st, RectF{0.0F, 0.0F, 2000.0F, 1200.0F}, 2.0F, 1000.0, 600.0);
    CHECK(d.inspector.w == Approx(1000.0F));
    // The list keeps kInspectorMinListExtent; the Inspector shrinks first.
    d = dock_inspector(st, RectF{0.0F, 0.0F, 700.0F, 500.0F}, 1.0F, 700.0, 500.0);
    CHECK(d.list.w == Approx(320.0F));
    CHECK(d.inspector.w == Approx(380.0F));
    // Hidden: the list gets everything.
    st.visible = false;
    d = dock_inspector(st, content, 1.0F, 1280.0, 800.0);
    CHECK(d.list == content);
    CHECK(d.inspector.w == 0.0F);
    // Splitter drags clamp to the documented ranges.
    CHECK(drag_extent(layout::InspectorPlacement::Right, content, 1.0F, 1270.0F) ==
          Approx(layout::kInspectorMinWidth));
    CHECK(drag_extent(layout::InspectorPlacement::Right, content, 1.0F, 800.0F) == Approx(480.0));
    CHECK(drag_extent(layout::InspectorPlacement::Bottom, content, 1.0F, 0.0F) == Approx(800.0));
    CHECK(drag_extent(layout::InspectorPlacement::Bottom, RectF{0.0F, 0.0F, 800.0F, 2000.0F}, 1.0F,
                      0.0F) == Approx(layout::kInspectorMaxHeight));
    layout::InspectorState bad;
    bad.width = 1.0e9;
    bad.height = -5.0;
    CHECK(clamped(bad).width == Approx(layout::kInspectorMaxWidth));
    CHECK(clamped(bad).height == Approx(layout::kInspectorMinHeight));
}

TEST_CASE("Inspector state is persisted through the workspace session", "[inspector][session]") {
    navigator::test::TempDir dir;
    Workspace::Options o;
    o.home = dir.path();
    Workspace ws(std::move(o));
    ws.start(std::nullopt, dir.path());
    const std::uint64_t serial = ws.persist_serial();
    layout::InspectorState st = ws.inspector();
    CHECK(st == layout::InspectorState{});
    st.visible = true;
    st.placement = layout::next_placement(st.placement);
    st.width = 5000.0; // clamped
    ws.set_inspector(st);
    CHECK(ws.persist_serial() > serial);
    CHECK(ws.inspector().visible);
    CHECK(ws.inspector().placement == layout::InspectorPlacement::Right);
    CHECK(ws.inspector().width == Approx(layout::kInspectorMaxWidth));
    CHECK(ws.snapshot().inspector == ws.inspector());
    // No change, no new serial.
    const std::uint64_t after = ws.persist_serial();
    ws.set_inspector(ws.inspector());
    CHECK(ws.persist_serial() == after);
    // A restored session brings the state back.
    layout::Session restored = ws.snapshot();
    Workspace::Options o2;
    o2.home = dir.path();
    Workspace ws2(std::move(o2));
    ws2.start(restored, Path());
    CHECK(ws2.inspector() == ws.inspector());
}

TEST_CASE("Thumbnails: visible rows only, epochs and cancel_older", "[inspector][thumbnails]") {
    FakePreviewService svc;
    ThumbnailScheduler sch(&svc);
    const auto req = [](int i) {
        return make_request(Path("/p") / ("img" + std::to_string(i) + ".jpg"),
                            file_entry("img" + std::to_string(i) + ".jpg"),
                            preview::PreviewPurpose::Thumbnail, preview::PixelSize{36, 36});
    };
    // Frame 1: rows 0..9 visible.
    sch.begin_frame();
    for (int i = 0; i < 10; ++i) {
        CHECK(sch.want(req(i), preview::RequestPriority::Visible) == nullptr);
    }
    sch.end_frame();
    CHECK(svc.submits == 10);
    const std::uint64_t e1 = sch.epoch();
    for (const auto& j : svc.pending) {
        CHECK(j.epoch == e1);
        CHECK(j.priority == preview::RequestPriority::Visible);
        CHECK(j.request.purpose == preview::PreviewPurpose::Thumbnail);
    }
    // Frame 2: nothing moved: nothing resubmitted.
    sch.begin_frame();
    for (int i = 0; i < 10; ++i) {
        (void)sch.want(req(i), preview::RequestPriority::Visible);
    }
    sch.end_frame();
    CHECK(svc.submits == 10);
    CHECK(sch.epoch() == e1);

    // A finished thumbnail is returned the next frame.
    REQUIRE(svc.finish(preview_key(req(3)), image_result(36, 27)));
    std::vector<preview::Completion> done;
    svc.poll(done);
    REQUIRE(sch.on_completion(done.front()));

    // Frame 3: scrolled to rows 5..14: new epoch, rows 5..9 refreshed, 10..14 new, 0..4 dropped.
    sch.begin_frame();
    for (int i = 5; i < 15; ++i) {
        (void)sch.want(req(i), preview::RequestPriority::Visible);
    }
    sch.end_frame();
    CHECK(sch.epoch() == e1 + 1);
    REQUIRE_FALSE(svc.older_calls.empty());
    CHECK(svc.older_calls.back().second == sch.epoch());
    CHECK(svc.find(preview_key(req(0))) == nullptr); // off screen: cancelled
    CHECK(svc.find(preview_key(req(4))) == nullptr);
    REQUIRE(svc.find(preview_key(req(7))) != nullptr);
    CHECK(svc.find(preview_key(req(7)))->epoch == sch.epoch()); // coalesced, refreshed
    REQUIRE(svc.find(preview_key(req(12))) != nullptr);
    CHECK(svc.pending.size() == 10);

    // Back up: row 3 is ready (memory), rows 0..2 resubmitted.
    sch.begin_frame();
    CHECK(sch.want(req(3), preview::RequestPriority::Visible) != nullptr);
    for (int i = 0; i < 3; ++i) {
        (void)sch.want(req(i), preview::RequestPriority::Visible);
    }
    sch.end_frame();
    CHECK(svc.find(preview_key(req(1))) != nullptr);
    CHECK(svc.find(preview_key(req(3))) == nullptr);

    // Failures keep the icon and are not retried for the same file version.
    REQUIRE(svc.fail(preview_key(req(1)), ErrorCode::InvalidArgument));
    done.clear();
    svc.poll(done);
    REQUIRE(sch.on_completion(done.front()));
    const std::uint64_t before = svc.submits;
    sch.begin_frame();
    CHECK(sch.want(req(1), preview::RequestPriority::Visible) == nullptr);
    sch.end_frame();
    CHECK(svc.submits == before);
    // Unknown tickets are not ours.
    CHECK_FALSE(sch.on_completion(preview::Completion{preview::Ticket{999999}, {}, nullptr}));
}

TEST_CASE("Thumbnails: prefetch, memory hits, disabled and forget", "[inspector][thumbnails]") {
    FakePreviewService svc;
    ThumbnailScheduler sch(&svc);
    const auto r = make_request(Path("/p/a.png"), file_entry("a.png"),
                                preview::PreviewPurpose::Thumbnail, preview::PixelSize{18, 18});
    const auto r2 = make_request(Path("/p/b.png"), file_entry("b.png"),
                                 preview::PreviewPurpose::Thumbnail, preview::PixelSize{18, 18});
    svc.cache[preview_key(r)] = image_result(18, 18);
    sch.begin_frame();
    (void)sch.want(r, preview::RequestPriority::Visible);
    (void)sch.want(r2, preview::RequestPriority::Prefetch);
    sch.end_frame();
    CHECK(svc.submits == 1); // a.png came from the memory cache
    CHECK(svc.pending.front().priority == preview::RequestPriority::Prefetch);
    sch.begin_frame();
    CHECK(sch.want(r, preview::RequestPriority::Visible) != nullptr);
    sch.end_frame();

    sch.forget(Path("/p"));
    CHECK(sch.slot_count() == 0);
    CHECK(svc.pending.empty());

    sch.set_enabled(false);
    sch.begin_frame();
    CHECK(sch.want(r2, preview::RequestPriority::Visible) == nullptr);
    sch.end_frame();
    CHECK(svc.pending.empty());

    ThumbnailScheduler none(nullptr);
    none.begin_frame();
    CHECK(none.want(r, preview::RequestPriority::Visible) == nullptr);
    none.end_frame();
    CHECK_FALSE(none.enabled());
}

TEST_CASE("Thumbnail candidates and cache roots", "[inspector][thumbnails]") {
    CHECK(wants_thumbnail("image/jpeg", file_entry("a.jpg")));
    CHECK(wants_thumbnail("video/mp4", file_entry("a.mp4")));
    CHECK_FALSE(wants_thumbnail("text/plain", file_entry("a.txt")));
    CHECK_FALSE(wants_thumbnail("image/png", file_entry("empty.png", 0)));
    fs::DirEntry dir = file_entry("photos");
    dir.kind = fs::EntryKind::Directory;
    CHECK_FALSE(wants_thumbnail("image/png", dir));
    fs::DirEntry link = file_entry("l.png");
    link.kind = fs::EntryKind::Symlink;
    link.target_kind = fs::EntryKind::File;
    CHECK(wants_thumbnail("image/png", link));

    ThumbnailRootInput in;
    in.platform_root = Path("/home/u/.cache/thumbnails");
    CHECK(resolve_thumbnail_root(in) == in.platform_root);
    in.cache_dir = "/tmp/c";
    CHECK(resolve_thumbnail_root(in) == Path("/tmp/c/thumbnails"));
    in.thumbnail_dir = "/tmp/t";
    CHECK(resolve_thumbnail_root(in) == Path("/tmp/t"));
    // Headless without explicit dirs: never the shared cache.
    ThumbnailRootInput headless;
    headless.headless = true;
    headless.platform_root = Path("/home/u/.cache/thumbnails");
    CHECK(resolve_thumbnail_root(headless).empty());
    in.enable_thumbnails = false;
    CHECK(resolve_thumbnail_root(in).empty());
}

TEST_CASE("Metadata formatting", "[inspector][metadata]") {
    CHECK(format_dimensions({4032, 3024}) == "4032 × 3024");
    CHECK(format_megapixels({4032, 3024}) == "12.2 MP");
    CHECK(format_exposure({1, 250}) == "1/250 s");
    CHECK(format_exposure({10, 4}) == "2.5 s");
    CHECK(format_exposure({1, 3}) == "0.3 s");
    CHECK(format_aperture(2.8) == "f/2.8");
    CHECK(format_aperture(8.0) == "f/8");
    CHECK(format_focal(4.32, 26U) == "4.3 mm  ·  26 mm equiv.");
    CHECK(format_focal(50.0, 50U) == "50 mm");
    CHECK(format_duration(42'400) == "0:42");
    CHECK(format_duration(3'723'000) == "1:02:03");
    CHECK(format_frame_rate({30000, 1001}) == "29.97 fps");
    CHECK(format_frame_rate({25, 1}) == "25 fps");
    CHECK(format_bit_rate(4'200'000) == "4.2 Mb/s");
    CHECK(format_bit_rate(128'000) == "128 kb/s");
    CHECK(format_sample_rate(48'000) == "48 kHz");
    CHECK(format_coordinate(41.0082, true) == "41.00820° N");
    CHECK(format_coordinate(-28.9784, false) == "28.97840° W");
    preview::ExifDateTime t{2024, 5, 17, 14, 3, 9, std::int16_t{180}};
    CHECK(format_exif_date(t) == "2024-05-17 14:03 UTC+03:00");
    {
        const LanguageGuard tr_lang(Language::Turkish);
        CHECK(format_megapixels({4032, 3024}) == "12,2 MP");
        CHECK(format_frame_rate({30000, 1001}) == "29,97 fps");
        CHECK(format_coordinate(41.0082, true) == "41,00820° K");
        CHECK(format_focal(4.32, 26U) == "4,3 mm  ·  26 mm eşdeğeri");
    }
}

TEST_CASE("Metadata groups skip empty ones and follow the language", "[inspector][metadata]") {
    auto res = std::make_shared<preview::PreviewResult>();
    preview::ImagePreview img;
    img.info.format = preview::ImageFormat::Jpeg;
    img.info.display = {4000, 3000};
    preview::ExifInfo x;
    x.make = "Canon";
    x.model = "Canon EOS R6";
    x.exposure_time = preview::Rational{1, 200};
    x.f_number = 4.0;
    x.iso = 400;
    x.has_gps = true;
    img.info.exif = x;
    res->content = img;
    const fs::DirEntry e = file_entry("IMG_0001.JPG", 5'000'000);
    const Timestamp now = e.mtime + std::chrono::hours{24 * 30};
    std::vector<MetaGroup> g = build_metadata(e, res.get(), now);
    REQUIRE(g.size() == 4); // File, Image, Camera, Location
    CHECK(g[0].title == "File");
    CHECK(g[1].title == "Image");
    CHECK(g[2].title == "Camera");
    CHECK(g[2].rows[0].label == "Model");
    CHECK(g[2].rows[0].value == "Canon EOS R6"); // the make is not repeated
    CHECK(g[3].rows[0].value == "Location data present");
    // No result yet: the File group only.
    CHECK(build_metadata(e, nullptr, now).size() == 1);
    {
        const LanguageGuard tr_lang(Language::Turkish);
        g = build_metadata(e, res.get(), now);
        CHECK(g[0].title == "Dosya");
        CHECK(g[2].title == "Fotoğraf makinesi");
    }
    // Video without FFmpeg metadata: no video group.
    auto vid = std::make_shared<preview::PreviewResult>();
    vid->content = preview::VideoPreview{};
    CHECK(build_metadata(file_entry("a.mp4"), vid.get(), now).size() == 1);
    preview::VideoPreview v;
    v.info.source = preview::MetadataSource::FFmpeg;
    v.info.duration_ms = 61'000;
    v.info.frame = {1920, 1080};
    v.info.video_codec = "h264";
    v.info.audio_codec = "aac";
    v.info.audio_channels = 2;
    vid->content = v;
    g = build_metadata(file_entry("a.mp4"), vid.get(), now);
    REQUIRE(g.size() == 3);
    CHECK(g[1].rows[0].value == "1:01");
    CHECK(g[1].rows[2].value == "H264");
    CHECK(g[2].rows[1].value == "Stereo");
}

TEST_CASE("Thumbnailer page previews of documents list no image facts", "[inspector][metadata]") {
    // A fake thumbnailer result: the page of a PDF as a 768 x 1024 image.
    auto page = image_result(768, 1024);
    auto res = std::make_shared<preview::PreviewResult>(*page);
    std::get<preview::ImagePreview>(res->content).origin =
        preview::ImageOrigin::ExternalThumbnailer;
    const fs::DirEntry pdf = file_entry("manual.pdf", 2'000'000);
    CHECK(is_page_preview(pdf, *res));
    const Timestamp now = pdf.mtime + std::chrono::hours{1};
    const std::vector<MetaGroup> g = build_metadata(pdf, res.get(), now);
    REQUIRE(g.size() == 1);
    CHECK(g[0].title == "File");
    for (const MetaRow& r : g[0].rows) {
        CHECK(r.value.find("768") == std::string::npos);
    }
    CHECK(header_subtitle(pdf, res.get()).find("768") == std::string::npos);
    // The disk thumbnail cache counts too; a decoded image of an image file does not.
    std::get<preview::ImagePreview>(res->content).origin = preview::ImageOrigin::ThumbnailCache;
    CHECK(is_page_preview(pdf, *res));
    CHECK_FALSE(is_page_preview(file_entry("photo.jpg"), *image_result(4, 4)));
    auto cached_photo = std::make_shared<preview::PreviewResult>(*image_result(4, 4));
    std::get<preview::ImagePreview>(cached_photo->content).origin =
        preview::ImageOrigin::ThumbnailCache;
    CHECK_FALSE(is_page_preview(file_entry("photo.jpg"), *cached_photo));
    // The list asks for document thumbnails as well, so row and Inspector agree.
    CHECK(wants_thumbnail("application/pdf", pdf));
    CHECK(wants_thumbnail("application/vnd.oasis.opendocument.text", file_entry("a.odt")));
    CHECK_FALSE(wants_thumbnail("application/zip", file_entry("a.zip")));
}

TEST_CASE("Header subtitles per kind; video kinds and containers", "[inspector][metadata]") {
    const fs::DirEntry jpg = file_entry("a.jpg", 719'000);
    auto img = image_result(64, 48);
    std::get<preview::ImagePreview>(std::const_pointer_cast<preview::PreviewResult>(img)->content)
        .info.display = {4000, 3000};
    CHECK(header_subtitle(jpg, img.get()) == "4000 × 3000  ·  719 kB");
    CHECK(header_subtitle(jpg, nullptr) == "JPEG image  ·  719 kB");

    auto vid = std::make_shared<preview::PreviewResult>();
    preview::VideoPreview v;
    v.info.source = preview::MetadataSource::FFmpeg;
    v.info.duration_ms = 8'000;
    v.info.frame = {1280, 720};
    v.info.container = "mov,mp4,m4a,3gp,3g2,mj2";
    v.info.container_long = "QuickTime / MOV";
    vid->content = v;
    const fs::DirEntry mp4 = file_entry("Trailer.mp4", 3'200'000);
    CHECK(header_subtitle(mp4, vid.get()) == "0:08  ·  1280 × 720  ·  3.2 MB");
    const Timestamp now = mp4.mtime + std::chrono::hours{1};
    const std::vector<MetaGroup> g = build_metadata(mp4, vid.get(), now);
    REQUIRE(g.size() >= 2);
    CHECK(g[0].rows[0].value == "MP4 video");
    bool container = false;
    for (const MetaRow& r : g[1].rows) {
        if (r.label == "Container") {
            container = true;
            CHECK(r.value == "MPEG-4 (MP4)");
        }
    }
    CHECK(container);
    CHECK(header_subtitle(file_entry("clip.mkv"), nullptr).starts_with("Matroska video"));
    {
        const LanguageGuard tr_lang(Language::Turkish);
        CHECK(header_subtitle(file_entry("clip.mp4", 10), nullptr).starts_with("MP4 videosu"));
    }

    auto txt = std::make_shared<preview::PreviewResult>();
    preview::TextPreview t;
    t.line_count = 312;
    txt->content = t;
    CHECK(header_subtitle(file_entry("notes.txt", 7'800), txt.get()) == "312 lines  ·  7.8 kB");
}

TEST_CASE("Inspector-local keys", "[inspector][keys]") {
    CHECK(inspector_local_key(KeyChord{Key::Digit0, Modifiers::None}) == LocalKey::Fit);
    CHECK(inspector_local_key(KeyChord{Key::Digit1, Modifiers::None}) == LocalKey::OneToOne);
    CHECK(inspector_local_key(KeyChord{Key::Equals, Modifiers::Shift}) == LocalKey::ZoomIn);
    CHECK(inspector_local_key(KeyChord{Key::Minus, Modifiers::None}) == LocalKey::ZoomOut);
    CHECK(inspector_local_key(KeyChord{Key::G, Modifiers::None}) == LocalKey::Grid);
    CHECK(inspector_local_key(KeyChord{Key::M, Modifiers::None}) == LocalKey::Minimap);
    CHECK_FALSE(inspector_local_key(KeyChord{Key::G, Modifiers::Ctrl}).has_value());
    CHECK_FALSE(inspector_local_key(KeyChord{Key::Space, Modifiers::None}).has_value());
}
