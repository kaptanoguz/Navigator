// User keymap model, lenient JSON v1 loader, deterministic writer, atomic file I/O, layering over
// load_default_keymap(), and pure editor helpers (navigator/keymap_file.hpp).
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/keymap_file.hpp>

namespace navigator {
namespace {

using InJson = nlohmann::json;
using OutJson = nlohmann::ordered_json;

constexpr std::uint64_t kMaxJsonArg =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

[[nodiscard]] char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        if (b0 < 0x80U) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        std::uint32_t min_cp = 0;
        if ((b0 & 0xE0U) == 0xC0U) {
            len = 2;
            cp = b0 & 0x1FU;
            min_cp = 0x80U;
        } else if ((b0 & 0xF0U) == 0xE0U) {
            len = 3;
            cp = b0 & 0x0FU;
            min_cp = 0x800U;
        } else if ((b0 & 0xF8U) == 0xF0U) {
            len = 4;
            cp = b0 & 0x07U;
            min_cp = 0x10000U;
        } else {
            return false;
        }
        if (i + len > text.size()) {
            return false;
        }
        for (std::size_t j = 1; j < len; ++j) {
            const auto bx = static_cast<unsigned char>(text[i + j]);
            if ((bx & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (bx & 0x3FU);
        }
        if (cp < min_cp || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
            return false;
        }
        i += len;
    }
    return true;
}

[[nodiscard]] bool is_runtime_id_arg(ArgKind kind) noexcept {
    return kind == ArgKind::OpId || kind == ArgKind::TabId || kind == ArgKind::PaneId ||
           kind == ArgKind::BookmarkId || kind == ArgKind::BookmarkRow;
}

void push_load_issue(LoadReport* report, LoadIssueKind kind, std::string_view section,
                     std::size_t index, std::string detail) {
    if (report != nullptr) {
        report->issues.push_back(LoadIssue{kind, std::string(section), index, std::move(detail)});
    }
}

void push_apply_issue(ApplyReport* report, ApplyIssueKind kind, std::string_view section,
                      std::size_t index, std::string detail,
                      std::optional<SequenceBinding> other = std::nullopt) {
    if (report != nullptr) {
        report->issues.push_back(
            ApplyIssue{kind, std::string(section), index, std::move(detail), std::move(other)});
    }
}

[[nodiscard]] std::optional<std::int64_t> parse_signed_int(const InJson& v) noexcept {
    if (v.is_boolean() || v.is_number_float()) {
        return std::nullopt;
    }
    if (v.is_number_integer()) {
        return v.get<std::int64_t>();
    }
    if (v.is_number_unsigned()) {
        const auto u = v.get<std::uint64_t>();
        if (u > kMaxJsonArg) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(u);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> parse_arg_field(const InJson& obj) noexcept {
    const auto it = obj.find("arg");
    if (it == obj.end()) {
        return std::uint64_t{0};
    }
    const auto val = parse_signed_int(*it);
    if (!val || *val < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*val);
}

[[nodiscard]] ErrorCode errno_to_code(int err) noexcept {
    switch (err) {
    case ENOENT: return ErrorCode::NotFound;
    case EACCES:
    case EPERM: return ErrorCode::PermissionDenied;
    case ENOMEM: return ErrorCode::OutOfMemory;
    default: return ErrorCode::IoError;
    }
}

[[nodiscard]] Result<std::string> read_bounded_file(const Path& file, std::size_t limit) {
    if (file.empty()) {
        return fail(ErrorCode::InvalidArgument, "empty keymap path");
    }
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        return fail(errno_to_code(err), std::strerror(err));
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        return fail(errno_to_code(err), std::strerror(err));
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        return fail(ErrorCode::IoError, "keymap path is not a regular file");
    }
    if (st.st_size > 0 && static_cast<std::uint64_t>(st.st_size) > limit) {
        ::close(fd);
        return fail(ErrorCode::InvalidArgument, "keymap file exceeds size limit");
    }

    std::string out;
    if (st.st_size > 0) {
        out.reserve(static_cast<std::size_t>(st.st_size));
    }
    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            ::close(fd);
            return fail(errno_to_code(err), std::strerror(err));
        }
        if (n == 0) {
            break;
        }
        const auto count = static_cast<std::size_t>(n);
        if (out.size() + count > limit) {
            ::close(fd);
            return fail(ErrorCode::InvalidArgument, "keymap file exceeds size limit");
        }
        out.append(buf, count);
    }
    ::close(fd);
    return out;
}

Result<void> ensure_parent_dirs(const Path& dir) {
    if (dir.empty()) {
        return {};
    }
    struct stat st {};
    if (::stat(dir.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return {};
        }
        return fail(ErrorCode::IoError, "parent path is not a directory");
    }
    const Path parent = dir.parent_path();
    if (!parent.empty() && parent != dir) {
        Result<void> up = ensure_parent_dirs(parent);
        if (!up) {
            return up;
        }
    }
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        const int err = errno;
        return fail(errno_to_code(err), std::strerror(err));
    }
    return {};
}

