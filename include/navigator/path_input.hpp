// navigator/path_input.hpp
//
// Purpose: turning text the user types or pastes (GoTo box, address bar) into a folder, and
//          folders into text (Copy as Path). Phase 4.
//            - expand_user_path(): "~", "~user", $VAR, ${VAR}, %VAR%, quotes, file:// URIs,
//              relative input, "." / "..", repeated and trailing separators. Pure.
//            - fix_path_case(): path case auto-correction (docs/features.md, GoTo). Blocking.
//            - resolve_input(): expand + case-fix + stat in one worker call (what Enter does).
//            - Directory-name completion: parse_completion_input() + match_completions()
//              (pure, for a cached listing) and complete_path() (blocking, lists the folder).
//            - format_paths_for_copy() / quote_for_shell(): Copy as Path text. Pure.
// Owner:   contracts (header); implementation in src/core (role: core-fs), tests in tests/core.
//          src/core does not call src/platform: the environment, the home directory and the
//          "~user" lookup come in through ExpandContext (the ui wires them to getenv,
//          platform::user_dir(Home) and platform::user_home).
// Thread-safety: all functions are reentrant and share no mutable state. Functions marked Pure
//                do no I/O and may run on the UI thread, provided the ExpandContext callbacks
//                they invoke do not block (an env lookup does not; platform::user_home may, so
//                the ui only resolves "~user" on a worker). fix_path_case(), resolve_input() and
//                complete_path() list directories / stat (blocking I/O): worker threads only;
//                they take a std::stop_token and are cancellable. ExpandContext and the result
//                structs are plain values; the std::function members must be safe to call from
//                the thread that uses the context.
// Errors:  navigator::Result (types.hpp). No exceptions escape (a callback that throws is a
//          contract violation by the caller); allocation failure is ErrorCode::OutOfMemory.
//
// Paths are raw bytes (types.hpp). Typed text is UTF-8 from the text field, but variable values
// and directory names are bytes and are never re-encoded or Unicode-normalized.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>

