// Compile-only check that the Phase 7 contracts (ops requests, conflicts, progress, results,
// undo records and stack, the operation queue and blocking engine, pure name/path helpers,
// the Trash service and .trashinfo helpers, clipboard / uri-list / drop-action helpers, the
// new ErrorCodes and the file-operation CommandIds) are usable as documented. Nothing here is
// linked or run.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/ops.hpp>
#include <navigator/platform.hpp>
#include <navigator/transfer.hpp>
#include <navigator/trash.hpp>
#include <navigator/types.hpp>
#include <navigator/volume.hpp>

namespace navigator::contracts_check {

namespace tr = navigator::trash;
namespace tx = navigator::transfer;

// Stable ids that must never change.
static_assert(static_cast<std::uint32_t>(CommandId::Copy) == 1281);
static_assert(static_cast<std::uint32_t>(CommandId::Cut) == 1282);
static_assert(static_cast<std::uint32_t>(CommandId::Paste) == 1283);
static_assert(static_cast<std::uint32_t>(CommandId::Undo) == 1284);
static_assert(static_cast<std::uint32_t>(CommandId::Redo) == 1285);
static_assert(static_cast<std::uint32_t>(CommandId::MoveToTrash) == 1340);
static_assert(static_cast<std::uint32_t>(CommandId::DeletePermanently) == 1341);
static_assert(static_cast<std::uint32_t>(CommandId::Rename) == 1342);
static_assert(static_cast<std::uint32_t>(CommandId::NewFolder) == 1343);
static_assert(static_cast<std::uint32_t>(CommandId::CopyToOtherPane) == 1344);
static_assert(static_cast<std::uint32_t>(CommandId::MoveToOtherPane) == 1345);
static_assert(static_cast<std::uint32_t>(CommandId::OpenTerminalHere) == 1346);
static_assert(static_cast<std::uint32_t>(CommandId::CancelOperation) == 1347);
static_assert(static_cast<std::uint32_t>(CommandId::TogglePauseOperation) == 1348);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleOperationsPanel) == 1349);
static_assert(static_cast<std::uint32_t>(CommandId::OpenTrash) == 1360);
static_assert(static_cast<std::uint32_t>(CommandId::RestoreFromTrash) == 1361);
static_assert(static_cast<std::uint32_t>(CommandId::EmptyTrash) == 1362);
static_assert(PasteArg::IntoCursorFolder == 1U);

// New error codes keep the old values and have stable names.
static_assert(static_cast<int>(ErrorCode::PlatformError) == 9);
static_assert(static_cast<int>(ErrorCode::NoSpace) == 10);
static_assert(static_cast<int>(ErrorCode::NotEmpty) == 13);
static_assert(to_string(ErrorCode::CrossDevice) == "cross_device");
static_assert(to_string(ErrorCode::ReadOnlyFilesystem) == "read_only_filesystem");

// Op vocabulary.
static_assert(ops::to_string(ops::OpKind::DeletePermanently) == "delete_permanently");
static_assert(!ops::OpId{}.valid());
static_assert(ops::OpId{3} == ops::OpId{3});
static_assert(!ops::OpOptions{}.dereference_top_level); // S4
static_assert(ops::OpOptions{}.trash_replaced);         // S8
static_assert(ops::kNameMaxBytes == 255);

// Safe defaults of non-literal types (checked at run time if this were ever run).
[[maybe_unused]] bool safe_defaults() {
    const ops::ConflictDecision decision;
    const ops::OpRequest request;
    return decision.action == ops::ConflictAction::KeepBoth &&                 // the sheet's Enter
           request.conflict == ops::ConflictPolicy::Ask && !request.confirmed; // S3
}

// Drop decisions (GNOME conventions; external drops copy).
static_assert(tx::choose_drop_action({}) == tx::DropAction::Copy);
static_assert(tx::choose_drop_action({.internal = true, .same_filesystem = true}) ==
              tx::DropAction::Move);
static_assert(tx::choose_drop_action({.internal = true, .same_filesystem = true, .ctrl = true}) ==
              tx::DropAction::Copy);
static_assert(tx::choose_drop_action({.shift = true}) == tx::DropAction::Move);
static_assert(tx::choose_drop_action({.ctrl = true, .shift = true}) == tx::DropAction::Link);
static_assert(tx::choose_drop_action({.alt = true}) == tx::DropAction::Ask);
static_assert(tx::choose_drop_action({.into_itself = true, .ctrl = true}) == tx::DropAction::None);
static_assert(tx::choose_drop_action({.internal = true, .into_source_folder = true}) ==
              tx::DropAction::None);

static_assert(tr::LocalDateTime{2026, 11, 23, 14, 5, 9} > tr::LocalDateTime{2026, 1, 1, 0, 0, 0});

