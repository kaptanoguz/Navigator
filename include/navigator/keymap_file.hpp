// navigator/keymap_file.hpp
//
// Purpose: Phase 8 user keymap: the persisted model (KeymapFile: bindings, removals of default
//          bindings, aliases, sequence timeout), its JSON v1 form with a lenient loader and a
//          deterministic writer, atomic file I/O under the config root, the layering of the user
//          file over load_default_keymap(), and the pure helpers the keymap editor is built on
//          (conflicts, warnings, set / remove / reset, aliases).
// Owner:   contracts (header); implementation in src/commands (role: commands), tests in
//          tests/commands. Needs commands.hpp only (no search, no core).
// Thread-safety: KeymapFile and the report types are plain values (types.hpp rule). Every free
//                function except load_keymap_file / save_keymap_file is pure and reentrant: any
//                thread. load_keymap_file / save_keymap_file block on file I/O: the startup load
//                and the save on quit are the only UI-thread uses (as for the session); the
//                editor saves from a worker with a copy of the KeymapFile (debounced about
//                250 ms after the last edit, plus once on quit when dirty). The Keymap built by
//                build_keymap is owned and mutated by the UI thread only.
// Errors:  navigator::Result (types.hpp). Loading is LENIENT: it never fails because of what the
//          file contains, only because of how it can be read (see load_keymap_file).
//
// =============================================================================================
// Model and layering
// =============================================================================================
//
// The effective keymap is `load_default_keymap()` with the user file layered on top:
//
//   1. every `unbound` entry removes the DEFAULT binding of that sequence, but only if that
//      default is bound to the entry's command (a stale entry, e.g. after a default moved to
//      another command in a newer Navigator, is ignored and reported, so it can never remove a
//      binding the user did not know about);
//   2. every `bindings` entry is then bound, in file order. The USER WINS over defaults: any
//      default binding the new one collides with (identical sequence, a strict prefix of it, or
//      one it is a prefix of: find_conflicts) is removed and reported as DisplacedDefault. A
//      collision with an EARLIER USER entry of the same file is resolved the other way round:
//      the earlier entry wins and the later one is dropped and reported, so the result does not
//      depend on anything but file order. (The editor never writes such a file: set_binding
//      removes what it displaces.)
//
// Aliases are independent of the keymap (effective_aliases). Unknown command names are NOT
// dropped from the model when loading or applying: they stay in the KeymapFile, so a keymap
// written by a newer Navigator (or naming a plugin command that is not loaded right now)
// survives a save by this one; apply / build_keymap just skip them and report UnknownCommand.
//
// =============================================================================================
// JSON schema, version 1 (UTF-8, one object; written with 2-space indentation, LF newlines,
// a final newline, keys in the order below; readers accept any whitespace and key order)
// =============================================================================================
//
//   {
//     "version": 1,
//     "sequence_timeout_ms": 1500,                      // optional; written only when != 1000
//     "bindings": [                                      // optional; applied in order
//       { "keys": "Ctrl+Alt+F",     "command": "search.flatten" },
//       { "keys": "Ctrl+K Ctrl+T",  "command": "view.toggle_theme" },
//       { "keys": "Keypad5",        "command": "edit.copy_path", "arg": 1 }   // arg optional, 0
//     ],
//     "unbound": [                                       // optional; defaults to remove
//       { "keys": "Ctrl+E", "command": "search.flatten" }
//     ],
//     "aliases": [                                       // optional
//       { "name": "nf", "command": "file.new_folder" },
//       { "name": "cpf", "command": "edit.copy_path", "arg": 1 }
//     ]
//   }
//
//   keys      canonical sequence text (to_string(KeySequence)); parse_key_sequence accepts the
//             synonyms and any case.
//   command   CommandInfo::name. The user-facing command, never an id number.
//   arg       unsigned integer (JSON number without fraction or exponent, 0..2^63-1); omitted
//             when 0.
// Writer determinism: for equal KeymapFile values the output is byte-identical (vectors keep
// their order; nothing is sorted behind the caller's back; numbers and strings are written in one
// fixed form; non-ASCII text in alias names is written as UTF-8, control characters, '"' and '\'
// escaped with \uXXXX / short escapes).
//
// Lenient load (keymap_from_json). Never crashes and never fails on content:
//   - the document must be a JSON object, else InvalidArgument (load_keymap_file also moves such
//     a file to "keymap.json.bad", see there);
//   - unknown keys at any level are ignored; "version" missing or not an integer counts as 1; a
//     version greater than kKeymapVersion is parsed best-effort and flagged
//     (LoadReport::newer_version);
//   - a section that is not an array (or a number that is not an integer) is ignored and reported;
//   - each array element that is not an object, lacks a usable field, or has a field of the wrong
//     type is dropped and reported (LoadIssue), the others are kept;
//   - an entry is dropped when: "keys" does not parse or fails validate_sequence (InvalidKeys);
//     "command" is empty or not a string (InvalidCommand); "arg" is negative, fractional or too
//     large (InvalidArg); an alias name violates is_valid_alias_name (InvalidAliasName); the
//     same sequence appears twice in "bindings" (DuplicateBinding: the first wins), twice in
//     "unbound" (DuplicateUnbound), or the same alias name twice, compared per
//     alias_names_equal (DuplicateAlias: the first wins); more than the limits below
//     (TooMany: the excess is dropped);
//   - prefix conflicts BETWEEN user entries are not detected here (that needs the whole set); they
//     are resolved by apply_keymap_file (earlier entry wins).
//   - "sequence_timeout_ms" outside [200, 5000] is clamped and reported (TimeoutClamped); not an
//     integer: ignored and reported (InvalidType).
// Limits: file size kMaxKeymapBytes; at most kMaxUserBindings bindings, kMaxUserUnbound
// unbound entries and kMaxAliases aliases.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/types.hpp>

