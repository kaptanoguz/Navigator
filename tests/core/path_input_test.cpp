// navigator::path_input: normalization, the expand_user_path matrix (pure, fake environment),
// case correction and completion on temporary directories, shell quoting and Copy as Path.
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/fs.hpp>
#include <navigator/path_input.hpp>

#include "temp_dir.hpp"

using namespace navigator;
using navigator::test::TempDir;
namespace pi = navigator::path_input;

namespace {

pi::ExpandContext test_context() {
    pi::ExpandContext ctx;
    ctx.home = "/home/u";
    ctx.cwd = "/home/u/src";
    const std::map<std::string, std::string, std::less<>> env{
        {"HOME", "/home/u"},        {"XDG_X", "/data/x"},   {"EMPTY", ""},
        {"USERPROFILE", "/home/u"}, {"TILDE", "~/literal"}, {"DOLLAR", "$HOME"},
        {"RAW", "/raw/\xff"},       {"A1_", "a1"}};
    ctx.env = [env](std::string_view name) -> std::optional<std::string> {
        const auto it = env.find(name);
        if (it == env.end()) {
            return std::nullopt;
        }
        return it->second;
    };
    ctx.user_home = [](std::string_view user) -> std::optional<Path> {
        if (user == "ayşe") {
            return Path("/home/ayşe");
        }
        if (user == "root") {
            return Path("/root");
        }
        return std::nullopt;
    };
    return ctx;
}

std::string expand(std::string_view input, const pi::ExpandContext& ctx = test_context()) {
    Result<pi::ExpandedPath> r = pi::expand_user_path(input, ctx);
    INFO("input: " << input);
    REQUIRE(r);
    return r->path.native();
}

std::vector<std::string> unresolved(std::string_view input,
                                    const pi::ExpandContext& ctx = test_context()) {
    Result<pi::ExpandedPath> r = pi::expand_user_path(input, ctx);
    REQUIRE(r);
    return r->unresolved;
}

fs::DirEntry dir_entry(std::string name, fs::EntryKind kind = fs::EntryKind::Directory) {
    fs::DirEntry e;
    e.name = std::move(name);
    e.kind = kind;
    e.hidden = fs::is_hidden_name(e.name);
    return e;
}

std::vector<std::string> names_of(const std::vector<pi::Completion>& completions) {
    std::vector<std::string> names;
    names.reserve(completions.size());
    for (const pi::Completion& c : completions) {
        names.push_back(c.name);
    }
    return names;
}

} // namespace

TEST_CASE("path_input: normalize_absolute", "[path_input]") {
    CHECK(pi::normalize_absolute("/") == "/");
    CHECK(pi::normalize_absolute("//") == "/");
    CHECK(pi::normalize_absolute("/a//b/./c/") == "/a/b/c");
    CHECK(pi::normalize_absolute("/a/b/../c") == "/a/c");
    CHECK(pi::normalize_absolute("/../..") == "/");
    CHECK(pi::normalize_absolute("/a/../../b") == "/b");
    CHECK(pi::normalize_absolute("/a/...") == "/a/...");
    CHECK(pi::normalize_absolute(std::string("/x/\xff/")).native() == std::string("/x/\xff"));
    CHECK(pi::normalize_absolute("").empty());
    CHECK(pi::normalize_absolute("rel/a").empty());
    CHECK(pi::normalize_absolute("./a").empty());
}

TEST_CASE("path_input: expand_user_path documented examples", "[path_input][expand]") {
    CHECK(expand("~/Downloads/") == "/home/u/Downloads");
    CHECK(expand("/") == "/");
    CHECK(expand("..") == "/home/u");
    CHECK(expand("$HOME/Music") == "/home/u/Music");
    CHECK(expand("${HOME}//x/./y") == "/home/u/x/y");
    CHECK(expand("%HOME%\n") == "/home/u");
    CHECK(expand("'/tmp/a b'") == "/tmp/a b");
    CHECK(expand("$NOPE/x") == "/home/u/src/$NOPE/x");
    CHECK(unresolved("$NOPE/x") == std::vector<std::string>{"$NOPE"});
}