Result<void> write_all_fd(int fd, std::string_view data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            return fail(errno_to_code(err), std::strerror(err));
        }
        written += static_cast<std::size_t>(n);
    }
    return {};
}

Result<void> write_atomic_private(const Path& file, std::string_view content) {
    const Path parent = file.parent_path();
    if (!parent.empty()) {
        Result<void> dir_res = ensure_parent_dirs(parent);
        if (!dir_res) {
            return dir_res;
        }
    }
    std::string tmpl = (parent.empty() ? Path(".") : parent).native();
    tmpl.append("/.keymap-XXXXXX");
    const int fd = ::mkostemp(tmpl.data(), O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        return fail(errno_to_code(err), std::strerror(err));
    }
    (void)::fchmod(fd, 0600);
    Result<void> wr = write_all_fd(fd, content);
    if (!wr) {
        ::close(fd);
        ::unlink(tmpl.c_str());
        return wr;
    }
    if (::fsync(fd) != 0) {
        const int err = errno;
        ::close(fd);
        ::unlink(tmpl.c_str());
        return fail(errno_to_code(err), std::strerror(err));
    }
    if (::close(fd) != 0) {
        const int err = errno;
        ::unlink(tmpl.c_str());
        return fail(errno_to_code(err), std::strerror(err));
    }
    if (::rename(tmpl.c_str(), file.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmpl.c_str());
        return fail(errno_to_code(err), std::strerror(err));
    }
    return {};
}

[[nodiscard]] bool is_consumed_by_text_fields(KeyChord chord) noexcept {
    const Key k = chord.key;
    const Modifiers m = chord.mods;
    if (m == Modifiers::Ctrl) {
        if (k == Key::A || k == Key::C || k == Key::X || k == Key::V || k == Key::Z ||
            k == Key::Y || k == Key::Backspace) {
            return true;
        }
    }
    if (m == (Modifiers::Ctrl | Modifiers::Shift) && k == Key::Z) {
        return true;
    }
    if (k == Key::Left || k == Key::Right || k == Key::Home || k == Key::End) {
        const Modifiers allowed = Modifiers::Shift | Modifiers::Ctrl;
        if ((m & ~allowed) == Modifiers::None) {
            return true;
        }
    }
    if (k == Key::Backspace && m == Modifiers::None) {
        return true;
    }
    if (k == Key::Delete && (m == Modifiers::None || m == Modifiers::Shift)) {
        return true;
    }
    if (k == Key::Space && (m == Modifiers::None || m == Modifiers::Shift)) {
        return true;
    }
    return false;
}

[[nodiscard]] bool is_punctuation_key(Key k) noexcept {
    const auto v = static_cast<std::uint16_t>(k);
    return v >= static_cast<std::uint16_t>(Key::Minus) &&
           v <= static_cast<std::uint16_t>(Key::Grave);
}

[[nodiscard]] bool has_layout_sensitive_punctuation(const KeySequence& seq) noexcept {
    for (const KeyChord& chord : seq.chords) {
        if (is_punctuation_key(chord.key) &&
            (has(chord.mods, Modifiers::Ctrl) || has(chord.mods, Modifiers::Alt))) {
            return true;
        }
    }
    return false;
}

} // namespace

bool is_valid_alias_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxAliasNameBytes) {
        return false;
    }
    if (name.front() == '>' || name.front() == '/') {
        return false;
    }
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20U || u == 0x7FU || c == ' ') {
            return false;
        }
    }
    return is_valid_utf8(name);
}