namespace navigator::path_input {

// ---------------------------------------------------------------------------------------------
// Lexical normalization
// ---------------------------------------------------------------------------------------------

// Absolute, lexically normalized form used for every location Navigator stores or compares
// (bookmarks, recents, GoTo results): std::filesystem::path::lexically_normal() semantics
// ("a//b" -> "a/b", "." removed, "x/.." removed), ".." above the root stays at "/", and a
// trailing separator is removed except for "/" itself. Symlinks are NOT resolved ("..": lexical
// parent, the same rule as nav.up). An empty or relative path is returned as an empty Path.
// Pure.
[[nodiscard]] Path normalize_absolute(const Path& path);

// ---------------------------------------------------------------------------------------------
// Expansion
// ---------------------------------------------------------------------------------------------

// Value of environment variable `name`, or nullopt when unset. The ui passes a getenv wrapper,
// tests a map. Must not block.
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Home directory of a named user ("~name"), or nullopt when unknown. The ui wraps
// platform::user_home (may block on NSS: worker threads only).
using UserHomeLookup = std::function<std::optional<Path>(std::string_view user_name)>;

struct ExpandContext {
    // "~" and "~/..." expand to this. Empty: a leading "~" is left literal and reported.
    Path home;
    // Base for relative input: the active tab's folder. Empty: relative input fails.
    Path cwd;
    // $VAR / ${VAR} / %VAR% lookup. Empty function: no variable is expanded; each one is left
    // literal and reported.
    EnvLookup env;
    // "~name" lookup. Empty function: "~name" is left literal and reported.
    UserHomeLookup user_home;
    // Windows-style %VAR% compatibility (docs/features.md: environment variable expansion).
    bool percent_variables = true;
};

struct ExpandedPath {
    // Absolute and normalize_absolute()d. Never empty on success.
    Path path;
    // Tokens that could not be expanded, verbatim as typed and in input order, e.g. "$NOPE",
    // "${NOPE}", "%NOPE%", "~nobody", "~" (empty home). They stay literal in `path`; the ui
    // names them in the "not found" notice when `path` does not exist.
    std::vector<std::string> unresolved;
};

// Expands what the user typed into an absolute path. Pure (apart from the ctx callbacks). Steps,
// in this order:
//   1. Trim leading and trailing ASCII whitespace (pasted text often ends with '\n'). Then, if
//      the text is wrapped in one pair of matching quotes ('...' or "..."), remove them -- this
//      accepts what format_paths_for_copy() produces for one path. Quotes inside are literal;
//      no other shell quoting or backslash escapes are interpreted.
//   2. "file://" URI (ASCII case-insensitive scheme): accepted when the authority is empty or
//      "localhost"; the rest is percent-decoded (%HH, any byte; a malformed escape stays
//      literal). Steps 3 and 4 are skipped for a URI. Another host: InvalidArgument.
//   3. Tilde, only at the very start: "~" or "~/..." -> ctx.home; "~name" or "~name/..." ->
//      ctx.user_home(name), where name is everything up to the first '/'. Unknown or no lookup:
//      the text stays literal and is reported in `unresolved`.
//   4. Variables, one left-to-right pass; expanded values are inserted verbatim and are NOT
//      expanded again (no "~" or "$" processing inside values). NAME = [A-Za-z_][A-Za-z0-9_]*.
//        $NAME      longest NAME after '$'
//        ${NAME}    braces must enclose exactly a NAME; any other "${...}" (e.g. "${A:-b}") or
//                   an unterminated "${" is literal text, not reported
//        %NAME%     only when ctx.percent_variables; "%20" and other non-NAME forms are literal
//      A '$' or '%' not starting one of these forms is literal. A well-formed variable whose
//      lookup returns nullopt (or with no ctx.env) stays literal and is reported in
//      `unresolved`. A variable set to the empty string expands to nothing.
//   5. A result that is not absolute is resolved against ctx.cwd.
//   6. normalize_absolute().
// Examples (home=/home/u, cwd=/home/u/src, HOME=/home/u):
//   "~/Downloads/" -> /home/u/Downloads        "/" -> /            ".." -> /home/u
//   "$HOME/Music"  -> /home/u/Music            "${HOME}//x/./y" -> /home/u/x/y
//   "%HOME%\n"     -> /home/u                  "'/tmp/a b'" -> /tmp/a b
//   "$NOPE/x"      -> /home/u/src/$NOPE/x, unresolved = {"$NOPE"}
// Errors: InvalidArgument (input empty after step 1; relative result with an empty or relative
// ctx.cwd; file:// with a remote host), OutOfMemory.
[[nodiscard]] Result<ExpandedPath> expand_user_path(std::string_view input,
                                                    const ExpandContext& ctx);

// ---------------------------------------------------------------------------------------------
// Case correction and resolution (blocking, worker threads)
// ---------------------------------------------------------------------------------------------

struct CaseFix {
    // The corrected path (== the input when nothing was changed).
    Path path;
    // At least one component was replaced by its differently-cased on-disk name.
    bool changed = false;
    // `path` exists (stat with symlinks followed succeeded).
    bool exists = false;
};

// Path case auto-correction (docs/features.md, GoTo). `path` must be absolute; it is
// normalize_absolute()d first.
//   1. If the path exists (fs::stat, follow symlinks), it is returned unchanged
//      (changed = false, exists = true). Exact names always win.
//   2. Otherwise walk the components from the root. Keep the longest existing prefix as is.
//      For the first missing component, list its parent (fs::list_directory, show_hidden =
//      true) and look for entries whose name is equivalent under fs::fold_name (Unicode simple
//      case folding plus the Turkish I/i/İ/ı rule from fs.hpp). A component is replaced only
//      when exactly ONE entry is equivalent; for components that are not the last, only
//      directory-like entries (fs::is_directory_like) count. Repeat for the next component.
//   3. Stop at the first component with zero or several candidates, or whose parent cannot be
//      listed (permission, I/O): the remaining components are kept as typed, exists = false.
// No Unicode normalization: an NFC name and its NFD spelling are not equivalent.
// Cost: one stat per component tried plus one listing per corrected component.
// Errors: InvalidArgument (empty or relative path), Cancelled (stop requested; checked before
// each listing and inside it), OutOfMemory. Missing paths are a result, not an error.
[[nodiscard]] Result<CaseFix> fix_path_case(const Path& path, std::stop_token stop = {});

struct ResolvedInput {
    // Absolute target after expansion and case correction.
    Path path;
    // Metadata of `path` (fs::stat, follow symlinks) when it exists.
    std::optional<fs::DirEntry> entry;
    // fix_path_case() changed the case of at least one component.
    bool case_corrected = false;
    // From expand_user_path().
    std::vector<std::string> unresolved;

