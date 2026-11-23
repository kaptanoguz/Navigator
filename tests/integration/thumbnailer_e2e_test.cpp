// External thumbnailers end to end (owner: qa), Phase 6: the installed *.thumbnailer entries
// (gdk-pixbuf-thumbnailer for TIFF / ICO, which have no built-in decoder; ffmpegthumbnailer for
// a short clip made with the ffmpeg CLI) through the REAL process runner (bubblewrap sandbox when
// it works here) into a TEMPORARY cache root; fake thumbnailer scripts (in the fixture dir, run
// unsandboxed so they can be found) for the timeout, the process-group kill, failure markers,
// the output size cap, bad output and the concurrency limit. A case SKIPs when its tool is not
// installed.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/platform.hpp>
#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>
#include <navigator/thumbnail.hpp>

#include "fixture.hpp"
#include "preview_support.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using preview::PixelSize;
using preview::PreviewPurpose;
using thumbnail::ThumbnailSize;

namespace {

const thumbnail::Thumbnailer* installed_for(const std::vector<thumbnail::Thumbnailer>& all,
                                            std::string_view mime, std::string_view program) {
    const thumbnail::Thumbnailer* t = thumbnail::choose_thumbnailer(
        Span<const thumbnail::Thumbnailer>(all.data(), all.size()), mime);
    if (t == nullptr || t->exec.find(program) == std::string::npos) {
        return nullptr;
    }
    return t;
}

// A fake thumbnailer: `script` (sh) gets the input as $1 and the output path as $2.
thumbnail::Thumbnailer fake_thumbnailer(const Path& dir, std::string_view id,
                                        std::string_view script, std::string_view mime) {
    const Path prog = dir / (std::string(id) + ".sh");
    write_text_file(prog, "#!/bin/sh\n" + std::string(script) + "\n", 0755);
    const std::string text = "[Thumbnailer Entry]\nExec=" + prog.string() +
                             " %i %o\nMimeType=" + std::string(mime) + ";\n";
    Result<thumbnail::Thumbnailer> t =
        thumbnail::parse_thumbnailer(text, id, dir / (std::string(id) + ".thumbnailer"));
    REQUIRE(t.has_value());
    return *t;
}

thumbnail::ThumbnailerLimits fast_limits(bool sandbox) {
    thumbnail::ThumbnailerLimits l;
    l.timeout = std::chrono::milliseconds(800);
    l.sandbox = sandbox;
    return l;
}

bool pid_alive(pid_t pid) { return pid > 0 && ::kill(pid, 0) == 0; }

pid_t read_pid(const Path& p) {
    const std::vector<std::uint8_t> b = read_bytes(p);
    return static_cast<pid_t>(std::strtol(std::string(b.begin(), b.end()).c_str(), nullptr, 10));
}

bool wait_gone(pid_t pid, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (pid_alive(pid) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !pid_alive(pid);
}

} // namespace

TEST_CASE("Installed gdk-pixbuf thumbnailer through the real runner: TIFF and ICO",
          "[preview][thumbnailer][e2e]") {
    if (!have_program("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const std::vector<thumbnail::Thumbnailer> all = system_thumbnailers();
    const thumbnail::Thumbnailer* tiff = installed_for(all, "image/tiff", "gdk-pixbuf-thumbnailer");
    if (tiff == nullptr) {
        SKIP("no gdk-pixbuf-thumbnailer entry for image/tiff");
    }
    TempDir t;
    std::filesystem::create_directories(t / "data");
    const Path src = t / "data" / "scan 1.tiff";
    REQUIRE(ffmpeg_image(src, 320, 240));
    const Path ico = t / "data" / "icon.ico";
    REQUIRE(ffmpeg_image(ico, 48, 48));
    std::shared_ptr<RecordingRunner> runner = recording_runner();
    REQUIRE(runner);
    const bool sandbox_expected = bwrap_works();

    SECTION("run_thumbnailer directly") {
        const Result<preview::ImageBuffer> img =
            thumbnail::run_thumbnailer(*tiff, source_of(src), ThumbnailSize::Normal, *runner,
                                       thumbnail::ThumbnailerLimits{}, {});
        const std::vector<Result<platform::ProcessResult>> res = runner->results();
        REQUIRE(res.size() == 1);
        REQUIRE(res[0].has_value());
        INFO("stderr: " << res[0]->stderr_text << " exit " << res[0]->exit_code);
        CHECK(res[0]->sandboxed == sandbox_expected);
        REQUIRE(img.has_value());
        CHECK(img->size() == PixelSize{128, 96});
        const platform::ProcessRequest req = runner->requests()[0];
        CHECK(req.sandbox);
        CHECK(std::find(req.read_only_paths.begin(), req.read_only_paths.end(), src) !=
              req.read_only_paths.end());
        CHECK(req.max_file_bytes == thumbnail::kDefaultThumbnailerOutputBytes);
        CHECK(no_children());
    }
    SECTION("through the service into a temporary cache root") {
        const Path root = t / "thumbnails";
        {
            std::unique_ptr<preview::IPreviewService> s =
                make_service(cache_at(root).with_thumbnailers(all, runner));
            REQUIRE(s);
            for (const Path& p : {src, ico}) {
                INFO(p);
                const Delivery c =
                    produce_one(*s, request_for(p, PreviewPurpose::Thumbnail, {128, 128}),
                                preview::RequestPriority::Visible);
                REQUIRE(c.has_value());
                if (!c->result) {
                    FAIL_CHECK("error: " << c->result.error().message);
                    continue;
                }
                const preview::ImagePreview* img = image_of(*c);
                REQUIRE(img != nullptr);
                CHECK(img->origin == preview::ImageOrigin::ExternalThumbnailer);
                CHECK(std::max(img->image.size().width, img->image.size().height) <= 128U);
                CHECK((*c->result)->provider_id.starts_with("thumbnailer."));
            }
            CHECK(s->stats().thumbnails_written == 2);
            s->shutdown();
        }
        for (const Result<platform::ProcessResult>& r : runner->results()) {
            REQUIRE(r.has_value());
            INFO("stderr: " << r->stderr_text);
            CHECK(r->succeeded());
            CHECK(r->sandboxed == sandbox_expected);
        }
        // Spec-valid files in the temporary root, reused by the next run without a process.
        const PngChunks png = parse_png(read_bytes(thumbnail::thumbnail_path(
            root, ThumbnailSize::Normal, thumbnail::file_uri(src).value_or(""))));
        CHECK(png.crc_ok);
        CHECK(png.key("Thumb::URI") == thumbnail::file_uri(src).value_or("?"));
        CHECK(png.key("Thumb::Mimetype") == "image/tiff");
        const std::size_t runs = runner->results().size();
        std::unique_ptr<preview::IPreviewService> s =
            make_service(cache_at(root).with_thumbnailers(all, runner));
        REQUIRE(s);
        const Delivery c = produce_one(*s, request_for(src, PreviewPurpose::Thumbnail, {128, 128}),
                                       preview::RequestPriority::Visible);
        REQUIRE(c.has_value());
        REQUIRE(image_of(*c) != nullptr);
        CHECK(image_of(*c)->origin == preview::ImageOrigin::ThumbnailCache);
        CHECK(runner->results().size() == runs);
        CHECK(no_children());
    }
}

TEST_CASE("Installed ffmpegthumbnailer on a clip made with the ffmpeg CLI",
          "[preview][thumbnailer][e2e][video]") {
    if (!have_program("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    const std::vector<thumbnail::Thumbnailer> all = system_thumbnailers();
    const thumbnail::Thumbnailer* ff = installed_for(all, "video/mp4", "ffmpegthumbnailer");
    if (ff == nullptr) {
        SKIP("no ffmpegthumbnailer entry for video/mp4");
    }
    TempDir t;
    const Path clip = t / "clip.mp4";
    REQUIRE(ffmpeg_clip(clip, 320, 240, 2));
    std::shared_ptr<RecordingRunner> runner = recording_runner();
    REQUIRE(runner);
    const Result<preview::ImageBuffer> img = thumbnail::run_thumbnailer(
        *ff, source_of(clip), ThumbnailSize::Large, *runner, thumbnail::ThumbnailerLimits{}, {});
    const std::vector<Result<platform::ProcessResult>> res = runner->results();
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].has_value());
    INFO("stderr: " << res[0]->stderr_text);
    CHECK(res[0]->sandboxed == bwrap_works());
    REQUIRE(img.has_value());
    // ffmpegthumbnailer -s 256 scales the longer side to 256 (it may upscale; we only fit).
    CHECK(std::max(img->size().width, img->size().height) <= 256U);
    CHECK(img->size().width > img->size().height);
    CHECK(no_children());
}

TEST_CASE("A hung thumbnailer is killed at the timeout with its process group",
          "[preview][thumbnailer][e2e][timeout]") {
    TempDir t;
    const Path src = t / "x.tiff";
    write_text_file(src, "not really a tiff");
    // The script and its child sleep record their pids, then hang.
    const thumbnail::Thumbnailer slow =
        fake_thumbnailer(t.path(), "slow",
                         "echo $$ > '" + (t / "sh.pid").string() + "'\nsleep 30 &\necho $! > '" +
                             (t / "sleep.pid").string() + "'\nwait",
                         "image/tiff");
    std::shared_ptr<RecordingRunner> runner = recording_runner();
    REQUIRE(runner);
    const auto start = std::chrono::steady_clock::now();
    const Result<preview::ImageBuffer> img = thumbnail::run_thumbnailer(
        slow, source_of(src), ThumbnailSize::Normal, *runner, fast_limits(false), {});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(img.has_value());
    CHECK(img.error().code == ErrorCode::IoError);
    CHECK(elapsed < std::chrono::seconds(5));
    CHECK(elapsed >= std::chrono::milliseconds(700));
    const std::vector<Result<platform::ProcessResult>> res = runner->results();
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].has_value());
    CHECK(res[0]->timed_out);
    CHECK_FALSE(res[0]->sandboxed); // limits.sandbox = false
    // Neither the script nor its background child survive (SIGKILL to the group).
    CHECK(wait_gone(read_pid(t / "sh.pid"), std::chrono::seconds(2)));
    CHECK(wait_gone(read_pid(t / "sleep.pid"), std::chrono::seconds(2)));
    CHECK(no_children());

    SECTION("through the service: a failure marker, and a stop is prompt") {
        const Path root = t / "thumbnails";
        ServiceSetup setup = cache_at(root).with_thumbnailers({slow}, runner, fast_limits(false));
        {
            std::unique_ptr<preview::IPreviewService> s = make_service(setup);
            REQUIRE(s);
            const Delivery c =
                produce_one(*s, request_for(src, PreviewPurpose::Thumbnail, {128, 128}),
                            preview::RequestPriority::Visible);
            REQUIRE(c.has_value());
            CHECK(image_of(*c) == nullptr);
            CHECK(s->stats().failures_written == 1);
        }
        CHECK(thumbnail::has_failure(root, source_of(src)).value_or(false));
        // A long timeout, then shutdown while it hangs: the child is killed, shutdown returns.
        setup.limits.timeout = std::chrono::seconds(60);
        const Path other = t / "y.tiff";
        write_text_file(other, "also not a tiff");
        std::unique_ptr<preview::IPreviewService> s = make_service(setup);
        REQUIRE(s);
        const preview::Ticket ticket =
            s->submit(request_for(other, PreviewPurpose::Thumbnail, {128, 128}),
                      preview::RequestPriority::Visible, 1);
        REQUIRE(ticket.valid());
        const auto spawned = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::filesystem::remove(t / "sleep.pid");
        while (!std::filesystem::exists(t / "sleep.pid") &&
               std::chrono::steady_clock::now() < spawned) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(std::filesystem::exists(t / "sleep.pid"));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const pid_t child = read_pid(t / "sleep.pid");
        const auto t0 = std::chrono::steady_clock::now();
        s->cancel(ticket);
        CHECK(wait_gone(child, std::chrono::seconds(3)));
        s->shutdown();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
        // Cancelled: no failure marker for this file.
        CHECK_FALSE(thumbnail::has_failure(root, source_of(other)).value_or(true));
        CHECK(no_children());
    }
}

TEST_CASE("Thumbnailer output: size cap, bad output, non-zero exit, no output",
          "[preview][thumbnailer][e2e]") {
    if (!have_program("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    TempDir t;
    const Path src = t / "x.tiff";
    write_text_file(src, "input");
    const Path good_png = t / "good.png";
    REQUIRE(ffmpeg_image(good_png, 200, 100));
    std::shared_ptr<RecordingRunner> runner = recording_runner();
    REQUIRE(runner);
    thumbnail::ThumbnailerLimits limits = fast_limits(false);
    limits.timeout = std::chrono::seconds(10);
    limits.max_output_bytes = std::uint64_t{256} * 1024U;

    const auto run = [&](std::string_view id, const std::string& script) {
        const thumbnail::Thumbnailer th = fake_thumbnailer(t.path(), id, script, "image/tiff");
        return thumbnail::run_thumbnailer(th, source_of(src), ThumbnailSize::Normal, *runner,
                                          limits, {});
    };
    // Control: a valid PNG within the cap is fitted into the class.
    const Result<preview::ImageBuffer> ok = run("ok", "cp '" + good_png.string() + "' \"$2\"");
    REQUIRE(ok.has_value());
    CHECK(ok->size() == PixelSize{128, 64});
    // Over the cap: RLIMIT_FSIZE kills the writer with SIGXFSZ (non-zero exit: IoError).
    const Result<preview::ImageBuffer> big = run("big", "head -c 1048576 /dev/zero > \"$2\"");
    REQUIRE_FALSE(big.has_value());
    CHECK(big.error().code == ErrorCode::IoError);
    // The script hides the kill and exits 0: the file stopped at the cap is no image.
    const Result<preview::ImageBuffer> hidden =
        run("hidden", "head -c 1048576 /dev/zero > \"$2\"; exit 0");
    REQUIRE_FALSE(hidden.has_value());
    CHECK((hidden.error().code == ErrorCode::IoError ||
           hidden.error().code == ErrorCode::InvalidArgument));
    // A valid PNG header followed by padding past the cap (the file itself is too large).
    const Result<preview::ImageBuffer> padded =
        run("padded", "cp '" + good_png.string() + R"(' "$2"; head -c 400000 /dev/zero >> "$2")");
    REQUIRE_FALSE(padded.has_value());
    CHECK(padded.error().code == ErrorCode::IoError);
    // Garbage output.
    const Result<preview::ImageBuffer> junk = run("junk", "echo garbage > \"$2\"");
    REQUIRE_FALSE(junk.has_value());
    CHECK(junk.error().code == ErrorCode::InvalidArgument);
    // Empty output, no output, non-zero exit (even with a valid image), killed by a signal.
    for (const auto& [id, script] : std::vector<std::pair<std::string, std::string>>{
             {"empty", ": > \"$2\""},
             {"none", "exit 0"},
             {"fail", "cp '" + good_png.string() + "' \"$2\"; exit 3"},
             {"signal", "kill -SEGV $$"}}) {
        INFO(id);
        const Result<preview::ImageBuffer> r = run(id, script);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::IoError);
    }
    // The temporary output directories are gone.
    for (const platform::ProcessRequest& req : runner->requests()) {
        for (const Path& w : req.writable_paths) {
            INFO(w);
            CHECK_FALSE(std::filesystem::exists(w));
        }
    }
    CHECK(no_children());
}

TEST_CASE("At most max_concurrent thumbnailer processes run at once",
          "[preview][thumbnailer][e2e]") {
    if (!have_program("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    TempDir t;
    const Path good_png = t / "good.png";
    REQUIRE(ffmpeg_image(good_png, 64, 64));
    const Path live = t / "live";
    std::filesystem::create_directories(live);
    // Each run registers itself in live/, records how many are registered, works, deregisters.
    const std::string script = "touch '" + live.string() + "/'$$\nls '" + live.string() +
                               "' | wc -l >> '" + (t / "counts").string() + "'\nsleep 0.2\ncp '" +
                               good_png.string() + "' \"$2\"\nrm -f '" + live.string() + "/'$$";
    const thumbnail::Thumbnailer th = fake_thumbnailer(t.path(), "busy", script, "image/tiff");
    std::shared_ptr<RecordingRunner> runner = recording_runner();
    REQUIRE(runner);
    thumbnail::ThumbnailerLimits limits = fast_limits(false);
    limits.timeout = std::chrono::seconds(10);
    limits.max_concurrent = 2;
    std::unique_ptr<preview::IPreviewService> s = make_service(
        cache_at(t / "thumbnails").with_workers(6).with_thumbnailers({th}, runner, limits));
    REQUIRE(s);
    std::vector<preview::Ticket> tickets;
    for (int i = 0; i < 8; ++i) {
        const Path src = t / ("f" + std::to_string(i) + ".tiff");
        write_text_file(src, "input " + std::to_string(i));
        tickets.push_back(s->submit(request_for(src, PreviewPurpose::Thumbnail, {128, 128}),
                                    preview::RequestPriority::Visible, 1));
    }
    const std::map<std::uint64_t, preview::Completion> got = wait_for(*s, tickets);
    CHECK(got.size() == 8);
    for (const auto& [ticket, c] : got) {
        CHECK(image_of(c) != nullptr);
    }
    s->shutdown();
    const std::vector<std::uint8_t> counts = read_bytes(t / "counts");
    std::size_t max_seen = 0;
    std::size_t runs = 0;
    for (const std::string& line : [&] {
             std::vector<std::string> lines;
             std::string cur;
             for (const std::uint8_t b : counts) {
                 if (b == '\n') {
                     lines.push_back(cur);
                     cur.clear();
                 } else {
                     cur.push_back(static_cast<char>(b));
                 }
             }
             return lines;
         }()) {
        ++runs;
        max_seen = std::max<std::size_t>(max_seen, std::stoul(line));
    }
    CHECK(runs == 8);
    CHECK(max_seen >= 1);
    CHECK(max_seen <= 2);
    CHECK(no_children());
}

TEST_CASE("A sandboxed thumbnailer sees only its input and output",
          "[preview][thumbnailer][e2e][sandbox]") {
    if (!bwrap_works()) {
        SKIP("bubblewrap is not usable here");
    }
    if (!have_program("ffmpeg")) {
        SKIP("ffmpeg is not installed");
    }
    TempDir t;
    std::filesystem::create_directories(t / "data");
    const Path src = t / "data" / "in.tiff";
    write_text_file(src, "input");
    const Path secret = t / "data" / "secret.txt";
    write_text_file(secret, "secret");
    const Path good_png = t / "good.png";
    REQUIRE(ffmpeg_image(good_png, 64, 64));
    // The thumbnailer program must live where the sandbox can run it: /bin/sh -c with the script
    // inline. It reports what it can see through its exit code.
    const std::string probe = "test -r \"$1\" || exit 11; test -e '" + secret.string() +
                              "' && exit 12; test -e '" + good_png.string() +
                              "' && exit 13; printf x > \"$2\" || exit 14; exit 0";
    const std::string text =
        "[Thumbnailer Entry]\nExec=/bin/sh -c '" + probe + "' probe %i %o\nMimeType=image/tiff;\n";
    Result<thumbnail::Thumbnailer> th = thumbnail::parse_thumbnailer(text, "probe", t / "p");
    REQUIRE(th.has_value());
    std::shared_ptr<RecordingRunner> runner = recording_runner();
    REQUIRE(runner);
    thumbnail::ThumbnailerLimits limits = fast_limits(true);
    limits.timeout = std::chrono::seconds(10);
    const Result<preview::ImageBuffer> img =
        thumbnail::run_thumbnailer(*th, source_of(src), ThumbnailSize::Normal, *runner, limits, {});
    const std::vector<Result<platform::ProcessResult>> res = runner->results();
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].has_value());
    INFO("exit " << res[0]->exit_code << " stderr: " << res[0]->stderr_text);
    CHECK(res[0]->sandboxed);
    // 0: input readable, sibling and other fixture files invisible, output writable. The output
    // ("x") is not an image, so run_thumbnailer itself fails with InvalidArgument.
    CHECK(res[0]->exit_code == 0);
    REQUIRE_FALSE(img.has_value());
    CHECK(img.error().code == ErrorCode::InvalidArgument);
}