bool alias_names_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

Result<KeymapFile> keymap_from_json(std::string_view text, LoadReport* report) {
    try {
        if (text.size() > kMaxKeymapBytes) {
            return fail(ErrorCode::InvalidArgument, "keymap JSON exceeds kMaxKeymapBytes");
        }
        const InJson doc = InJson::parse(text.begin(), text.end(), nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) {
            return fail(ErrorCode::InvalidArgument, "keymap document must be a JSON object");
        }

        KeymapFile file;
        if (const auto it = doc.find("version"); it != doc.end()) {
            const auto ver = parse_signed_int(*it);
            if (ver) {
                if (*ver > std::numeric_limits<int>::max()) {
                    file.version = std::numeric_limits<int>::max();
                } else if (*ver < std::numeric_limits<int>::min()) {
                    file.version = std::numeric_limits<int>::min();
                } else {
                    file.version = static_cast<int>(*ver);
                }
                if (file.version > kKeymapVersion && report != nullptr) {
                    report->newer_version = true;
                }
            } else {
                push_load_issue(report, LoadIssueKind::InvalidType, "root", 0,
                                "\"version\" is not an integer");
            }
        }

        if (const auto it = doc.find("sequence_timeout_ms"); it != doc.end()) {
            const auto ms = parse_signed_int(*it);
            if (!ms) {
                push_load_issue(report, LoadIssueKind::InvalidType, "root", 0,
                                "\"sequence_timeout_ms\" is not an integer");
            } else if (*ms < kMinSequenceTimeout.count() || *ms > kMaxSequenceTimeout.count()) {
                const auto clamped = std::clamp<std::int64_t>(*ms, kMinSequenceTimeout.count(),
                                                              kMaxSequenceTimeout.count());
                file.sequence_timeout = std::chrono::milliseconds{clamped};
                push_load_issue(report, LoadIssueKind::TimeoutClamped, "root", 0,
                                "sequence_timeout_ms clamped to [200, 5000]");
            } else {
                file.sequence_timeout = std::chrono::milliseconds{*ms};
            }
        }

        if (const auto it = doc.find("bindings"); it != doc.end()) {
            if (!it->is_array()) {
                push_load_issue(report, LoadIssueKind::InvalidType, "bindings", 0,
                                "\"bindings\" is not an array");
            } else {
                for (std::size_t idx = 0; idx < it->size(); ++idx) {
                    const InJson& elem = (*it)[idx];
                    if (!elem.is_object()) {
                        push_load_issue(report, LoadIssueKind::InvalidType, "bindings", idx,
                                        "binding entry is not an object");
                        continue;
                    }
                    const auto keys_it = elem.find("keys");
                    if (keys_it == elem.end() || !keys_it->is_string()) {
                        push_load_issue(report, LoadIssueKind::InvalidKeys, "bindings", idx,
                                        "missing or non-string \"keys\"");
                        continue;
                    }
                    Result<KeySequence> seq =
                        parse_key_sequence(keys_it->get_ref<const std::string&>());
                    if (!seq) {
                        push_load_issue(report, LoadIssueKind::InvalidKeys, "bindings", idx,
                                        seq.error().message);
                        continue;
                    }
                    Result<void> valid = validate_sequence(*seq);
                    if (!valid) {
                        push_load_issue(report, LoadIssueKind::InvalidKeys, "bindings", idx,
                                        valid.error().message);
                        continue;
                    }
                    const auto cmd_it = elem.find("command");
                    if (cmd_it == elem.end() || !cmd_it->is_string() ||
                        cmd_it->get_ref<const std::string&>().empty()) {
                        push_load_issue(report, LoadIssueKind::InvalidCommand, "bindings", idx,
                                        "missing or empty \"command\"");
                        continue;
                    }
                    const auto arg = parse_arg_field(elem);
                    if (!arg) {
                        push_load_issue(report, LoadIssueKind::InvalidArg, "bindings", idx,
                                        "invalid \"arg\"");
                        continue;
                    }
                    const bool dup =
                        std::any_of(file.bindings.begin(), file.bindings.end(),
                                    [&](const UserBinding& b) { return b.sequence == *seq; });
                    if (dup) {
                        push_load_issue(report, LoadIssueKind::DuplicateBinding, "bindings", idx,
                                        "duplicate sequence in \"bindings\"");
                        continue;
                    }
                    if (file.bindings.size() >= kMaxUserBindings) {
                        push_load_issue(report, LoadIssueKind::TooMany, "bindings", idx,
                                        "exceeded kMaxUserBindings");
                        continue;
                    }
                    file.bindings.push_back(
                        UserBinding{std::move(*seq), cmd_it->get<std::string>(), *arg});
                }
            }
        }

        if (const auto it = doc.find("unbound"); it != doc.end()) {
            if (!it->is_array()) {
                push_load_issue(report, LoadIssueKind::InvalidType, "unbound", 0,
                                "\"unbound\" is not an array");
            } else {
                for (std::size_t idx = 0; idx < it->size(); ++idx) {
                    const InJson& elem = (*it)[idx];
                    if (!elem.is_object()) {
                        push_load_issue(report, LoadIssueKind::InvalidType, "unbound", idx,
                                        "unbound entry is not an object");
                        continue;
                    }
                    const auto keys_it = elem.find("keys");
                    if (keys_it == elem.end() || !keys_it->is_string()) {
                        push_load_issue(report, LoadIssueKind::InvalidKeys, "unbound", idx,
                                        "missing or non-string \"keys\"");
                        continue;
                    }
                    Result<KeySequence> seq =
                        parse_key_sequence(keys_it->get_ref<const std::string&>());
                    if (!seq) {
                        push_load_issue(report, LoadIssueKind::InvalidKeys, "unbound", idx,
                                        seq.error().message);
                        continue;
                    }
                    Result<void> valid = validate_sequence(*seq);
                    if (!valid) {
                        push_load_issue(report, LoadIssueKind::InvalidKeys, "unbound", idx,
                                        valid.error().message);
                        continue;
                    }
                    const auto cmd_it = elem.find("command");
                    if (cmd_it == elem.end() || !cmd_it->is_string() ||
                        cmd_it->get_ref<const std::string&>().empty()) {
                        push_load_issue(report, LoadIssueKind::InvalidCommand, "unbound", idx,
                                        "missing or empty \"command\"");
                        continue;
                    }
                    const bool dup =
                        std::any_of(file.unbound.begin(), file.unbound.end(),
                                    [&](const UserUnbind& u) { return u.sequence == *seq; });
                    if (dup) {
                        push_load_issue(report, LoadIssueKind::DuplicateUnbound, "unbound", idx,
                                        "duplicate sequence in \"unbound\"");
                        continue;
                    }
                    if (file.unbound.size() >= kMaxUserUnbound) {
                        push_load_issue(report, LoadIssueKind::TooMany, "unbound", idx,
                                        "exceeded kMaxUserUnbound");
                        continue;
                    }
                    file.unbound.push_back(UserUnbind{std::move(*seq), cmd_it->get<std::string>()});
                }
            }
        }

        if (const auto it = doc.find("aliases"); it != doc.end()) {
            if (!it->is_array()) {
                push_load_issue(report, LoadIssueKind::InvalidType, "aliases", 0,
                                "\"aliases\" is not an array");
            } else {
                for (std::size_t idx = 0; idx < it->size(); ++idx) {
                    const InJson& elem = (*it)[idx];
                    if (!elem.is_object()) {
                        push_load_issue(report, LoadIssueKind::InvalidType, "aliases", idx,
                                        "alias entry is not an object");
                        continue;
                    }
                    const auto name_it = elem.find("name");
                    if (name_it == elem.end() || !name_it->is_string() ||
                        !is_valid_alias_name(name_it->get_ref<const std::string&>())) {
                        push_load_issue(report, LoadIssueKind::InvalidAliasName, "aliases", idx,
                                        "invalid alias \"name\"");
                        continue;
                    }
                    const auto cmd_it = elem.find("command");
                    if (cmd_it == elem.end() || !cmd_it->is_string() ||
                        cmd_it->get_ref<const std::string&>().empty()) {
                        push_load_issue(report, LoadIssueKind::InvalidCommand, "aliases", idx,
                                        "missing or empty \"command\"");
                        continue;
                    }
                    const auto arg = parse_arg_field(elem);
                    if (!arg) {
                        push_load_issue(report, LoadIssueKind::InvalidArg, "aliases", idx,
                                        "invalid \"arg\"");
                        continue;
                    }
                    const std::string& name = name_it->get_ref<const std::string&>();
                    const bool dup =
                        std::any_of(file.aliases.begin(), file.aliases.end(), [&](const Alias& a) {
                            return alias_names_equal(a.name, name);
                        });
                    if (dup) {
                        push_load_issue(report, LoadIssueKind::DuplicateAlias, "aliases", idx,
                                        "duplicate alias name");
                        continue;
                    }
                    if (file.aliases.size() >= kMaxAliases) {
                        push_load_issue(report, LoadIssueKind::TooMany, "aliases", idx,
                                        "exceeded kMaxAliases");
                        continue;
                    }
                    file.aliases.push_back(Alias{name, cmd_it->get<std::string>(), *arg});
                }
            }
        }

        return file;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while parsing keymap JSON");
    }
}