TEST_CASE("path_input: expand_user_path matrix", "[path_input][expand]") {
    const pi::ExpandContext ctx = test_context();
    SECTION("tilde") {
        CHECK(expand("~") == "/home/u");
        CHECK(expand("~/") == "/home/u");
        CHECK(expand("~ayşe/Belgeler") == "/home/ayşe/Belgeler");
        CHECK(expand("~root") == "/root");
        CHECK(expand("~nobody/x") == "/home/u/src/~nobody/x");
        CHECK(unresolved("~nobody/x") == std::vector<std::string>{"~nobody"});
        CHECK(expand("a/~/b") == "/home/u/src/a/~/b"); // only at the very start
        CHECK(unresolved("a/~/b").empty());
        pi::ExpandContext no_home = ctx;
        no_home.home.clear();
        no_home.user_home = nullptr;
        CHECK(expand("~/x", no_home) == "/home/u/src/~/x");
        CHECK(unresolved("~/x", no_home) == std::vector<std::string>{"~"});
        CHECK(unresolved("~ayşe", no_home) == std::vector<std::string>{"~ayşe"});
        // Values are not expanded again; tilde after a variable is literal.
        CHECK(expand("$TILDE") == "/home/u/src/~/literal");
        CHECK(expand("~$DOLLAR") == "/home/u/src/~$DOLLAR");
    }
    SECTION("variables") {
        CHECK(expand("${XDG_X}/y") == "/data/x/y");
        CHECK(expand("$XDG_X/y") == "/data/x/y");
        CHECK(expand("$HOME$EMPTY/z") == "/home/u/z");
        CHECK(expand("/a/$A1_/b") == "/a/a1/b");
        CHECK(expand("$DOLLAR/q") == "/home/u/src/$HOME/q"); // inserted verbatim
        CHECK(expand("/a/$/b$") == "/a/$/b$");
        CHECK(expand("/a/$1") == "/a/$1");
        CHECK(expand("/a/${A:-b}") == "/a/${A:-b}");
        CHECK(unresolved("/a/${A:-b}").empty());
        CHECK(expand("/a/${HOME") == "/a/${HOME");
        CHECK(unresolved("/a/${HOME").empty());
        CHECK(expand("/x/${NOPE}/$NOPE2") == "/x/${NOPE}/$NOPE2");
        CHECK(unresolved("/x/${NOPE}/$NOPE2/%NOPE3%") ==
              std::vector<std::string>{"${NOPE}", "$NOPE2", "%NOPE3%"});
        CHECK(expand("$RAW/y") == std::string("/raw/\xff/y"));
        pi::ExpandContext no_env = ctx;
        no_env.env = nullptr;
        CHECK(expand("/a/$HOME", no_env) == "/a/$HOME");
        CHECK(unresolved("/a/$HOME/%HOME%", no_env) == std::vector<std::string>{"$HOME", "%HOME%"});
    }
    SECTION("percent variables on and off") {
        CHECK(expand("%USERPROFILE%") == "/home/u");
        CHECK(expand("%USERPROFILE%/Desktop") == "/home/u/Desktop");
        CHECK(expand("/a%20b") == "/a%20b");
        CHECK(expand("/100%") == "/100%");
        CHECK(expand("/%HOME") == "/%HOME");
        CHECK(expand("/a/%HOME%%XDG_X%") == "/a/home/u/data/x");
        pi::ExpandContext off = ctx;
        off.percent_variables = false;
        CHECK(expand("%USERPROFILE%", off) == "/home/u/src/%USERPROFILE%");
        CHECK(unresolved("%USERPROFILE%", off).empty());
        CHECK(expand("$HOME", off) == "/home/u");
    }
    SECTION("quotes and whitespace") {
        CHECK(expand("  \"/tmp/x y\"  \r\n") == "/tmp/x y");
        CHECK(expand("'~'") == "/home/u");
        CHECK(expand("'/a\"b'") == "/a\"b");
        CHECK(expand("'/a") == "/home/u/src/'/a"); // unbalanced: literal
        CHECK(expand("/a'") == "/a'");
        CHECK(expand("\"/a'") == "/home/u/src/\"/a'");
        CHECK(expand("/a\\ b") == "/a\\ b"); // no backslash escapes
        CHECK(pi::expand_user_path("   \t\n", ctx).error().code == ErrorCode::InvalidArgument);
        CHECK(pi::expand_user_path("''", ctx).error().code == ErrorCode::InvalidArgument);
        CHECK(pi::expand_user_path("", ctx).error().code == ErrorCode::InvalidArgument);
    }
    SECTION("file URIs") {
        CHECK(expand("file:///a%20b") == "/a b");
        CHECK(expand("FILE:///tmp/%C4%B0stanbul/") == "/tmp/İstanbul");
        CHECK(expand("file://localhost/etc") == "/etc");
        CHECK(expand("file://LocalHost/etc") == "/etc");
        CHECK(expand("file:///raw/%ff") == std::string("/raw/\xff"));
        CHECK(expand("file:///bad%zz%4") == "/bad%zz%4");
        CHECK(expand("file:///$HOME/~") == "/$HOME/~"); // no variable or tilde processing
        CHECK(unresolved("file:///$HOME").empty());
        CHECK(expand("'file:///a b/'") == "/a b");
        CHECK(expand("file://") == "/");
        CHECK(pi::expand_user_path("file://otherhost/a", ctx).error().code ==
              ErrorCode::InvalidArgument);
        CHECK(pi::expand_user_path("file://server.lan", ctx).error().code ==
              ErrorCode::InvalidArgument);
    }
    SECTION("relative input and dot segments") {
        CHECK(expand(".") == "/home/u/src");
        CHECK(expand("lib/../include/") == "/home/u/src/include");
        CHECK(expand("../../../../..") == "/");
        CHECK(expand("Çalışma Alanı") == "/home/u/src/Çalışma Alanı");
        CHECK(expand("$EMPTY") == "/home/u/src");
        CHECK(expand("/a/b/../") == "/a");
        CHECK(expand("///a///") == "/a");
        pi::ExpandContext no_cwd = ctx;
        no_cwd.cwd.clear();
        CHECK(pi::expand_user_path("x", no_cwd).error().code == ErrorCode::InvalidArgument);
        CHECK(expand("/abs", no_cwd) == "/abs");
        no_cwd.cwd = "rel";
        CHECK(pi::expand_user_path("x", no_cwd).error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("path_input: fix_path_case on a temporary tree", "[path_input][case]") {
    TempDir tmp;
    tmp.dir("Documents");
    tmp.dir("Documents/Projects");
    tmp.file("Documents/Projects/Notes.TXT");
    tmp.dir("İndirilenler");
    tmp.dir("ılık");
    tmp.dir("Foo");
    tmp.dir("foo");
    tmp.dir("FOO2");
    tmp.file("Readme");
    tmp.dir("readme.d");
    const std::string base = tmp.path().native();

    SECTION("existing paths are returned unchanged") {
        const Result<pi::CaseFix> fix = pi::fix_path_case(base + "/Documents/./Projects/");
        REQUIRE(fix);
        CHECK(fix->path == base + "/Documents/Projects");
        CHECK_FALSE(fix->changed);
        CHECK(fix->exists);
        // Exact names always win even when a case-variant also exists.
        const Result<pi::CaseFix> exact = pi::fix_path_case(base + "/foo");
        REQUIRE(exact);
        CHECK(exact->path == base + "/foo");
        CHECK_FALSE(exact->changed);
    }
    SECTION("unique case-insensitive matches are corrected") {
        const Result<pi::CaseFix> fix = pi::fix_path_case(base + "/documents/PROJECTS/notes.txt");
        REQUIRE(fix);
        CHECK(fix->path == base + "/Documents/Projects/Notes.TXT");
        CHECK(fix->changed);
        CHECK(fix->exists);
        const Result<pi::CaseFix> two = pi::fix_path_case(base + "/foo2");
        REQUIRE(two);
        CHECK(two->path == base + "/FOO2");
    }
    SECTION("Turkish dotted and dotless I fold together") {
        Result<pi::CaseFix> fix = pi::fix_path_case(base + "/indirilenler");
        REQUIRE(fix);
        CHECK(fix->path == base + "/İndirilenler");
        CHECK(fix->exists);
        fix = pi::fix_path_case(base + "/INDIRILENLER");
        REQUIRE(fix);
        CHECK(fix->path == base + "/İndirilenler");
        fix = pi::fix_path_case(base + "/Ilık");
        REQUIRE(fix);
        CHECK(fix->path == base + "/ılık");
    }
    SECTION("ambiguous or missing components stop the walk") {
        Result<pi::CaseFix> fix = pi::fix_path_case(base + "/FOO/x");
        REQUIRE(fix);
        CHECK(fix->path == base + "/FOO/x");
        CHECK_FALSE(fix->changed);
        CHECK_FALSE(fix->exists);
        fix = pi::fix_path_case(base + "/documents/Missing/projects");
        REQUIRE(fix);
        CHECK(fix->path == base + "/Documents/Missing/projects");
        CHECK(fix->changed);
        CHECK_FALSE(fix->exists);
    }
    SECTION("non-last components must be folders") {
        // "Readme" is a file, so it is no candidate for "README/"; "readme.d" does not fold equal.
        const Result<pi::CaseFix> fix = pi::fix_path_case(base + "/README/x");
        REQUIRE(fix);
        CHECK(fix->path == base + "/README/x");
        CHECK_FALSE(fix->exists);
        const Result<pi::CaseFix> last = pi::fix_path_case(base + "/README");
        REQUIRE(last);
        CHECK(last->path == base + "/Readme");
        CHECK(last->exists);
    }
    SECTION("NFC and NFD spellings are not equivalent") {
        tmp.dir("caf\xc3\xa9"); // NFC
        const Result<pi::CaseFix> fix = pi::fix_path_case(base + "/CAFE\xcc\x81");
        REQUIRE(fix);
        CHECK_FALSE(fix->changed);
        CHECK_FALSE(fix->exists);
    }
    SECTION("errors") {
        CHECK(pi::fix_path_case("").error().code == ErrorCode::InvalidArgument);
        CHECK(pi::fix_path_case("rel").error().code == ErrorCode::InvalidArgument);
        std::stop_source stop;
        stop.request_stop();
        CHECK(pi::fix_path_case(base + "/documents", stop.get_token()).error().code ==
              ErrorCode::Cancelled);
    }
}

TEST_CASE("path_input: resolve_input", "[path_input][case]") {
    TempDir tmp;
    tmp.dir("Downloads");
    tmp.file("Downloads/Report.pdf");
    pi::ExpandContext ctx = test_context();
    ctx.home = tmp.path();
    ctx.cwd = tmp.path();

    Result<pi::ResolvedInput> r = pi::resolve_input("~/Downloads", ctx);
    REQUIRE(r);
    CHECK(r->path == tmp.path() / "Downloads");
    CHECK(r->is_directory());
    CHECK_FALSE(r->case_corrected);
    r = pi::resolve_input("/", ctx);
    REQUIRE(r);
    CHECK(r->path == "/");
    CHECK(r->is_directory());
    r = pi::resolve_input("~/downloads/report.PDF\n", ctx);
    REQUIRE(r);
    CHECK(r->path == tmp.path() / "Downloads" / "Report.pdf");
    CHECK(r->exists());
    CHECK_FALSE(r->is_directory());
    CHECK(r->case_corrected);
    r = pi::resolve_input("$NOPE/x", ctx);
    REQUIRE(r);
    CHECK_FALSE(r->exists());
    CHECK(r->unresolved == std::vector<std::string>{"$NOPE"});
    CHECK(pi::resolve_input("file://far/x", ctx).error().code == ErrorCode::InvalidArgument);
    std::stop_source stop;
    stop.request_stop();
    CHECK(pi::resolve_input("~/Downloads", ctx, stop.get_token()).error().code ==
          ErrorCode::Cancelled);
}

TEST_CASE("path_input: parse_completion_input", "[path_input][complete]") {
    const pi::ExpandContext ctx = test_context();
    Result<pi::CompletionQuery> q = pi::parse_completion_input("~/Dow", ctx);
    REQUIRE(q);
    CHECK(q->typed_dir == "~/");
    CHECK(q->dir == "/home/u");
    CHECK(q->prefix == "Dow");
    q = pi::parse_completion_input("~", ctx);
    REQUIRE(q);
    CHECK(q->typed_dir == "~/");
    CHECK(q->dir == "/home/u");
    CHECK(q->prefix.empty());
    q = pi::parse_completion_input("~ayşe", ctx);
    REQUIRE(q);
    CHECK(q->typed_dir == "~ayşe/");
    CHECK(q->dir == "/home/ayşe");
    q = pi::parse_completion_input("lib", ctx);
    REQUIRE(q);
    CHECK(q->typed_dir.empty());
    CHECK(q->dir == "/home/u/src");
    CHECK(q->prefix == "lib");
    q = pi::parse_completion_input("  '$HOME/Mü", ctx);
    REQUIRE(q);
    CHECK(q->typed_dir == "$HOME/");
    CHECK(q->dir == "/home/u");
    CHECK(q->prefix == "Mü");
    q = pi::parse_completion_input("'/tmp/a b'", ctx);
    REQUIRE(q);
    CHECK(q->typed_dir == "/tmp/");
    CHECK(q->prefix == "a b");
    q = pi::parse_completion_input("/", ctx);
    REQUIRE(q);
    CHECK(q->dir == "/");
    CHECK(q->typed_dir == "/");
    CHECK(q->prefix.empty());
    pi::ExpandContext no_cwd = ctx;
    no_cwd.cwd.clear();
    CHECK(pi::parse_completion_input("x", no_cwd).error().code == ErrorCode::InvalidArgument);
    CHECK(pi::parse_completion_input("x/y", no_cwd).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("path_input: match_completions ranking and filters", "[path_input][complete]") {
    pi::CompletionQuery query;
    query.typed_dir = "~/";
    query.dir = "/home/u";
    query.prefix = "do";
    fs::DirEntry link = dir_entry("Docs-link", fs::EntryKind::Symlink);
    link.target_kind = fs::EntryKind::Directory;
    fs::DirEntry file_link = dir_entry("doc.lnk", fs::EntryKind::Symlink);
    file_link.target_kind = fs::EntryKind::File;
    const std::vector<fs::DirEntry> entries{
        dir_entry("Videos"),
        dir_entry("Downloads"),
        dir_entry("Photos dog"),
        dir_entry("documents10"),
        dir_entry("Documents9"),
        dir_entry(".dotfiles"),
        dir_entry(".hidden-do"),
        dir_entry("notes.doc", fs::EntryKind::File),
        dir_entry(std::string("do\xff")),
        link,
        file_link,
        dir_entry("İDO"),
    };
    std::vector<pi::Completion> got = pi::match_completions(query, entries);
    std::vector<std::string> names = names_of(got);
    CHECK(names == std::vector<std::string>{"Docs-link", "Documents9", "documents10", "Downloads",
                                            "İDO", "Photos dog"});
    REQUIRE(got.size() == 6);
    CHECK(got[0].match == pi::CompletionMatch::Prefix);
    CHECK(got[0].is_symlink);
    CHECK(got[0].is_directory);
    CHECK(got[0].text == "~/Docs-link/");
    CHECK(got[0].path == "/home/u/Docs-link");
    CHECK(got[4].match == pi::CompletionMatch::Substring);
    CHECK(got[5].match == pi::CompletionMatch::Substring);

    SECTION("files, hidden and substring options") {
        pi::CompletionOptions options;
        options.include_files = true;
        options.include_hidden = true;
        options.substring_matches = false;
        got = pi::match_completions(query, entries, options);
        names = names_of(got);
        CHECK(names == std::vector<std::string>{"doc.lnk", "Docs-link", "Documents9", "documents10",
                                                "Downloads"});
        CHECK(got[0].text == "~/doc.lnk");
        CHECK_FALSE(got[0].is_directory);
    }
    SECTION("a dot prefix offers hidden names") {
        query.prefix = ".d";
        got = pi::match_completions(query, entries);
        REQUIRE(got.size() == 1);
        CHECK(got[0].name == ".dotfiles");
    }
    SECTION("an empty prefix offers every visible folder, bounded") {
        query.prefix.clear();
        pi::CompletionOptions options;
        options.max_results = 3;
        got = pi::match_completions(query, entries, options);
        REQUIRE(got.size() == 3);
        CHECK(got[0].name == "Docs-link");
        options.max_results = 0; // treated as 1
        CHECK(pi::match_completions(query, entries, options).size() == 1);
        options.max_results = 1000;
        CHECK(pi::match_completions(query, entries, options).size() == 7);
    }
    SECTION("Turkish I folding in prefixes") {
        query.prefix = "ido";
        got = pi::match_completions(query, entries);
        REQUIRE(got.size() == 1);
        CHECK(got[0].name == "İDO");
    }
}

TEST_CASE("path_input: complete_path on a temporary tree", "[path_input][complete]") {
    TempDir tmp;
    tmp.dir("Downloads");
    tmp.dir("Documents");
    tmp.dir("Documents/Projeler");
    tmp.dir("Documents/Proje Ödevleri");
    tmp.file("Documents/proje.txt");
    tmp.dir(std::string("Documents/Proj\xff"));
    tmp.dir("Desktop");
    pi::ExpandContext ctx = test_context();
    ctx.home = tmp.path();
    ctx.cwd = tmp.path();

    Result<std::vector<pi::Completion>> r = pi::complete_path("~/Dow", ctx);
    REQUIRE(r);
    REQUIRE(r->size() == 1);
    CHECK((*r)[0].text == "~/Downloads/");

    r = pi::complete_path("~/Documents/proj", ctx);
    REQUIRE(r);
    REQUIRE(r->size() == 2); // folders only; the non-UTF-8 name is skipped
    CHECK((*r)[0].name == "Proje Ödevleri");
    CHECK((*r)[1].name == "Projeler");

    // A mis-cased directory part is corrected and typed_dir becomes the absolute folder.
    r = pi::complete_path("~/documents/proj", ctx);
    REQUIRE(r);
    REQUIRE(r->size() == 2);
    CHECK((*r)[1].text == tmp.path().native() + "/Documents/Projeler/");

    r = pi::complete_path("~/nope/x", ctx);
    REQUIRE(r);
    CHECK(r->empty());
    r = pi::complete_path("D", ctx);
    REQUIRE(r);
    CHECK(r->size() == 3);
    CHECK((*r)[0].text == "Desktop/");

    std::stop_source stop;
    stop.request_stop();
    CHECK(pi::complete_path("~/D", ctx, {}, stop.get_token()).error().code == ErrorCode::Cancelled);
}

TEST_CASE("path_input: quote_for_shell", "[path_input][copy]") {
    CHECK(pi::quote_for_shell("/home/u/file.txt") == "/home/u/file.txt");
    CHECK(pi::quote_for_shell("a_b@c%d+e=f:g,h.i/j-k") == "a_b@c%d+e=f:g,h.i/j-k");
    CHECK(pi::quote_for_shell("/home/u/Çalışma") == "/home/u/Çalışma");
    CHECK(pi::quote_for_shell(std::string("/raw/\xff")) == std::string("/raw/\xff"));
    CHECK(pi::quote_for_shell("") == "''");
    CHECK(pi::quote_for_shell("/a b") == "'/a b'");
    CHECK(pi::quote_for_shell("it's") == "'it'\\''s'");
    CHECK(pi::quote_for_shell("$HOME") == "'$HOME'");
    CHECK(pi::quote_for_shell("~/x") == "'~/x'");
    CHECK(pi::quote_for_shell("a\nb") == "'a\nb'");
    CHECK(pi::quote_for_shell("*?[]!;&|<>()\"\\`#") == "'*?[]!;&|<>()\"\\`#'");
}

TEST_CASE("path_input: format_paths_for_copy", "[path_input][copy]") {
    const std::vector<Path> one{"/home/u/a b"};
    const std::vector<Path> two{"/home/u/a b", "/tmp/c"};
    CHECK(pi::format_paths_for_copy({}).empty());
    CHECK(pi::format_paths_for_copy(one) == "/home/u/a b");
    CHECK(pi::format_paths_for_copy(two) == "'/home/u/a b' /tmp/c");
    CHECK(pi::format_paths_for_copy(one, pi::PathQuoting::Never) == "/home/u/a b");
    CHECK(pi::format_paths_for_copy(two, pi::PathQuoting::Never) == "/home/u/a b\n/tmp/c");
    CHECK(pi::format_paths_for_copy(one, pi::PathQuoting::Shell) == "'/home/u/a b'");
    CHECK(pi::format_paths_for_copy(two, pi::PathQuoting::Shell) == "'/home/u/a b' /tmp/c");
    // What Copy as Path produces for one path pastes back into GoTo.
    const pi::ExpandContext ctx = test_context();
    CHECK(expand(pi::format_paths_for_copy(one, pi::PathQuoting::Shell), ctx) == "/home/u/a b");
}
