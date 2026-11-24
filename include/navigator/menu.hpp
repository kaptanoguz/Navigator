// navigator/menu.hpp
//
// Purpose: Phase 8 searchable context menus, the pure part: the item model, the builders that
//          turn a menu kind and a context into items, the pin list (pinned items float to the
//          top; persisted), the ordering rules that keep destructive items last and separated,
//          and the search-field filter. Drawing, the search field widget and keyboard handling
//          are src/ui.
// Owner:   contracts (header); implementation in src/ui (role: ui, src/ui/menu, tests/ui):
//          the ui already links navigator_search (the filter reuses search::Matcher) and owns the
//          menus the builders replace. The coordinator may move the builders and the pin file
//          to src/commands if ui is overloaded; the header does not change.
// Thread-safety: Item, Pins, FilterResult and every free function except the file functions are
//                pure and reentrant (any thread, in practice the UI thread). load_pins /
//                save_pins block on file I/O: load at startup (UI-thread exception, as for the
//                session), save from a worker with a copy, debounced, and on quit.
// Errors:  navigator::Result (types.hpp) for file I/O and pin edits only. build / arrange /
//          filter never fail.
//
// =============================================================================================
// Rules (normative)
// =============================================================================================
//
//   M1  Order of the final top-level list (arrange): pinned items (pin order) ; separator ;
//       the builder's regular items in builder order (they keep their own separators) ;
//       separator ; destructive items last. A pinned item is MOVED to the top, not duplicated.
//       A group that is empty contributes no separator; two separators never touch.
//   M2  Destructive items (Item::danger == Destructive: file.delete_permanently, trash.empty
//       and, in the Trash view, file.trash) are always in the last group, drawn in the danger
//       colour, separated from everything above, have no accelerator letter, and are NOT
//       pinnable (arrange ignores a pin that names one, even from a hand-edited file).
//   M3  Safe items in the middle are never reordered by search, danger or pins beyond M1.
//   M4  Search: typing any printable character while the menu is open goes to the search field
//       (type-ahead; no accelerator letters exist, so nothing else can claim it). filter()
//       flattens submenus (a match inside "Open With" shows the parent as breadcrumb), drops
//       disabled items and separators, and ranks by search::Matcher fuzzy score of the
//       translated label (label_key when label is empty) and, at a discount, the command name.
//       Pinned items get a small boost. Destructive matches come LAST, separated, even when
//       they score highest.
//   M5  Safe default: FilterResult::default_row is the first non-destructive row; Enter runs it.
//       When the only matches are destructive nothing is highlighted and Enter does nothing
//       until the user moves with Down and presses Enter on the destructive row explicitly. A
//       single destructive match is never auto-activated. Activating a destructive item only
//       enqueues its command: the confirmation sheet follows (palette.hpp P3).
//   M6  Everything runs through the CommandQueue (Item::command) exactly like a key binding.
//       The one exception is OpenWith items, which the ui turns into a handler launch
//       (openwith::launch_with) at frame end; they are not destructive.
//   M7  Shortcuts: Item::shortcut is the primary binding of the command (Keymap::sequences_for),
//       filled by apply_shortcuts(); menus show them right-aligned, in every ui language the
//       modifier names are translated.
//
// Shown in the right-click menu and, identically, by menu.show_context (Menu key, Shift+F10).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/search.hpp>
#include <navigator/types.hpp>

namespace navigator::menu {

inline constexpr std::size_t kMaxPins = 16;
inline constexpr std::string_view kPinsFileName = "menu_pins.json";
inline constexpr std::int32_t kPinnedBoost = 30; // added to the score of a pinned match

[[nodiscard]] inline Path pins_file(const Path& config_root) { return config_root / kPinsFileName; }

// ---------------------------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------------------------

struct Item {
    enum class Kind : std::uint8_t {
        Command,   // enqueues `command`
        Submenu,   // opens `children`
        OpenWith,  // launches the application `handler_id` with the target paths (M6)
        Separator, // a line; never selectable, never matched
        Note,      // a disabled informational row ("Loading...", "No applications found")
    };

