# Platform limitations (Phases 1-7)

Owner: platform (`src/platform`). This page lists what the Linux watch and volume backends
cannot see or do. Users and later phases should rely on these limits being real, not on them
being worked around. Contracts: `include/navigator/watch.hpp`, `include/navigator/volume.hpp`,
`include/navigator/platform.hpp`, `include/navigator/mime.hpp`, `include/navigator/trash.hpp`,
`include/navigator/transfer.hpp`.

## Directory watching (inotify)

- **Network and FUSE filesystems.** NFS, SMB/CIFS, sshfs, rclone and GVfs FUSE mounts only
  report changes made *through this kernel* (by programs on this machine). Changes made on the
  server or by other clients are never reported. The UI needs a manual refresh, or a polling
  backend (planned later; `WatcherFactory` allows one to be swapped in).
- **Pseudo filesystems.** `/proc`, `/sys` and similar filesystems produce no useful events.
- **Non-recursive.** One watch covers the direct children of one directory. A change deep inside
  a subdirectory shows up only as an `Attrib`/`Modified` of that child, if at all.
- **Watch limit.** Each watched directory uses one inotify watch from
  `fs.inotify.max_user_watches`, which is per user and shared with every other program (IDEs,
  sync clients). When it runs out, `add()` fails with `PlatformError` and the message names the
  sysctl. Raising it needs root: `sysctl fs.inotify.max_user_watches=524288`.
- **Instance limit.** Each watcher uses one inotify instance from
  `fs.inotify.max_user_instances`. Hitting it makes `create_inotify_watcher()` fail with
  `PlatformError`.
- **Queue overflow.** The kernel queue holds `fs.inotify.max_queued_events` events (default
  16384). This limit is global and only root can change it. When the queue overflows, the
  consumer gets one `Overflow` event and must re-list every watched directory. A failed read
  (`IoError`) is handled the same way: the next successful `poll()` starts with `Overflow`.
- **Per-poll cap.** One `poll()` reads at most 4 MiB of kernel records, so a writer that never
  stops cannot keep the consumer stuck. The rest stays queued and `wait()` returns at once. A
  rename that crosses the cap shows up as `MovedFrom` and then `MovedTo`, not as `Renamed`.
- **Rename pairing** is per handle and within one `poll()`. A move between two watched
  directories always shows up as `MovedFrom` and `MovedTo`.
- **Same inode, several paths.** Adding a directory through a symlink, or adding a bind-mounted
  copy of it, returns the existing handle (the watch is per inode). If the directory was moved
  away but its `WatchedDirGone` has not been polled yet, re-adding it through its new path
  returns the old handle, and that `WatchedDirGone` then ends it. Add it again after polling.
- **Watch descriptor reuse.** Stale records of a removed watch are dropped by counting the
  `IN_IGNORED` records still expected. After a kernel overflow, an `IN_IGNORED` may be lost. That
  descriptor number could then misattribute events only if the kernel reused it, and the kernel
  hands out descriptors in a cycle, so in practice it never does.

## Volumes

- **No hardware in CI.** USB, SD and optical media, unplug and remount are all tested against a
  fake `IMountSource`, a fake sysfs tree and a fake `/dev/disk` tree (`VolumeQuery::sysfs_root`
  / `devfs_root`). Only the smoke tests touch the real `/proc/self/mountinfo`: a Root volume
  exists, and `volume_for(temp dir)` works. Real hot-plug has to be checked by hand.
- **Device-mapper is not traced** (rule R2). LVM, LUKS and other `dm-*` / `/dev/mapper/*`
  volumes are never removable by sysfs. A LUKS USB stick counts as removable only when it is
  mounted under `/media` or `/run/media` (rule R3). Labels and UUIDs come from
  `/dev/disk/by-label|by-uuid` symlinks pointing at the device's own basename, so mapper names
  (`/dev/mapper/vg-root`, whose links point at `dm-N`) usually get no label.
