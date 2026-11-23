// src/ui/i18n/i18n.cpp — built-in English and Turkish string tables (see navigator/i18n.hpp and
// i18n/catalog.hpp).
//
// Adding a string: append a row to kStrings. An empty Turkish column falls back to English.
// Placeholders are "{name}" and are filled by ui::substitute(); plural forms are "<base>.one" and
// "<base>.other" (see catalog.hpp).
// Thread-safety: the language is an atomic; tables are immutable; the key index is built once
// (thread-safe static initialization).
#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <string>
#include <unordered_map>

#include <navigator/i18n.hpp>

#include "i18n/catalog.hpp"

namespace navigator {
namespace {

struct Entry {
    std::string_view key;
    std::string_view en;
    std::string_view tr;
};

// A plain array: CTAD of std::array over this many entries exceeds clang's fold limit.
constexpr Entry kStrings[] = {
    Entry{"app.title", "Navigator", "Navigator"},
    Entry{"app.tagline", "A fast, keyboard-first file manager for Linux",
          "Linux için hızlı, klavye odaklı dosya yöneticisi"},
    Entry{"fps.label", "FPS", "FPS"},
    Entry{"fps.frame_time", "Frame", "Kare süresi"},
    Entry{"fps.unit_ms", "ms", "ms"},
    Entry{"fps.driver", "Renderer", "Çizici"},
    Entry{"fps.renderer.software", "Software", "Yazılım"},
    Entry{"hint.quit", "Quit", "Çıkış"},
    Entry{"hint.theme", "Switch theme", "Temayı değiştir"},
    Entry{"hint.fps", "Performance overlay", "Performans göstergesi"},
    Entry{"hint.go_up", "Parent folder", "Üst klasör"},
    Entry{"hint.show_hidden", "Show hidden items", "Gizli öğeleri göster"},

    // Commands (CommandInfo::title_key = "cmd." + name).
    Entry{"cmd.app.quit", "Quit Navigator", "Navigator'dan çık"},
    Entry{"cmd.view.toggle_fps_overlay", "Toggle performance overlay",
          "Performans göstergesini aç/kapat"},
    Entry{"cmd.view.toggle_theme", "Toggle light/dark theme", "Açık/koyu temayı değiştir"},
    Entry{"cmd.view.toggle_hidden", "Show/hide hidden items", "Gizli öğeleri göster/gizle"},
    Entry{"cmd.nav.back", "Go back", "Geri git"},
    Entry{"cmd.nav.forward", "Go forward", "İleri git"},
    Entry{"cmd.nav.up", "Go to parent folder", "Üst klasöre git"},
    Entry{"cmd.nav.refresh", "Refresh", "Yenile"},
    Entry{"cmd.nav.focus_address_bar", "Focus address bar", "Adres çubuğuna odaklan"},
    Entry{"cmd.list.activate", "Open", "Aç"},
    Entry{"cmd.list.select_all", "Select all", "Tümünü seç"},
    Entry{"cmd.list.clear_selection", "Clear selection", "Seçimi temizle"},
    Entry{"cmd.list.cursor_up", "Move cursor up", "İmleci yukarı taşı"},
    Entry{"cmd.list.cursor_down", "Move cursor down", "İmleci aşağı taşı"},
    Entry{"cmd.list.cursor_page_up", "Move cursor one page up", "İmleci bir sayfa yukarı taşı"},
    Entry{"cmd.list.cursor_page_down", "Move cursor one page down", "İmleci bir sayfa aşağı taşı"},
    Entry{"cmd.list.cursor_home", "Move cursor to the first item", "İmleci ilk öğeye taşı"},
    Entry{"cmd.list.cursor_end", "Move cursor to the last item", "İmleci son öğeye taşı"},
    // Phase 3 commands.
    Entry{"cmd.list.open_in_new_tab", "Open in new tab", "Yeni sekmede aç"},
    Entry{"cmd.tab.new", "New tab", "Yeni sekme"},
    Entry{"cmd.tab.close", "Close tab", "Sekmeyi kapat"},
    Entry{"cmd.tab.duplicate", "Duplicate tab", "Sekmeyi çoğalt"},
    Entry{"cmd.tab.close_left", "Close tabs to the left", "Soldaki sekmeleri kapat"},
    Entry{"cmd.tab.reopen_closed", "Reopen closed tab", "Kapatılan sekmeyi yeniden aç"},
    Entry{"cmd.tab.next", "Next tab", "Sonraki sekme"},
    Entry{"cmd.tab.previous", "Previous tab", "Önceki sekme"},
    Entry{"cmd.tab.search", "Search tabs", "Sekmelerde ara"},
    Entry{"cmd.tab.move_left", "Move tab left", "Sekmeyi sola taşı"},
    Entry{"cmd.tab.move_right", "Move tab right", "Sekmeyi sağa taşı"},
    Entry{"cmd.pane.split_right", "Split pane right", "Paneli sağa böl"},
    Entry{"cmd.pane.split_down", "Split pane down", "Paneli aşağı böl"},
    Entry{"cmd.pane.focus_next", "Focus next pane", "Sonraki panele geç"},
    Entry{"cmd.pane.focus_previous", "Focus previous pane", "Önceki panele geç"},
    Entry{"cmd.pane.close", "Close pane", "Paneli kapat"},
    Entry{"cmd.layout.save", "Save layout…", "Yerleşimi kaydet…"},
    Entry{"cmd.layout.load", "Load layout…", "Yerleşim yükle…"},
    Entry{"cmd.layout.delete", "Delete layout…", "Yerleşim sil…"},
    Entry{"cmd.window.new", "Duplicate tab in new window", "Sekmeyi yeni pencerede çoğalt"},
    // Phase 4 commands.
    Entry{"cmd.view.toggle_sidebar", "Show/hide sidebar", "Kenar çubuğunu göster/gizle"},
    Entry{"cmd.view.toggle_view_lock", "Lock view settings", "Görünüm ayarlarını kilitle"},
    Entry{"cmd.nav.goto", "Go to folder…", "Klasöre git…"},
    Entry{"cmd.sidebar.focus", "Focus sidebar", "Kenar çubuğuna odaklan"},
    Entry{"cmd.bookmarks.toggle", "Bookmark this folder", "Bu klasörü yer imlerine ekle"},
    Entry{"cmd.bookmarks.rename", "Rename bookmark", "Yer imini yeniden adlandır"},
    Entry{"cmd.bookmarks.move_up", "Move bookmark up", "Yer imini yukarı taşı"},
    Entry{"cmd.bookmarks.move_down", "Move bookmark down", "Yer imini aşağı taşı"},
    Entry{"cmd.recents.clear", "Clear recent folders", "Son kullanılanları temizle"},
    Entry{"cmd.recents.toggle_tracking", "Track recent folders", "Son kullanılanları izle"},
    Entry{"cmd.edit.copy_path", "Copy path", "Yolu kopyala"},

    // Modifier names for key hints (key legends themselves are not translated).
    Entry{"key.ctrl", "Ctrl", "Ctrl"},
    Entry{"key.shift", "Shift", "Shift"},
    Entry{"key.alt", "Alt", "Alt"},
    Entry{"key.super", "Super", "Super"},

    // ErrorCode texts (to_string(ErrorCode) suffix), shown under a notice title.
    Entry{"error.no_font", "No usable UI font was found.",
          "Kullanılabilir arayüz yazı tipi bulunamadı."},
    Entry{"error.unknown", "Something went wrong.", "Bilinmeyen bir hata oluştu."},
    Entry{"error.invalid_argument", "It is not a folder or not a valid path.",
          "Bir klasör ya da geçerli bir yol değil."},
    Entry{"error.not_found", "No such file or folder.", "Böyle bir dosya ya da klasör yok."},
    Entry{"error.permission_denied", "You don’t have permission to access it.",
          "Buna erişim izniniz yok."},
    Entry{"error.already_exists", "It already exists.", "Zaten var."},
    Entry{"error.io_error", "A read or write error occurred.",
          "Bir okuma ya da yazma hatası oluştu."},
    Entry{"error.cancelled", "The operation was cancelled.", "İşlem iptal edildi."},
    Entry{"error.unsupported", "No program is available to do this.",
          "Bunu yapabilecek bir program yok."},
    Entry{"error.out_of_memory", "There is not enough memory.", "Yeterli bellek yok."},
    Entry{"error.platform_error", "The system reported an error.", "Sistem bir hata bildirdi."},

    // Notices.
    Entry{"notice.open_folder_failed", "Couldn’t open “{name}”", "“{name}” açılamadı"},
    Entry{"notice.open_file_failed", "Couldn’t open “{name}” with its default app",
          "“{name}” varsayılan uygulamasıyla açılamadı"},
    Entry{"notice.refresh_failed", "Couldn’t refresh “{name}”", "“{name}” yenilenemedi"},
    Entry{"notice.folder_gone", "This folder was moved or deleted",
          "Bu klasör taşındı ya da silindi"},
    Entry{"notice.folder_gone_detail", "The list shows its last known contents.",
          "Liste, bilinen son içeriğini gösteriyor."},
    Entry{"notice.dismiss", "Dismiss", "Kapat"},

    // Details view.
    Entry{"column.name", "Name", "Ad"},
    Entry{"column.size", "Size", "Boyut"},
    Entry{"column.type", "Type", "Tür"},
    Entry{"column.modified", "Modified", "Değiştirilme"},
    Entry{"empty.folder", "This folder is empty", "Bu klasör boş"},
    Entry{"empty.hidden_only.one", "This folder has only {n} hidden item", ""},
    Entry{"empty.hidden_only.other", "This folder has only {n} hidden items",
          "Bu klasörde yalnızca {n} gizli öğe var"},
    Entry{"empty.unreadable", "This folder can’t be shown", "Bu klasör gösterilemiyor"},

    // Status bar.
    Entry{"status.items.one", "{n} item", ""},
    Entry{"status.items.other", "{n} items", "{n} öğe"},
    Entry{"status.hidden.one", "{n} hidden", ""},
    Entry{"status.hidden.other", "{n} hidden", "{n} gizli"},
    Entry{"status.selected.one", "{n} selected", ""},
    Entry{"status.selected.other", "{n} selected", "{n} seçili"},
    Entry{"status.loading", "Loading…", "Yükleniyor…"},

    // Phase 5: commands.
    Entry{"cmd.list.filter", "Filter this folder", "Bu klasörü süz"},
    Entry{"cmd.list.clear_filter", "Clear filter", "Süzgeci temizle"},
    Entry{"cmd.search.flatten", "Flatten subfolders", "Alt klasörleri düzleştir"},
    Entry{"cmd.search.flatten_volume", "Search the whole drive", "Tüm sürücüde ara"},
    Entry{"cmd.search.types", "Show or hide file types", "Dosya türlerini göster/gizle"},
    Entry{"cmd.search.toggle_fuzzy", "Toggle fuzzy matching", "Bulanık eşleştirmeyi aç/kapat"},
    Entry{"cmd.search.cancel", "Stop searching", "Aramayı durdur"},
    Entry{"cmd.search.rebuild_index", "Rebuild the file-name index",
          "Dosya adı indeksini yeniden oluştur"},

    // Phase 6: Inspector commands.
    Entry{"cmd.view.inspect", "Show or hide the Inspector", "Önizleme panelini göster/gizle"},
    Entry{"cmd.inspector.toggle_lock", "Lock the Inspector on this item",
          "Önizlemeyi bu öğeye kilitle"},
    Entry{"cmd.inspector.zoom_fit", "Fit image to the Inspector", "Görüntüyü panele sığdır"},
    Entry{"cmd.inspector.zoom_1to1", "Show image at actual size (1:1)",
          "Görüntüyü gerçek boyutunda göster (1:1)"},
    Entry{"cmd.inspector.play", "Play with the default app", "Varsayılan uygulamayla oynat"},
    Entry{"cmd.inspector.cycle_placement", "Move the Inspector (auto, right, bottom)",
          "Önizleme panelini taşı (otomatik, sağ, alt)"},
    Entry{"cmd.inspector.toggle_grid", "Show or hide the pixel grid",
          "Piksel ızgarasını göster/gizle"},
    Entry{"cmd.inspector.toggle_minimap", "Show or hide the minimap", "Mini haritayı göster/gizle"},

    // Phase 6: Inspector states and header.
    Entry{"inspector.title", "Inspector", "Önizleme"},
    Entry{"inspector.tip.lock", "Lock to this item", "Bu öğeye kilitle"},
    Entry{"inspector.tip.unlock", "Unlock (follow the cursor again)",
          "Kilidi aç (imleci yeniden izle)"},
    Entry{"inspector.tip.placement", "Move (auto, right, bottom)", "Taşı (otomatik, sağ, alt)"},
    Entry{"inspector.tip.play", "Open in the default player", "Varsayılan oynatıcıda aç"},
    Entry{"inspector.page_preview", "Page preview", "Sayfa önizlemesi"},
    Entry{"inspector.lines.one", "{n} line", ""},
    Entry{"inspector.lines.other", "{n} lines", "{n} satır"},
    Entry{"type.video_named", "{name} video", "{name} videosu"},
    Entry{"inspector.empty", "Nothing to preview", "Önizlenecek bir şey yok"},
    Entry{"inspector.empty_detail", "Move the cursor to a file to see it here",
          "Burada görmek için imleci bir dosyaya getirin"},
    Entry{"inspector.loading", "Loading preview…", "Önizleme yükleniyor…"},
    Entry{"inspector.unsupported", "No preview available", "Önizleme yok"},
    Entry{"inspector.unsupported_detail", "Navigator can’t show this kind of file yet",
          "Navigator bu tür dosyaları henüz gösteremiyor"},
    Entry{"inspector.coming_soon", "Preview coming soon", "Önizleme yakında"},
    Entry{"inspector.coming_soon_pdf", "PDF previews arrive in a later version",
          "PDF önizlemesi sonraki bir sürümde gelecek"},
    Entry{"inspector.coming_soon_detail", "This format gets a preview in a later version",
          "Bu biçim sonraki bir sürümde önizleme alacak"},
    Entry{"inspector.binary", "Binary file", "İkili dosya"},
    Entry{"inspector.binary_detail", "This file doesn’t contain readable text",
          "Bu dosya okunabilir metin içermiyor"},
    Entry{"inspector.error", "Can’t preview this file", "Bu dosya önizlenemiyor"},
    Entry{"inspector.error.permission", "You don’t have permission to read it",
          "Okuma izniniz yok"},
    Entry{"inspector.error.not_found", "The file no longer exists", "Dosya artık yok"},
    Entry{"inspector.error.too_large", "It is too large to preview", "Önizlemek için çok büyük"},
    Entry{"inspector.error.damaged", "The file appears to be damaged", "Dosya hasarlı görünüyor"},
    Entry{"inspector.error.generic", "Something went wrong while reading it",
          "Okunurken bir sorun oluştu"},
    Entry{"inspector.play", "Play", "Oynat"},
    Entry{"inspector.play_failed", "Couldn’t open it with the default app",
          "Varsayılan uygulamayla açılamadı"},
    Entry{"inspector.no_poster", "No preview available", "Önizleme yok"},
    Entry{"inspector.video_no_metadata", "Video details unavailable", "Video ayrıntıları yok"},
    Entry{"inspector.text_truncated", "Showing the first {size}", "İlk {size} gösteriliyor"},
    Entry{"inspector.folder_empty", "This folder is empty", "Bu klasör boş"},
    Entry{"inspector.folder_only_hidden", "Only hidden items", "Yalnızca gizli öğeler"},
    Entry{"inspector.copied", "Copied", "Kopyalandı"},
    Entry{"inspector.unlocked_gone",
          "The locked item is gone; the Inspector follows the cursor again",
          "Kilitli öğe artık yok; önizleme yeniden imleci izliyor"},
    Entry{"inspector.selected.one", "{n} item selected", ""},
    Entry{"inspector.selected.other", "{n} items selected", "{n} öğe seçili"},
    Entry{"inspector.items.one", "{n} item", ""},
    Entry{"inspector.items.other", "{n} items", "{n} öğe"},
    Entry{"inspector.folders.one", "{n} folder", ""},
    Entry{"inspector.folders.other", "{n} folders", "{n} klasör"},
    Entry{"inspector.files.one", "{n} file", ""},
    Entry{"inspector.files.other", "{n} files", "{n} dosya"},
    Entry{"inspector.hidden.one", "{n} hidden", ""},
    Entry{"inspector.hidden.other", "{n} hidden", "{n} gizli"},
    Entry{"inspector.more.one", "and {n} more item", ""},
    Entry{"inspector.more.other", "and {n} more items", "ve {n} öğe daha"},

    // Phase 6: metadata (design review "EXIF and metadata").
    Entry{"meta.group.file", "File", "Dosya"},
    Entry{"meta.group.image", "Image", "Görüntü"},
    Entry{"meta.group.camera", "Camera", "Fotoğraf makinesi"},
    Entry{"meta.group.location", "Location", "Konum"},
    Entry{"meta.group.video", "Video", "Video"},
    Entry{"meta.group.audio", "Audio", "Ses"},
    Entry{"meta.group.text", "Text", "Metin"},
    Entry{"meta.group.contents", "Contents", "İçerik"},
    Entry{"meta.group.selection", "Selection", "Seçim"},
    Entry{"meta.kind", "Kind", "Tür"},
    Entry{"meta.size", "Size", "Boyut"},
    Entry{"meta.bytes", "{n} bytes", "{n} bayt"},
    Entry{"meta.modified", "Modified", "Değiştirilme"},
    Entry{"meta.permissions", "Permissions", "İzinler"},
    Entry{"meta.link", "Symbolic link", "Sembolik bağ"},
    Entry{"meta.link_yes", "Yes", "Evet"},
    Entry{"meta.link_broken", "Broken", "Kırık"},
    Entry{"meta.dimensions", "Dimensions", "Boyutlar"},
    Entry{"meta.format", "Format", "Biçim"},
    Entry{"meta.bit_depth", "Bit depth", "Bit derinliği"},
    Entry{"meta.bits_per_channel", "{n} bits per channel", "Kanal başına {n} bit"},
    Entry{"meta.transparency", "Transparency", "Saydamlık"},
    Entry{"meta.animated", "Animated", "Hareketli"},
    Entry{"meta.frames.one", "{n} frame", ""},
    Entry{"meta.frames.other", "{n} frames", "{n} kare"},
    Entry{"meta.orientation", "Orientation", "Yön"},
    Entry{"meta.orientation.rotate90", "Rotated 90° clockwise", "Saat yönünde 90° döndürülmüş"},
    Entry{"meta.orientation.rotate180", "Rotated 180°", "180° döndürülmüş"},
    Entry{"meta.orientation.rotate270", "Rotated 90° counter-clockwise",
          "Saat yönünün tersine 90° döndürülmüş"},
    Entry{"meta.orientation.mirrored", "Mirrored", "Aynalanmış"},
    Entry{"meta.color_profile", "Colour profile", "Renk profili"},
    Entry{"meta.embedded", "Embedded", "Gömülü"},
    Entry{"meta.camera", "Model", "Model"},
    Entry{"meta.lens", "Lens", "Objektif"},
    Entry{"meta.taken", "Taken", "Çekim"},
    Entry{"meta.exposure", "Exposure", "Pozlama"},
    Entry{"meta.aperture", "Aperture", "Diyafram"},
    Entry{"meta.iso", "ISO", "ISO"},
    Entry{"meta.focal_length", "Focal length", "Odak uzaklığı"},
    Entry{"meta.equivalent_35mm", "{n} mm equiv.", "{n} mm eşdeğeri"},
    Entry{"meta.flash", "Flash", "Flaş"},
    Entry{"meta.flash_fired", "Fired", "Patladı"},
    Entry{"meta.flash_off", "Did not fire", "Patlamadı"},
    Entry{"meta.software", "Software", "Yazılım"},
    Entry{"meta.coordinates", "Coordinates", "Koordinatlar"},
    Entry{"meta.gps_present", "Location data present", "Konum verisi var"},
    Entry{"meta.altitude", "Altitude", "Yükseklik"},
    Entry{"meta.north", "N", "K"},
    Entry{"meta.south", "S", "G"},
    Entry{"meta.east", "E", "D"},
    Entry{"meta.west", "W", "B"},
    Entry{"meta.duration", "Duration", "Süre"},
    Entry{"meta.resolution", "Resolution", "Çözünürlük"},
    Entry{"meta.frame_rate", "Frame rate", "Kare hızı"},
    Entry{"meta.video_codec", "Video codec", "Video kodeki"},
    Entry{"meta.container", "Container", "Kapsayıcı"},
    Entry{"meta.bit_rate", "Bit rate", "Bit hızı"},
    Entry{"meta.rotation", "Rotation", "Döndürme"},
    Entry{"meta.subtitles", "Subtitle tracks", "Altyazı izleri"},
    Entry{"meta.audio_codec", "Audio codec", "Ses kodeki"},
    Entry{"meta.audio_channels", "Channels", "Kanallar"},
    Entry{"meta.mono", "Mono", "Mono"},
    Entry{"meta.stereo", "Stereo", "Stereo"},
    Entry{"meta.channels.one", "{n} channel", ""},
    Entry{"meta.channels.other", "{n} channels", "{n} kanal"},
    Entry{"meta.sample_rate", "Sample rate", "Örnekleme hızı"},
    Entry{"meta.audio_tracks", "Audio tracks", "Ses izleri"},
    Entry{"meta.encoding", "Encoding", "Kodlama"},
    Entry{"meta.lines", "Lines", "Satırlar"},
    Entry{"meta.folders", "Folders", "Klasörler"},
    Entry{"meta.files", "Files", "Dosyalar"},
    Entry{"meta.hidden_items", "Hidden items", "Gizli öğeler"},
    Entry{"meta.items", "Items", "Öğeler"},
    Entry{"meta.total_size", "Total size", "Toplam boyut"},
    Entry{"meta.yes", "Yes", "Evet"},
    Entry{"meta.no", "No", "Hayır"},

    // Phase 5: filter. Turkish counts avoid a suffix on the number (its vowel harmony varies).
    Entry{"filter.placeholder", "Filter", "Filtrele"},
    Entry{"filter.placeholder_search", "Search names", "Adlarda ara"},
    Entry{"filter.scope_subfolders", "Subfolders", "Alt klasörler"},
    Entry{"filter.scope_drive", "Drive", "Sürücü"},
    Entry{"filter.fuzzy_on", "Fuzzy matching: on", "Bulanık eşleştirme: açık"},
    Entry{"filter.fuzzy_off", "Fuzzy matching: off", "Bulanık eşleştirme: kapalı"},
    Entry{"filter.count", "{shown} of {total}", "{shown} / {total}"},
    Entry{"filter.filtering", "Filtering…", "Süzülüyor…"},
    Entry{"filter.no_matches", "No items match “{query}”", "“{query}” ile eşleşen öğe yok"},
    Entry{"filter.no_matches_types", "Some file types are hidden", "Bazı dosya türleri gizli"},
    Entry{"filter.all_types_hidden", "Every type here is hidden", "Buradaki tüm türler gizli"},
    Entry{"hint.clear_filter", "Clear filter", "Süzgeci temizle"},
    Entry{"hint.show_types", "Choose types", "Türleri seç"},
    Entry{"hint.leave_flatten", "Back to the folder", "Klasöre dön"},
    Entry{"column.folder", "Folder", "Klasör"},

    // Phase 5: flattened view and status bar.
    Entry{"flatten.searching", "Searching subfolders…", "Alt klasörler taranıyor…"},
    Entry{"flatten.querying", "Searching the index…", "İndekste aranıyor…"},
    Entry{"flatten.empty", "There is nothing below this folder", "Bu klasörün altında öğe yok"},
    Entry{"flatten.unreadable", "This folder can’t be searched", "Bu klasörde arama yapılamıyor"},
    Entry{"status.filtered", "{shown} of {total}", "{total} öğede {shown} sonuç"},
    Entry{"status.types_hidden.one", "{n} type hidden", ""},
    Entry{"status.types_hidden.other", "{n} types hidden", "{n} tür gizli"},
    Entry{"status.found.one", "{n} item", ""},
    Entry{"status.found.other", "{n} items", "{n} öğe"},
    Entry{"status.folders.one", "{n} folder read", ""},
    Entry{"status.folders.other", "{n} folders read", "{n} klasör tarandı"},
    Entry{"status.skipped.one", "{n} skipped", ""},
    Entry{"status.skipped.other", "{n} skipped", "{n} atlandı"},
    Entry{"status.index_matches.one", "{n} match", ""},
    Entry{"status.index_matches.other", "{n} matches", "{n} eşleşme"},
    Entry{"status.searching", "Searching…", "Aranıyor…"},
    Entry{"status.listing_drive", "Listing drive…", "Sürücü listeleniyor…"},
    Entry{"status.hidden_files.one", "{n} hidden file", ""},
    Entry{"status.hidden_files.other", "{n} hidden files", "{n} gizli dosya"},
    Entry{"status.querying", "Querying…", "Sorgulanıyor…"},
    Entry{"status.search_stopped", "Stopped", "Durduruldu"},
    Entry{"status.search_limit", "Result limit reached", "Sonuç sınırına ulaşıldı"},
    Entry{"status.search_error", "Search failed", "Arama başarısız oldu"},
    Entry{"status.stop", "Stop", "Durdur"},

    // Phase 5: types popup.
    Entry{"types.title", "File types", "Dosya türleri"},
    Entry{"types.all", "All types", "Tüm türler"},
    Entry{"types.folders", "Folders", "Klasörler"},
    Entry{"types.no_extension", "No extension", "Uzantısız"},
    Entry{"types.none", "No items to filter", "Süzülecek öğe yok"},
    Entry{"types.hidden_items.one", "{n} item hidden", ""},
    Entry{"types.hidden_items.other", "{n} items hidden", "{n} öğe gizli"},

    // Phase 5: name index.
    Entry{"index.idle", "Index waiting", "İndeks bekliyor"},
    Entry{"index.loading", "Loading index…", "İndeks yükleniyor…"},
    Entry{"index.scanning", "Indexing… {items}", "İndeksleniyor… {items}"},
    Entry{"index.scanning_pct", "Indexing {pct}%", "İndeksleniyor %{pct}"},
    Entry{"index.verifying", "Index ready, checking changes",
          "İndeks hazır, değişiklikler denetleniyor"},
    Entry{"index.ready", "Index ready ({items})", "İndeks hazır ({items})"},
    Entry{"index.ready_truncated", "Index full ({items})", "İndeks dolu ({items})"},
    Entry{"index.failed", "Index unavailable", "İndeks kullanılamıyor"},
    Entry{"index.stopped", "Index stopped", "İndeks durduruldu"},
    Entry{"index.items.one", "{n} item", ""},
    Entry{"index.items.other", "{n} items", "{n} öğe"},
    Entry{"index.disabled", "Indexing is turned off", "İndeksleme kapalı"},
    Entry{"index.not_indexed", "This drive is not indexed", "Bu sürücü indekslenmiyor"},
    Entry{"index.rebuilding", "Rebuilding the index…", "İndeks yeniden oluşturuluyor…"},

    // Toolbar and path.
    Entry{"tooltip.back", "Back", "Geri"},
    Entry{"tooltip.forward", "Forward", "İleri"},
    Entry{"tooltip.up", "Parent folder", "Üst klasör"},
    Entry{"tooltip.theme", "Switch theme", "Temayı değiştir"},
    Entry{"path.home", "Home", "Ev"},
    Entry{"path.root", "File System", "Dosya Sistemi"},

    // Tabs, panes and drag and drop (Phase 3).
    Entry{"drop.move_here", "Move to this pane", "Bu panele taşı"},
    Entry{"drop.split_left", "Split left", "Sola böl"},
    Entry{"drop.split_right", "Split right", "Sağa böl"},
    Entry{"drop.split_up", "Split up", "Yukarı böl"},
    Entry{"drop.split_down", "Split down", "Aşağı böl"},
    Entry{"menu.new_tab", "New tab", "Yeni sekme"},
    Entry{"menu.duplicate_tab", "Duplicate", "Çoğalt"},
    Entry{"menu.close_tab", "Close", "Kapat"},
    Entry{"menu.close_left_tabs", "Close tabs to the left", "Soldaki sekmeleri kapat"},
    Entry{"menu.reopen_closed_tab", "Reopen closed tab", "Kapatılan sekmeyi yeniden aç"},

    // Search Tabs popup.
    Entry{"search_tabs.placeholder", "Search open and recently closed tabs",
          "Açık ve son kapatılan sekmelerde ara"},
    Entry{"search_tabs.no_match", "No matching tabs", "Eşleşen sekme yok"},
    Entry{"search_tabs.open", "Open tabs", "Açık sekmeler"},
    Entry{"search_tabs.closed", "Recently closed", "Son kapatılanlar"},
    Entry{"search_tabs.closed_chip", "Closed", "Kapalı"},
    Entry{"search_tabs.current", "Current", "Şu anki"},
    Entry{"hint.move", "Move", "Gezin"},
    Entry{"hint.switch", "Switch", "Geç"},
    Entry{"hint.close", "Close", "Kapat"},
    Entry{"hint.load", "Load", "Yükle"},
    Entry{"hint.delete", "Delete", "Sil"},

    // Named layouts.
    Entry{"layout.save_title", "Save layout", "Yerleşimi kaydet"},
    Entry{"layout.name_placeholder", "Layout name", "Yerleşim adı"},
    Entry{"layout.save_hint", "Saves the panes, their tabs and sizes.",
          "Panelleri, sekmelerini ve boyutlarını kaydeder."},
    Entry{"layout.name_required", "Type a name for the layout.", "Yerleşim için bir ad yazın."},
    Entry{"layout.name_invalid",
          "Names can’t contain “/”, start with a dot or start or end with a space.",
          "Ad “/” içeremez, nokta ile başlayamaz, boşlukla başlayıp bitemez."},
    Entry{"layout.replace_confirm", "“{name}” already exists. Save again to replace it.",
          "“{name}” zaten var. Değiştirmek için yeniden kaydedin."},
    Entry{"layout.save", "Save", "Kaydet"},
    Entry{"layout.replace", "Replace", "Değiştir"},
    Entry{"layout.cancel", "Cancel", "Vazgeç"},
    Entry{"layout.filter_placeholder", "Filter saved layouts", "Kayıtlı yerleşimleri süz"},
    Entry{"layout.load_title", "Load a saved layout", "Kayıtlı bir yerleşim yükle"},
    Entry{"layout.delete_title", "Delete a saved layout", "Kayıtlı bir yerleşimi sil"},
    Entry{"layout.none", "No saved layouts yet", "Henüz kayıtlı yerleşim yok"},
    Entry{"layout.none_hint", "Save the current one", "Şu ankini kaydedin"},
    Entry{"layout.no_match", "No matching layouts", "Eşleşen yerleşim yok"},
    Entry{"layout.delete_confirm", "Delete? Press Enter or Del",
          "Silinsin mi? Enter ya da Del’e basın"},
    Entry{"layout.saved", "Saved layout “{name}”", "“{name}” yerleşimi kaydedildi"},
    Entry{"layout.loaded", "Loaded layout “{name}”", "“{name}” yerleşimi yüklendi"},
    Entry{"layout.deleted", "Deleted layout “{name}”", "“{name}” yerleşimi silindi"},
    Entry{"layout.save_failed", "Couldn’t save layout “{name}”.",
          "“{name}” yerleşimi kaydedilemedi."},
    Entry{"layout.load_failed", "Couldn’t load layout “{name}”.",
          "“{name}” yerleşimi yüklenemedi."},
    Entry{"layout.delete_failed", "Couldn’t delete layout “{name}”.",
          "“{name}” yerleşimi silinemedi."},

    // Places (labelled by kind in the UI language, places::label_key).
    Entry{"place.home", "Home", "Ev"},
    Entry{"place.desktop", "Desktop", "Masaüstü"},
    Entry{"place.documents", "Documents", "Belgeler"},
    Entry{"place.downloads", "Downloads", "İndirilenler"},
    Entry{"place.music", "Music", "Müzik"},
    Entry{"place.pictures", "Pictures", "Resimler"},
    Entry{"place.videos", "Videos", "Videolar"},
    Entry{"place.trash", "Trash", "Çöp"},
    Entry{"place.file_system", "File System", "Dosya Sistemi"},

    // Sidebar (Phase 4).
    Entry{"sidebar.bookmarks", "Bookmarks", "Yer imleri"},
    Entry{"sidebar.recents", "Recents", "Son kullanılanlar"},
    Entry{"sidebar.places", "Places", "Yerler"},
    Entry{"sidebar.storage", "Storage", "Depolama"},
    Entry{"sidebar.bookmarks_empty", "No bookmarks yet", "Henüz yer imi yok"},
    Entry{"sidebar.bookmarks_hint", "{key} bookmarks this folder", "{key} ile yer imlerine ekle"},
    Entry{"sidebar.recents_empty", "No recent folders yet", "Henüz son kullanılan yok"},
    Entry{"sidebar.recents_hint", "Folders you open show up here",
          "Açtığınız klasörler burada görünür"},
    Entry{"sidebar.recents_off", "Recents tracking is off", "Son kullanılanlar izlenmiyor"},
    Entry{"sidebar.recents_off_hint", "Turn it on from the context menu",
          "Sağ tık menüsünden açabilirsiniz"},
    Entry{"sidebar.clear", "Clear", "Temizle"},
    Entry{"sidebar.paused", "paused", "duraklatıldı"},
    Entry{"sidebar.badge_offline", "offline", "çevrimdışı"},
    Entry{"sidebar.badge_missing", "missing", "kayıp"},
    Entry{"sidebar.free", "{n} free", "{n} boş"},
    Entry{"sidebar.free_of", "{free} free of {total}", "{total} içinde {free} boş"},
    Entry{"sidebar.network_label", "{share} on {host}", "{host} üzerinde {share}"},
    Entry{"sidebar.rename_placeholder", "Bookmark name", "Yer imi adı"},
    Entry{"sidebar.offline_notice", "“{name}” is on a volume that isn’t mounted",
          "“{name}” bağlı olmayan bir birimde"},
    Entry{"sidebar.missing_notice", "“{name}” was moved or deleted",
          "“{name}” taşınmış ya da silinmiş"},
    Entry{"bookmarks.added", "Bookmarked “{name}”", "“{name}” yer imlerine eklendi"},
    Entry{"bookmarks.removed", "Removed bookmark “{name}”", "“{name}” yer imlerinden çıkarıldı"},
    Entry{"bookmarks.exists", "“{name}” is already bookmarked", "“{name}” zaten yer imlerinde"},
    Entry{"bookmarks.add_failed", "Couldn’t bookmark “{name}”", "“{name}” yer imlerine eklenemedi"},
    Entry{"bookmarks.rename_failed", "That name can’t be used for a bookmark.",
          "Bu ad bir yer imi için kullanılamaz."},
    Entry{"recents.tracking_on", "Recent folders are tracked again",
          "Son kullanılan klasörler yeniden izleniyor"},
    Entry{"recents.tracking_off", "Recent folders are no longer tracked",
          "Son kullanılan klasörler artık izlenmiyor"},
    Entry{"menu.open", "Open", "Aç"},
    Entry{"menu.open_in_new_tab", "Open in new tab", "Yeni sekmede aç"},
    Entry{"menu.copy_path", "Copy path", "Yolu kopyala"},
    Entry{"menu.rename", "Rename…", "Yeniden adlandır…"},
    Entry{"menu.move_up", "Move up", "Yukarı taşı"},
    Entry{"menu.move_down", "Move down", "Aşağı taşı"},
    Entry{"menu.remove_bookmark", "Remove bookmark", "Yer imini kaldır"},
    Entry{"menu.clear_recents", "Clear recents", "Son kullanılanları temizle"},
    Entry{"menu.stop_tracking", "Stop tracking recents", "Son kullanılanları izlemeyi durdur"},
    Entry{"menu.resume_tracking", "Resume tracking recents", "Son kullanılanları yeniden izle"},
    Entry{"menu.bookmark_folder", "Bookmark the current folder",
          "Geçerli klasörü yer imlerine ekle"},
    Entry{"copy.copied_one", "Copied “{path}”", "“{path}” kopyalandı"},
    Entry{"copy.copied.one", "Copied {n} path", ""},
    Entry{"copy.copied.other", "Copied {n} paths", "{n} yol kopyalandı"},

    // GoTo and the address bar (Phase 4).
    Entry{"goto.placeholder", "Type a path, or search recents and bookmarks",
          "Bir yol yazın ya da son kullanılanlarda ve yer imlerinde arayın"},
    Entry{"goto.go_to", "Go to", "Git"},
    Entry{"goto.section_matches", "Folders", "Klasörler"},
    Entry{"goto.section_recents", "Recents", "Son kullanılanlar"},
    Entry{"goto.section_places", "Places & bookmarks", "Yerler ve yer imleri"},
    Entry{"goto.empty", "Nothing here yet", "Henüz bir şey yok"},
    Entry{"goto.not_found", "“{path}” doesn’t exist", "“{path}” yok"},
    Entry{"goto.unknown_variable", "Unknown variable {name}", "Bilinmeyen değişken {name}"},
    Entry{"goto.invalid", "That isn’t a local path", "Bu yerel bir yol değil"},
    Entry{"goto.offline", "“{name}” is on a volume that isn’t mounted",
          "“{name}” bağlı olmayan bir birimde"},
    Entry{"hint.go", "Go", "Git"},
    Entry{"hint.new_tab", "New tab", "Yeni sekme"},
    Entry{"hint.complete", "Complete", "Tamamla"},

    // Sizes ({n} is already formatted with the language's decimal separator).
    Entry{"size.bytes.one", "{n} byte", ""},
    Entry{"size.bytes.other", "{n} bytes", "{n} bayt"},
    Entry{"size.kb", "{n} kB", "{n} kB"},
    Entry{"size.mb", "{n} MB", "{n} MB"},
    Entry{"size.gb", "{n} GB", "{n} GB"},
    Entry{"size.tb", "{n} TB", "{n} TB"},
    Entry{"size.pb", "{n} PB", "{n} PB"},
    Entry{"size.unit.b", "B", "B"},
    Entry{"size.unit.kb", "kB", "kB"},
    Entry{"size.unit.mb", "MB", "MB"},
    Entry{"size.unit.gb", "GB", "GB"},
    Entry{"size.unit.tb", "TB", "TB"},
    Entry{"size.unit.pb", "PB", "PB"},

    // Dates: {d} day, {mon} short month, {y} year, {date}, {time} (24-hour HH:MM).
    Entry{"date.format", "{mon} {d}, {y}", "{d} {mon} {y}"},
    Entry{"date.format_this_year", "{mon} {d}", "{d} {mon}"},
    Entry{"date.with_time", "{date} {time}", "{date} {time}"},
    Entry{"date.today", "Today", "Bugün"},
    Entry{"date.yesterday", "Yesterday", "Dün"},
    Entry{"date.month.1", "Jan", "Oca"},
    Entry{"date.month.2", "Feb", "Şub"},
    Entry{"date.month.3", "Mar", "Mar"},
    Entry{"date.month.4", "Apr", "Nis"},
    Entry{"date.month.5", "May", "May"},
    Entry{"date.month.6", "Jun", "Haz"},
    Entry{"date.month.7", "Jul", "Tem"},
    Entry{"date.month.8", "Aug", "Ağu"},
    Entry{"date.month.9", "Sep", "Eyl"},
    Entry{"date.month.10", "Oct", "Eki"},
    Entry{"date.month.11", "Nov", "Kas"},
    Entry{"date.month.12", "Dec", "Ara"},

    // Type column (keys from navigator/mime.hpp, plus type.link_broken and type.file_ext).
    Entry{"type.folder", "Folder", "Klasör"},
    Entry{"type.link", "Link", "Bağlantı"},
    Entry{"type.link_broken", "Broken link", "Kırık bağlantı"},
    Entry{"type.fifo", "Pipe", "Adlandırılmış kanal"},
    Entry{"type.socket", "Socket", "Soket"},
    Entry{"type.block_device", "Block device", "Blok aygıtı"},
    Entry{"type.char_device", "Character device", "Karakter aygıtı"},
    Entry{"type.executable", "Program", "Program"},
    Entry{"type.file", "File", "Dosya"},
    Entry{"type.file_ext", "{ext} file", "{ext} dosyası"},
    Entry{"type.image", "Image", "Görüntü"},
    Entry{"type.image_png", "PNG image", "PNG görüntüsü"},
    Entry{"type.image_jpeg", "JPEG image", "JPEG görüntüsü"},
    Entry{"type.image_gif", "GIF image", "GIF görüntüsü"},
    Entry{"type.image_webp", "WebP image", "WebP görüntüsü"},
    Entry{"type.image_svg", "SVG image", "SVG görüntüsü"},
    Entry{"type.text", "Text document", "Metin belgesi"},
    Entry{"type.markdown", "Markdown document", "Markdown belgesi"},
    Entry{"type.csv", "CSV document", "CSV belgesi"},
    Entry{"type.html", "HTML document", "HTML belgesi"},
    Entry{"type.source", "Source code", "Kaynak kodu"},
    Entry{"type.script", "Shell script", "Kabuk betiği"},
    Entry{"type.json", "JSON document", "JSON belgesi"},
    Entry{"type.xml", "XML document", "XML belgesi"},
    Entry{"type.pdf", "PDF document", "PDF belgesi"},
    Entry{"type.archive", "Archive", "Arşiv"},
    Entry{"type.archive_zip", "ZIP archive", "ZIP arşivi"},
    Entry{"type.package", "Package", "Paket"},
    Entry{"type.package_deb", "Debian package", "Debian paketi"},
    Entry{"type.appimage", "AppImage application", "AppImage uygulaması"},
    Entry{"type.disk_image", "Disk image", "Disk görüntüsü"},
    Entry{"type.desktop_entry", "Desktop entry", "Masaüstü girdisi"},
    Entry{"type.document", "Document", "Belge"},
    Entry{"type.spreadsheet", "Spreadsheet", "Hesap tablosu"},
    Entry{"type.presentation", "Presentation", "Sunum"},
    Entry{"type.audio", "Audio", "Ses"},
    Entry{"type.video", "Video", "Video"},
    Entry{"type.font", "Font", "Yazı tipi"},
};

std::atomic<Language> g_language{Language::English};

const Entry* find_entry(std::string_view key) noexcept {
    static const std::unordered_map<std::string_view, const Entry*> index = [] {
        std::unordered_map<std::string_view, const Entry*> m;
        m.reserve(std::size(kStrings));
        for (const Entry& e : kStrings) {
            m.emplace(e.key, &e);
        }
        return m;
    }();
    const auto it = index.find(key);
    return it == index.end() ? nullptr : it->second;
}

} // namespace

void set_language(Language language) { g_language.store(language, std::memory_order_relaxed); }

Language language() noexcept { return g_language.load(std::memory_order_relaxed); }

std::string_view tr(std::string_view key) noexcept {
    const Entry* e = find_entry(key);
    if (e == nullptr) {
        return key;
    }
    if (language() == Language::Turkish && !e->tr.empty()) {
        return e->tr;
    }
    return e->en;
}

namespace ui {

bool has_translation(std::string_view key, Language lang) noexcept {
    const Entry* e = find_entry(key);
    if (e == nullptr) {
        return false;
    }
    return lang == Language::Turkish ? !e->tr.empty() : !e->en.empty();
}

std::vector<std::string_view> catalog_keys() {
    std::vector<std::string_view> out;
    out.reserve(std::size(kStrings));
    for (const Entry& e : kStrings) {
        out.push_back(e.key);
    }
    return out;
}

std::string plural_key(std::string_view base, std::uint64_t count) {
    std::string key(base);
    key += (language() == Language::English && count == 1) ? ".one" : ".other";
    return key;
}

std::string substitute(std::string_view pattern, std::string_view name, std::string_view value) {
    std::string token;
    token.reserve(name.size() + 2);
    token += '{';
    token += name;
    token += '}';
    std::string out;
    out.reserve(pattern.size() + value.size());
    std::size_t pos = 0;
    while (pos < pattern.size()) {
        const std::size_t hit = pattern.find(token, pos);
        if (hit == std::string_view::npos) {
            out.append(pattern.substr(pos));
            break;
        }
        out.append(pattern.substr(pos, hit - pos));
        out.append(value);
        pos = hit + token.size();
    }
    return out;
}

std::string format_count(std::uint64_t value) {
    const char sep = language() == Language::Turkish ? '.' : ',';
    const std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + (digits.size() / 3));
    for (std::size_t i = 0; i < digits.size(); ++i) {
        const std::size_t remaining = digits.size() - i;
        if (i != 0 && remaining % 3 == 0) {
            out += sep;
        }
        out += digits[i];
    }
    return out;
}

std::string tr_count(std::string_view base, std::uint64_t count) {
    const std::string key = plural_key(base, count);
    return substitute(tr(key), "n", format_count(count));
}

} // namespace ui
} // namespace navigator