    Kind kind = Kind::Command;
    // Stable identity for pins and tests. Command: the command name, or "<name>#<arg>" for a
    // variant ("edit.copy_path#1"); Submenu: "submenu.<name>" ("submenu.open_with"); OpenWith:
    // "open_with:<desktop id>"; Separator and Note: empty.
    std::string id;
    Command command;       // Kind::Command; for Tab / bookmark targets, the arg is the TabId /
                           // BookmarkId of the menu's Target (see build)
    std::string label_key; // i18n key; "cmd.<name>" or the variant key
    std::string label;     // translated (or dynamic: undo label, application name); the ui
                           // fills it via localize() before showing or filtering
    std::string shortcut;  // display text, empty when none (apply_shortcuts)
    Danger danger = Danger::Safe;
    bool enabled = true;
    bool pinned = false;        // set by arrange() for items that came from the pin list
    bool pinnable = true;       // false: Submenu, OpenWith, Separator, Note, destructive items
    std::string handler_id;     // Kind::OpenWith: the desktop id (openwith::Handler::desktop_id)
    std::vector<Item> children; // Kind::Submenu

    friend bool operator==(const Item&, const Item&) = default;
};

// ---------------------------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------------------------

// Which menu. One builder per kind; the list below is the builder order BEFORE pins and the
// destructive grouping (M1). Items marked [ctx] are present only when the CommandContext allows
// them (is_applicable): absent, not greyed, except Paste and the like, which stay as disabled
// rows when their only missing fact is the clipboard (Item::enabled == false).
//   ListRow         Open (list.activate) ; Open in New Tab (list.open_in_new_tab) [CursorOnFolder]
//   ;
//                   Open With > (submenu, files and folders) ; - ; Cut ; Copy ; Paste into
//                   Folder (edit.paste, PasteArg::IntoCursorFolder) [CursorOnFolder] ; Copy as Path
//                   (edit.copy_path) ; - ; Rename (file.rename) ; Move to Trash (file.trash) ;
//                   [last group] Delete Permanently (file.delete_permanently)
//   ListBackground  New Folder (file.new_folder) ; Paste (edit.paste) ; - ; Select All ;
//                   Refresh ; Show Hidden Files (view.toggle_hidden) ; Copy Folder Path
//                   (edit.copy_path, CopyPathArg::CurrentFolder) ; - ; Open Trash (trash.open)
//   Tab             New Tab ; Duplicate Tab ; Close Tab ; Close Left Tabs ; Reopen Closed Tab
//                   [ClosedTabsAvailable] ; Copy Folder Path ; (all with Target::tab as arg)
//   TabBar          New Tab ; Reopen Closed Tab [ClosedTabsAvailable] ; Search Tabs
//   SidebarPlace    Open (list.activate semantics via the row) ; Open in New Tab ; Open Trash
//                   for the Trash row
//   SidebarBookmark Open in New Tab ; Rename (bookmarks.rename) ; Move Up ; Move Down ; Remove
//                   Bookmark (bookmarks.toggle with Target::bookmark)
//   SidebarRecents  Clear Recents (recents.clear) ; Stop Tracking Recents (recents.toggle_tracking)
//   TrashRow        Restore (trash.restore) ; [last group] Delete Permanently
//                   (file.delete_permanently) ; Empty Trash (trash.empty)
//   TrashBackground [last group] Empty Trash (trash.empty)
//   Breadcrumb      Open in New Tab ; Copy Path (edit.copy_path, CurrentFolder)
enum class MenuKind : std::uint8_t {
    ListRow,
    ListBackground,
    Tab,
    TabBar,
    SidebarPlace,
    SidebarBookmark,
    SidebarRecents,
    TrashRow,
    TrashBackground,
    Breadcrumb,
};

[[nodiscard]] std::string_view to_string(MenuKind kind) noexcept;

// What the menu is about; becomes the `arg` of the items that take a runtime id (0 = active).
struct Target {
    std::uint64_t tab = 0;      // layout::TabId::value for Tab menus
    std::uint64_t bookmark = 0; // places::BookmarkId::value for SidebarBookmark menus
};

// Builds the items of `kind` in builder order from the registry (labels keys, danger from
// effective_danger, arg kinds) and the context. Labels are NOT localized yet. Pure.
[[nodiscard]] std::vector<Item> build(MenuKind kind, const CommandRegistry& registry,
                                      const CommandContext& context, const Target& target = {});

// Localization is the ui's job (it owns tr()): before drawing or filtering it sets Item::label of
// every item and child to tr(label_key), or to the dynamic text (the undo label, an
// application name). filter() falls back to the label_key text when a label is empty.

// Fills Item::shortcut of command items from the keymap (primary sequence_for, to_string form).
// `lookup` receives (command name, arg) and returns the shortcut text. A plain function pointer
// plus context keeps this header free of <functional>.
using ShortcutLookup = std::string (*)(void* user, std::string_view command, std::uint64_t arg);
void apply_shortcuts(std::vector<Item>& items, ShortcutLookup lookup, void* user);

// ---------------------------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------------------------

// The user's pin list: Item ids, most important first. Global across menu kinds (a pinned id
// appears on top in every menu that has the item). Never holds more than kMaxPins ids.
class Pins {
public:
    Pins() = default;