std::string to_json(const KeymapFile& file) {
    OutJson doc = OutJson::object();
    doc["version"] = kKeymapVersion;
    if (file.sequence_timeout != kDefaultSequenceTimeout) {
        doc["sequence_timeout_ms"] = file.sequence_timeout.count();
    }
    if (!file.bindings.empty()) {
        OutJson arr = OutJson::array();
        for (const UserBinding& b : file.bindings) {
            OutJson obj = OutJson::object();
            obj["keys"] = to_string(b.sequence);
            obj["command"] = b.command;
            if (b.arg != 0) {
                obj["arg"] = b.arg;
            }
            arr.push_back(std::move(obj));
        }
        doc["bindings"] = std::move(arr);
    }
    if (!file.unbound.empty()) {
        OutJson arr = OutJson::array();
        for (const UserUnbind& u : file.unbound) {
            OutJson obj = OutJson::object();
            obj["keys"] = to_string(u.sequence);
            obj["command"] = u.command;
            arr.push_back(std::move(obj));
        }
        doc["unbound"] = std::move(arr);
    }
    if (!file.aliases.empty()) {
        OutJson arr = OutJson::array();
        for (const Alias& a : file.aliases) {
            OutJson obj = OutJson::object();
            obj["name"] = a.name;
            obj["command"] = a.command;
            if (a.arg != 0) {
                obj["arg"] = a.arg;
            }
            arr.push_back(std::move(obj));
        }
        doc["aliases"] = std::move(arr);
    }
    std::string out = doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    out.push_back('\n');
    return out;
}

