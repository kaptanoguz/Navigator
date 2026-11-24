// Compile-only check that the Phase 8 contracts (key sequences and the SequenceMatcher, command
// metadata, the keymap file model with JSON / layering / editor helpers, palette entries /
// ranking / history, the context menu model / pins / filter, Open With handlers / Exec expansion
// / launching, the keypad keys and the new CommandIds) are usable as documented. Nothing here is
// linked or run.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/keymap_file.hpp>
#include <navigator/menu.hpp>
#include <navigator/openwith.hpp>
#include <navigator/palette.hpp>
#include <navigator/platform.hpp>
#include <navigator/search.hpp>
#include <navigator/types.hpp>

namespace navigator::contracts_check {

namespace pal = navigator::palette;

// Stable ids that must never change.
static_assert(static_cast<std::uint32_t>(CommandId::ToggleCursorSelected) == 1103);
static_assert(static_cast<std::uint32_t>(CommandId::OpenPalette) == 1370);
static_assert(static_cast<std::uint32_t>(CommandId::OpenSettings) == 1371);
static_assert(static_cast<std::uint32_t>(CommandId::EditKeymap) == 1372);
static_assert(static_cast<std::uint32_t>(CommandId::ResetKeymap) == 1373);
static_assert(static_cast<std::uint32_t>(CommandId::ExportKeymap) == 1374);
static_assert(static_cast<std::uint32_t>(CommandId::ShowContextMenu) == 1375);
static_assert(static_cast<std::uint32_t>(CommandId::OpenWith) == 1390);
static_assert((CursorArg::KeepSelection & CursorArg::Extend) == 0);

// New keys keep their values and the ones around them.
static_assert(static_cast<std::uint16_t>(Key::Menu) == 116);
static_assert(static_cast<std::uint16_t>(Key::Keypad0) == 160);
static_assert(static_cast<std::uint16_t>(Key::Keypad9) == 169);
static_assert(static_cast<std::uint16_t>(Key::KeypadPlus) == 170);
static_assert(static_cast<std::uint16_t>(Key::KeypadPeriod) == 174);
static_assert(static_cast<std::uint16_t>(Key::F13) == 72 &&
              static_cast<std::uint16_t>(Key::F24) == 83);

// Sequence limits and the default timeout.
static_assert(kMaxSequenceLength == 4);
static_assert(kDefaultSequenceTimeout == std::chrono::milliseconds{1000});
static_assert(kMinSequenceTimeout < kDefaultSequenceTimeout);
static_assert(kDefaultSequenceTimeout < kMaxSequenceTimeout);

// Context flags and applicability are constexpr.
constexpr CommandInfo kSampleInfo{.id = CommandId::MoveToTrash,
                                  .name = "file.trash",
                                  .title_key = "cmd.file.trash",
                                  .category = CommandCategory::Files,
                                  .requires_all = ContextFlags::HasSelection,
                                  .hidden_if_any = ContextFlags::SidebarFocused};
static_assert(is_applicable(kSampleInfo, CommandContext{ContextFlags::HasSelection, 0}));
static_assert(!is_applicable(kSampleInfo, CommandContext{}));
static_assert(!is_applicable(
    kSampleInfo, CommandContext{ContextFlags::HasSelection | ContextFlags::SidebarFocused, 0}));
static_assert(effective_danger(kSampleInfo, CommandContext{}) == Danger::Safe);
static_assert(effective_danger(kSampleInfo, CommandContext{ContextFlags::TrashView, 0}) ==
              Danger::Destructive);
static_assert(has_any(ContextFlags::CanUndo | ContextFlags::CanRedo, ContextFlags::CanRedo));
static_assert(!has_any(ContextFlags::None, ContextFlags::CanRedo));

// A pre-Phase-8 aggregate initialiser still compiles.
[[maybe_unused]] constexpr CommandInfo kLegacyInfo{CommandId::Quit, "app.quit", "cmd.app.quit"};

std::size_t use_sequences(const CommandRegistry& registry, CommandQueue& queue) {
    std::size_t total = 0;
    Keymap keymap;
    if (Result<void> r = load_default_keymap(keymap); !r) {
        total += r.error().message.size();
    }

    // Parse and print sequences and chords with the new key names.
    const Result<KeySequence> parsed = parse_key_sequence("Ctrl+K Ctrl+S");
    if (parsed) {
        total += to_string(*parsed).size() + parsed->size();
    }
    const Result<KeyChord> keypad = parse_key_chord("Numpad5");
    if (keypad && keypad->key == Key::Keypad5) {
        total += to_string(*keypad).size();
    }
    const KeySequence ctrl_k{KeyChord{Key::K, Modifiers::Ctrl}};
    const KeySequence two{{KeyChord{Key::K, Modifiers::Ctrl}, KeyChord{Key::T, Modifiers::Ctrl}}};

    // Binding: the single-chord overload and the sequence overload.
    if (Result<void> r = keymap.bind(KeyChord{Key::F13, Modifiers::None}, "nav.refresh"); !r) {
        total += 1;
    }
    if (Result<void> r = keymap.bind(two, "view.toggle_theme"); !r) {
        total += r.error().code == ErrorCode::AlreadyExists ? 1U : 0U;
    }
    if (Result<void> r = validate_sequence(two); !r) {
        total += 1;
    }
    total += is_text_input_chord(KeyChord{Key::G, Modifiers::Shift}) ? 1U : 0U;
    for (const BindingConflict& c : find_conflicts(keymap, ctrl_k)) {
        total += static_cast<std::size_t>(c.kind) + c.existing.command.size();
    }

    // Queries.
    if (const std::optional<SequenceBinding> b = keymap.find(two)) {
        total += b->command.size();
        if (const std::optional<Command> c = to_command(registry, *b)) {
            queue.enqueue(*c);
        }
    }
    const Keymap::SequenceLookup lookup = keymap.lookup(Span<const KeyChord>{two.chords.data(), 1});
    total += lookup.is_prefix ? 1U : 0U;
    for (const KeySequence& s : keymap.sequences_for("keymap.edit")) {
        total += to_string(s).size();
    }
    for (const SequenceBinding& b : keymap.sequence_bindings()) {
        total += b.sequence.size();
    }
    for (const SequenceBinding& b : keymap.all_bindings()) {
        total += b.command.size();
    }
    total += keymap.unbind(two) ? 1U : 0U;
    total += keymap.unbind(KeyChord{Key::F13, Modifiers::None}) ? 1U : 0U;

    // The matcher: feed with synthetic times, expire, reset.
    SequenceMatcher matcher;
    matcher.set_timeout(std::chrono::milliseconds{1500});
    const SequenceMatcher::Clock::time_point t0{};
    SequenceResult r1 = matcher.feed(keymap, KeyChord{Key::K, Modifiers::Ctrl}, t0);
    switch (r1.kind) {
    case SequenceResult::Kind::NoMatch: break;
    case SequenceResult::Kind::Pending: total += r1.sequence.size(); break;
    case SequenceResult::Kind::Match:
        if (r1.binding) {
            total += r1.binding->command.size();
        }
        break;
    case SequenceResult::Kind::Cancelled:
    case SequenceResult::Kind::Aborted: total += r1.consumed ? 1U : 0U; break;
    }
    if (matcher.pending()) {
        total += matcher.prefix().size();
        if (const auto deadline = matcher.deadline()) {
            total += matcher.expire(*deadline + std::chrono::seconds{10}) ? 1U : 0U;
        }
    }
    matcher.reset();
    return total;
}

// A metadata walk: what the palette needs from the registry.
std::size_t use_metadata(const CommandRegistry& registry) {
    std::size_t total = 0;
    const CommandContext context{ContextFlags::HasSelection | ContextFlags::MultiplePanes, 2};
    for (const CommandInfo& info : registry.all()) {
        if (!info.palette_visible || !is_applicable(info, context)) {
            continue;
        }
        total += to_string(info.category).size() + category_title_key(info.category).size();
        total += info.description_key.size();
        switch (info.arg_kind) {
        case ArgKind::None:
        case ArgKind::Flags:
        case ArgKind::TabId:
        case ArgKind::PaneId:
        case ArgKind::BookmarkId: break;
        case ArgKind::OpId: total += context.running_operations >= 2 ? 1U : 0U; break;
        case ArgKind::BookmarkRow:
            total += has(context.flags, ContextFlags::BookmarkRowFocused) ? 0U : 1U;
            break;
        }
        total += effective_danger(info, context) == Danger::Destructive ? 1U : 0U;
    }
    for (const CommandVariant& v : builtin_command_variants()) {
        total += v.title_key.size() + static_cast<std::size_t>(v.arg);
    }
    return total;
}

// The keymap file: build, edit, save.
std::size_t use_keymap_file(const CommandRegistry& registry, const Path& config_root) {
    std::size_t total = 0;
    LoadReport load_report;
    Result<KeymapFile> loaded = load_keymap_file(keymap_file_path(config_root), &load_report);
    KeymapFile file = loaded ? std::move(*loaded) : KeymapFile{};
    for (const LoadIssue& issue : load_report.issues) {
        total += static_cast<std::size_t>(issue.kind) + issue.detail.size();
    }
    total += load_report.newer_version ? 1U : 0U;

    ApplyReport apply_report;
    Result<Keymap> effective = build_keymap(file, registry, &apply_report);
    if (!effective) {
        return total;
    }
    for (const ApplyIssue& issue : apply_report.issues) {
        total += issue.other ? issue.other->command.size() : 0U;
    }
    const std::vector<Alias> aliases = effective_aliases(file, registry, nullptr);
    total += aliases.size();

    Keymap defaults;
    if (!load_default_keymap(defaults)) {
        return total;
    }

    // "Rebind Ctrl+E": remove the default, bind another chord.
    const KeySequence ctrl_e{KeyChord{Key::E, Modifiers::Ctrl}};
    const KeySequence ctrl_alt_f{KeyChord{Key::F, Modifiers::Ctrl | Modifiers::Alt}};
    total += remove_binding(file, defaults, ctrl_e) ? 1U : 0U;
    const BindingCheck check = check_binding(*effective, registry, ctrl_alt_f, "search.flatten", 0);
    if (check.error) {
        total += check.error->message.size();
    }
    for (const BindingWarning w : check.warnings) {
        total += static_cast<std::size_t>(w);
    }
    Result<std::vector<BindingConflict>> set =
        set_binding(file, *effective, defaults, registry, ctrl_alt_f, "search.flatten", 0, false);
    if (!set) {
        total += set.error().code == ErrorCode::AlreadyExists ? 1U : 0U;
    }
    total +=
        is_destructive_bare_binding(registry, KeySequence{KeyChord{Key::Delete, Modifiers::None}},
                                    "file.delete_permanently")
            ? 1U
            : 0U;
    total += reset_command(file, "search.flatten") ? 1U : 0U;
    total += reset_all(file) ? 1U : 0U;
    if (Result<void> r = set_alias(file, registry, Alias{"nf", "file.new_folder", 0}); !r) {
        total += 1;
    }
    total += remove_alias(file, "nf") ? 1U : 0U;
    set_sequence_timeout(file, std::chrono::milliseconds{1500});
    total += is_valid_alias_name("nf") && alias_names_equal("NF", "nf") ? 1U : 0U;

    const std::string text = to_json(file);
    LoadReport again;
    const Result<KeymapFile> reparsed = keymap_from_json(text, &again);
    total += reparsed && *reparsed == file ? 1U : 0U;
    if (Result<void> r = save_keymap_file(keymap_file_path(config_root), file); !r) {
        total += 1;
    }
    return total;
}

// The palette: build entries, rank, record history.
std::size_t use_palette(const CommandRegistry& registry, const Path& config_root) {
    std::size_t total = 0;
    pal::History history;
    if (Result<pal::History> h = pal::load_history(pal::history_file(config_root))) {
        history = std::move(*h);
    }

    const CommandContext context{ContextFlags::HasSelection, 0};
    std::vector<pal::Entry> entries;
    for (const CommandInfo& info : registry.all()) {
        if (!info.palette_visible || !is_applicable(info, context)) {
            continue;
        }
        pal::Entry entry;
        entry.command = Command{info.id, 0};
        entry.name = std::string{info.name};
        entry.title = std::string{info.title_key}; // the ui uses tr(info.title_key)
        entry.category_id = info.category;
        entry.danger = effective_danger(info, context);
        entry.arg_kind = info.arg_kind;
        entry.requires_all = info.requires_all;
        entry.hidden_if_any = info.hidden_if_any;
        entry.aliases.emplace_back("nf");
        entries.push_back(std::move(entry));
    }

    const pal::SearchResult result =
        pal::rank("toggle side", Span<const pal::Entry>{entries}, history, context);
    for (const pal::Hit& hit : result.hits) {
        total += hit.entry + static_cast<std::size_t>(hit.via) + hit.highlights.size();
    }
    if (const std::optional<std::size_t> index = result.default_index; index.has_value()) {
        const pal::Entry& run = entries[result.hits[index.value()].entry];
        if (pal::is_recordable(run)) {
            history.record(pal::history_key(run));
        }
        total += pal::history_boost(history, run.name) > 0 ? 1U : 0U;
    }
    total += history.rank_of("view.toggle_sidebar").value_or(0);
    total += pal::to_json(history).size();
    if (Result<void> r = pal::save_history(pal::history_file(config_root), history); !r) {
        total += 1;
    }
    return total;
}

// The context menu: build, pin, arrange, localise, filter.
std::size_t use_menu(const CommandRegistry& registry, const Path& config_root) {
    namespace mn = navigator::menu;
    std::size_t total = 0;
    mn::Pins pins;
    if (Result<mn::Pins> loaded = mn::load_pins(mn::pins_file(config_root))) {
        pins = std::move(*loaded);
    }
    if (Result<void> r = pins.pin("edit.copy_path"); !r) {
        total += r.error().code == ErrorCode::InvalidArgument ? 1U : 0U;
    }
    // Destructive commands can not be pinned.
    if (Result<void> r = pins.pin("file.delete_permanently"); !r) {
        total += 1;
    }

    const CommandContext context{ContextFlags::HasSelection | ContextFlags::HasCursorRow, 0};
    std::vector<mn::Item> items = mn::build(mn::MenuKind::ListRow, registry, context);
    for (mn::Item& item : items) {
        if (item.label.empty()) {
            item.label = item.label_key; // the ui uses tr(item.label_key)
        }
    }
    mn::apply_shortcuts(
        items, [](void*, std::string_view command, std::uint64_t) { return std::string{command}; },
        nullptr);
    const std::vector<mn::Item> arranged = mn::arrange(std::move(items), pins);
    const mn::FilterResult filtered = mn::filter(arranged, "Trash");
    if (filtered.active) {
        for (const mn::Row& row : filtered.rows) {
            total += row.score > 0 ? 1U : 0U;
            total += row.separated_before ? 1U : 0U;
            if (row.item.kind == mn::Item::Kind::Command && row.item.danger == Danger::Safe) {
                total += static_cast<std::size_t>(row.item.command.id);
            }
        }
        total += filtered.default_row.value_or(0);
    }
    total += mn::to_json(pins).size();
    if (Result<void> r = mn::save_pins(mn::pins_file(config_root), pins); !r) {
        total += 1;
    }
    return total;
}

// A launcher fake that records requests.
class RecordingLauncher final : public platform::IAppLauncher {
public:
    [[nodiscard]] Result<void> launch(const platform::LaunchRequest& request) override {
        requests.push_back(request);
        return ok();
    }
    std::vector<platform::LaunchRequest> requests;
};

std::size_t use_open_with(const Path& file) {
    namespace ow = navigator::openwith;
    std::size_t total = 0;

    ow::Options options = ow::system_options();
    options.locale = "tr_TR.UTF-8";
    options.current_desktops = {"X-Cinnamon", "GNOME"};
    options.program_exists = [](std::string_view program) { return !program.empty(); };

    Result<std::vector<ow::Handler>> handlers = ow::list_handlers("text/plain", options);
    if (handlers) {
        for (const ow::Handler& h : *handlers) {
            total += h.name.size() + h.desktop_id.size() + h.icon_name.size() +
                     (h.is_default ? 1U : 0U) + (h.via_parent_type ? 1U : 0U);
            total += ow::shown_in(h, Span<const std::string>{options.current_desktops}) ? 1U : 0U;
        }
    }
    if (Result<ow::Handler> d = ow::default_handler("image/png", options)) {
        total += d->exec.size();
    }
    if (Result<ow::Handler> h = ow::find_handler("org.gnome.gedit.desktop", options)) {
        total += h->comment.size();
    }
    if (Result<std::vector<std::string>> parents = ow::mime_parents("text/x-log", options)) {
        total += parents->size();
    }
    if (Result<void> r = ow::set_default_handler("text/plain", "org.gnome.gedit.desktop", options);
        !r) {
        total += static_cast<std::size_t>(r.error().code);
    }

    constexpr std::string_view kEntry =
        "[Desktop Entry]\nType=Application\nName=Editor\nName[tr]=D\xC3\xBCzenleyici\n"
        "Exec=editor --new-window %F\nMimeType=text/plain;\nTerminal=false\n";
    const Result<ow::Handler> parsed = ow::parse_desktop_entry(
        kEntry, "editor.desktop", "/usr/share/applications/editor.desktop", "tr_TR.UTF-8");
    if (!parsed) {
        return total;
    }

    if (const Result<std::vector<std::string>> tokens = ow::parse_exec_tokens(parsed->exec)) {
        total += tokens->size();
    }
    const std::vector<Path> paths{file};
    ow::ExpandOptions expand;
    expand.terminal_prefix = ow::default_terminal_prefix(options.program_exists);
    if (const Result<std::vector<std::vector<std::string>>> argvs =
            ow::expand_exec(*parsed, Span<const Path>{paths}, expand)) {
        total += argvs->size();
    }

    RecordingLauncher launcher;
    ow::LaunchOptions launch_options;
    launch_options.expand = expand;
    if (Result<void> r =
            ow::launch_with(*parsed, Span<const Path>{paths}, launcher, launch_options);
        !r) {
        total += static_cast<std::size_t>(r.error().code);
    }
    total += launcher.requests.size();
    return total;
}

} // namespace navigator::contracts_check
