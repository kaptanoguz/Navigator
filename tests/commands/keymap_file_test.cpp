// Phase 8 tests: key sequences, SequenceMatcher, KeymapFile JSON & atomic I/O, layering
// (apply_keymap_file / build_keymap / effective_aliases), and editor helpers.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <navigator/commands.hpp>
#include <navigator/keymap_file.hpp>

namespace navigator {
namespace {

class TempDir {
public:
    TempDir() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "navigator-cmd-XXXXXX").native();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        path_ = pattern;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] const Path& path() const noexcept { return path_; }
    [[nodiscard]] Path operator/(std::string_view name) const { return path_ / std::string(name); }

private:
    Path path_;
};

KeySequence seq(std::string_view text) {
    const Result<KeySequence> r = parse_key_sequence(text);
    REQUIRE(r.has_value());
    return *r;
}

KeyChord ch(std::string_view text) {
    const Result<KeyChord> r = parse_key_chord(text);
    REQUIRE(r.has_value());
    return *r;
}

CommandRegistry builtins() {
    CommandRegistry reg;
    REQUIRE(register_builtin_commands(reg).has_value());
    return reg;
}

TEST_CASE("Phase 8 key synonyms, keypad, Menu and KeySequence parse/validate",
          "[commands][sequence]") {
    CHECK(to_string(ch("Numpad5")) == "Keypad5");
    CHECK(to_string(ch("NumpadEnter")) == "KeypadEnter");
    CHECK(to_string(ch("ContextMenu")) == "Menu");
    CHECK(to_string(ch("Esc")) == "Escape");
    CHECK(to_string(ch("Return")) == "Enter");
    CHECK(to_string(ch("Del")) == "Delete");
    CHECK(to_string(ch("Ins")) == "Insert");
    CHECK(to_string(ch("PgUp")) == "PageUp");
    CHECK(to_string(ch("PgDn")) == "PageDown");

    const KeySequence ks = seq("ctrl+k   ctrl+s");
    CHECK(ks.size() == 2U);
    CHECK(to_string(ks) == "Ctrl+K Ctrl+S");
    CHECK(validate_sequence(ks).has_value());

    // V1: empty or > kMaxSequenceLength.
    CHECK_FALSE(parse_key_sequence("").has_value());
    CHECK_FALSE(parse_key_sequence("Ctrl+A Ctrl+B Ctrl+C Ctrl+D Ctrl+E").has_value());
    // V2: multi-chord starting with text-input chord rejected.
    CHECK_FALSE(validate_sequence(seq("G G")).has_value());
    CHECK_FALSE(validate_sequence(seq("Shift+G Ctrl+S")).has_value());
    CHECK(validate_sequence(seq("G")).has_value()); // single bare chord allowed (warned in editor)
    // V3: multi-chord containing bare Escape or mouse key rejected.
    CHECK_FALSE(validate_sequence(seq("Ctrl+K Escape")).has_value());
    CHECK_FALSE(validate_sequence(seq("Ctrl+K MouseBack")).has_value());
}

TEST_CASE("Keymap prefix-free conflicts and sequences_for ordering", "[commands][sequence]") {
    Keymap km;
    REQUIRE(km.bind(seq("Ctrl+K Ctrl+S"), "keymap.edit").has_value());
    // Identical
    CHECK(km.bind(seq("Ctrl+K Ctrl+S"), "app.quit").error().code == ErrorCode::AlreadyExists);
    // PrefixOfExisting
    CHECK(km.bind(ch("Ctrl+K"), "app.quit").error().code == ErrorCode::AlreadyExists);
    // ExtendsExisting
    CHECK(km.bind(seq("Ctrl+K Ctrl+S Ctrl+T"), "app.quit").error().code ==
          ErrorCode::AlreadyExists);

    // Single chord appears before sequence in sequences_for().
    REQUIRE(km.bind(ch("F9"), "keymap.edit").has_value());
    const auto list = km.sequences_for("keymap.edit");
    REQUIRE(list.size() == 2U);
    CHECK(to_string(list[0]) == "F9");
    CHECK(to_string(list[1]) == "Ctrl+K Ctrl+S");
}

