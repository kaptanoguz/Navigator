// navigator/openwith.hpp
//
// Purpose: Phase 8 "Open With": the applications registered for a MIME type, found by reading the
//          freedesktop.org files directly (no GIO): Desktop Entry files, the MIME Applications
//          Associations specification (mimeapps.list, mimeinfo.cache, the legacy defaults.list),
//          and shared-mime-info's aliases / subclasses. Plus Exec expansion (field codes, no
//          shell) and launching through platform::IAppLauncher.
// Owner:   contracts (header); implementation in src/platform (role: platform), tests in
//          tests/platform with fixture directories (every root is injectable through Options).
//          Specifications followed: Desktop Entry Specification 1.5, MIME Applications
//          Associations Specification 1.0.1, shared-mime-info, XDG Base Directory.
// Thread-safety: Handler, Options and every pure function (parse_desktop_entry, expand_exec,
//                parse_exec_tokens, mime_parents' text helpers, default_terminal_prefix with a
//                pure ProgramExists) are reentrant, any thread. list_handlers, default_handler,
//                find_handler, set_default_handler and mime_parents read or write files and BLOCK:
//                call them from a worker (the Open With submenu / popup loads asynchronously and
//                shows a "Loading..." note meanwhile). launch_with only spawns (one detached
//                process per invocation, never waits): fine at frame end, like
//                platform::open_with_default. Each call re-reads the files it needs (no cache);
//                the ui may cache a result per MIME type for the lifetime of one open menu.
// Errors:  navigator::Result (types.hpp). A missing, unreadable or malformed file under one
//          directory is skipped silently (system directories are not under our control): list
//          functions only fail on invalid arguments.
//
// =============================================================================================
// What is read, in what order (normative)
// =============================================================================================
//
// Directories. All roots come from Options (system_options() fills them from the environment):
//   config_home   $XDG_CONFIG_HOME (default $HOME/.config)
//   config_dirs   $XDG_CONFIG_DIRS (default /etc/xdg), highest priority first
//   data_home     $XDG_DATA_HOME   (default $HOME/.local/share)
//   data_dirs     $XDG_DATA_DIRS   (default /usr/local/share:/usr/share), highest priority first
//
// mimeapps.list files, HIGHEST precedence first (MIME Applications spec 1.0.1, "File names and
// locations"); for each directory the desktop-specific file comes before the plain one, where
// "<desktop>" is each entry of Options::current_desktops, lowercased, in order:
//   1. <config_home>/<desktop>-mimeapps.list           2. <config_home>/mimeapps.list
//   3. each <config_dir>/<desktop>-mimeapps.list, then <config_dir>/mimeapps.list
//   4. <data_home>/applications/<desktop>-mimeapps.list, then .../applications/mimeapps.list
//      (deprecated location, still read)
//   5. each <data_dir>/applications/<desktop>-mimeapps.list, then .../applications/mimeapps.list
// Groups: [Default Applications], [Added Associations], [Removed Associations]; each line is
// `mime/type=a.desktop;b.desktop;`. Lines are read without allocating more than the groups
// needs; comments (#) and blank lines are skipped; an unknown group is ignored.
// Legacy defaults.list: <data_home>/applications/defaults.list and each
// <data_dir>/applications/defaults.list, lowest precedence, group [Default Applications], read
// after every mimeapps.list.
//
// mimeinfo.cache (written by update-desktop-database): <data_home>/applications/mimeinfo.cache
// then each <data_dir>/applications/mimeinfo.cache, group [MIME Cache], `mime/type=id;id;`.
// Only the line for the wanted type is parsed (the file is scanned line by line; at most
// kMaxMimeinfoCacheBytes are read).
//
// Desktop file lookup by desktop id ("org.gnome.gedit.desktop", "kde4-okular.desktop"): for each
// of data_home, then each data_dir, look for <dir>/applications/<id>; if missing, replace '-' by
// '/' at each dash left to right and try again (the "kde4-okular.desktop" ->
// "kde4/okular.desktop" rule). The first file found wins; an id whose file cannot be found or
// parsed is skipped.
//
// =============================================================================================
// Candidate order for one MIME type M (list_handlers)
// =============================================================================================
//
// Let the "types" be M followed by its parents (mime_parents, in order). For each type T, in
// order, build the list L(T) of desktop ids:
//   a. DEFAULT: walking the mimeapps.list files (and then defaults.list) in precedence order,
//      the first id that is installed (desktop file found, parsed, passes the filters below) in
//      any [Default Applications] line for T becomes Handler::is_default (only for T == M) and
//      is placed first.
//   b. ADDED: for each mimeapps.list in precedence order: its [Added Associations] ids for T in
//      line order; then add that file's [Removed Associations] ids for T to a removed set that
//      applies to everything of LOWER precedence (later files and mimeinfo.cache): a removed id
//      is skipped from then on. (Added ids of the same file are not affected by its own
//      Removed group, per the spec's "lower precedence" wording.)
//   c. CACHE: mimeinfo.cache ids for T in directory order, minus the removed set.
//   Duplicates keep their first position. Ids of types after M get Handler::via_parent_type =
//   true and are appended after all of L(M); an id already present keeps its earlier place.
// Then every id is resolved to a Handler (parse_desktop_entry) and FILTERED:
//   F1  [Desktop Entry] present, Type=Application, Exec present and non-empty;
//   F2  Hidden=true: dropped; NoDisplay=true: dropped unless Options::include_no_display;
//   F3  OnlyShowIn / NotShowIn evaluated against Options::current_desktops;
//   F4  TryExec set: the program must exist (Options::program_exists; production default
//       platform::find_program); otherwise dropped;
//   F5  the Exec line must contain a file or URL field code (%f %F %u %U), else the application
//       cannot open files and is dropped (apps without it are launchers, not handlers).
// The result is therefore: [default] + added + cache for M, then the same for parents.
// default_handler(M) is the first element of that list when its is_default is true; otherwise
// (no configured default) the first non-parent element, otherwise the first element; NotFound
// when the list is empty. This matches what xdg-open / GLib would pick, without GIO.
//
// MIME parents (mime_parents). From <dir>/mime/aliases ("alias canonical") and
// <dir>/mime/subclasses ("child parent") of data_home then data_dirs (shared-mime-info
// databases; missing files are fine): M is first mapped through the aliases to its canonical
// name (which is then the first type: handlers registered under an alias name are found too,
// because the alias line itself is also searched, see below), then its parents are the
// transitive closure of the subclass lines, breadth first, each type once, capped at
// kMaxMimeParents. Rules added by the spec's "implicit subclass" text: "text/*" types
// (except text/plain) have text/plain as a parent when no subclass line says otherwise; and any
// "*/*+xml" type has application/xml. application/octet-stream is NEVER added (it would
// list every application). Both the original name and the canonical name are consulted for the
// DEFAULT / ADDED / CACHE lookups.
//
// Limitations (documented, deliberate in Phase 8):
//   - The MIME type of a file comes from platform::mime_type / mime::guess_for_entry, i.e. the
//     fixed name table of mime.hpp; there is no glob-database or magic sniffing yet. A file of
//     an unknown extension is "application/octet-stream" and has no handlers (the ui offers
//     "Open with default application" and a note). Parent types still apply, so a "text/x-log"
//     file lists the text/plain editors.
//   - Handlers are listed for ONE type. For a selection of mixed types the ui calls
//     list_handlers per distinct type and intersects by desktop_id (keeping the first type's
//     order); launch_with then passes all paths to every chosen handler.
//   - Desktop actions ([Desktop Action ...]), D-Bus activation without Exec, X-* keys,
//     StartupNotify and the Path= working directory beyond LaunchRequest::working_dir are not
//     interpreted. No GIO means no GVfs URI handling: only local file paths are launched.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/platform.hpp>
#include <navigator/types.hpp>

