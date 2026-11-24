// src/ops/copy.cpp: see copy.hpp.
//
// One top-level item is placed in three passes over its scanned tree (tree.hpp):
//   1. pre-order: every entry is created at the destination. Files go through a temp name
//      (S6: ".<name>.navigator-<random>.part", O_EXCL, 0600) and are committed with
//      RENAME_NOREPLACE; links and FIFOs use the same temp + commit path, directories are
//      mkdir'ed under their final name (atomic EEXIST) and entered at once, so their metadata
//      is applied when they are left (post-order, S6).
//   2. cross-filesystem move only, when pass 1 was not cancelled or aborted: the verified
//      source entries are removed bottom up, each re-checked against its scan identity (S7).
//   3. cross-filesystem move only, on cancellation or a destination-side failure: everything
//      pass 1 created for THIS item is removed again (identity-checked, bottom up), replaced
//      items are restored from the trash, and the source stays intact (S1d, S9).
// Names are decided before data is copied; a name taken between check and commit (a race)
// goes through the conflict rules again.
#include "copy.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "conflict.hpp"
#include "sys.hpp"

namespace navigator::ops::detail {
namespace {

struct FileIdHash {
    std::size_t operator()(const FileId& id) const noexcept {
        std::uint64_t x = id.inode ^ (id.device * 0x9E3779B97F4A7C15ULL);
        x ^= x >> 31U;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 29U;
        return static_cast<std::size_t>(x);
    }
};

[[nodiscard]] bool xattr_wanted(std::string_view name, bool is_dir) noexcept {
    if (name.starts_with("user.")) {
        return true;
    }
    if (name == "system.posix_acl_access") {
        return true;
    }
    return is_dir && name == "system.posix_acl_default";
}

[[nodiscard]] mode_t kept_mode(const struct stat& src, int dst_fd) noexcept {
    mode_t m = src.st_mode & 07777;
    struct stat dst {};
    if (::fstat(dst_fd, &dst) != 0 || dst.st_uid != src.st_uid || dst.st_gid != src.st_gid) {
        m &= static_cast<mode_t>(~(S_ISUID | S_ISGID)); // S10
    }
    return m;
}

enum class Side : std::uint8_t { Source, Destination };

// Outcome of creating one entry.
struct Step {
    enum class R : std::uint8_t { Ok, Skip, Fail, Cancel };
    R r = R::Ok;
    Error error;
    Side side = Side::Destination;
};

[[nodiscard]] Step ok() { return {}; }
[[nodiscard]] Step skip() {
    Step s;
    s.r = Step::R::Skip;
    return s;
}
[[nodiscard]] Step cancel() {
    Step s;
    s.r = Step::R::Cancel;
    s.error = make_error(ErrorCode::Cancelled, "cancelled");
    return s;
}
[[nodiscard]] Step failure(Error e, Side side) {
    Step s;
    s.r = Step::R::Fail;
    s.error = std::move(e);
    s.side = side;
    return s;
}

class Exec {
public:
    Exec(Context& ctx, Tree& tree, const TransferParams& p) : ctx_(ctx), t_(tree), p_(p) {}

    TransferResult run();

private:
    Node& node(std::uint32_t i) { return t_.nodes[i]; }
    Path src_path(std::uint32_t i) const { return node_path(t_, i); }
    Path dst_path(std::uint32_t i) const {
        return p_.dst_dir /
               node_rel(t_, i, [](const Node& n) -> const std::string& { return n.dst_name; });
    }
    Path dst_parent_path(std::uint32_t i) const {
        const Node& n = t_.nodes[i];
        return n.parent == kNoParent ? p_.dst_dir : dst_path(n.parent);
    }
    [[nodiscard]] bool sync() const { return p_.move || ctx_.opt().sync_copies; }

    void process(std::uint32_t i);
    void record_failure(std::uint32_t i, const Step& s);
    // Name decision for node i. Fills n.dst_name (and n.dst_id for Merge).
    [[nodiscard]] Step decide(std::uint32_t i, bool check_existing, Placement& pl, bool& merge);
    [[nodiscard]] Step make_dir(std::uint32_t i, bool check_existing);
    [[nodiscard]] Step make_file(std::uint32_t i, bool check_existing);
    [[nodiscard]] Step make_link(std::uint32_t i, bool check_existing);
    [[nodiscard]] Step make_fifo(std::uint32_t i, bool check_existing);
    // Renames temp `tmp` to n.dst_name following `pl` (S6, S8); handles races.
    [[nodiscard]] Step commit(std::uint32_t i, const std::string& tmp, Placement pl,
                              bool check_existing);
    [[nodiscard]] Step copy_data(int in, int out, const struct stat& st, const Path& dst);
    [[nodiscard]] Step copy_range(int in, int out, off_t off, std::uint64_t len, bool to_eof,
                                  const Path& dst, std::vector<char>& buf, bool& use_cfr);
    void apply_file_meta(int src_fd, int dst_fd, const struct stat& st);
    void on_dst_pop(std::uint32_t j, int fd);
    void remove_sources();
    void rollback();
    void collect(TransferResult& out);