Result<KeymapFile> load_keymap_file(const Path& file, LoadReport* report) {
    try {
        Result<std::string> text = read_bounded_file(file, kMaxKeymapBytes);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        Result<KeymapFile> parsed = keymap_from_json(*text, report);
        if (!parsed && parsed.error().code == ErrorCode::InvalidArgument) {
            const Path bad_path(file.native() + ".bad");
            (void)::rename(file.c_str(), bad_path.c_str());
            return parsed;
        }
        return parsed;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while loading keymap file");
    }
}

Result<void> save_keymap_file(const Path& file, const KeymapFile& keymap) {
    try {
        if (file.empty()) {
            return fail(ErrorCode::InvalidArgument, "empty keymap path");
        }
        // Back up a newer-version file before overwriting it.
        if (Result<std::string> existing = read_bounded_file(file, kMaxKeymapBytes); existing) {
            const InJson doc = InJson::parse(existing->begin(), existing->end(), nullptr, false);
            if (!doc.is_discarded() && doc.is_object()) {
                if (const auto it = doc.find("version"); it != doc.end()) {
                    const auto ver = parse_signed_int(*it);
                    if (ver && *ver > kKeymapVersion) {
                        const Path bak_path(file.native() + ".bak");
                        (void)write_atomic_private(bak_path, *existing);
                    }
                }
            }
        }
        return write_atomic_private(file, to_json(keymap));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while saving keymap file");
    }
}