- **Removable detection is heuristic.** It uses sysfs `removable=1` or a `usb*` component in the
  device's sysfs path. A udisks mount point (`/media`, `/run/media`) counts only when the device
  cannot be resolved in sysfs, so internal partitions auto-mounted under `/media` (dual-boot
  NTFS) stay Internal. Thunderbolt, eSATA and FireWire disks that report `removable=0` therefore
  show up as Internal even under `/media`.
- **Hidden by default:** everything at or below `/var/lib` (lxcfs, Waydroid, Docker), service
  FUSE types (`fuse.lxcfs`, `fuse.portal`, and `fuse.gvfsd-fuse` until the GVfs phase), and
  single-file bind mounts (a mount point that is not a directory). `include_hidden` shows them.
- **Network volumes.** `statvfs()` on a hung NFS/SMB mount can block for a long time. Never call
  `list_volumes()`, `volume_for()` or `IVolumeMonitor::poll()` on the UI thread. Network volumes
  have no label or UUID.
- **GVfs is not covered.** Phase 1 has no GIO/GVfs code. GVfs FUSE (`fuse.gvfsd-fuse`) and
  `fuse.portal` are treated as pseudo filesystems and hidden. MTP phones, `smb://` and `sftp://`
  locations opened through GNOME do not appear as volumes.
- **Monitor change detection.** With a change fd (`/proc/self/mountinfo`, POLLPRI), `poll()`
  re-reads the mount table every time and runs the full pipeline only when the text changed.
  That also covers signals consumed by an external loop polling `native_handle()`. As a result,
  a filesystem relabel without a remount (a `/dev/disk/by-label` change only) is not reported.
  Sources without a change fd re-run the pipeline on every `poll()` and do report it.
- **Space** is read when the table changes, not live. Space changes are not events.
- **Containers.** If `/` is an `overlay` (Docker, Flatpak build sandboxes), it is kept, because
  `/` is never dropped, but its kind is Virtual: rule K checks Virtual first.

- **Overflow can hide a directory removal.** When the inotify queue overflows, the kernel may drop the event that would produce `WatchedDirGone`. Consumers get `Overflow`; if the re-list then fails with `NotFound`, treat the watched directory as gone and `remove()` its handle.

## Launching and opening (Phase 2)

- **Mechanism.** The default `IAppLauncher` resolves `argv[0]` in `$PATH` first (execvp rules;
  `confstr(_CS_PATH)` when `$PATH` is unset), so a missing or non-executable program is
  `NotFound` before anything is forked. It then double-forks: the intermediate child calls
  `setsid()`, forks the program and exits at once and is reaped synchronously. The program is
  re-parented to init (or the nearest subreaper, e.g. `systemd --user`), so Navigator never has
  a zombie. The program is in a new session and process group, but it is not the session leader,
  so it can never acquire a controlling terminal.
- **Child state.** fds 0-2 are `/dev/null`. Every other fd is closed with `close_range(2)`, or a
  `close()` loop up to `RLIMIT_NOFILE` (capped at 2^20) on kernels before 5.9. All signals get
  `SIG_DFL` and the mask is cleared. The environment is inherited unchanged.
- **What `launch()` reports.** Success means `execve` succeeded. A failed `chdir(working_dir)`
  or `execve` in the child (for example a `#!` interpreter that does not exist) comes back
  synchronously as `PlatformError` through a close-on-exec pipe. A script without `#!` runs
  through `/bin/sh`, as `execvp` does.
- **Cost.** `fork()` copies Navigator's page tables, so its cost grows with the address space
  (not measured yet in the full app). If it shows up in frame times, switch to
  `posix_spawn`/`clone(CLONE_VFORK)` behind the same interface.
- **Opener outcome is not observed.** `open_with_default` returns once `xdg-open` (or
  `gio open`) has started. "No application for this type" and opener crashes are not reported
  in Phase 2, and `xdg-open`'s exit status is never read. Tests use a fake launcher and never
  start an opener.