    Context& ctx_;
    Tree& t_;
    const TransferParams& p_;
    std::unique_ptr<TreeCursor> src_;
    std::unique_ptr<TreeCursor> dst_;
    std::unordered_map<FileId, std::uint32_t, FileIdHash> links_;
    std::optional<Error> first_error_;
    bool partial_ = false;
    bool aborted_ = false;
    Error abort_error_;
    bool cancelled_ = false;
    bool source_removed_ = false;
};

void Exec::record_failure(std::uint32_t i, const Step& s) {
    Node& n = node(i);
    n.state = Node::State::Failed;
    if (!first_error_) {
        first_error_ = s.error;
    }
    if (i != 0) {
        partial_ = true;
    }
    ctx_.count_failure();
    if (p_.move && s.side == Side::Destination && s.error.code != ErrorCode::AlreadyExists &&
        s.error.code != ErrorCode::Unsupported) {
        aborted_ = true; // S1d: a destination-side failure rolls this item back
        abort_error_ = s.error;
    }
}

Step Exec::decide(std::uint32_t i, bool check_existing, Placement& pl, bool& merge) {
    Node& n = node(i);
    merge = false;
    pl = Placement{};
    pl.action = Placement::Action::KeepBoth; // "create under n.dst_name"
    n.dst_name = i == 0 ? p_.dst_name : n.name;
    pl.name = n.dst_name;
    if (!check_existing) {
        return ok();
    }
    struct stat ex {};
    const int rc = lstat_at(dst_->fd(), n.dst_name, ex);
    if (rc == ENOENT) {
        return ok();
    }
    if (rc != 0) {
        return failure(errno_error(rc, "cannot stat", dst_parent_path(i) / n.dst_name),
                       Side::Destination);
    }
    if (i == 0 && p_.auto_replace && same_inode(ex, *p_.auto_replace) &&
        replace_can_trash(ctx_, dst_parent_path(i) / n.dst_name)) {
        pl.action = Placement::Action::Replace;
        pl.existing = ex;
        return ok();
    }
    ConflictInput in;
    in.source_path = src_path(i);
    in.source = &n.st;
    in.dst_dirfd = dst_->fd();
    in.dst_dir = dst_parent_path(i);
    in.name = n.dst_name;
    in.existing = ex;
    in.force_keep_both = i == 0 && p_.force_keep_both;
    in.permanent_replace_ok = p_.permanent_replace_ok;
    pl = resolve_placement(ctx_, in);
    switch (pl.action) {
    case Placement::Action::Skip: return skip();
    case Placement::Action::Cancel: return cancel();
    case Placement::Action::Fail: return failure(pl.error, Side::Destination);
    case Placement::Action::Merge:
        merge = true;
        n.dst_id = id_of(ex);
        return ok();
    case Placement::Action::KeepBoth: n.dst_name = pl.name; return ok();
    case Placement::Action::Replace: return ok();
    }
    return ok();
}

void Exec::on_dst_pop(std::uint32_t j, int fd) {
    Node& d = node(j);
    if (d.state == Node::State::Created) {
        if (ctx_.opt().preserve_mode && ::fchmod(fd, kept_mode(d.st, fd)) != 0) {
            ctx_.warn_metadata();
        }
        if (ctx_.opt().preserve_times) {
            const std::array<struct timespec, 2> times{d.st.st_atim, d.st.st_mtim};
            if (::futimens(fd, times.data()) != 0) {
                ctx_.warn_metadata();
            }
        }
    }
    if (sync() && (d.state == Node::State::Created || d.state == Node::State::Merged)) {
        if (io_fault(IoSite::Fsync, dst_path(j)) != 0 || ::fsync(fd) != 0) {
            if (p_.move && !aborted_) {
                aborted_ = true;
                abort_error_ = errno_error(EIO, "cannot sync folder", dst_path(j));
            }
        }
    }
}

Step Exec::make_dir(std::uint32_t i, bool check_existing) {
    Node& n = node(i);
    for (int attempt = 0; attempt < 4; ++attempt) {
        Placement pl;
        bool merge = false;
        Step s = decide(i, check_existing, pl, merge);
        if (s.r != Step::R::Ok) {
            return s;
        }
        if (merge) {
            if (!S_ISDIR(pl.existing.st_mode)) {
                return failure(make_error(ErrorCode::AlreadyExists, "not a folder"),
                               Side::Destination);
            }
            n.state = Node::State::Merged;
            return ok();
        }
        const Path target = dst_parent_path(i) / n.dst_name;
        if (pl.action == Placement::Action::Replace) {
            auto item =
                trash_existing(ctx_, dst_->fd(), dst_parent_path(i), n.dst_name, pl.existing);
            if (!item) {
                return failure(item.error(), Side::Destination);
            }
            n.replaced = std::move(*item);
        }
        if (const int f = io_fault(IoSite::Mkdir, target); f != 0) {
            return failure(errno_error(f, "cannot create folder", target), Side::Destination);
        }
        const mode_t mode = ctx_.opt().preserve_mode ? 0700 : 0777;
        if (::mkdirat(dst_->fd(), n.dst_name.c_str(), mode) != 0) {
            const int err = errno;
            if (err == EEXIST && check_existing) {
                continue; // a name appeared: decide again
            }
            return failure(errno_error(err, "cannot create folder", target), Side::Destination);
        }
        struct stat st {};
        if (lstat_at(dst_->fd(), n.dst_name, st) != 0 || !S_ISDIR(st.st_mode)) {
            return failure(errno_error(errno, "cannot stat new folder", target), Side::Destination);
        }
        n.dst_id = id_of(st);
        n.state = Node::State::Created;
        // Enter it now so its metadata is applied on the way out (post-order), and copy the
        // extended attributes (default ACLs then apply to its new children).
        if (const int rc = dst_->go_to(i, [this](std::uint32_t j, int fd) { on_dst_pop(j, fd); });
            rc != 0) {
            return failure(errno_error(rc, "cannot open new folder", target), Side::Destination);
        }
        if (const int rc = src_->go_to(i); rc != 0) {
            return failure(errno_error(rc, "cannot open folder", src_path(i)), Side::Source);
        }
        if (ctx_.opt().preserve_owner && ::geteuid() == 0 &&
            ::fchown(dst_->fd(), n.st.st_uid, n.st.st_gid) != 0) {
            ctx_.warn_metadata();
        }
        if (ctx_.opt().preserve_xattrs) {
            ctx_.warn_metadata(copy_xattrs(src_->fd(), dst_->fd(), true));
        }
        return ok();
    }
    return failure(make_error(ErrorCode::AlreadyExists, "name keeps being taken"),
                   Side::Destination);
}

Step Exec::copy_range(int in, int out, off_t off, std::uint64_t len, bool to_eof, const Path& dst,
                      std::vector<char>& buf, bool& use_cfr) {
    const std::size_t chunk = ctx_.chunk();
    while (to_eof || len > 0) {
        if (!ctx_.checkpoint()) {
            return cancel();
        }
        std::size_t want = chunk;
        if (!to_eof && len < want) {
            want = static_cast<std::size_t>(len);
        }
        if (const int f = io_fault(IoSite::Write, dst, static_cast<std::uint64_t>(off)); f != 0) {
            return failure(errno_error(f, "cannot write", dst), Side::Destination);
        }
        ssize_t n = 0;
        if (use_cfr) {
            off_t in_off = off;
            off_t out_off = off;
            n = ::copy_file_range(in, &in_off, out, &out_off, want, 0);
            if (n < 0) {
                const int err = errno;
                if (err == EINTR) {
                    continue;
                }
                if (err == EXDEV || err == EINVAL || err == ENOSYS || err == EOPNOTSUPP ||
                    err == EBADF || err == EPERM || err == ETXTBSY) {
                    use_cfr = false;
                    continue;
                }
                return failure(errno_error(err, "cannot copy into", dst), Side::Destination);
            }
        } else {
            if (buf.size() < want) {
                buf.resize(want);
            }
            n = ::pread(in, buf.data(), want, off);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return failure(errno_error(errno, "cannot read", dst), Side::Source);
            }
            std::size_t done = 0;
            while (done < static_cast<std::size_t>(n)) {
                const ssize_t w =
                    ::pwrite(out, buf.data() + done, static_cast<std::size_t>(n) - done,
                             off + static_cast<off_t>(done));
                if (w < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    return failure(errno_error(errno, "cannot write", dst), Side::Destination);
                }
                done += static_cast<std::size_t>(w);
            }
        }
        if (n == 0) {
            break; // end of file (it may have shrunk; a move verifies that separately)
        }
        off += static_cast<off_t>(n);
        if (!to_eof) {
            len -= static_cast<std::uint64_t>(n);
        }
        ctx_.add_bytes(static_cast<std::uint64_t>(n));
    }
    return ok();
}

Step Exec::copy_data(int in, int out, const struct stat& st, const Path& dst) {
    const auto size = static_cast<std::uint64_t>(st.st_size);
    if (size == 0) {
        return ok();
    }
    // Reflink first (S11): same filesystem, instant, shares blocks copy-on-write.
    if (io_fault(IoSite::Write, dst, 0) == 0 && ::ioctl(out, FICLONE, in) == 0) {
        ctx_.add_bytes(size);
        return ok();
    }
    std::vector<char> buf;
    bool use_cfr = true;
    const bool sparse = static_cast<std::uint64_t>(st.st_blocks) * 512U < size;
    if (sparse) {
        off_t pos = 0;
        bool seek_ok = true;
        std::uint64_t accounted = 0;
        while (static_cast<std::uint64_t>(pos) < size) {
            const off_t data = ::lseek(in, pos, SEEK_DATA);
            if (data < 0) {
                if (errno == ENXIO) {
                    break; // only a hole remains
                }
                seek_ok = false;
                break;
            }
            off_t hole = ::lseek(in, data, SEEK_HOLE);
            if (hole < 0) {
                seek_ok = false;
                break;
            }
            hole = std::min<off_t>(hole, static_cast<off_t>(size));
            if (ctx_.cancelled()) {
                return cancel();
            }
            const Step s = copy_range(in, out, data, static_cast<std::uint64_t>(hole - data), false,
                                      dst, buf, use_cfr);
            if (s.r != Step::R::Ok) {
                return s;
            }
            ctx_.add_bytes(static_cast<std::uint64_t>(data - pos)); // the hole counts as done
            accounted += static_cast<std::uint64_t>(hole - pos);
            pos = hole;
        }
        if (seek_ok) {
            if (::ftruncate(out, static_cast<off_t>(size)) != 0) {
                return failure(errno_error(errno, "cannot size", dst), Side::Destination);
            }
            if (accounted < size) {
                ctx_.add_bytes(size - accounted);
            }
            return ok();
        }
        // SEEK_DATA unsupported: dense copy from the start (overwrites what was written).
        if (::ftruncate(out, 0) != 0) {
            return failure(errno_error(errno, "cannot truncate", dst), Side::Destination);
        }
    }
    return copy_range(in, out, 0, 0, true, dst, buf, use_cfr);
}

} // namespace