namespace navigator {

inline constexpr int kKeymapVersion = 1;
inline constexpr std::string_view kKeymapFileName = "keymap.json";
inline constexpr std::size_t kMaxKeymapBytes = std::size_t{1} * 1024U * 1024U;
inline constexpr std::size_t kMaxUserBindings = 2000;
inline constexpr std::size_t kMaxUserUnbound = 2000;
inline constexpr std::size_t kMaxAliases = 500;
inline constexpr std::size_t kMaxAliasNameBytes = 32;

// <config_root>/keymap.json. `config_root` is the Navigator config root (AppConfig::config_dir,
// default ~/.config/navigator), the same root as layout::session_file.
[[nodiscard]] inline Path keymap_file_path(const Path& config_root) {
    return config_root / kKeymapFileName;
}

// ---------------------------------------------------------------------------------------------
// Aliases
// ---------------------------------------------------------------------------------------------

// A short typed name for a command (+ arg), matched in the palette: typing exactly "nf" ranks
// "New Folder" first (palette.hpp). Aliases never chain (the target is a command name, not an
// alias), never carry a "confirmed" flag (no arg does), and run through the same queue as any
// binding, so the confirmation sheet of a destructive target always appears: an alias for
// file.delete_permanently opens the permanent-delete sheet and nothing more.
struct Alias {
    std::string name;
    std::string command; // CommandInfo::name
    std::uint64_t arg = 0;

    friend bool operator==(const Alias&, const Alias&) = default;
};

// Valid alias name: 1..kMaxAliasNameBytes bytes, valid UTF-8, no ASCII whitespace, no control
// characters (< 0x20, 0x7F), not starting with '>' or '/' (reserved for palette modes). Pure.
[[nodiscard]] bool is_valid_alias_name(std::string_view name) noexcept;

// Aliases are compared ASCII-case-insensitively; bytes >= 0x80 compare exactly (no Unicode
// folding in this module). Pure.
[[nodiscard]] bool alias_names_equal(std::string_view a, std::string_view b) noexcept;

// ---------------------------------------------------------------------------------------------
// The file model
// ---------------------------------------------------------------------------------------------

// A user binding layered over the defaults.
struct UserBinding {
    KeySequence sequence;
    std::string command;
    std::uint64_t arg = 0;

    friend bool operator==(const UserBinding&, const UserBinding&) = default;
};

// Removes the default binding `sequence` -> `command` (see "Model and layering" step 1).
struct UserUnbind {
    KeySequence sequence;
    std::string command;

