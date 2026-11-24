// Tests for navigator/openwith.hpp: .desktop parsing, Exec expansion, MIME parents,
// mimeapps.list / mimeinfo.cache precedence, set_default_handler, and launch_with (mock launcher).
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/openwith.hpp>

#include "test_support.hpp"

namespace navigator::openwith {
namespace {

using platform_test::TempDir;
using platform_test::write_file;

class MockLauncher final : public platform::IAppLauncher {
public:
    std::vector<platform::LaunchRequest> requests;
    std::optional<Error> fail_on_call;
    std::size_t fail_index = 0;

    Result<void> launch(const platform::LaunchRequest& request) override {
        if (fail_on_call && requests.size() == fail_index) {
            return Unexpected<Error>(*fail_on_call);
        }
        requests.push_back(request);
        return {};
    }
};

std::string read_all(const Path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

Options make_options(const TempDir& tmp, std::set<std::string> programs = {"gedit", "code", "vim",
                                                                           "okular", "firefox"}) {
    Options opt;
    opt.home = tmp.path();
    opt.config_home = tmp / "config";
    opt.config_dirs = {tmp / "etc_xdg"};
    opt.data_home = tmp / "data_home";
    opt.data_dirs = {tmp / "usr_share"};
    opt.current_desktops = {"X-Cinnamon", "GNOME"};
    opt.locale = "tr_TR.UTF-8";
    opt.program_exists = [progs = std::move(programs)](std::string_view p) {
        return progs.contains(std::string(p));
    };
    return opt;
}

TEST_CASE("openwith::parse_desktop_entry parses fields, escapes, locales and rejects bad entries",
          "[platform][openwith]") {
    const std::string desktop = R"([Desktop Entry]
Type=Application
Name=Text Editor
Name[tr]=Metin Düzenleyici
Name[tr_TR]=Metin Düzenleyici TR
GenericName=Editor
GenericName[tr]=Düzenleyici
Comment=Line1\nLine2\tTab\sSpace\\Slash
Icon=org.gnome.gedit
Exec=gedit --new-window %U
TryExec=gedit
Path=/tmp
Terminal=false
NoDisplay=false
MimeType=text/plain;text/x-c++src;
OnlyShowIn=GNOME;X-Cinnamon;
NotShowIn=KDE;

[Desktop Action new-window]
Name=Ignored Action
Exec=ignored
)";

    const Result<Handler> h =
        parse_desktop_entry(desktop, "org.gnome.gedit.desktop",
                            "/usr/share/applications/org.gnome.gedit.desktop", "tr_TR.UTF-8");
    REQUIRE(h.has_value());
    CHECK(h->desktop_id == "org.gnome.gedit.desktop");
    CHECK(h->name == "Metin Düzenleyici TR");
    CHECK(h->generic_name == "Düzenleyici");
    CHECK(h->comment == "Line1\nLine2\tTab Space\\Slash");
    CHECK(h->icon_name == "org.gnome.gedit");
    CHECK(h->exec == "gedit --new-window %U");
    CHECK(h->try_exec == "gedit");
    CHECK(h->working_dir == Path("/tmp"));
    CHECK_FALSE(h->terminal);
    CHECK(h->accepts_multiple);
    CHECK(h->accepts_uris);
    CHECK(h->mime_types == std::vector<std::string>{"text/plain", "text/x-c++src"});
    CHECK(h->only_show_in == std::vector<std::string>{"GNOME", "X-Cinnamon"});
    CHECK(h->not_show_in == std::vector<std::string>{"KDE"});

    // C locale falls back to unlocalized keys.
    const Result<Handler> c_loc =
        parse_desktop_entry(desktop, "org.gnome.gedit.desktop", "/tmp/a.desktop", "C");
    REQUIRE(c_loc.has_value());
    CHECK(c_loc->name == "Text Editor");

    // Rejects Hidden=true, missing Exec, non-Application Type, missing group.
    CHECK_FALSE(
        parse_desktop_entry("[Desktop Entry]\nType=Application\nName=X\nExec=x %f\nHidden=true\n",
                            "x.desktop", "/x.desktop", "C")
            .has_value());
    CHECK_FALSE(parse_desktop_entry("[Desktop Entry]\nType=Link\nName=X\nExec=x %f\n", "x.desktop",
                                    "/x.desktop", "C")
                    .has_value());
    CHECK_FALSE(parse_desktop_entry("[Desktop Entry]\nType=Application\nName=X\n", "x.desktop",
                                    "/x.desktop", "C")
                    .has_value());
    CHECK_FALSE(parse_desktop_entry("Name=NoGroup\nExec=x %f\n", "x.desktop", "/x.desktop", "C")
                    .has_value());
}

TEST_CASE("openwith::shown_in evaluates OnlyShowIn and NotShowIn case-insensitively",
          "[platform][openwith]") {
    Handler h;
    const std::vector<std::string> gnome = {"X-Cinnamon", "GNOME"};
    const std::vector<std::string> empty;

    CHECK(shown_in(h, gnome));
    CHECK(shown_in(h, empty));

    h.only_show_in = {"gnome"};
    CHECK(shown_in(h, gnome));
    CHECK_FALSE(shown_in(h, empty));

    h.not_show_in = {"x-cinnamon"};
    CHECK_FALSE(shown_in(h, gnome));

    h.only_show_in.clear();
    CHECK_FALSE(shown_in(h, gnome));
    CHECK(shown_in(h, empty));
}

TEST_CASE(
    "openwith::parse_exec_tokens and expand_exec handle quoting, codes, chunking and terminal",
    "[platform][openwith]") {
    const auto tokens = parse_exec_tokens(R"(app "--flag with space" "a\"b\$c\\d" %F)");
    REQUIRE(tokens.has_value());
    CHECK(*tokens == std::vector<std::string>{"app", "--flag with space", R"(a"b$c\d)", "%F"});

    CHECK_FALSE(parse_exec_tokens("").has_value());
    CHECK_FALSE(parse_exec_tokens(R"(app "unterminated)").has_value());

    Handler multi;
    multi.name = "My App";
    multi.icon_name = "my-icon";
    multi.desktop_file = "/usr/share/applications/my.desktop";
    multi.exec = "myapp %i --name=%c --desktop=%k --pct=%% %d %F";
    const std::vector<Path> paths = {"/tmp/a b.txt", "/tmp/$quote\"file.txt"};

    const auto expanded = expand_exec(multi, paths, {});
    REQUIRE(expanded.has_value());
    REQUIRE(expanded->size() == 1U);
    CHECK((*expanded)[0] == std::vector<std::string>{"myapp", "--icon", "my-icon", "--name=My App",
                                                     "--desktop=/usr/share/applications/my.desktop",
                                                     "--pct=%", "/tmp/a b.txt",
                                                     "/tmp/$quote\"file.txt"});

    // Single-file %f produces one invocation per path.
    Handler single;
    single.name = "Single";
    single.exec = "viewer --file=%f";
    const auto single_exp = expand_exec(single, paths, {});
    REQUIRE(single_exp.has_value());
    REQUIRE(single_exp->size() == 2U);
    CHECK((*single_exp)[0] == std::vector<std::string>{"viewer", "--file=/tmp/a b.txt"});
    CHECK((*single_exp)[1] == std::vector<std::string>{"viewer", "--file=/tmp/$quote\"file.txt"});

    // %U encodes file:// URIs.
    Handler uri_app;
    uri_app.name = "Uri";
    uri_app.exec = "browser %U";
    const std::vector<Path> one_space = {Path("/tmp/hello world.txt")};
    const auto uri_exp = expand_exec(uri_app, one_space, {});
    REQUIRE(uri_exp.has_value());
    REQUIRE(uri_exp->size() == 1U);
    CHECK((*uri_exp)[0] == std::vector<std::string>{"browser", "file:///tmp/hello%20world.txt"});

    // Terminal=true requires terminal_prefix.
    Handler term;
    term.name = "Vim";
    term.exec = "vim %F";
    term.terminal = true;
    const std::vector<Path> one_a = {Path("/tmp/a.txt")};
    CHECK(expand_exec(term, one_a, {}).error().code == ErrorCode::Unsupported);
    ExpandOptions term_opt;
    term_opt.terminal_prefix = {"gnome-terminal", "--"};
    const auto term_exp = expand_exec(term, one_a, term_opt);
    REQUIRE(term_exp.has_value());
    CHECK((*term_exp)[0] == std::vector<std::string>{"gnome-terminal", "--", "vim", "/tmp/a.txt"});

    // Rejects relative paths, empty paths, unknown field codes.
    const std::vector<Path> empty_paths;
    const std::vector<Path> rel_paths = {Path("relative.txt")};
    CHECK_FALSE(expand_exec(multi, empty_paths, {}).has_value());
    CHECK_FALSE(expand_exec(multi, rel_paths, {}).has_value());
    Handler bad_code;
    bad_code.exec = "app %z %f";
    CHECK_FALSE(expand_exec(bad_code, one_a, {}).has_value());
}

TEST_CASE("openwith::default_terminal_prefix follows fixed priority", "[platform][openwith]") {
    const auto none = default_terminal_prefix([](std::string_view) { return false; });
    CHECK(none.empty());

    const auto kitty =
        default_terminal_prefix([](std::string_view p) { return p == "kitty" || p == "xterm"; });
    CHECK(kitty == std::vector<std::string>{"kitty"});

    const auto gnome = default_terminal_prefix(
        [](std::string_view p) { return p == "gnome-terminal" || p == "kitty"; });
    CHECK(gnome == std::vector<std::string>{"gnome-terminal", "--"});
}

TEST_CASE("openwith::mime_parents resolves aliases, subclasses and implicit rules",
          "[platform][openwith]") {
    TempDir tmp;
    Options opt = make_options(tmp);
    write_file(opt.data_dirs[0] / "mime" / "aliases", "text/xml application/xml\n");
    write_file(opt.data_dirs[0] / "mime" / "subclasses", "text/x-c++src text/x-csrc\n"
                                                         "text/x-csrc text/plain\n"
                                                         "text/plain application/octet-stream\n");

    const auto parents = mime_parents("text/x-c++src", opt);
    REQUIRE(parents.has_value());
    CHECK(*parents == std::vector<std::string>{"text/x-csrc", "text/plain"});

    // Implicit *+xml -> application/xml.
    const auto svg = mime_parents("image/svg+xml", opt);
    REQUIRE(svg.has_value());
    CHECK(*svg == std::vector<std::string>{"application/xml"});

    // Implicit text/* -> text/plain.
    const auto log = mime_parents("text/x-log", opt);
    REQUIRE(log.has_value());
    CHECK(*log == std::vector<std::string>{"text/plain"});

    // Invalid MIME rejected.
    CHECK(mime_parents("notamime", opt).error().code == ErrorCode::InvalidArgument);
    CHECK(mime_parents("a/b/c", opt).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("openwith::list_handlers, default_handler, find_handler and set_default_handler",
          "[platform][openwith]") {
    TempDir tmp;
    Options opt = make_options(tmp);

    // Install desktop files (including subdir dash-to-slash kde4-okular.desktop).
    write_file(opt.data_dirs[0] / "applications" / "gedit.desktop",
               "[Desktop Entry]\nType=Application\nName=Gedit\nExec=gedit %U\nTryExec=gedit\n"
               "MimeType=text/plain;\n");
    write_file(opt.data_dirs[0] / "applications" / "code.desktop",
               "[Desktop Entry]\nType=Application\nName=Code\nExec=code %F\nTryExec=code\n"
               "MimeType=text/plain;text/x-c++src;\n");
    write_file(opt.data_dirs[0] / "applications" / "vim.desktop",
               "[Desktop Entry]\nType=Application\nName=Vim\nExec=vim %F\nTryExec=vim\n"
               "Terminal=true\nMimeType=text/plain;\n");
    write_file(opt.data_dirs[0] / "applications" / "kde4" / "okular.desktop",
               "[Desktop Entry]\nType=Application\nName=Okular\nExec=okular %U\nTryExec=okular\n"
               "MimeType=text/plain;\n");
    // Filtered out: missing %f/%F/%u/%U (launcher only).
    write_file(
        opt.data_dirs[0] / "applications" / "launcher.desktop",
        "[Desktop Entry]\nType=Application\nName=Launcher\nExec=gedit\nMimeType=text/plain;\n");
    // Filtered out: TryExec not found.
    write_file(opt.data_dirs[0] / "applications" / "missing.desktop",
               "[Desktop Entry]\nType=Application\nName=Missing\nExec=nope %f\nTryExec=nope\n"
               "MimeType=text/plain;\n");

    write_file(
        opt.data_dirs[0] / "applications" / "mimeinfo.cache",
        "[MIME Cache]\n"
        "text/"
        "plain=gedit.desktop;vim.desktop;kde4-okular.desktop;launcher.desktop;missing.desktop;\n"
        "text/x-c++src=code.desktop;\n");
    write_file(opt.data_dirs[0] / "mime" / "subclasses", "text/x-c++src text/plain\n");

    // User mimeapps.list sets gedit as default for text/plain and removes vim.desktop.
    write_file(opt.config_home / "mimeapps.list", "# keep this comment\n"
                                                  "[Default Applications]\n"
                                                  "text/plain=gedit.desktop;\n"
                                                  "[Removed Associations]\n"
                                                  "text/plain=vim.desktop;\n"
                                                  "[Other Group]\n"
                                                  "foo=bar\n");

    const auto plain = list_handlers("text/plain", opt);
    REQUIRE(plain.has_value());
    REQUIRE(plain->size() == 2U);
    CHECK((*plain)[0].desktop_id == "gedit.desktop");
    CHECK((*plain)[0].is_default);
    CHECK_FALSE((*plain)[0].via_parent_type);
    CHECK((*plain)[1].desktop_id == "kde4-okular.desktop");
    CHECK_FALSE((*plain)[1].is_default);

    const auto def = default_handler("text/plain", opt);
    REQUIRE(def.has_value());
    CHECK(def->desktop_id == "gedit.desktop");

    // text/x-c++src gets code.desktop first, then text/plain handlers with via_parent_type = true.
    const auto cpp = list_handlers("text/x-c++src", opt);
    REQUIRE(cpp.has_value());
    REQUIRE(cpp->size() == 3U);
    CHECK((*cpp)[0].desktop_id == "code.desktop");
    CHECK_FALSE((*cpp)[0].via_parent_type);
    CHECK((*cpp)[1].desktop_id == "gedit.desktop");
    CHECK((*cpp)[1].via_parent_type);
    CHECK((*cpp)[2].desktop_id == "kde4-okular.desktop");
    CHECK((*cpp)[2].via_parent_type);

    // find_handler resolves dash-to-slash and rejects invalid ids.
    REQUIRE(find_handler("kde4-okular.desktop", opt).has_value());
    CHECK(find_handler("bad/id.desktop", opt).error().code == ErrorCode::InvalidArgument);
    CHECK(find_handler("notinstalled.desktop", opt).error().code == ErrorCode::NotFound);

    // set_default_handler switches text/plain default to vim.desktop (removing it from Removed),
    // preserving unrelated comments and groups.
    REQUIRE(set_default_handler("text/plain", "vim.desktop", opt).has_value());
    const std::string updated = read_all(opt.config_home / "mimeapps.list");
    CHECK(updated.find("# keep this comment") != std::string::npos);
    CHECK(updated.find("[Other Group]\nfoo=bar") != std::string::npos);

    const auto new_def = default_handler("text/plain", opt);
    REQUIRE(new_def.has_value());
    CHECK(new_def->desktop_id == "vim.desktop");
    CHECK(new_def->is_default);

    // set_default_handler rejects uninstalled app.
    CHECK(set_default_handler("text/plain", "nonexistent.desktop", opt).error().code ==
          ErrorCode::NotFound);
}

TEST_CASE("openwith::launch_with invokes IAppLauncher per expanded invocation",
          "[platform][openwith]") {
    Handler h;
    h.name = "Single";
    h.exec = "viewer %f";
    h.working_dir = "/tmp";

    MockLauncher launcher;
    const std::vector<Path> paths = {"/tmp/one.txt", "/tmp/two.txt"};
    REQUIRE(launch_with(h, paths, launcher).has_value());
    REQUIRE(launcher.requests.size() == 2U);
    CHECK(launcher.requests[0].argv == std::vector<std::string>{"viewer", "/tmp/one.txt"});
    CHECK(launcher.requests[0].working_dir == Path("/tmp"));
    CHECK(launcher.requests[1].argv == std::vector<std::string>{"viewer", "/tmp/two.txt"});

    // Stops at first failure.
    MockLauncher failing;
    failing.fail_on_call = Error{ErrorCode::NotFound, "missing binary"};
    failing.fail_index = 1;
    const Result<void> res = launch_with(h, paths, failing);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().code == ErrorCode::NotFound);
    CHECK(failing.requests.size() == 1U);
}

} // namespace
} // namespace navigator::openwith