std::size_t copy_xattrs(int src_fd, int dst_fd, bool is_dir) {
    std::size_t warnings = 0;
    ssize_t len = ::flistxattr(src_fd, nullptr, 0);
    if (len <= 0) {
        return (len < 0 && errno != ENOTSUP && errno != EOPNOTSUPP) ? 1 : 0;
    }
    std::vector<char> names(static_cast<std::size_t>(len));
    len = ::flistxattr(src_fd, names.data(), names.size());
    if (len < 0) {
        return 1;
    }
    std::vector<char> value;
    for (std::size_t pos = 0; pos < static_cast<std::size_t>(len);) {
        const std::string_view name(names.data() + pos);
        pos += name.size() + 1;
        if (!xattr_wanted(name, is_dir)) {
            continue; // security.*, trusted.* never (S10)
        }
        const std::string key(name);
        const ssize_t vlen = ::fgetxattr(src_fd, key.c_str(), nullptr, 0);
        if (vlen < 0) {
            ++warnings;
            continue;
        }
        value.resize(static_cast<std::size_t>(vlen));
        const ssize_t got = ::fgetxattr(src_fd, key.c_str(), value.data(), value.size());
        if (got < 0 ||
            ::fsetxattr(dst_fd, key.c_str(), value.data(), static_cast<std::size_t>(got), 0) != 0) {
            ++warnings;
        }
    }
    return warnings;
}