Result<void> apply_keymap_file(Keymap& keymap, const KeymapFile& file,
                               const CommandRegistry& registry, ApplyReport* report) {
    try {
        // Step 1: apply unbound entries against defaults currently in `keymap`.
        for (std::size_t idx = 0; idx < file.unbound.size(); ++idx) {
            const UserUnbind& u = file.unbound[idx];
            const std::optional<SequenceBinding> existing = keymap.find(u.sequence);
            if (existing && existing->command == u.command) {
                keymap.unbind(u.sequence);
                if (report != nullptr) {
                    ++report->defaults_removed;
                }
            } else {
                push_apply_issue(report, ApplyIssueKind::StaleUnbind, "unbound", idx,
                                 "no matching default binding for " + to_string(u.sequence));
            }
        }

        // Track user-added sequences so earlier user bindings win over later user bindings,
        // while any user binding displaces defaults.
        std::vector<KeySequence> user_sequences;
        user_sequences.reserve(file.bindings.size());

        // Step 2: apply user bindings in file order.
        for (std::size_t idx = 0; idx < file.bindings.size(); ++idx) {
            const UserBinding& b = file.bindings[idx];
            const CommandInfo* info = registry.find(b.command);
            if (info == nullptr) {
                push_apply_issue(report, ApplyIssueKind::UnknownCommand, "bindings", idx,
                                 "unknown command: " + b.command);
                continue;
            }
            if (!info->bindable) {
                push_apply_issue(report, ApplyIssueKind::UnbindableCommand, "bindings", idx,
                                 "command is not bindable: " + b.command);
                continue;
            }
            if (!validate_sequence(b.sequence)) {
                push_apply_issue(report, ApplyIssueKind::InvalidSequence, "bindings", idx,
                                 "invalid sequence: " + to_string(b.sequence));
                continue;
            }
            if (b.arg != 0 && is_runtime_id_arg(info->arg_kind)) {
                push_apply_issue(report, ApplyIssueKind::InvalidArg, "bindings", idx,
                                 "non-zero arg not allowed for runtime-id command: " + b.command);
                continue;
            }

            const std::vector<BindingConflict> conflicts = find_conflicts(keymap, b.sequence);
            const auto user_conflict =
                std::find_if(conflicts.begin(), conflicts.end(), [&](const BindingConflict& c) {
                    return std::find(user_sequences.begin(), user_sequences.end(),
                                     c.existing.sequence) != user_sequences.end();
                });
            if (user_conflict != conflicts.end()) {
                push_apply_issue(report, ApplyIssueKind::LostToEarlierBinding, "bindings", idx,
                                 to_string(b.sequence) + " conflicts with earlier user binding " +
                                     to_string(user_conflict->existing.sequence),
                                 user_conflict->existing);
                continue;
            }

            // Displace every conflicting default binding.
            for (const BindingConflict& c : conflicts) {
                if (keymap.unbind(c.existing.sequence)) {
                    if (report != nullptr) {
                        ++report->defaults_removed;
                    }
                    push_apply_issue(report, ApplyIssueKind::DisplacedDefault, "bindings", idx,
                                     "displaced default " + to_string(c.existing.sequence) + " (" +
                                         c.existing.command + ")",
                                     c.existing);
                }
            }

            Result<void> bound = keymap.bind(b.sequence, b.command, b.arg);
            if (!bound) {
                return bound;
            }
            user_sequences.push_back(b.sequence);
            if (report != nullptr) {
                ++report->bindings_applied;
            }
        }
        return {};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while applying keymap file");
    }
}

Result<Keymap> build_keymap(const KeymapFile& file, const CommandRegistry& registry,
                            ApplyReport* report) {
    try {
        Keymap km;
        Result<void> def = load_default_keymap(km);
        if (!def) {
            return Unexpected<Error>(def.error());
        }
        Result<void> applied = apply_keymap_file(km, file, registry, report);
        if (!applied) {
            return Unexpected<Error>(applied.error());
        }
        return km;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while building keymap");
    }
}