    friend bool operator==(const UserUnbind&, const UserUnbind&) = default;
};

struct KeymapFile {
    int version = kKeymapVersion; // as read; to_json always writes kKeymapVersion
    std::chrono::milliseconds sequence_timeout = kDefaultSequenceTimeout;
    std::vector<UserBinding> bindings;
    std::vector<UserUnbind> unbound;
    std::vector<Alias> aliases;

    // True when the file changes nothing (no bindings, unbinds or aliases, default timeout).
    [[nodiscard]] bool empty() const noexcept {
        return bindings.empty() && unbound.empty() && aliases.empty() &&
               sequence_timeout == kDefaultSequenceTimeout;
    }
    friend bool operator==(const KeymapFile&, const KeymapFile&) = default;
};

// ---------------------------------------------------------------------------------------------
// JSON and file I/O
// ---------------------------------------------------------------------------------------------

enum class LoadIssueKind : std::uint8_t {
    InvalidType,      // a section or value has the wrong JSON type (ignored)
    InvalidKeys,      // "keys" missing, not a string, unparsable, or fails validate_sequence
    InvalidCommand,   // "command" missing, empty or not a string
    InvalidArg,       // "arg" not an unsigned integer in range
    InvalidAliasName, // "name" fails is_valid_alias_name
    DuplicateBinding, // same sequence twice in "bindings" (first wins)
    DuplicateUnbound, // same sequence twice in "unbound" (first wins)
    DuplicateAlias,   // same alias name (alias_names_equal) twice (first wins)
    TooMany,          // limit exceeded; the excess entries were dropped
    TimeoutClamped,   // sequence_timeout_ms was outside [200, 5000]
};

// One thing the loader dropped or changed. `section` is "root", "bindings", "unbound" or
// "aliases"; `index` the array index (0 for "root"); `detail` English developer text.
struct LoadIssue {
    LoadIssueKind kind = LoadIssueKind::InvalidType;
    std::string section;
    std::size_t index = 0;
    std::string detail;
};

struct LoadReport {
    std::vector<LoadIssue> issues;
    bool newer_version = false; // "version" > kKeymapVersion