namespace navigator::openwith {

inline constexpr std::size_t kMaxMimeParents = 16;
inline constexpr std::size_t kMaxDesktopFileBytes = std::size_t{1} * 1024U * 1024U;
inline constexpr std::size_t kMaxMimeinfoCacheBytes = std::size_t{32} * 1024U * 1024U;
inline constexpr std::size_t kMaxMimeappsBytes = std::size_t{4} * 1024U * 1024U;
// expand_exec splits one launch into several invocations so that the arguments of one stay below
// this many bytes (a conservative share of ARG_MAX; one huge selection never fails to exec).
inline constexpr std::size_t kMaxArgvBytes = std::size_t{64} * 1024U;

// True when the program can be run (production: platform::find_program succeeds).
using ProgramExists = std::function<bool(std::string_view program)>;

struct Options {
    Path home;        // $HOME (used by system_options only to derive defaults)
    Path config_home; // see "Directories"
    std::vector<Path> config_dirs;
    Path data_home;
    std::vector<Path> data_dirs;
    // $XDG_CURRENT_DESKTOP split at ':' in order, e.g. {"X-Cinnamon", "GNOME"}. Compared
    // ASCII-case-insensitively with OnlyShowIn / NotShowIn; lowercased for the
    // "<desktop>-mimeapps.list" names. Empty: no desktop (OnlyShowIn entries are hidden).
    std::vector<std::string> current_desktops;
    // POSIX locale for localized keys, e.g. "tr_TR.UTF-8" or "de_DE@euro"; the encoding part is
    // ignored. Empty or "C"/"POSIX": unlocalized keys only.
    std::string locale;
    ProgramExists program_exists; // empty: platform::find_program
    bool include_no_display = false;
};

// Options from the environment ($HOME, XDG_*, XDG_CURRENT_DESKTOP, and the locale from
// LC_ALL, then LC_MESSAGES, then LANG, using the first non-empty value). Never fails; missing
// variables use the defaults above. Reads the environment on every call (no caching).
[[nodiscard]] Options system_options();

// ---------------------------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------------------------

struct Handler {
    std::string desktop_id;   // "org.gnome.gedit.desktop" (the file's id, see lookup rule)
    Path desktop_file;        // absolute path the entry was read from (used for %k)
    std::string name;         // Name, localized per Options::locale (spec lookup order)
    std::string generic_name; // GenericName, localized; may be empty
    std::string comment;      // Comment, localized; may be empty
    std::string icon_name;    // Icon (a theme icon name or an absolute path); may be empty
    std::string exec;         // Exec value, string escapes resolved (\\ -> \, \s -> space,
                              // ...), field codes and quoting NOT yet processed
    std::string try_exec;     // TryExec; may be empty
    Path working_dir;         // Path=; empty when not set
    std::vector<std::string> mime_types; // MimeType, split at ';' (escapes honoured)
    bool terminal = false;         // Terminal=true: needs a terminal emulator (see expand_exec)
    bool accepts_multiple = false; // Exec has %F or %U
    bool accepts_uris = false;     // Exec has %u or %U (else it wants paths: %f %F)
    bool is_default = false;       // configured default for the requested type (see order a.)
    bool via_parent_type = false;  // registered for a parent type, not the requested one
    bool no_display = false;       // NoDisplay=true (only listed when Options::include_no_display)
    std::vector<std::string> only_show_in; // OnlyShowIn (empty: no restriction)
    std::vector<std::string> not_show_in;  // NotShowIn