std::vector<Alias> effective_aliases(const KeymapFile& file, const CommandRegistry& registry,
                                     ApplyReport* report) {
    std::vector<Alias> out;
    out.reserve(file.aliases.size());
    for (std::size_t idx = 0; idx < file.aliases.size(); ++idx) {
        const Alias& a = file.aliases[idx];
        if (!is_valid_alias_name(a.name)) {
            continue;
        }
        const bool dup = std::any_of(out.begin(), out.end(), [&](const Alias& prev) {
            return alias_names_equal(prev.name, a.name);
        });
        if (dup) {
            continue;
        }
        const CommandInfo* info = registry.find(a.command);
        if (info == nullptr || !info->bindable) {
            push_apply_issue(report, ApplyIssueKind::UnknownAliasTarget, "aliases", idx,
                             "alias target not registered or unbindable: " + a.command);
            continue;
        }
        if (a.arg != 0 && is_runtime_id_arg(info->arg_kind)) {
            push_apply_issue(report, ApplyIssueKind::InvalidAliasArg, "aliases", idx,
                             "non-zero arg not allowed for runtime-id alias target: " + a.command);
            continue;
        }
        out.push_back(a);
    }
    return out;
}

bool is_destructive_bare_binding(const CommandRegistry& registry, const KeySequence& sequence,
                                 std::string_view command) {
    if (sequence.empty()) {
        return false;
    }
    const CommandInfo* info = registry.find(command);
    if (info == nullptr || info->danger != Danger::Destructive) {
        return false;
    }
    return std::all_of(sequence.chords.begin(), sequence.chords.end(),
                       [](const KeyChord& c) { return c.mods == Modifiers::None; });
}

BindingCheck check_binding(const Keymap& effective, const CommandRegistry& registry,
                           const KeySequence& sequence, std::string_view command,
                           std::uint64_t arg) {
    BindingCheck result;
    const CommandInfo* info = registry.find(command);
    if (info == nullptr) {
        result.error =
            Error{ErrorCode::InvalidArgument, "unknown command: " + std::string(command)};
        return result;
    }
    if (!info->bindable) {
        result.error =
            Error{ErrorCode::InvalidArgument, "command is not bindable: " + std::string(command)};
        return result;
    }
    Result<void> valid = validate_sequence(sequence);
    if (!valid) {
        result.error = valid.error();
        return result;
    }
    if (arg != 0 && is_runtime_id_arg(info->arg_kind)) {
        result.error =
            Error{ErrorCode::InvalidArgument, "non-zero arg not allowed for runtime-id command"};
        return result;
    }

    result.conflicts = find_conflicts(effective, sequence);
    if (is_destructive_bare_binding(registry, sequence, command)) {
        result.warnings.push_back(BindingWarning::DestructiveBareKey);
    }
    if (sequence.size() == 1U && is_text_input_chord(sequence.chords[0])) {
        result.warnings.push_back(BindingWarning::ShadowsTypeToFilter);
    }
    if (!sequence.empty() && is_consumed_by_text_fields(sequence.chords[0])) {
        result.warnings.push_back(BindingWarning::ConsumedByTextFields);
    }
    if (has_layout_sensitive_punctuation(sequence)) {
        result.warnings.push_back(BindingWarning::LayoutSensitivePunctuation);
    }
    return result;
}