TEST_CASE("SequenceMatcher handles match, cancel, abort and timeout", "[commands][sequence]") {
    using Clock = SequenceMatcher::Clock;
    Keymap km;
    REQUIRE(km.bind(ch("Ctrl+Q"), "app.quit").has_value());
    REQUIRE(km.bind(seq("Ctrl+K Ctrl+S"), "keymap.edit").has_value());

    SequenceMatcher matcher(std::chrono::milliseconds{1000});
    const Clock::time_point t0{};

    // Unrelated single chord: NoMatch, not consumed.
    const SequenceResult r0 = matcher.feed(km, ch("Ctrl+Q"), t0);
    CHECK(r0.kind == SequenceResult::Kind::NoMatch);
    CHECK_FALSE(r0.consumed);
    CHECK_FALSE(matcher.pending());

    // Start sequence -> Pending.
    const SequenceResult r1 = matcher.feed(km, ch("Ctrl+K"), t0);
    CHECK(r1.kind == SequenceResult::Kind::Pending);
    CHECK(r1.consumed);
    CHECK(matcher.pending());
    REQUIRE(matcher.deadline().has_value());

    // Complete exactly at timeout boundary (not expired!).
    const SequenceResult r2 = matcher.feed(km, ch("Ctrl+S"), t0 + std::chrono::milliseconds{1000});
    CHECK(r2.kind == SequenceResult::Kind::Match);
    CHECK(r2.consumed);
    CHECK_FALSE(r2.expired);
    REQUIRE(r2.binding.has_value());
    if (r2.binding.has_value()) {
        CHECK(r2.binding->command == "keymap.edit");
    }
    CHECK_FALSE(matcher.pending());

    // Escape cancels pending sequence.
    REQUIRE(matcher.feed(km, ch("Ctrl+K"), t0).kind == SequenceResult::Kind::Pending);
    const SequenceResult r3 = matcher.feed(km, ch("Escape"), t0 + std::chrono::milliseconds{100});
    CHECK(r3.kind == SequenceResult::Kind::Cancelled);
    CHECK(r3.consumed);
    CHECK_FALSE(matcher.pending());

    // Offending chord aborts pending sequence without firing Ctrl+Q!
    REQUIRE(matcher.feed(km, ch("Ctrl+K"), t0).kind == SequenceResult::Kind::Pending);
    const SequenceResult r4 = matcher.feed(km, ch("Ctrl+Q"), t0 + std::chrono::milliseconds{100});
    CHECK(r4.kind == SequenceResult::Kind::Aborted);
    CHECK(r4.consumed);
    CHECK_FALSE(matcher.pending());

    // Strictly greater than timeout expires pending and handles new chord as fresh.
    REQUIRE(matcher.feed(km, ch("Ctrl+K"), t0).kind == SequenceResult::Kind::Pending);
    const SequenceResult r5 = matcher.feed(km, ch("Ctrl+Q"), t0 + std::chrono::milliseconds{1001});
    CHECK(r5.expired);
    CHECK(r5.kind == SequenceResult::Kind::NoMatch);
    CHECK_FALSE(r5.consumed);
}

TEST_CASE("keymap_from_json lenient loading, determinism and file I/O (.bad / .bak)",
          "[commands][keymap_file]") {
    KeymapFile f;
    f.sequence_timeout = std::chrono::milliseconds{1500};
    f.bindings.push_back(UserBinding{seq("Ctrl+Alt+F"), "search.flatten", 0});
    f.bindings.push_back(UserBinding{seq("Keypad5"), "edit.copy_path", 1});
    f.unbound.push_back(UserUnbind{seq("Ctrl+E"), "search.flatten"});
    f.aliases.push_back(Alias{"nf", "file.new_folder", 0});

    const std::string j1 = to_json(f);
    const std::string j2 = to_json(f);
    CHECK(j1 == j2);
    CHECK(j1.ends_with('\n'));

    LoadReport rep;
    const Result<KeymapFile> parsed = keymap_from_json(j1, &rep);
    REQUIRE(parsed.has_value());
    CHECK(rep.clean());
    CHECK(*parsed == f);

    // Lenient load drops bad entries and clamps timeout, preserving valid ones.
    const std::string messy = R"({
        "version": 99,
        "unknown_field": [1, 2, 3],
        "sequence_timeout_ms": 50,
        "bindings": [
            42,
            {"keys": "NotAKey", "command": "app.quit"},
            {"keys": "G G", "command": "app.quit"},
            {"keys": "F9", "command": ""},
            {"keys": "F9", "command": "app.quit", "arg": -1},
            {"keys": "F9", "command": "app.quit"},
            {"keys": "F9", "command": "nav.refresh"}
        ],
        "aliases": [
            {"name": ">bad", "command": "app.quit"},
            {"name": "nf", "command": "file.new_folder"},
            {"name": "NF", "command": "nav.goto"}
        ]
    })";
    LoadReport messy_rep;
    const Result<KeymapFile> messy_f = keymap_from_json(messy, &messy_rep);
    REQUIRE(messy_f.has_value());
    CHECK(messy_rep.newer_version);
    CHECK(messy_f->sequence_timeout == kMinSequenceTimeout);
    REQUIRE(messy_f->bindings.size() == 1U);
    CHECK(messy_f->bindings[0].command == "app.quit");
    REQUIRE(messy_f->aliases.size() == 1U);
    CHECK(messy_f->aliases[0].name == "nf");

    // Disk round-trip, .bak on newer version, .bad on corrupt non-object.
    TempDir tmp;
    const Path file = keymap_file_path(tmp / "cfg");
    CHECK(load_keymap_file(file, nullptr).error().code == ErrorCode::NotFound);

    REQUIRE(save_keymap_file(file, f).has_value());
    const Result<KeymapFile> reloaded = load_keymap_file(file, nullptr);
    REQUIRE(reloaded.has_value());
    CHECK(*reloaded == f);

    // Write a newer-version file, then save v1 -> creates .bak.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "{\"version\": 99}\n";
    }
    REQUIRE(save_keymap_file(file, f).has_value());
    CHECK(std::filesystem::exists(Path(file.native() + ".bak")));

    // Write corrupt non-object -> load moves aside to .bad.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "not json";
    }
    CHECK(load_keymap_file(file, nullptr).error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK(std::filesystem::exists(Path(file.native() + ".bad")));
}

