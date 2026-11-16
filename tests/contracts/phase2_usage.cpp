// Compile-only check that the Phase 2 contracts (keymap, new CommandIds, app launcher seam, MIME
// labels, AppConfig start_path/animations) are usable as documented and that IAppLauncher can be
// implemented by a test fake. Nothing here is linked or run.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/mime.hpp>
#include <navigator/platform.hpp>
#include <navigator/types.hpp>
#include <navigator/ui.hpp>

namespace navigator::contracts_check {

// Modifiers / KeyChord are constexpr value types.
static_assert((Modifiers::Ctrl | Modifiers::Shift) != Modifiers::Ctrl);
static_assert(has(Modifiers::Ctrl | Modifiers::Shift, Modifiers::Shift));
static_assert(has(Modifiers::Alt, Modifiers::None));
static_assert(!has(Modifiers::Ctrl, Modifiers::Ctrl | Modifiers::Alt));
static_assert((~Modifiers::None) ==
              (Modifiers::Ctrl | Modifiers::Shift | Modifiers::Alt | Modifiers::Super));
static_assert(KeyChord{Key::A, Modifiers::Ctrl} == KeyChord{Key::A, Modifiers::Ctrl});
static_assert(KeyChord{Key::A, Modifiers::Ctrl} != KeyChord{Key::A, Modifiers::None});
static_assert(KeyChord{}.key == Key::Unknown);
static_assert(sizeof(Key) == 2 && sizeof(Modifiers) == 1);

// Stable ids that must never change.
static_assert(static_cast<std::uint32_t>(CommandId::Quit) == 1);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleTheme) == 3);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleHidden) == 4);
static_assert(static_cast<std::uint32_t>(CommandId::NavigateBack) == 1000);
static_assert(static_cast<std::uint32_t>(CommandId::Activate) == 1100);
static_assert(static_cast<std::uint32_t>(CommandId::CursorEnd) == 1115);
static_assert((CursorArg::Extend & CursorArg::None) == 0);
static_assert(Command{CommandId::CursorDown, CursorArg::Extend} ==
              Command{CommandId::CursorDown, CursorArg::Extend});

std::size_t use_keymap(const CommandRegistry& registry, CommandQueue& queue) {
    Keymap keymap;
    std::size_t total = 0;
    if (Result<void> r = load_default_keymap(keymap); !r) {
        total += r.error().code == ErrorCode::AlreadyExists ? 1U : 0U;
    }
    const KeyChord shift_down{Key::Down, Modifiers::Shift};
    if (Result<void> r = keymap.bind(shift_down, "list.cursor_down", CursorArg::Extend); !r) {
        total += r.error().message.size();
    }
    if (const KeyBinding* b = keymap.resolve(KeyChord{Key::F5, Modifiers::None})) {
        total += b->command.size() + static_cast<std::size_t>(b->arg);
    }
    if (const std::optional<Command> c = resolve_command(keymap, registry, shift_down)) {
        queue.enqueue(*c);
    }
    for (const KeyChord& chord : keymap.chords_for("nav.back")) {
        total += to_string(chord).size() + to_string(chord.key).size();
    }
    total += keymap.chords_for("list.cursor_down", CursorArg::None).size();
    total += keymap.bindings().size();
    total += keymap.unbind(shift_down) ? 1U : 0U;
    if (Result<KeyChord> parsed = parse_key_chord("Ctrl+Shift+L")) {
        total += parsed->mods == (Modifiers::Ctrl | Modifiers::Shift) ? 1U : 0U;
    }
    keymap.clear();
    return total + to_string(CommandId::NavigateUp).size();
}

// Fake launcher: records requests instead of spawning (how platform tests inject it).
class RecordingLauncher final : public platform::IAppLauncher {
public:
    Result<void> launch(const platform::LaunchRequest& request) override {
        if (request.argv.empty()) {
            return fail(ErrorCode::InvalidArgument, "empty argv");
        }
        if (request.argv.front() == "xdg-open" && !have_xdg_open_) {
            return fail(ErrorCode::NotFound, "xdg-open");
        }
        requests_.push_back(request);
        return ok();
    }
    [[nodiscard]] const std::vector<platform::LaunchRequest>& requests() const noexcept {
        return requests_;
    }

private:
    bool have_xdg_open_ = false;
    std::vector<platform::LaunchRequest> requests_;
};

std::size_t use_platform() {
    RecordingLauncher fake;
    std::size_t total = 0;
    if (Result<void> r = platform::open_with_default("/tmp/a.txt", fake); !r) {
        total += r.error().code == ErrorCode::Unsupported ? 1U : 0U;
    }
    total += fake.requests().size();
    if (Result<std::unique_ptr<platform::IAppLauncher>> real =
            platform::create_default_launcher()) {
        platform::LaunchRequest request;
        request.argv = {"true"};
        request.working_dir = "/";
        total += (*real)->launch(request) ? 1U : 0U;
    }
    total += platform::open_with_default("/tmp") ? 1U : 0U;
    total += platform::mime_type("/tmp").value_or(std::string(mime::kOctetStream)).size();
    total += platform::user_dir(platform::UserDir::Home).value_or("/").native().size();
    return total;
}

std::size_t use_mime() {
    fs::DirEntry entry;
    entry.name = "photo.PNG";
    entry.kind = fs::EntryKind::File;
    const std::string_view type = mime::guess_for_entry(entry);
    std::size_t total = type.size() + mime::guess_from_name("a.tar.gz").size();
    total += mime::type_label_key(type).size() + mime::type_label_key(mime::kDirectory).size();
    return total;
}

AppConfig phase2_config() {
    AppConfig config;
    config.start_path = "/tmp";
    config.animations = false;
    return config;
}

} // namespace navigator::contracts_check