    [[nodiscard]] bool exists() const noexcept { return entry.has_value(); }
    [[nodiscard]] bool is_directory() const noexcept {
        // fs::stat keeps kind == Symlink for a link even when following it.
        return entry.has_value() && fs::is_directory_like(*entry);
    }
};

// What Enter / Ctrl+Enter in the GoTo box or address bar runs (on a worker): expand_user_path,
// then fix_path_case, then fs::stat(path, true). A path that does not exist (or cannot be
// stat'ed) is a successful result with entry == nullopt; the ui shows the notice.
// Errors: those of expand_user_path(), Cancelled, OutOfMemory.
[[nodiscard]] Result<ResolvedInput> resolve_input(std::string_view input, const ExpandContext& ctx,
                                                  std::stop_token stop = {});

// ---------------------------------------------------------------------------------------------
// Completion of directory names
// ---------------------------------------------------------------------------------------------

// The input split at its last '/':
//   typed_dir: the text up to and including the last '/', exactly as typed (variables, "~" and
//              all); empty when the input has no '/'. A bare "~" or "~name" (no '/') is treated
//              as "~/" / "~name/" with an empty prefix.
//   dir:       expand_user_path(typed_dir) (ctx.cwd for an empty typed_dir).
//   prefix:    the text after the last '/' (what the user is typing), not expanded.
struct CompletionQuery {
    std::string typed_dir;
    Path dir;
    std::string prefix;
};

// Pure (ctx callbacks aside). Leading whitespace and one opening quote are skipped as in
// expand_user_path() step 1; an unbalanced opening quote is allowed while typing.
// Errors: those of expand_user_path() for the directory part (InvalidArgument for a relative
// input without ctx.cwd), OutOfMemory.
[[nodiscard]] Result<CompletionQuery> parse_completion_input(std::string_view input,
                                                             const ExpandContext& ctx);

enum class CompletionMatch : std::uint8_t {
    Prefix = 0,    // fold_name(name) starts with fold_name(prefix) (every entry when prefix
                   // is empty)
    Substring = 1, // fold_name(name) contains fold_name(prefix) elsewhere
};

struct CompletionOptions {
    // At most this many results (after ranking). 0 is treated as 1.
    std::size_t max_results = 64;
    // Also offer regular files and other non-directories (default: folders and links to
    // folders only, as GoTo navigates to folders).
    bool include_files = false;
    // Names starting with '.' are offered only when this is set or the prefix itself starts
    // with '.'.
    bool include_hidden = false;
    // Offer Substring matches after the Prefix matches.
    bool substring_matches = true;
};

struct Completion {
    std::string name; // on-disk name (always valid UTF-8, see match_completions)
    Path path;        // query.dir / name
    std::string text; // replacement for the whole input: typed_dir + name + "/" for
                      // folders (just + name for files), so "~/Dow" -> "~/Downloads/"
    CompletionMatch match = CompletionMatch::Prefix;
    bool is_symlink = false; // a link (to a folder when !include_files)
    bool is_directory = false;
};

// Filters and ranks an existing listing of query.dir (the ui caches the listing while the user
// keeps typing in the same folder, so this runs on the UI thread per keystroke). Pure.
//   - Names that are not valid UTF-8 are skipped: they cannot round-trip through a text field.
//   - Matching uses fs::fold_name on both sides (case-insensitive, Turkish I rule); no fuzzy
//     matching (Phase 5 brings fuzzy search).
//   - Order: Prefix matches before Substring matches; within a group fs::compare_names with
//     natural = true, case_insensitive = true, then raw bytes. Deterministic.
//   - Hidden-name and files rules from `options`; then cut to max_results.
// Cost: O(n) folds + O(m log m) for the m matches; no I/O.
[[nodiscard]] std::vector<Completion> match_completions(const CompletionQuery& query,
                                                        Span<const fs::DirEntry> entries,
                                                        const CompletionOptions& options = {});

// Blocking convenience for a worker: parse_completion_input, then (if query.dir does not exist)
// fix_path_case on query.dir -- when that corrects it, typed_dir is replaced by the corrected
// absolute directory + "/" so the completion text is a usable path -- then
// fs::list_directory(query.dir, {show_hidden = true}, stop) and match_completions.
// A directory that cannot be listed gives an empty vector (not an error) so typing never shows
// errors. The ui debounces keystrokes (about 50-100 ms) and requests stop on the previous job.
// Errors: those of parse_completion_input(), Cancelled, OutOfMemory.
[[nodiscard]] Result<std::vector<Completion>> complete_path(std::string_view input,
                                                            const ExpandContext& ctx,
                                                            const CompletionOptions& options = {},
                                                            std::stop_token stop = {});

// ---------------------------------------------------------------------------------------------
// Copy as Path (edit.copy_path)
// ---------------------------------------------------------------------------------------------

enum class PathQuoting : std::uint8_t {
    // Context-aware (default): one path -> its raw text, unquoted (pastes into GoTo, file
    // dialogs, editors). Several paths -> each quote_for_shell()ed, joined by single spaces
    // (pastes into a terminal command line).
    Auto = 0,
    // Raw text, one path per line ('\n' between paths, none after the last).
    Never = 1,
    // quote_for_shell() each path, joined by single spaces, also for a single path.
    Shell = 2,
};

// POSIX shell quoting, only when needed: returned unchanged when non-empty and every byte is
// in [A-Za-z0-9] or one of  _ @ % + = : , . / -  or >= 0x80 (UTF-8 and other non-ASCII bytes are
// left alone; shells pass them through). Otherwise wrapped in single quotes, with each "'"
// written as '\''. The empty string gives ''. Pure.
[[nodiscard]] std::string quote_for_shell(std::string_view text);

// Clipboard text for the given paths (the selection's absolute paths in list order, or the
// current folder when nothing is selected; the ui decides which). Raw bytes: a path that is not
// valid UTF-8 cannot be represented exactly on the (UTF-8) clipboard; the ui converts the result
// with fs::to_display_name, which is lossy for such paths. Empty input gives "". Pure.
[[nodiscard]] std::string format_paths_for_copy(Span<const Path> paths,
                                                PathQuoting quoting = PathQuoting::Auto);

} // namespace navigator::path_input
