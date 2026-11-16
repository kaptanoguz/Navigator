// src/ui/i18n/i18n.cpp — built-in English and Turkish string tables (see navigator/i18n.hpp).
//
// Adding a string: append a row to kStrings (keys sorted is not required; the table is small and
// scanned linearly). An empty Turkish column falls back to English.
// Thread-safety: the language is an atomic; tables are immutable.
#include <array>
#include <atomic>

#include <navigator/i18n.hpp>

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
    Entry{"cmd.app.quit", "Quit Navigator", "Navigator'dan çık"},
    Entry{"cmd.view.toggle_fps_overlay", "Toggle performance overlay",
          "Performans göstergesini aç/kapat"},
    Entry{"cmd.view.toggle_theme", "Toggle light/dark theme", "Açık/koyu temayı değiştir"},
    Entry{"error.no_font", "No usable UI font was found.",
          "Kullanılabilir arayüz yazı tipi bulunamadı."},
};

std::atomic<Language> g_language{Language::English};

} // namespace

void set_language(Language language) { g_language.store(language, std::memory_order_relaxed); }

Language language() noexcept { return g_language.load(std::memory_order_relaxed); }

std::string_view tr(std::string_view key) noexcept {
    const Language lang = language();
    for (const Entry& e : kStrings) {
        if (e.key == key) {
            if (lang == Language::Turkish && !e.tr.empty()) {
                return e.tr;
            }
            return e.en;
        }
    }
    return key;
}

} // namespace navigator