    // Convenience: the file loaded without dropping anything.
    [[nodiscard]] bool clean() const noexcept { return issues.empty() && !newer_version; }
};

// Parses a keymap document (schema above). `report` may be null. Errors: InvalidArgument (not
// a JSON object, or text longer than kMaxKeymapBytes), OutOfMemory. Pure.
[[nodiscard]] Result<KeymapFile> keymap_from_json(std::string_view text, LoadReport* report);

// Deterministic JSON text (schema above). Pure; never fails.
[[nodiscard]] std::string to_json(const KeymapFile& file);

// Reads and parses <file> with keymap_from_json. Errors: NotFound (no file: first start; the
// caller uses a default-constructed KeymapFile), InvalidArgument (empty path; file larger than
// kMaxKeymapBytes; not a JSON object -- in that case the file is first renamed to
// "<file>.bad", replacing an older one, so the next save does not destroy what may be
// recoverable; the report is not filled), PermissionDenied, IoError, OutOfMemory. Everything
// inside a readable JSON object is handled leniently and reported through `report`.
[[nodiscard]] Result<KeymapFile> load_keymap_file(const Path& file, LoadReport* report);

// Serializes with to_json() and writes atomically exactly like layout::save_session: missing
// parent directories are created (mode 0700), the text goes to a unique temporary file in the
// same directory (mode 0600), which is fsync'ed and then rename()d over `file`; on any failure
// the temporary file is removed and the previous file is untouched. Additionally, when the file
// being replaced exists and has a "version" greater than kKeymapVersion (written by a newer
// Navigator) it is first copied to "<file>.bak" (replacing an older backup), because this
// version drops fields it does not know.
// Errors: InvalidArgument (empty path), PermissionDenied, IoError (includes ENOSPC/EROFS),
// OutOfMemory.
Result<void> save_keymap_file(const Path& file, const KeymapFile& keymap);

// ---------------------------------------------------------------------------------------------
// Applying the file
// ---------------------------------------------------------------------------------------------

enum class ApplyIssueKind : std::uint8_t {
    UnknownCommand,       // the command name is not registered (entry skipped, kept in the model)
    UnbindableCommand,    // registered but CommandInfo::bindable is false (entry skipped)
    InvalidSequence,      // validate_sequence failed (entry skipped)
    InvalidArg,           // a non-zero arg for a command whose ArgKind is a runtime id (skipped)
    StaleUnbind,          // no default binding for that sequence and command (ignored)
    DisplacedDefault,     // a default binding was removed to make room for a user binding
    LostToEarlierBinding, // collides with an earlier user binding (this entry skipped)
    UnknownAliasTarget,   // alias command not registered, or unbindable (alias skipped)
    InvalidAliasArg,      // alias arg non-zero for a runtime-id ArgKind (alias skipped)
};

struct ApplyIssue {
    ApplyIssueKind kind = ApplyIssueKind::UnknownCommand;
    std::string section; // "bindings", "unbound" or "aliases"
    std::size_t index = 0;
    std::string detail;
    // DisplacedDefault: the default binding that was removed. LostToEarlierBinding: the earlier
    // user binding it collided with.
    std::optional<SequenceBinding> other;
};

struct ApplyReport {
    std::vector<ApplyIssue> issues;
    std::size_t bindings_applied = 0; // user bindings now in the keymap
    std::size_t defaults_removed = 0; // unbound entries that took effect + displaced defaults
};

// Applies `file` to a keymap that already holds the DEFAULT bindings (load_default_keymap), per
// "Model and layering". Validity per binding: the command is registered and bindable, the
// sequence passes validate_sequence, and for ArgKind OpId / TabId / PaneId / BookmarkId /
// BookmarkRow commands arg is 0. Never fails on content; `report` may be null.
// Errors: OutOfMemory only. On error the keymap may be partially modified: build_keymap is the
// all-or-nothing form.
Result<void> apply_keymap_file(Keymap& keymap, const KeymapFile& file,
                               const CommandRegistry& registry, ApplyReport* report);

// load_default_keymap + apply_keymap_file into a fresh Keymap. This is what the ui calls at
// startup and after every edit (the editor replaces its Keymap with the result and calls
// SequenceMatcher::reset()). Errors: OutOfMemory, or the error of load_default_keymap (a
// programming error caught by tests).
[[nodiscard]] Result<Keymap> build_keymap(const KeymapFile& file, const CommandRegistry& registry,
                                          ApplyReport* report);

// The aliases that are usable: valid name, unique per alias_names_equal (first wins), target
// registered and bindable, arg valid for the target. Order of the file. Reports
// UnknownAliasTarget / InvalidAliasArg through `report` (may be null). Pure.
[[nodiscard]] std::vector<Alias>
effective_aliases(const KeymapFile& file, const CommandRegistry& registry, ApplyReport* report);

// ---------------------------------------------------------------------------------------------
// Editor helpers (all pure)
// ---------------------------------------------------------------------------------------------

// Things the editor warns about but allows. The sheets still stand between a key and the data:
// a destructive command bound to any key only opens its confirmation sheet.
enum class BindingWarning : std::uint8_t {
    // A destructive command (static danger, see Danger) bound to a sequence none of whose chords
    // has a modifier ("Delete", "Ctrl-less" Backspace): one stray key press opens a deletion
    // sheet. Shift+Delete counts as modified and is the shipped default of
    // file.delete_permanently. See is_destructive_bare_binding.
    DestructiveBareKey,
    // A single bare printable chord (is_text_input_chord, e.g. "G" or "Shift+5"): while the list
    // has focus the binding wins over type-to-filter, so typing that character no longer starts
    // the filter.
    ShadowsTypeToFilter,
    // The first chord is one that text fields consume (Ctrl+A/C/X/V/Z, Left/Right/Home/End with
    // or without Shift/Ctrl, Backspace, Delete, Ctrl+Backspace, Shift+Delete, Ctrl+Y,
    // Ctrl+Shift+Z, Space, Shift+Space): the binding works with the list focused but not while
    // a text field has focus.
    ConsumedByTextFields,
    // A punctuation key (Minus ... Grave, Keypad operators excluded) with Ctrl and/or Alt: on
    // layouts where that legend needs AltGr (Turkish Q/F, German, ...) the key may not be
    // reachable with Ctrl; the editor suggests a letter or function key.
    LayoutSensitivePunctuation,
};

// Result of check_binding. `error` set: it can not be bound at all (the same checks apply_
// keymap_file makes). Otherwise `conflicts` lists the existing bindings it would displace
// (find_conflicts on the effective keymap; the editor asks the user before set_binding with
// replace_conflicts) and `warnings` what to show next to the field.
struct BindingCheck {
    std::optional<Error> error;
    std::vector<BindingConflict> conflicts;
    std::vector<BindingWarning> warnings;
};

[[nodiscard]] BindingCheck check_binding(const Keymap& effective, const CommandRegistry& registry,
                                         const KeySequence& sequence, std::string_view command,
                                         std::uint64_t arg);

// The warn rule of docs/review/phase-7.md: true when `command` is registered with
// Danger::Destructive (static danger: file.delete_permanently, trash.empty, layout.delete,
// keymap.reset_defaults) and EVERY chord of `sequence` has Modifiers::None. False for an
// unregistered command or an empty sequence. Pure.
[[nodiscard]] bool is_destructive_bare_binding(const CommandRegistry& registry,
                                               const KeySequence& sequence,
                                               std::string_view command);

// Adds (command, arg) at `sequence` to `file` so that build_keymap yields exactly that binding.
//   - Fails (file unchanged) with the check_binding error, or with AlreadyExists when the check
//     reports conflicts and `replace_conflicts` is false (message names the first one).
//   - With replace_conflicts, every conflicting binding is displaced in the FILE: a conflicting
//     user binding is erased from `bindings`; a conflicting default binding gets an `unbound`
//     entry (so the file says so explicitly and a later default change is not mistaken).
//   - If `sequence` -> (command, arg) is already a default binding that only an `unbound` entry
//     hides, that entry is erased instead of adding a user binding (round trip: rebinding a
//     removed default restores it; the file does not grow).
//   - Limits: AlreadyExists is NOT used for them; OutOfMemory / InvalidArgument when the
//     kMaxUserBindings / kMaxUserUnbound limits would be exceeded.
// Returns the conflicts that were displaced (empty when none). `effective` is the keymap built
// from `file` right now; `defaults` is a fresh load_default_keymap(). After a successful call the
// caller rebuilds the effective keymap with build_keymap(file).
Result<std::vector<BindingConflict>>
set_binding(KeymapFile& file, const Keymap& effective, const Keymap& defaults,
            const CommandRegistry& registry, const KeySequence& sequence, std::string_view command,
            std::uint64_t arg, bool replace_conflicts);

// Removes the binding at exactly `sequence`: a user binding is erased from the file; otherwise,
// when `defaults` binds it, an `unbound` entry is added (the entry carries the default's command).
// Returns false when nothing changed (no user binding and no default at that sequence, or already
// unbound).
bool remove_binding(KeymapFile& file, const Keymap& defaults, const KeySequence& sequence);

// "Reset to default" of one command: erases its user bindings and the `unbound` entries that
// name it, so its default bindings return. User bindings of OTHER commands that displaced one of
// its defaults stay; at the next build_keymap that default is displaced again and reported.
// Returns false when nothing changed.
bool reset_command(KeymapFile& file, std::string_view command);

// Clears bindings and unbound entries (what keymap.reset_defaults does). Aliases and the
// sequence timeout stay. Returns false when there was nothing to clear.
bool reset_all(KeymapFile& file);

// Adds an alias, or replaces the alias of the same name (alias_names_equal) keeping its position.
// Errors: InvalidArgument (invalid name; command not registered or unbindable; non-zero arg for
// a runtime-id ArgKind), OutOfMemory / InvalidArgument at kMaxAliases.
Result<void> set_alias(KeymapFile& file, const CommandRegistry& registry, Alias alias);

// Removes the alias of that name. Returns false when there is none.
bool remove_alias(KeymapFile& file, std::string_view name);

// Sets the timeout, clamped to [kMinSequenceTimeout, kMaxSequenceTimeout].
void set_sequence_timeout(KeymapFile& file, std::chrono::milliseconds timeout) noexcept;

} // namespace navigator