    friend bool operator==(const Handler&, const Handler&) = default;
};

// Parses the text of one .desktop file into a Handler (pure; no filesystem access, TryExec and
// OnlyShowIn are NOT evaluated: they are list_handlers' filters F2..F4 on the result of this).
//   desktop_id     becomes Handler::desktop_id; desktop_file the Handler::desktop_file.
//   Only the "[Desktop Entry]" group is read. Keys: Type, Name, Name[locale], GenericName,
//   Comment, Icon, Exec, TryExec, Path, Terminal, NoDisplay, Hidden, MimeType, OnlyShowIn,
//   NotShowIn. Value rules: UTF-8; string escapes \s \n \t \r \\ in strings and \; in lists;
//   booleans are exactly "true" / "false". Localized lookup order for Name, GenericName and
//   Comment: lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang, then the unlocalized key
//   ("tr_TR.UTF-8" tries Name[tr_TR], Name[tr], Name). The first of a duplicate key wins.
// Errors: InvalidArgument (no [Desktop Entry] group; Type missing or not Application; Exec
// missing or empty; Hidden=true; text larger than kMaxDesktopFileBytes; invalid UTF-8 in Name or
// Exec). NoDisplay, OnlyShowIn and NotShowIn do not fail the parse: they are recorded in the
// Handler and evaluated by list_handlers (F2, F3).
[[nodiscard]] Result<Handler> parse_desktop_entry(std::string_view text,
                                                  std::string_view desktop_id,
                                                  const Path& desktop_file,
                                                  std::string_view locale);

// F3 of "Candidate order": true when the handler may be shown on the given desktops. OnlyShowIn
// non-empty: at least one current desktop must be in it; NotShowIn: no current desktop may be in
// it; comparisons are ASCII-case-insensitive. With no current desktops an OnlyShowIn handler is
// hidden and NotShowIn does not hide anything. Pure.
[[nodiscard]] bool shown_in(const Handler& handler,
                            Span<const std::string> current_desktops) noexcept;

// The MIME types M's handlers may be registered under after M itself: aliases and subclasses per
// "MIME parents" above, nearest first, without M, capped at kMaxMimeParents. Reads the mime/
// databases under data_home and data_dirs (blocks). Errors: InvalidArgument (not "type/subtype").
[[nodiscard]] Result<std::vector<std::string>> mime_parents(std::string_view mime,
                                                            const Options& options);

// Applications for `mime`, in the order of "Candidate order". Blocks (reads files).
// Errors: InvalidArgument (mime is not "type/subtype": exactly one '/', both parts non-empty, no
// whitespace or control characters, at most 255 bytes). No application is an empty list, not an
// error.
[[nodiscard]] Result<std::vector<Handler>> list_handlers(std::string_view mime,
                                                         const Options& options);

// The handler xdg-open would use for `mime` (see the end of "Candidate order"). Errors:
// InvalidArgument (as list_handlers), NotFound (no application at all).
[[nodiscard]] Result<Handler> default_handler(std::string_view mime, const Options& options);

// One installed application by desktop id, with the same filters as list_handlers (used to
// re-resolve a stored choice). Errors: InvalidArgument (id not ending in ".desktop", or
// containing '/' or a NUL), NotFound.
[[nodiscard]] Result<Handler> find_handler(std::string_view desktop_id, const Options& options);

// "Always open with this": makes `desktop_id` the default for `mime` by writing
// <config_home>/mimeapps.list atomically (temp file + fsync + rename in the same directory, dirs
// 0700, file 0600; an existing file is read, edited line-wise and written back with every
// unrelated line, comment and group preserved verbatim). The line `mime=<desktop_id>;` in
// [Default Applications] replaces any earlier line for that type; `desktop_id` is also ensured
// at the front of the [Added Associations] line for the type and removed from its
// [Removed Associations] line. The desktop id must resolve (find_handler). Blocks.
// Errors: InvalidArgument (bad mime / id), NotFound (the application is not installed),
// PermissionDenied, IoError, OutOfMemory.
Result<void> set_default_handler(std::string_view mime, std::string_view desktop_id,
                                 const Options& options);

// ---------------------------------------------------------------------------------------------
// Exec, terminals, launching
// ---------------------------------------------------------------------------------------------

// Splits an Exec value into arguments per the Desktop Entry spec "The Exec key": arguments are
// separated by unquoted spaces; a double-quoted argument may contain spaces, and within quotes
// the characters " ` $ \ are escaped with a backslash; the quote characters themselves are
// removed. Single quotes have no meaning. Field codes are left in place (they are expanded by
// expand_exec per argument). Pure. Errors: InvalidArgument (unterminated quote, empty command,
// an escape that is not one of the four).
[[nodiscard]] Result<std::vector<std::string>> parse_exec_tokens(std::string_view exec);

struct ExpandOptions {
    // Argv prefix for Terminal=true applications, e.g. {"gnome-terminal", "--"} or
    // {"x-terminal-emulator", "-e"}; the application's argv follows. Empty: expand_exec fails
    // with Unsupported for such handlers.
    std::vector<std::string> terminal_prefix;
};

// Expands the Exec line of `handler` for `paths` (absolute local paths, at least one) into one or
// more argv vectors, no shell involved, no argument is ever re-split or re-expanded (a path with
// spaces, quotes or '$' stays one argument).
//   Field codes (Desktop Entry spec, "Exec variables"), applied per argument:
//     %f  one file path;                      %F  all file paths
//     %u  one file URL (transfer::file_uri);  %U  all file URLs
//     %i  "--icon" and the Icon value as two arguments, or nothing when Icon is empty
//     %c  the translated Name;                %k  the desktop file's path
//     %%  a literal '%'
//     deprecated %d %D %n %N %v %m: expand to nothing (the argument is dropped)
//   An argument consisting of a single %F / %U is replaced by one argument per path; %f / %u
//   alone is replaced by one path. A field code embedded in a longer argument ("--file=%f",
//   "%u/x") is expanded in place with the single path / URL of that invocation. Unknown codes
//   fail the call.
//   Invocations: when the Exec uses %f / %u (single) one invocation is produced PER path, in
//   order; when it uses %F / %U one invocation takes all paths, split in order into several
//   invocations when the arguments would exceed kMaxArgvBytes. With no field code at all
//   nothing is appended (such handlers are filtered out of lists, F5) and one invocation is
//   returned.
//   Terminal=true: ExpandOptions::terminal_prefix is prepended to every invocation.
// Errors: InvalidArgument (no paths; a relative path or one containing a NUL; unknown field
// code; the Exec parse errors), Unsupported (Terminal=true without a prefix).
[[nodiscard]] Result<std::vector<std::vector<std::string>>>
expand_exec(const Handler& handler, Span<const Path> paths, const ExpandOptions& options);

// The first installed terminal emulator from this fixed list, as an ExpandOptions::terminal_prefix:
//   x-terminal-emulator -e ; gnome-terminal -- ; kgx -e ; konsole -e ; xfce4-terminal -x ;
//   mate-terminal -x ; tilix -e ; alacritty -e ; kitty ; xterm -e
// Empty when none exists. ($TERMINAL is deliberately not consulted: its argument convention
// is unknown.) Pure given `exists`.
[[nodiscard]] std::vector<std::string> default_terminal_prefix(const ProgramExists& exists);

struct LaunchOptions {
    ExpandOptions expand;
};

// Starts `handler` for `paths` through `launcher`: expand_exec, then one launcher.launch per
// invocation, in order, stopping at the first failure (invocations already started stay
// started). Each LaunchRequest has argv from expand_exec and working_dir = Handler::working_dir.
// Never waits (launcher contract). It does not stat the paths: the ui just listed them.
// Errors: those of expand_exec, plus the launcher's (NotFound: the program is not in $PATH,
// PlatformError).
[[nodiscard]] Result<void> launch_with(const Handler& handler, Span<const Path> paths,
                                       platform::IAppLauncher& launcher,
                                       const LaunchOptions& options = {});

} // namespace navigator::openwith