- **No GIO.** `gio` is used only as an external program when `xdg-open` is missing. Navigator
  links no GLib/GIO and builds without them. Flatpak/portal opening (`org.freedesktop.portal.
  OpenURI`) is not implemented. Inside a Flatpak sandbox this relies on the runtime's `xdg-open`
  forwarding to the portal (untested).

## MIME (Phase 2)

- **Name and metadata only.** `mime_type()` and `mime::guess_for_entry()` never read file
  content. Files without a known extension are `application/octet-stream`, or
  `application/x-executable` if they have no extension and an execute bit is set. Text files
  without an extension (`README`, `Makefile`) show as "File". `.ts` is deliberately unknown.
- **Symlinks listed without `follow_symlinks`.** The entry's `mode` is the link's own
  (`S_IFLNK | 0777`), so a link to an extensionless file is never reported as executable, and a
  link to a FIFO, socket or device is `application/octet-stream`. `mime_type(path)` fills the
  target's mode itself and classifies such links fully.
- **Compound suffixes** are only `tar.gz`, `tar.bz2`, `tar.xz` and `tar.zst`. The part before
  the last dot must be exactly `tar` (ASCII case-insensitive), so a file named `tar.gz` counts too.

## XDG directories (Phases 2 and 4)

- `Home`, `Config` and `Cache` follow the header. Relative or empty `$HOME`, `$XDG_CONFIG_HOME`
  and `$XDG_CACHE_HOME` values are ignored, as the XDG Base Directory spec says.
- `Desktop`, `Documents`, `Downloads`, `Music`, `Pictures` and `Videos` come from
  `$XDG_CONFIG_HOME/user-dirs.dirs` (Phase 4). Only that file is read: the system defaults in
  `/etc/xdg/user-dirs.defaults` are not, and `xdg-user-dirs-update` is never run. On a system
  without the file only Desktop has a value (`$HOME/Desktop`, as in GLib); the others are
  `NotFound`, and the sidebar leaves them out.
- **Parser scope.** The parser accepts exactly the format `xdg-user-dirs-update` writes and GLib
  reads: `XDG_<NAME>_DIR="$HOME/..."` or `="/..."`, no spaces around `=`, double quotes,
  backslash escapes. It is not a shell: `~`, `${HOME}`, other variables, single quotes and
  `export` lines are ignored, not expanded. Unlike GLib, spaces around `=` make the line invalid
  (the contract says so; `xdg-user-dirs-update` never writes them).
- **No caching, no watching.** `user_dir()` re-reads the file on every call. A rename done by
  `xdg-user-dirs-update` (for example after a locale change) is seen on the next call, but no
  event announces it.
- Files over 64 KiB are treated as empty (defaults). The file is opened with `O_NONBLOCK`, so a
  FIFO in its place cannot hang the caller.
- Names are bytes: a Turkish file's `Masaüstü` or `İndirilenler` is used exactly as written,
  without Unicode normalization. A file in another encoding than the file system's is not
  converted.
- `user_home("name")` uses `getpwnam_r`, so it sees every NSS source (LDAP, SSSD) and may block
  on the network. It is meant for the GoTo worker, never the UI thread.
- `move_to_trash` and `open_terminal` are defined but always return `Unsupported` until their
  phases land, so nothing links against a missing symbol.

## Data dirs, program lookup, thumbnail cache (Phase 6)

- `data_dirs()` returns lexically normalized paths without a trailing `/`, so callers compare
  them directly. Symlinked duplicates (`/usr/share` and a link to it) are not resolved and both
  stay in the list.
- `find_program()` checks the execute bit for the effective user (`faccessat` with
  `AT_EACCESS`). A file on a `noexec` mount passes the check and then fails at exec time; the
  runner reports that as `NotFound` (`EACCES` from `execve`).
- `thumbnail_cache_dir()` is only a path. Nothing in `src/platform` creates it or checks
  permissions; `src/preview` does that when it writes.

## Bounded child processes (Phase 6, `IProcessRunner`)

