# Platform limitations (Phases 1-2)

Owner: platform (`src/platform`). This page lists what the Linux watch and volume backends
cannot see or do. Users and later phases should rely on these limits being real, not on them
being worked around. Contracts: `include/navigator/watch.hpp`, `include/navigator/volume.hpp`,
`include/navigator/platform.hpp`, `include/navigator/mime.hpp`.

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

## XDG directories (Phase 2)

- `Home`, `Config` and `Cache` follow the header. Relative or empty `$HOME`, `$XDG_CONFIG_HOME`
  and `$XDG_CACHE_HOME` values are ignored, as the XDG Base Directory spec says.
- `Desktop`, `Documents`, `Downloads`, `Music`, `Pictures` and `Videos` return `Unsupported`
  until Phase 4 parses `user-dirs.dirs`.
- `move_to_trash` and `open_terminal` are defined but always return `Unsupported` until their
  phases land, so nothing links against a missing symbol.