    // Appends at the end. Errors: InvalidArgument (empty id; id is not pinnable: starts with
    // "submenu.", "open_with:"; or names a destructive static command "file.delete_permanently",
    // "trash.empty", "layout.delete", "keymap.reset_defaults"), AlreadyExists (already pinned),
    // OutOfMemory / InvalidArgument at kMaxPins.
    Result<void> pin(std::string_view id);
    // Returns false when it was not pinned.
    bool unpin(std::string_view id);
    // Moves a pin to `index` (clamped). Returns false when not pinned.
    bool move(std::string_view id, std::size_t index);
    void clear() noexcept;

    [[nodiscard]] bool contains(std::string_view id) const noexcept;
    [[nodiscard]] Span<const std::string> ids() const noexcept { return ids_; }

    friend bool operator==(const Pins&, const Pins&) = default;

private:
    std::vector<std::string> ids_;
};

// JSON v1 (UTF-8, 2-space indent, final newline, deterministic):
//   { "version": 1, "pins": [ "edit.copy_path", "edit.copy_path#1" ] }
// Lenient load: unknown keys ignored; a missing or non-array "pins" is empty; non-string,
// empty, duplicate and non-pinnable ids are dropped; more than kMaxPins are dropped. An id that
// names no item of any menu is kept (harmless; plugin or newer-version items).
// Errors: InvalidArgument (not a JSON object, or longer than 64 KiB), OutOfMemory.
[[nodiscard]] Result<Pins> pins_from_json(std::string_view text);
[[nodiscard]] std::string to_json(const Pins& pins);

// Errors of load_pins: NotFound (first start), InvalidArgument (empty path; larger than 64 KiB;
// not a JSON object -- renamed to "<file>.bad"), PermissionDenied, IoError, OutOfMemory.
[[nodiscard]] Result<Pins> load_pins(const Path& file);
// Atomic write (layout::save_session rules). Errors: InvalidArgument, PermissionDenied, IoError,
// OutOfMemory.
Result<void> save_pins(const Path& file, const Pins& pins);

// ---------------------------------------------------------------------------------------------
// Arrangement and search
// ---------------------------------------------------------------------------------------------

// Applies M1/M2 to the builder's items: pinned (pinnable, safe) items first in pin order with
// Item::pinned = true, then the rest, then the destructive group, with single separators between
// non-empty groups. Submenus are kept as they are (their children are not rearranged). Pure.
[[nodiscard]] std::vector<Item> arrange(std::vector<Item> items, const Pins& pins);

struct Row {
    Item item;                // a copy; Kind::Command or Kind::OpenWith
    std::string parent_label; // "Open With" for a submenu child; empty for top-level items
    std::int32_t score = 0;
    std::vector<search::HighlightRange> highlights; // byte ranges of item.label
    bool separated_before = false; // true on the first destructive row (draw a separator above)
};

struct FilterResult {
    bool active = false;                    // false: the query was empty, show the arranged menu
    std::vector<Row> rows;                  // safe matches by score, then destructive matches (M4)
    std::optional<std::size_t> default_row; // M5
};

// Filters an ARRANGED, localized item list by `query` (M4/M5). Ties keep the arranged order.
// An empty (after ASCII trim) query returns {active = false}. Pure.
[[nodiscard]] FilterResult filter(const std::vector<Item>& arranged, std::string_view query);

} // namespace navigator::menu