- **Exec rules.** argv[0] is resolved like `find_program()`, then made absolute, and run with
  `execve`. A script without a `#!` line fails (`ENOEXEC`, `PlatformError`). It is never handed
  to `/bin/sh`. That is the only place where we differ from `execvp`.
- **Process group.** The child gets its own group, and timeout, cancel and normal exit all send
  `SIGKILL` to it. A descendant that leaves the group (`setsid`, `setpgid`) escapes the kill.
  Unsandboxed, such a process can survive. Sandboxed, it cannot outlive bwrap because the whole
  pid namespace dies with it. After the direct child exits, the runner keeps reading its pipes
  for at most 500 ms, so an escaped process holding them open cannot block a worker.
- **Parent death.** `PR_SET_PDEATHSIG(SIGKILL)` is tied to the thread that forked. That thread
  waits in `run()` until the child is reaped, so the signal only fires when Navigator dies.
- **rlimits.** These are best effort. A value above the current hard limit is clamped to it,
  because an unprivileged process can only lower limits. `RLIMIT_CPU` sends `SIGXCPU` at the soft
  limit and `SIGKILL` one second later. `RLIMIT_FSIZE` sends `SIGXFSZ`, and the write fails with
  `EFBIG` if the child ignores the signal. `RLIMIT_AS` counts virtual memory, so a
  sanitizer-instrumented or JIT-heavy thumbnailer may fail even at modest caps.
- **Exit detection.** The runner uses `pidfd_open` (Linux 5.3+) when it exists and otherwise
  polls every 10 ms. If the application ignores `SIGCHLD`, children are reaped automatically, and
  `exit_code` and `term_signal` then stay `-1` and `0`.
- **Blocking.** A hung `chdir` into a dead network mount happens in the child, so the wall-clock
  timeout still covers it. If the child is stuck in uninterruptible sleep (`D` state) when it is
  killed, reaping waits until the kernel lets it go.
- **Sandbox (bubblewrap).** When `bwrap` is in `$PATH`, the runner first probes it once per
  runner instance. The probe runs `true` under the same namespace flags and system binds, and its
  result is cached. If the probe fails (no unprivileged user namespaces, an AppArmor
  `userns` restriction, a seccomp-filtered container), every sandboxed request in that runner
  runs unsandboxed and reports `sandboxed = false`. Callers that require a sandbox must check
  that flag. If a real run fails inside bwrap after the probe succeeded (for example a bind path
  that does not exist), the result is a normal non-zero exit and the run is not retried. Inside
  the sandbox: no network (`--unshare-all` gives an empty network namespace), read-only `/usr`,
  `/etc` and `/lib*`/`/bin`/`/sbin` (as binds or symlinks), an empty tmpfs `/tmp`, a private
  `/proc` and `/dev`, and only the listed read-only and writable paths. bwrap builds its root on a
  writable tmpfs and creates the parent directories of every bind target there (for a program
  under `$HOME`, that includes `/home/<user>/...`). The runner therefore remounts `/` read-only
  after all binds (`--remount-ro /`, which is not recursive). Writes then succeed only in
  `writable_paths`, in the private `/tmp` (never visible on the host) and in `/dev` nodes such
  as `/dev/null`. The environment is
  cleared except `PATH`, `HOME`, `LANG` and `LC_ALL`, and `HOME` usually does not exist inside.
  The program has to live in a bound path: system thumbnailers in `/usr` always do, while a
  program elsewhere has to be in `read_only_paths`.
- **Tests.** `tests/platform` runs the real bwrap only when the probe succeeds on the machine
  (`[bwrap]` tests skip otherwise). Argument construction and the fallback are always tested with
  fake `bwrap` scripts.

## Trash (Phase 7, `trash.hpp`)

- **Same filesystem only.** An item goes to the trash on its own filesystem or nowhere
  (`Unsupported`). The spec would allow copying it into the home trash; Navigator never does.
  The ui then offers the separate, confirmed permanent delete.