TEST_CASE("apply_keymap_file, build_keymap, effective_aliases and editor helpers",
          "[commands][keymap_file]") {
    const CommandRegistry reg = builtins();
    Keymap defaults;
    REQUIRE(load_default_keymap(defaults).has_value());

    // Rebind Ctrl+E (default search.flatten) to nav.refresh, persist, reload, verify.
    KeymapFile file;
    Keymap effective = *build_keymap(file, reg, nullptr);

    // Without replace_conflicts, Ctrl+E fails with AlreadyExists.
    CHECK(set_binding(file, effective, defaults, reg, seq("Ctrl+E"), "nav.refresh", 0, false)
              .error()
              .code == ErrorCode::AlreadyExists);

    // With replace_conflicts, Ctrl+E moves to nav.refresh and adds an unbound entry for default.
    const auto displaced =
        set_binding(file, effective, defaults, reg, seq("Ctrl+E"), "nav.refresh", 0, true);
    REQUIRE(displaced.has_value());
    REQUIRE(displaced->size() == 1U);
    CHECK((*displaced)[0].existing.command == "search.flatten");

    effective = *build_keymap(file, reg, nullptr);
    REQUIRE(effective.resolve(ch("Ctrl+E")) != nullptr);
    CHECK(effective.resolve(ch("Ctrl+E"))->command == "nav.refresh");

    // Warnings check: destructive bare key, shadows type-to-filter, text field, layout punctuation.
    const BindingCheck warn_del =
        check_binding(effective, reg, seq("Delete"), "file.delete_permanently", 0);
    CHECK_FALSE(warn_del.error.has_value());
    CHECK(std::find(warn_del.warnings.begin(), warn_del.warnings.end(),
                    BindingWarning::DestructiveBareKey) != warn_del.warnings.end());
    CHECK(std::find(warn_del.warnings.begin(), warn_del.warnings.end(),
                    BindingWarning::ConsumedByTextFields) != warn_del.warnings.end());

    const BindingCheck warn_bare = check_binding(effective, reg, seq("G"), "nav.goto", 0);
    CHECK(std::find(warn_bare.warnings.begin(), warn_bare.warnings.end(),
                    BindingWarning::ShadowsTypeToFilter) != warn_bare.warnings.end());

    const BindingCheck warn_punct = check_binding(effective, reg, seq("Ctrl+Slash"), "nav.goto", 0);
    CHECK(std::find(warn_punct.warnings.begin(), warn_punct.warnings.end(),
                    BindingWarning::LayoutSensitivePunctuation) != warn_punct.warnings.end());

    // Aliases & reset_all (reset_all keeps aliases and timeout).
    REQUIRE(set_alias(file, reg, Alias{"nf", "file.new_folder", 0}).has_value());
    set_sequence_timeout(file, std::chrono::milliseconds{1400});
    CHECK(effective_aliases(file, reg, nullptr).size() == 1U);
    CHECK(reset_all(file));
    CHECK(file.bindings.empty());
    CHECK(file.unbound.empty());
    CHECK(file.aliases.size() == 1U);
    CHECK(file.sequence_timeout == std::chrono::milliseconds{1400});
}

} // namespace
} // namespace navigator
