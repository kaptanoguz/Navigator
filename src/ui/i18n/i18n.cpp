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

constexpr std::array kStrings{
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
    Entry{"search_tabs.pane_chip", "Pane {n}", "Panel {n}"},
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
    Entry{"layout.delete_confirm", "Delete? Press Enter", "Silinsin mi? Enter’a basın"},
    Entry{"layout.saved", "Saved layout “{name}”", "“{name}” yerleşimi kaydedildi"},
    Entry{"layout.loaded", "Loaded layout “{name}”", "“{name}” yerleşimi yüklendi"},
    Entry{"layout.deleted", "Deleted layout “{name}”", "“{name}” yerleşimi silindi"},
    Entry{"layout.save_failed", "Couldn’t save layout “{name}”.",
          "“{name}” yerleşimi kaydedilemedi."},
    Entry{"layout.load_failed", "Couldn’t load layout “{name}”.",
          "“{name}” yerleşimi yüklenemedi."},
    Entry{"layout.delete_failed", "Couldn’t delete layout “{name}”.",
          "“{name}” yerleşimi silinemedi."},

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
        m.reserve(kStrings.size());
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
    out.reserve(kStrings.size());
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