// A fake trash as tests/ops may write one (no I/O).
class FakeTrash final : public tr::ITrash {
public:
    Result<tr::TrashDir> trash_dir_for(const Path& /*path*/) override { return dir_; }
    Result<tr::TrashedItem> trash(const Path& path) override {
        tr::TrashedItem item;
        item.dir = dir_;
        item.name = path.filename().string();
        item.original_path = path;
        item.deletion_date = tr::LocalDateTime{};
        return item;
    }
    Result<std::vector<tr::TrashDir>> trash_dirs() override {
        return std::vector<tr::TrashDir>{dir_};
    }
    Result<tr::TrashListing> list(std::stop_token stop) override {
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled);
        }
        return tr::TrashListing{};
    }
    Result<void> restore(const tr::TrashedItem& item, const Path& destination) override {
        if (destination.empty() || item.name.empty()) {
            return fail(ErrorCode::InvalidArgument);
        }
        return ok();
    }
    Result<void> forget(const tr::TrashedItem& /*item*/) override { return ok(); }
    [[nodiscard]] bool is_inside_trash(const Path& path) const override {
        return ops::is_same_or_inside(path, dir_.root);
    }

private:
    tr::TrashDir dir_{tr::TrashDirKind::Home, "/fixture/Trash", {}, 0};
};

// Trash helpers and service usage.
[[maybe_unused]] Result<void> trash_usage() {
    const std::string date = tr::format_deletion_date({2026, 11, 23, 14, 5, 9});
    const std::optional<tr::LocalDateTime> parsed = tr::parse_deletion_date(date);
    const std::string escaped = tr::escape_path("/home/o\xC4\x9Fuz/a b.txt");
    const std::optional<std::string> raw = tr::unescape_path(escaped);
    tr::TrashInfo info{raw.value_or(std::string{}), parsed};
    const std::string text = tr::format_trashinfo(info);
    Result<tr::TrashInfo> back = tr::parse_trashinfo(text);
    if (!back) {
        return Unexpected<Error>(back.error());
    }
    Result<std::string> rel = tr::info_path_for("/media/usb/docs/a.txt", "/media/usb");
    Result<Path> original = tr::original_path_from(rel.value_or("docs/a.txt"), "/media/usb");
    const Path shared = tr::shared_topdir_trash("/media/usb", 1000);
    const Path user = tr::user_topdir_trash("/media/usb", 1000);
    (void)original;
    (void)shared;
    (void)user;

    tr::TrashOptions options;
    options.home_trash = "/fixture/home/.local/share/Trash";
    options.uid = 1000U;
    options.volumes.sysfs_root = "/fixture/sys";
    options.now = [] { return tr::LocalDateTime{2026, 11, 23, 0, 0, 0}; };
    Result<std::shared_ptr<tr::ITrash>> created = tr::create_trash(options);
    if (!created) {
        return Unexpected<Error>(created.error());
    }
    tr::ITrash& trash = **created;
    Result<tr::TrashedItem> item = trash.trash("/fixture/home/a.txt");
    if (!item) {
        return Unexpected<Error>(item.error());
    }
    const Path files = item->files_path();
    const Path info_file = item->info_path();
    (void)files;
    (void)info_file;
    std::stop_source stop;
    Result<tr::TrashListing> listing = trash.list(stop.get_token());
    if (listing && !listing->items.empty()) {
        return trash.restore(listing->items.front(), listing->items.front().original_path);
    }
    return platform::move_to_trash("/fixture/home/b.txt");
}

// Clipboard and uri-list helpers.
[[maybe_unused]] std::optional<tx::FileClipboard> transfer_usage() {
    Result<std::string> uri = tx::file_uri("/home/o\xC4\x9Fuz/a b.jpg");
    Result<Path> path = tx::path_from_file_uri(uri.value_or("file:///tmp"));
    (void)path;
    const std::string list = tx::encode_uri_list(std::vector<std::string>{"file:///a"});
    Result<std::string> list2 = tx::encode_uri_list(std::vector<Path>{"/a", "/b"});
    (void)list2;
    const tx::UriList decoded = tx::decode_uri_list(list);
    tx::FileClipboard clip{tx::ClipboardAction::Cut, decoded.paths};
    Result<std::string> gnome = tx::encode_gnome_copied_files(clip);
    std::optional<tx::FileClipboard> again = tx::decode_gnome_copied_files(gnome.value_or(""));
    std::optional<tx::FileClipboard> naut =
        tx::decode_nautilus_text(std::string(tx::kNautilusTextHeader) + "\ncut\nfile:///a\n");
    (void)again;
    (void)naut;
    Result<std::vector<std::pair<std::string, std::string>>> offer = tx::make_clipboard_offer(clip);
    const tx::ClipboardGetter get = [&offer](std::string_view mime) -> std::optional<std::string> {
        if (offer) {
            for (const auto& [type, bytes] : *offer) {
                if (type == mime) {
                    return bytes;
                }
            }
        }
        return std::nullopt;
    };
    return tx::read_clipboard(get);
}