- **Which filesystem.** The home trash is used when the item's `st_dev` equals that of the home
  trash (or of its nearest existing parent while it does not exist yet). Otherwise the topdir
  is the mount point from the mount table (`volume_for`; a directory is looked up by itself, any
  other item by its parent, never through a symlink), and it must have the item's `st_dev`. Bind
  mounts of the same filesystem therefore share one trash per mount point, and a bind-mounted
  subtree gets its own `.Trash-$uid` at the bind's mount point.
- **Per-volume trash checks.** `$topdir/.Trash` counts only as a real directory (not a symlink)
  with the sticky bit. `.Trash/$uid` must be a directory owned by the uid and writable; if it is
  a symlink, foreign, or cannot be created, method (2) `.Trash-$uid` is used. An existing
  `.Trash-$uid` that is a symlink, not a directory or foreign makes the volume `Unsupported`; it
  is never repaired or replaced. `files/` and `info/` are opened with `O_NOFOLLOW`.
- **Ownership on vfat / exfat / NTFS.** These filesystems report the mount's `uid=` option as the
  owner of every file. A stick mounted for another user, or with `uid=0`, fails the owner check
  and is `Unsupported`. udisks mounts use the logged-in user's uid, so they work.
- **Read-only volumes** report `ReadOnlyFilesystem` (from `statvfs` or the mount table) before
  anything is created.
- **Race-free names.** `info/<name>.trashinfo` is reserved with `O_EXCL` before the rename, and
  `files/<name>` is filled with `renameat2(RENAME_NOREPLACE)`. A `files/` entry without an info
  file (left by another program) makes Navigator try the next name. Filesystems without
  `RENAME_NOREPLACE` (some FUSE filesystems) fall back to check-then-rename. A program that
  writes `files/<name>` without reserving the info file first can still race there.
- **Long names** are cut at a UTF-8 code point boundary so that `<name>.trashinfo` fits 255
  bytes. `Path=` always keeps the full original name.
- **No `directorysizes`.** It is neither read nor written. Other programs re-validate its entries,
  so a missing entry only costs them a tree walk.
- **Identity on restore** is `(st_dev, st_ino)`. If another program empties the trash and a new
  file reuses that inode under the same trash name, Navigator cannot tell the two apart.
- **Info file left after a restore.** If the info file cannot be removed after a successful
  restore, the restore still succeeds. The stale info file is then an orphan, and `list()` skips it.
- **Listing** skips info files larger than 64 KiB, info files that are not regular files,
  unparsable ones, relative `Path=` values in the home trash, values that escape their topdir,
  and `files/` entries without an info file. They are counted as `orphans` and never deleted.
- **Interop checks.** The tests run `gio trash` (local VFS, `HOME` and `XDG_DATA_HOME` inside a
  fixture) and compare GIO's `.trashinfo` with Navigator's byte for byte, apart from the date.
  The reverse direction (`gio trash --list` reading Navigator's files) cannot be pointed at a
  fixture: `trash:///` needs `gvfsd-trash`, which serves the session's real trash. It is
  therefore not automated. Nautilus and KDE formats are covered by fixture files.
- **GVfs locations** (`sftp://`, `smb://` through GVfs, `trash://`) are not handled (Phase 10).

## Clipboard formats (Phase 7, `transfer.hpp`)

- **Pure code.** The functions only build and parse bytes. Ownership of the clipboard, and its
  loss when another program copies, belong to the ui (SDL).
- **Text form.** `text/plain` carries the paths only when every path is valid UTF-8 and has no
  newline. Otherwise it carries the URIs, so a pasted name is never split or garbled.
- **Remote URIs** (`sftp://`, `smb://`, `file://otherhost/`) on the clipboard are reported but
  not pasted. Plain-text paths are never treated as files.
- **`file:/path`** (RFC 8089 without an authority, as some KDE tools write) is accepted when
  reading. Navigator itself always writes `file:///path`.