Result<std::vector<BindingConflict>>
set_binding(KeymapFile& file, const Keymap& effective, const Keymap& defaults,
            const CommandRegistry& registry, const KeySequence& sequence, std::string_view command,
            std::uint64_t arg, bool replace_conflicts) {
    try {
        const BindingCheck check = check_binding(effective, registry, sequence, command, arg);
        if (check.error) {
            return Unexpected<Error>(*check.error);
        }
        if (!check.conflicts.empty() && !replace_conflicts) {
            const SequenceBinding& first = check.conflicts.front().existing;
            return fail(ErrorCode::AlreadyExists, to_string(sequence) + " conflicts with " +
                                                      to_string(first.sequence) + " (" +
                                                      first.command + ")");
        }

        // Work on a copy so any limit failure leaves `file` unchanged.
        KeymapFile next = file;

        for (const BindingConflict& c : check.conflicts) {
            const auto erased_user = std::erase_if(next.bindings, [&](const UserBinding& b) {
                return b.sequence == c.existing.sequence;
            });
            if (erased_user == 0) {
                const std::optional<SequenceBinding> def = defaults.find(c.existing.sequence);
                if (def && def->command == c.existing.command) {
                    const bool already_unbound = std::any_of(
                        next.unbound.begin(), next.unbound.end(),
                        [&](const UserUnbind& u) { return u.sequence == def->sequence; });
                    if (!already_unbound) {
                        if (next.unbound.size() >= kMaxUserUnbound) {
                            return fail(ErrorCode::InvalidArgument, "exceeded kMaxUserUnbound");
                        }
                        next.unbound.push_back(UserUnbind{def->sequence, def->command});
                    }
                }
            }
        }

        // If the target matches a default binding, restoring it just erases its `unbound` entry.
        const std::optional<SequenceBinding> def_exact = defaults.find(sequence);
        if (def_exact && def_exact->command == command && def_exact->arg == arg) {
            std::erase_if(next.unbound,
                          [&](const UserUnbind& u) { return u.sequence == sequence; });
            std::erase_if(next.bindings,
                          [&](const UserBinding& b) { return b.sequence == sequence; });
        } else {
            // Erase any stale unbound for the exact sequence (if it was for another command, leave
            // it or let the user binding displace it; if same sequence already in bindings, erase).
            std::erase_if(next.bindings,
                          [&](const UserBinding& b) { return b.sequence == sequence; });
            if (next.bindings.size() >= kMaxUserBindings) {
                return fail(ErrorCode::InvalidArgument, "exceeded kMaxUserBindings");
            }
            next.bindings.push_back(UserBinding{sequence, std::string(command), arg});
        }

        file = std::move(next);
        return check.conflicts;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while editing keymap");
    }
}

bool remove_binding(KeymapFile& file, const Keymap& defaults, const KeySequence& sequence) {
    const auto erased_user =
        std::erase_if(file.bindings, [&](const UserBinding& b) { return b.sequence == sequence; });
    if (erased_user > 0) {
        return true;
    }
    const std::optional<SequenceBinding> def = defaults.find(sequence);
    if (!def) {
        return false;
    }
    const bool already_unbound =
        std::any_of(file.unbound.begin(), file.unbound.end(),
                    [&](const UserUnbind& u) { return u.sequence == sequence; });
    if (already_unbound) {
        return false;
    }
    if (file.unbound.size() >= kMaxUserUnbound) {
        return false;
    }
    file.unbound.push_back(UserUnbind{def->sequence, def->command});
    return true;
}

bool reset_command(KeymapFile& file, std::string_view command) {
    const auto b_removed =
        std::erase_if(file.bindings, [&](const UserBinding& b) { return b.command == command; });
    const auto u_removed =
        std::erase_if(file.unbound, [&](const UserUnbind& u) { return u.command == command; });
    return (b_removed + u_removed) > 0;
}

bool reset_all(KeymapFile& file) {
    const bool changed = !file.bindings.empty() || !file.unbound.empty();
    file.bindings.clear();
    file.unbound.clear();
    return changed;
}

Result<void> set_alias(KeymapFile& file, const CommandRegistry& registry, Alias alias) {
    try {
        if (!is_valid_alias_name(alias.name)) {
            return fail(ErrorCode::InvalidArgument, "invalid alias name");
        }
        const CommandInfo* info = registry.find(alias.command);
        if (info == nullptr || !info->bindable) {
            return fail(ErrorCode::InvalidArgument, "alias target is not registered or bindable");
        }
        if (alias.arg != 0 && is_runtime_id_arg(info->arg_kind)) {
            return fail(ErrorCode::InvalidArgument,
                        "non-zero arg not allowed for runtime-id alias target");
        }
        for (Alias& existing : file.aliases) {
            if (alias_names_equal(existing.name, alias.name)) {
                existing = std::move(alias);
                return {};
            }
        }
        if (file.aliases.size() >= kMaxAliases) {
            return fail(ErrorCode::InvalidArgument, "exceeded kMaxAliases");
        }
        file.aliases.push_back(std::move(alias));
        return {};
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while setting alias");
    }
}

bool remove_alias(KeymapFile& file, std::string_view name) {
    return std::erase_if(file.aliases,
                         [&](const Alias& a) { return alias_names_equal(a.name, name); }) > 0;
}

void set_sequence_timeout(KeymapFile& file, std::chrono::milliseconds timeout) noexcept {
    file.sequence_timeout = std::clamp(timeout, kMinSequenceTimeout, kMaxSequenceTimeout);
}

} // namespace navigator
