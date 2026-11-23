// tests/preview/thumbnailer_test.cpp: .thumbnailer parsing, Exec splitting / expansion (no shell,
// no re-splitting), XDG loading with shadowing and TryExec, choice by MIME, and running through
// a fake IProcessRunner (temp dir, limits, output checks, cleanup).
#include <catch2/catch_test_macros.hpp>

#include <navigator/thumbnail.hpp>

#include "fake_runner.hpp"
#include "test_util.hpp"

using namespace navigator;
using namespace navigator::thumbnail;

namespace {

constexpr std::string_view kFfmpeg = "[Thumbnailer Entry]\n"
                                     "TryExec=ffmpegthumbnailer\n"
                                     "Exec=ffmpegthumbnailer -i %i -o %o -s %s -f\n"
                                     "MimeType=video/mp4;Video/X-Matroska;;video/mp4;\n";

std::vector<std::string> split(std::string_view s) {
    auto r = split_exec(s);
    REQUIRE(r);
    return *r;
}

} // namespace

TEST_CASE("parse_thumbnailer reads the Thumbnailer Entry group", "[thumbnailer][parse]") {
    auto t = parse_thumbnailer(kFfmpeg, "ffmpegthumbnailer", "/usr/share/thumbnailers/x");
    REQUIRE(t);
    CHECK(t->id == "ffmpegthumbnailer");
    CHECK(t->try_exec == "ffmpegthumbnailer");
    CHECK(t->exec == "ffmpegthumbnailer -i %i -o %o -s %s -f");
    CHECK(t->mime_types == std::vector<std::string>{"video/mp4", "video/x-matroska"});

    const std::string_view mixed =
        "# comment\n[Desktop Entry]\nExec=wrong\n"
        "[Thumbnailer Entry]\n  Exec = a\\sb %u\n"
        "Exec[tr]=localized\nMimeType = x/a\\;b;x/c\n[Other]\nMimeType=y/z\n";
    auto m = parse_thumbnailer(mixed, "m", "/f");
    REQUIRE(m);
    CHECK(m->exec == "a b %u");
    CHECK(m->mime_types == std::vector<std::string>{"x/a;b", "x/c"});
    CHECK(m->try_exec.empty());

    CHECK(parse_thumbnailer("Exec=a\nMimeType=x/y\n", "n", "/f").error().code ==
          ErrorCode::InvalidArgument); // no group
    CHECK_FALSE(parse_thumbnailer("[Thumbnailer Entry]\nMimeType=x/y\n", "n", "/f"));
    CHECK_FALSE(parse_thumbnailer("[Thumbnailer Entry]\nExec=a %i\n", "n", "/f"));
    CHECK_FALSE(parse_thumbnailer("[Thumbnailer Entry]\nExec=a %i\nMimeType=;;\n", "n", "/f"));
    CHECK_FALSE(parse_thumbnailer("[Thumbnailer Entry]\nExec=a %x\nMimeType=x/y\n", "n", "/f"));
    CHECK_FALSE(parse_thumbnailer(std::string(70000, '#'), "n", "/f"));
}