// Pure ops helpers.
[[maybe_unused]] void helper_usage() {
    const ops::NameParts parts = ops::split_extension("archive.tar.gz", false);
    const std::size_t stem = ops::stem_length(".bashrc", false);
    const std::string copy = ops::keep_both_name("photo (2).jpg", 3, false);
    const ops::NameError error = ops::validate_new_name("Yeni Klas\xC3\xB6r");
    const bool inside = ops::is_same_or_inside("/a/b", "/a");
    ops::ProtectedPaths protected_paths{
        "/home/u", {"/", "/media/usb"}, {"/home/u/.local/share/Trash"}};
    const ops::Protection protection = ops::protection_of("/home", protected_paths);
    (void)parts;
    (void)stem;
    (void)copy;
    (void)error;
    (void)inside;
    (void)protection;
}

// Blocking engine, as tests/ops drive it.
[[maybe_unused]] Result<ops::OpResult> engine_usage(std::shared_ptr<tr::ITrash> trash) {
    ops::EngineOptions options;
    options.trash = std::move(trash);
    options.protected_paths.home = "/fixture/home";
    options.fault_injector = [](const Path& p) { return p.filename() != "fail.bin"; };
    ops::OpRequest request;
    request.kind = ops::OpKind::Move;
    request.sources = {"/fixture/a", "/fixture/b"};
    request.destination = "/fixture/dest";
    request.options.dry_run = false;
    const ops::ConflictResolver resolve = [](const ops::Conflict& c) {
        ops::ConflictDecision d;
        d.action = c.merge_allowed ? ops::ConflictAction::Merge : ops::ConflictAction::Skip;
        d.apply_to_all = true;
        return d;
    };
    const ops::ProgressSink progress = [](const ops::OpProgress& p) {
        (void)p.bytes_per_second;
        (void)p.eta;
    };
    std::stop_source stop;
    return ops::run_operation(request, options, resolve, progress, stop.get_token());
}

// Queue + undo stack, as the ui drives them on the UI thread.
[[maybe_unused]] Result<void> queue_usage(ops::IOperationQueue& queue, ops::UndoStack& undo) {
    ops::OpRequest request;
    request.kind = ops::OpKind::DeletePermanently;
    request.sources = {"/fixture/x"};
    request.confirmed = true; // only after the confirmation sheet (S3)
    Result<ops::OpId> id = queue.submit(std::move(request));
    if (!id) {
        return Unexpected<Error>(id.error());
    }
    std::vector<ops::OpEvent> events;
    (void)queue.poll(events);
    for (const ops::OpEvent& event : events) {
        switch (event.type) {
        case ops::OpEvent::Type::ConflictRaised:
            if (event.conflict) {
                Result<void> r = queue.resolve(event.conflict->id, ops::ConflictDecision{});
                if (!r) {
                    return r;
                }
            }
            break;
        case ops::OpEvent::Type::Finished:
            if (event.result) {
                const std::optional<ops::UndoRecord>& record = event.result->record;
                if (record.has_value()) {
                    const ops::UndoId uid = undo.record_done(*record, event.result->origin);
                    (void)uid;
                }
            }
            break;
        case ops::OpEvent::Type::Started:
        case ops::OpEvent::Type::Progress:
        case ops::OpEvent::Type::ItemFinished: break;
        }
    }
    const std::vector<ops::OpProgress> running = queue.snapshot();
    const std::vector<ops::Conflict> pending = queue.pending_conflicts();
    (void)running;
    (void)pending;
    queue.pause(*id);
    queue.resume(*id);
    queue.cancel(*id);
    queue.set_protected_paths(ops::ProtectedPaths{});

    if (const ops::UndoRecord* top = undo.peek_undo()) {
        const ops::UndoLabel label = ops::make_label(*top);
        (void)label.key;
    }
    std::shared_ptr<const ops::UndoRecord> record = undo.take_undo();
    if (record) {
        ops::OpRequest inverse;
        inverse.kind = ops::OpKind::Undo;
        inverse.origin = ops::OpOrigin::Undo;
        inverse.record = record;
        Result<ops::OpId> uid = queue.submit(std::move(inverse));
        if (!uid) {
            undo.restore(record, ops::OpOrigin::Undo);
        }
    }
    (void)undo.can_redo();
    return ok();
}

[[maybe_unused]] Result<std::unique_ptr<ops::IOperationQueue>> make_queue() {
    ops::QueueOptions options;
    options.max_concurrent = 2;
    options.on_event = [] {};
    return ops::create_operation_queue(std::move(options));
}

} // namespace navigator::contracts_check