namespace {

void Exec::apply_file_meta(int src_fd, int dst_fd, const struct stat& st) {
    const OpOptions& o = ctx_.opt();
    if (o.preserve_owner && ::geteuid() == 0 && ::fchown(dst_fd, st.st_uid, st.st_gid) != 0) {
        ctx_.warn_metadata();
    }
    const mode_t mode = o.preserve_mode ? kept_mode(st, dst_fd) : (0666 & ~process_umask());
    if (::fchmod(dst_fd, mode) != 0) {
        ctx_.warn_metadata();
    }
    if (o.preserve_xattrs) {
        ctx_.warn_metadata(copy_xattrs(src_fd, dst_fd, false));
    }
    if (o.preserve_times) {
        const std::array<struct timespec, 2> times{st.st_atim, st.st_mtim};
        if (::futimens(dst_fd, times.data()) != 0) {
            ctx_.warn_metadata();
        }
    }
}

Step Exec::commit(std::uint32_t i, const std::string& tmp, Placement pl, bool check_existing) {
    Node& n = node(i);
    const int dfd = dst_->fd();
    auto drop_temp = [&] { ::unlinkat(dfd, tmp.c_str(), 0); }; // S1d: our own temp
    for (int attempt = 0; attempt < 4; ++attempt) {
        const Path target = dst_parent_path(i) / n.dst_name;
        if (const int f = io_fault(IoSite::Commit, target); f != 0) {
            drop_temp();
            return failure(errno_error(f, "cannot commit", target), Side::Destination);
        }
        int rc = 0;
        if (pl.action == Placement::Action::Replace && !pl.permanent) {
            auto item = trash_existing(ctx_, dfd, dst_parent_path(i), n.dst_name, pl.existing);
            if (!item) {
                drop_temp();
                return failure(item.error(), Side::Destination);
            }
            rc = rename_no_overwrite(dfd, tmp, dfd, n.dst_name, true);
            if (rc != 0) {
                // Put the replaced item back where it was; the new item is not placed.
                (void)ctx_.trash()->restore(*item, target);
            } else {
                n.replaced = std::move(*item);
            }
        } else if (pl.action == Placement::Action::Replace) {
            // S8: no trash for this file; atomic overwrite by rename after an explicit decision.
            struct stat now {};
            if (lstat_at(dfd, n.dst_name, now) == 0 && !same_inode(now, pl.existing)) {
                rc = EEXIST; // something else is there now: decide again
            } else {
                rc = ::renameat(dfd, tmp.c_str(), dfd, n.dst_name.c_str()) == 0 ? 0 : errno;
                if (rc == 0) {
                    n.replaced_permanently = true;
                }
            }
        } else {
            rc = rename_no_overwrite(dfd, tmp, dfd, n.dst_name, true);
        }
        if (rc == 0) {
            n.state = Node::State::Created;
            return ok();
        }
        if (rc != EEXIST || !check_existing) {
            drop_temp();
            return failure(errno_error(rc, "cannot place", target), Side::Destination);
        }
        // The name was taken between the check and the commit: the conflict rules again.
        struct stat ex {};
        if (lstat_at(dfd, n.dst_name, ex) != 0) {
            pl.action = Placement::Action::KeepBoth; // vanished again: plain retry
            continue;
        }
        ConflictInput in;
        in.source_path = src_path(i);
        in.source = &n.st;
        in.dst_dirfd = dfd;
        in.dst_dir = dst_parent_path(i);
        in.name = n.dst_name;
        in.existing = ex;
        in.merge_supported = false;
        in.permanent_replace_ok = p_.permanent_replace_ok;
        pl = resolve_placement(ctx_, in);
        switch (pl.action) {
        case Placement::Action::Skip:
        case Placement::Action::Merge: drop_temp(); return skip();
        case Placement::Action::Cancel: drop_temp(); return cancel();
        case Placement::Action::Fail: drop_temp(); return failure(pl.error, Side::Destination);
        case Placement::Action::KeepBoth: n.dst_name = pl.name; break;
        case Placement::Action::Replace: break;
        }
    }
    drop_temp();
    return failure(make_error(ErrorCode::AlreadyExists, "name keeps being taken"),
                   Side::Destination);
}

Step Exec::make_file(std::uint32_t i, bool check_existing) {
    Node& n = node(i);
    Placement pl;
    bool merge = false;
    Step s = decide(i, check_existing, pl, merge);
    if (s.r != Step::R::Ok) {
        return s;
    }
    if (merge) {
        return skip();
    }
    const int sfd_dir = src_->fd();
    const int dfd = dst_->fd();
    const Path target = dst_parent_path(i) / n.dst_name;
    if (const int f = io_fault(IoSite::CreateTemp, target); f != 0) {
        return failure(errno_error(f, "cannot create", target), Side::Destination);
    }

    // Hard link of an inode already copied in this tree (S11).
    if (ctx_.opt().preserve_hardlinks && n.st.st_nlink > 1) {
        if (auto it = links_.find(id_of(n.st)); it != links_.end()) {
            const std::string tmp = temp_name_for(n.dst_name);
            const Path first = dst_path(it->second);
            if (::linkat(AT_FDCWD, first.c_str(), dfd, tmp.c_str(), 0) == 0) {
                n.verified = node(it->second).verified;
                const Step c = commit(i, tmp, pl, check_existing);
                if (c.r == Step::R::Ok) {
                    struct stat st {};
                    if (lstat_at(dfd, n.dst_name, st) == 0) {
                        n.dst_id = id_of(st);
                    }
                    ctx_.add_bytes(static_cast<std::uint64_t>(n.st.st_size));
                }
                return c;
            }
            // EXDEV / EMLINK / EPERM: copy it separately.
        }
    }

    int flags = O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK;
    if (i != 0 || !t_.follow_top) {
        flags |= O_NOFOLLOW;
    }
    Fd in(::openat(sfd_dir, n.name.c_str(), flags));
    if (!in.valid()) {
        return failure(errno_error(errno, "cannot open", src_path(i)), Side::Source);
    }
    struct stat before {};
    if (::fstat(in.get(), &before) != 0) {
        return failure(errno_error(errno, "cannot stat", src_path(i)), Side::Source);
    }
    if (!S_ISREG(before.st_mode) || !same_inode(before, n.st)) {
        return failure(
            make_error(ErrorCode::IoError, "changed since the scan: " + src_path(i).native()),
            Side::Source);
    }
    if (p_.move && (before.st_size != n.st.st_size ||
                    to_timestamp(before.st_mtim) != to_timestamp(n.st.st_mtim))) {
        return failure(
            make_error(ErrorCode::IoError, "changed during the move: " + src_path(i).native()),
            Side::Source);
    }

    std::string tmp;
    Fd out;
    for (int attempt = 0; attempt < 8 && !out.valid(); ++attempt) {
        tmp = temp_name_for(n.dst_name);
        out.reset(
            ::openat(dfd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (!out.valid() && errno != EEXIST) {
            return failure(errno_error(errno, "cannot create", target), Side::Destination);
        }
    }
    if (!out.valid()) {
        return failure(make_error(ErrorCode::AlreadyExists, "no free temp name"),
                       Side::Destination);
    }
    auto drop_temp = [&] {
        out.reset();
        ::unlinkat(dfd, tmp.c_str(), 0); // S6 / S9: never leave a temp file behind
    };

    Step d = copy_data(in.get(), out.get(), before, target);
    if (d.r != Step::R::Ok) {
        drop_temp();
        return d;
    }
    if (p_.move) {
        struct stat after {};
        if (::fstat(in.get(), &after) != 0 || !same_inode(after, before) ||
            after.st_size != before.st_size ||
            to_timestamp(after.st_mtim) != to_timestamp(before.st_mtim)) {
            drop_temp();
            return failure(
                make_error(ErrorCode::IoError, "changed during the move: " + src_path(i).native()),
                Side::Source);
        }
        n.verified = true;
    }
    apply_file_meta(in.get(), out.get(), before);
    if (sync()) {
        const int f = io_fault(IoSite::Fsync, target);
        if (f != 0 || ::fsync(out.get()) != 0) {
            const int err = f != 0 ? f : errno;
            drop_temp();
            return failure(errno_error(err, "cannot sync", target), Side::Destination);
        }
    }
    struct stat st {};
    if (::fstat(out.get(), &st) == 0) {
        n.dst_id = id_of(st);
    }
    if (const int err = out.close_checked(); err != 0) {
        ::unlinkat(dfd, tmp.c_str(), 0);
        return failure(errno_error(err, "cannot write", target), Side::Destination);
    }
    const Step c = commit(i, tmp, pl, check_existing);
    if (c.r == Step::R::Ok && n.st.st_nlink > 1) {
        links_.emplace(id_of(n.st), i);
    }
    return c;
}

Step Exec::make_link(std::uint32_t i, bool check_existing) {
    Node& n = node(i);
    Placement pl;
    bool merge = false;
    Step s = decide(i, check_existing, pl, merge);
    if (s.r != Step::R::Ok) {
        return s;
    }
    if (merge) {
        return skip();
    }
    const Path target_path = dst_parent_path(i) / n.dst_name;
    std::string target(static_cast<std::size_t>(n.st.st_size) + 1, '\0');
    for (;;) {
        const ssize_t len = ::readlinkat(src_->fd(), n.name.c_str(), target.data(), target.size());
        if (len < 0) {
            return failure(errno_error(errno, "cannot read link", src_path(i)), Side::Source);
        }
        if (static_cast<std::size_t>(len) < target.size()) {
            target.resize(static_cast<std::size_t>(len));
            break;
        }
        target.resize(target.size() * 2);
    }
    if (const int f = io_fault(IoSite::CreateTemp, target_path); f != 0) {
        return failure(errno_error(f, "cannot create link", target_path), Side::Destination);
    }
    const int dfd = dst_->fd();
    const std::string tmp = temp_name_for(n.dst_name);
    if (::symlinkat(target.c_str(), dfd, tmp.c_str()) != 0) {
        return failure(errno_error(errno, "cannot create link", target_path), Side::Destination);
    }
    if (ctx_.opt().preserve_times) {
        const std::array<struct timespec, 2> times{n.st.st_atim, n.st.st_mtim};
        if (::utimensat(dfd, tmp.c_str(), times.data(), AT_SYMLINK_NOFOLLOW) != 0) {
            ctx_.warn_metadata();
        }
    }
    struct stat st {};
    if (lstat_at(dfd, tmp, st) == 0) {
        n.dst_id = id_of(st);
    }
    return commit(i, tmp, pl, check_existing);
}

Step Exec::make_fifo(std::uint32_t i, bool check_existing) {
    Node& n = node(i);
    Placement pl;
    bool merge = false;
    Step s = decide(i, check_existing, pl, merge);
    if (s.r != Step::R::Ok) {
        return s;
    }
    if (merge) {
        return skip();
    }
    const Path target_path = dst_parent_path(i) / n.dst_name;
    if (const int f = io_fault(IoSite::CreateTemp, target_path); f != 0) {
        return failure(errno_error(f, "cannot create", target_path), Side::Destination);
    }
    const int dfd = dst_->fd();
    const std::string tmp = temp_name_for(n.dst_name);
    // S11: recreated, never opened for reading or writing (that would block).
    if (::mkfifoat(dfd, tmp.c_str(), 0600) != 0) {
        return failure(errno_error(errno, "cannot create", target_path), Side::Destination);
    }
    if (ctx_.opt().preserve_mode) {
        // chmod through an O_PATH descriptor of the fifo itself (no open for I/O, no link
        // following): /proc/self/fd/N names exactly that inode.
        Fd path_fd(::openat(dfd, tmp.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
        struct stat pst {};
        if (!path_fd.valid() || ::fstat(path_fd.get(), &pst) != 0 || !S_ISFIFO(pst.st_mode) ||
            ::chmod(("/proc/self/fd/" + std::to_string(path_fd.get())).c_str(),
                    n.st.st_mode & 01777) != 0) {
            ctx_.warn_metadata();
        }
    }
    if (ctx_.opt().preserve_times) {
        const std::array<struct timespec, 2> times{n.st.st_atim, n.st.st_mtim};
        if (::utimensat(dfd, tmp.c_str(), times.data(), AT_SYMLINK_NOFOLLOW) != 0) {
            ctx_.warn_metadata();
        }
    }
    struct stat st {};
    if (lstat_at(dfd, tmp, st) == 0) {
        n.dst_id = id_of(st);
    }
    return commit(i, tmp, pl, check_existing);
}

void Exec::process(std::uint32_t i) {
    Node& n = node(i);
    bool check_existing = true;
    if (i != 0) {
        const Node& par = node(n.parent);
        if (par.state != Node::State::Created && par.state != Node::State::Merged) {
            n.state = Node::State::Skipped; // its folder was skipped or failed
            return;
        }
        check_existing = par.state == Node::State::Merged;
    }
    const auto pop = [this](std::uint32_t j, int fd) { on_dst_pop(j, fd); };
    if (const int rc = dst_->go_to(n.parent, pop); rc != 0) {
        record_failure(i, failure(errno_error(rc, "destination folder changed", dst_parent_path(i)),
                                  Side::Destination));
        return;
    }
    if (const int rc = src_->go_to(n.parent); rc != 0) {
        record_failure(
            i, failure(errno_error(rc, "folder changed", src_path(i).parent_path()), Side::Source));
        return;
    }
    const Path spath = src_path(i);
    ctx_.set_current(spath);
    Step s;
    if ((i != 0 || p_.inject_top) && ctx_.inject(spath)) {
        s = failure(make_error(ErrorCode::IoError, "injected: " + spath.native()), Side::Source);
    } else if (n.mount_point) {
        s = failure(make_error(ErrorCode::Unsupported, "mount point inside: " + spath.native()),
                    Side::Source);
    } else if (n.scan_errno != 0) {
        s = failure(errno_error(n.scan_errno, "cannot read", spath), Side::Source);
    } else {
        const mode_t m = n.st.st_mode;
        if (S_ISDIR(m)) {
            s = make_dir(i, check_existing);
        } else if (S_ISREG(m)) {
            s = make_file(i, check_existing);
        } else if (S_ISLNK(m)) {
            s = make_link(i, check_existing);
        } else if (S_ISFIFO(m)) {
            s = make_fifo(i, check_existing);
        } else {
            s = failure(
                make_error(ErrorCode::Unsupported, "special file not copied: " + spath.native()),
                Side::Source); // S11: sockets and devices
        }
    }
    ctx_.add_items(1);
    switch (s.r) {
    case Step::R::Ok: break;
    case Step::R::Skip:
        n.state = Node::State::Skipped;
        if (i != 0) {
            partial_ = true;
        }
        break;
    case Step::R::Cancel: cancelled_ = true; break;
    case Step::R::Fail: record_failure(i, s); break;
    }
}

void Exec::remove_sources() {
    // Bottom up over the scan (S7): only verified, recorded entries; never follow links.
    const auto count = static_cast<std::uint32_t>(t_.nodes.size());
    for (std::uint32_t k = count; k-- > 0;) {
        Node& n = node(k);
        auto keep = [&](std::optional<Error> why) {
            if (n.parent != kNoParent) {
                node(n.parent).kept_child = true;
            }
            if (k != 0 || n.state == Node::State::Created || n.state == Node::State::Merged) {
                partial_ = true;
            }
            if (why && !first_error_) {
                first_error_ = std::move(why);
            }
        };
        const bool placed = n.state == Node::State::Created || n.state == Node::State::Merged;
        if (!placed || n.kept_child || (S_ISREG(n.st.st_mode) && !n.verified)) {
            keep(std::nullopt);
            continue;
        }
        if (const int rc = src_->go_to(n.parent); rc != 0) {
            keep(errno_error(rc, "folder changed", src_path(k).parent_path()));
            continue;
        }
        struct stat now {};
        const int rc = lstat_at(src_->fd(), n.name, now);
        const bool dir = S_ISDIR(n.st.st_mode);
        if (rc != 0 || !same_inode(now, n.st) ||
            (now.st_mode & S_IFMT) != (n.st.st_mode & S_IFMT) ||
            (!dir && (now.st_size != n.st.st_size ||
                      to_timestamp(now.st_mtim) != to_timestamp(n.st.st_mtim)))) {
            keep(
                make_error(ErrorCode::IoError, "changed during the move: " + src_path(k).native()));
            continue;
        }
        if (const int f = io_fault(IoSite::Unlink, src_path(k)); f != 0) {
            keep(errno_error(f, "cannot remove", src_path(k)));
            continue;
        }
        if (::unlinkat(src_->fd(), n.name.c_str(), dir ? AT_REMOVEDIR : 0) != 0) {
            const int err = errno;
            keep(errno_error(err,
                             err == ENOTEMPTY || err == EEXIST
                                 ? "new entries appeared during the move in"
                                 : "cannot remove",
                             src_path(k)));
            continue;
        }
        if (k == 0) {
            source_removed_ = true;
        }
    }
    (void)src_->unwind([](std::uint32_t, int) {});
}

void Exec::rollback() {
    // S1d: remove exactly what this item created (identity-checked, bottom up).
    (void)dst_->unwind([](std::uint32_t, int) {});
    const auto count = static_cast<std::uint32_t>(t_.nodes.size());
    for (std::uint32_t k = count; k-- > 0;) {
        Node& n = node(k);
        if (n.state != Node::State::Created) {
            continue;
        }
        if (dst_->go_to(n.parent) != 0) {
            continue;
        }
        struct stat now {};
        if (lstat_at(dst_->fd(), n.dst_name, now) == 0 && same_inode(now, n.dst_id)) {
            ::unlinkat(dst_->fd(), n.dst_name.c_str(), S_ISDIR(now.st_mode) ? AT_REMOVEDIR : 0);
        }
        n.state = Node::State::Skipped;
        if (n.replaced && ctx_.trash() != nullptr) {
            (void)ctx_.trash()->restore(*n.replaced, dst_parent_path(k) / n.dst_name);
            n.replaced.reset();
        }
    }
    (void)dst_->unwind([](std::uint32_t, int) {});
}

void Exec::collect(TransferResult& out) {
    const Node& top = node(0);
    out.outcome.source = src_path(0);
    out.source_removed = source_removed_;
    switch (top.state) {
    case Node::State::Created:
    case Node::State::Merged:
        out.outcome.status = cancelled_ ? ItemStatus::Cancelled : ItemStatus::Done;
        out.outcome.destination = p_.dst_dir / top.dst_name;
        break;
    case Node::State::Skipped:
        out.outcome.status = cancelled_ ? ItemStatus::Cancelled : ItemStatus::Skipped;
        break;
    case Node::State::Failed: out.outcome.status = ItemStatus::Failed; break;
    case Node::State::Pending:
    case Node::State::Removed: out.outcome.status = ItemStatus::Cancelled; break;
    }
    out.outcome.partial = partial_ && out.outcome.status != ItemStatus::Failed &&
                          out.outcome.status != ItemStatus::Skipped;
    out.outcome.error = first_error_;
    if (cancelled_ && !out.outcome.error) {
        out.outcome.error = make_error(ErrorCode::Cancelled, "cancelled");
    }
    // One undo step per created subtree root: the top-level item, or entries created inside a
    // merged folder (S12; a merged folder itself is not Navigator's and is never undone).
    const auto count = static_cast<std::uint32_t>(t_.nodes.size());
    for (std::uint32_t k = 0; k < count; ++k) {
        const Node& n = node(k);
        if (n.state != Node::State::Created) {
            continue;
        }
        if (k != 0 && node(n.parent).state != Node::State::Merged) {
            continue;
        }
        UndoStep step;
        step.kind = p_.step_kind;
        step.before = src_path(k);
        const Path dst = dst_path(k);
        struct stat st {};
        if (lstat_path(dst, st) == 0) {
            step.after = guard_from(dst, st);
        } else {
            step.after.path = dst;
        }
        step.replaced = n.replaced;
        step.replaced_permanently = n.replaced_permanently;
        out.steps.push_back(std::move(step));
    }
}

TransferResult Exec::run() {
    TransferResult out;
    Fd src_root(open_dir_path(t_.parent));
    struct stat sst {};
    if (!src_root.valid() || ::fstat(src_root.get(), &sst) != 0 || !same_inode(sst, t_.parent_id)) {
        out.outcome.source = src_path(0);
        out.outcome.status = ItemStatus::Failed;
        out.outcome.error =
            errno_error(src_root.valid() ? ESTALE : errno, "cannot open folder", t_.parent);
        return out;
    }
    Fd dst_root(open_dir_path(p_.dst_dir));
    struct stat dst_st {};
    if (!dst_root.valid() || ::fstat(dst_root.get(), &dst_st) != 0) {
        out.outcome.source = src_path(0);
        out.outcome.status = ItemStatus::Failed;
        out.outcome.error = errno_error(errno, "cannot open destination", p_.dst_dir);
        return out;
    }
    const FileId dst_root_id = id_of(dst_st);
    src_ = std::make_unique<TreeCursor>(std::move(src_root), t_.parent_id, t_,
                                        TreeCursor::Side::Source);
    dst_ = std::make_unique<TreeCursor>(std::move(dst_root), dst_root_id, t_,
                                        TreeCursor::Side::Destination);

    const auto count = static_cast<std::uint32_t>(t_.nodes.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!ctx_.checkpoint()) {
            cancelled_ = true;
            break;
        }
        process(i);
        if (cancelled_ || aborted_) {
            break;
        }
        if (i == 0 && node(0).state != Node::State::Created &&
            node(0).state != Node::State::Merged) {
            break; // top-level item skipped or failed: nothing below to do
        }
    }
    if (ctx_.cancelled()) {
        cancelled_ = true;
    }
    // Leave every directory (post-order metadata, fsync for moves).
    (void)dst_->unwind([this](std::uint32_t j, int fd) { on_dst_pop(j, fd); });
    if (p_.move && !cancelled_ && !aborted_) {
        if (::fsync(dst_->fd()) != 0) {
            aborted_ = true;
            abort_error_ = errno_error(errno, "cannot sync folder", p_.dst_dir);
        }
    }
    if (p_.move && (cancelled_ || aborted_)) {
        rollback();
        out.outcome.source = src_path(0);
        out.outcome.status = cancelled_ ? ItemStatus::Cancelled : ItemStatus::Failed;
        out.outcome.error =
            cancelled_ ? make_error(ErrorCode::Cancelled, "cancelled") : abort_error_;
        return out;
    }
    if (p_.move) {
        PhaseScope finishing(ctx_, OpPhase::Finishing);
        remove_sources();
    }
    collect(out);
    return out;
}

} // namespace

TransferResult transfer_tree(Context& ctx, Tree& tree, const TransferParams& params) {
    Exec exec(ctx, tree, params);
    return exec.run();
}

} // namespace navigator::ops::detail