TEST_CASE("split_exec follows the g_shell_parse_argv subset", "[thumbnailer][exec]") {
    CHECK(split("a  b\tc") == std::vector<std::string>{"a", "b", "c"});
    CHECK(split("a 'b c' \"d e\"") == std::vector<std::string>{"a", "b c", "d e"});
    CHECK(split("a \"x\\\"y\\\\z\\$w\\n\"") == std::vector<std::string>{"a", "x\"y\\z$w\\n"});
    CHECK(split("a b\\ c") == std::vector<std::string>{"a", "b c"});
    CHECK(split("a 'it''s'") == std::vector<std::string>{"a", "its"});
    // Shell metacharacters stay literal: no shell ever runs.
    CHECK(split("sh -c $(rm) ; | > *") ==
          std::vector<std::string>{"sh", "-c", "$(rm)", ";", "|", ">", "*"});
    CHECK(split("t %i%% %o") == std::vector<std::string>{"t", "%i%%", "%o"});
    CHECK(split_exec("a 'b").error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(split_exec("a \"b"));
    CHECK_FALSE(split_exec("a b\\"));
    CHECK_FALSE(split_exec("   "));
    CHECK_FALSE(split_exec("a %f"));
    CHECK_FALSE(split_exec("a 100%"));
}

TEST_CASE("expand_exec substitutes field codes without re-splitting", "[thumbnailer][exec]") {
    ExecInputs in;
    in.input_path = "/home/u/my file; rm -rf ~.mp4";
    in.input_uri = "file:///home/u/my%20file%3B%20rm%20-rf%20~.mp4";
    in.output_path = "/tmp/navigator-thumb-x/out.png";
    in.size = 256;
    auto r = expand_exec("t -i %i -o %o -s %s --uri=%u 100%%", in);
    REQUIRE(r);
    CHECK(*r == std::vector<std::string>{"t", "-i", "/home/u/my file; rm -rf ~.mp4", "-o",
                                         "/tmp/navigator-thumb-x/out.png", "-s", "256",
                                         "--uri=file:///home/u/my%20file%3B%20rm%20-rf%20~.mp4",
                                         "100%"});
    ExecInputs missing = in;
    missing.input_uri = {};
    CHECK(expand_exec("t %u", missing).error().code == ErrorCode::InvalidArgument);
    CHECK(expand_exec("t %i", missing)); // %u unused: fine
    missing.size = 0;
    CHECK_FALSE(expand_exec("t %s", missing));
}

TEST_CASE("load_thumbnailers: XDG order, shadowing, TryExec, invalid files skipped",
          "[thumbnailer][load]") {
    test::TempDir tmp;
    const auto home = tmp / "home" / "thumbnailers";
    const auto sys = tmp / "usr" / "thumbnailers";
    std::filesystem::create_directories(home);
    std::filesystem::create_directories(sys);
    test::write_text(sys / "ffmpeg.thumbnailer", kFfmpeg);
    test::write_text(sys / "evince.thumbnailer",
                     "[Thumbnailer Entry]\nTryExec=evince-thumbnailer\n"
                     "Exec=evince-thumbnailer -s %s %u %o\nMimeType=application/pdf;\n");
    test::write_text(sys / "gone.thumbnailer",
                     "[Thumbnailer Entry]\nExec=not-installed %i %o\nMimeType=image/x-gone;\n");
    test::write_text(sys / "broken.thumbnailer", "garbage");
    test::write_text(sys / "README", "[Thumbnailer Entry]\nExec=x\nMimeType=a/b\n");
    // The user's file shadows the system one with the same id.
    test::write_text(home / "ffmpeg.thumbnailer",
                     "[Thumbnailer Entry]\nExec=my-thumbs %i %o %s\nMimeType=video/webm;\n");
    const std::vector<std::filesystem::path> data_dirs = {tmp / "home", tmp / "missing",
                                                          tmp / "usr"};
    const auto dirs = thumbnailer_dirs(data_dirs);
    REQUIRE(dirs.size() == 3);
    CHECK(dirs[0] == home);
    std::vector<std::string> asked;
    const ProgramExists exists = [&](std::string_view p) {
        asked.emplace_back(p);
        return p != "not-installed";
    };
    auto loaded = load_thumbnailers(dirs, exists, {});
    REQUIRE(loaded);
    REQUIRE(loaded->size() == 2);
    CHECK((*loaded)[0].id == "ffmpeg");
    CHECK((*loaded)[0].exec == "my-thumbs %i %o %s");
    CHECK((*loaded)[1].id == "evince");
    CHECK(choose_thumbnailer(*loaded, "VIDEO/WEBM") == &(*loaded)[0]);
    CHECK(choose_thumbnailer(*loaded, "video/mp4") == nullptr); // shadowed
    CHECK(choose_thumbnailer(*loaded, "application/pdf") == &(*loaded)[1]);
    CHECK(choose_thumbnailer(*loaded, "application/pdf-x") == nullptr);

    const ProgramExists none = [](std::string_view) { return false; };
    auto empty = load_thumbnailers(dirs, none, {});
    REQUIRE(empty);
    CHECK(empty->empty());

    std::stop_source stop;
    stop.request_stop();
    CHECK(load_thumbnailers(dirs, exists, stop.get_token()).error().code == ErrorCode::Cancelled);
}

TEST_CASE("run_thumbnailer through a fake runner", "[thumbnailer][run]") {
    test::TempDir tmp;
    test::write_text(tmp / "clip.mp4", "fake video bytes");
    auto t = parse_thumbnailer(kFfmpeg, "ffmpeg", "/f");
    REQUIRE(t);
    SourceInfo src;
    src.path = tmp / "clip.mp4";
    src.uri = *file_uri(src.path);
    src.mime = "video/mp4";
    test::FakeRunner runner;
    ThumbnailerLimits limits;
    limits.timeout = std::chrono::milliseconds(1234);
    auto img = run_thumbnailer(*t, src, ThumbnailSize::Normal, runner, limits, {});
    REQUIRE(img);
    CHECK(img->size() == preview::PixelSize{128, 85});
    const auto reqs = runner.requests();
    REQUIRE(reqs.size() == 1);
    const platform::ProcessRequest& rq = reqs[0];
    REQUIRE(rq.argv.size() == 8);
    CHECK(rq.argv[0] == "ffmpegthumbnailer");
    CHECK(rq.argv[2] == src.path.native());
    CHECK(rq.argv[6] == "128");
    const std::filesystem::path out = rq.argv[4];
    CHECK(out.filename() == "out.png");
    CHECK(out.parent_path() == rq.working_dir);
    CHECK(rq.timeout == std::chrono::milliseconds(1234));
    CHECK(rq.max_file_bytes == limits.max_output_bytes);
    CHECK(rq.address_space_bytes == limits.address_space_bytes);
    CHECK(rq.sandbox);
    CHECK(rq.read_only_paths == std::vector<std::filesystem::path>{src.path});
    CHECK(rq.writable_paths == std::vector<std::filesystem::path>{rq.working_dir});
    CHECK_FALSE(std::filesystem::exists(rq.working_dir)); // removed afterwards

    struct Failure {
        test::FakeRunner::Mode mode;
        ErrorCode code;
    };
    for (const Failure f : {Failure{test::FakeRunner::Mode::ExitFailure, ErrorCode::IoError},
                            Failure{test::FakeRunner::Mode::Timeout, ErrorCode::IoError},
                            Failure{test::FakeRunner::Mode::NoOutput, ErrorCode::IoError},
                            Failure{test::FakeRunner::Mode::Symlink, ErrorCode::IoError},
                            Failure{test::FakeRunner::Mode::Garbage, ErrorCode::InvalidArgument}}) {
        test::FakeRunner r2;
        r2.mode = f.mode;
        auto res = run_thumbnailer(*t, src, ThumbnailSize::Large, r2, limits, {});
        REQUIRE_FALSE(res);
        CHECK(res.error().code == f.code);
        CHECK_FALSE(std::filesystem::exists(r2.requests().at(0).working_dir));
    }
    std::stop_source stop;
    stop.request_stop();
    CHECK(run_thumbnailer(*t, src, ThumbnailSize::Large, runner, limits, stop.get_token())
              .error()
              .code == ErrorCode::Cancelled);
}
