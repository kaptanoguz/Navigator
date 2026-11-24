// src/ui/search/content_search_overlay.cpp — see content_search_overlay.hpp.
#include "search/content_search_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <navigator/i18n.hpp>

#include "search/filter_rules.hpp"
#include "widgets/text_field.hpp"

namespace navigator::ui {

void ContentSearchOverlay::open(const Path& root) {
    root_ = root;
    open_ = true;
    query_.select_all();
    if (!query_.text().empty()) {
        restart_search();
    }
}

void ContentSearchOverlay::close() {
    session_.cancel();
    open_ = false;
}

void ContentSearchOverlay::restart_search() {
    matches_.clear();
    stats_ = search::ContentSearchStats{};
    cursor_ = 0;
    scroll_row_ = 0;
    if (query_.text().empty() || root_.empty()) {
        session_.cancel();
        stats_.done = true;
        return;
    }
    search::ContentSearchOptions opts;
    opts.root = root_;
    opts.query = query_.text();
    opts.case_sensitive = case_sensitive_;
    session_.start(std::move(opts));
}

bool ContentSearchOverlay::poll() {
    if (!open_) {
        return false;
    }
    auto p = session_.poll();
    const bool changed = !p.new_matches.empty() || p.stats.files_scanned != stats_.files_scanned ||
                         p.stats.done != stats_.done;
    for (auto& m : p.new_matches) {
        matches_.push_back(std::move(m));
    }
    stats_ = p.stats;
    return changed;
}

std::optional<Path> ContentSearchOverlay::on_key(Key key, Modifiers mods) {
    if (!open_) {
        return std::nullopt;
    }
    if (key == Key::Escape) {
        if (session_.active()) {
            session_.cancel();
        } else {
            close();
        }
        return std::nullopt;
    }
    if (key == Key::Enter || key == Key::KeypadEnter) {
        if (cursor_ < matches_.size()) {
            const Path picked = matches_[cursor_].file_path;
            close();
            return picked;
        }
        return std::nullopt;
    }
    if (key == Key::Down) {
        if (!matches_.empty() && cursor_ + 1 < matches_.size()) {
            ++cursor_;
        }
        return std::nullopt;
    }
    if (key == Key::Up) {
        if (cursor_ > 0) {
            --cursor_;
        }
        return std::nullopt;
    }
    const std::string before = query_.text();
    (void)apply_field_key(query_, KeyChord{key, mods}, {});
    if (query_.text() != before) {
        restart_search();
    }
    return std::nullopt;
}

void ContentSearchOverlay::on_text(std::string_view utf8) {
    if (!open_) {
        return;
    }
    query_.insert(utf8);
    restart_search();
}

std::optional<Path> ContentSearchOverlay::draw(const Painter& p, const Palette& pal,
                                               const PointerInput& ptr, float width, float height) {
    if (!open_) {
        return std::nullopt;
    }
    Renderer& r = p.renderer();
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    const float rad = std::round(p.px(10.0F));

    // Dim backdrop.
    r.fill_rect(RectF{0.0F, 0.0F, width, height}, Color{0.0F, 0.0F, 0.0F, 0.42F});

    const float box_w = std::round(std::min(width - p.px(48.0F), p.px(680.0F)));
    const float box_h = std::round(std::min(height - p.px(64.0F), p.px(460.0F)));
    const RectF box{std::round((width - box_w) * 0.5F), std::round((height - box_h) * 0.35F), box_w,
                    box_h};

    if (ptr.pressed && !contains(box, ptr.x, ptr.y)) {
        close();
        return std::nullopt;
    }

    r.drop_shadow(box, rad, p.px(20.0F), p.px(6.0F), pal.shadow);
    r.fill_rounded_rect(box, rad, pal.surface_raised);
    r.stroke_rounded_rect(box, rad, hair, pal.border);

    const float pad = std::round(p.px(14.0F));
    float y = box.y + pad;

    // Header: Title + Case-sensitive toggle + Stop button (when running).
    const ShapedText title = p.shape(type::section, tr("content_search.title"));
    p.draw(title, box.x + pad, y + title.cap_height(), pal.text_primary);

    const float btn_h = std::round(p.px(24.0F));
    float rx = box.right() - pad;
    if (session_.active()) {
        const ShapedText stop_t = p.shape(type::caption, tr("content_search.cancel"));
        const float sw = stop_t.width() + p.px(16.0F);
        rx -= sw;
        const RectF sb{rx, y - p.px(2.0F), sw, btn_h};
        const bool hot = contains(sb, ptr.x, ptr.y);
        if (hot && ptr.pressed) {
            session_.cancel();
        }
        r.fill_rounded_rect(sb, p.px(5.0F), hot ? pal.danger : pal.danger_surface);
        p.draw(stop_t, sb.x + p.px(8.0F), std::round(sb.center_y() + (0.5F * stop_t.cap_height())),
               pal.text_primary);
        rx -= p.px(8.0F);
    }

    const ShapedText cs_t = p.shape(type::caption, tr("content_search.case_sensitive"));
    const float cw = cs_t.width() + p.px(16.0F);
    rx -= cw;
    const RectF cb{rx, y - p.px(2.0F), cw, btn_h};
    if (contains(cb, ptr.x, ptr.y) && ptr.pressed) {
        case_sensitive_ = !case_sensitive_;
        restart_search();
    }
    r.fill_rounded_rect(cb, p.px(5.0F),
                        case_sensitive_ ? pal.accent.with_alpha(0.25F) : pal.surface);
    r.stroke_rounded_rect(cb, p.px(5.0F), hair, case_sensitive_ ? pal.accent : pal.border);
    p.draw(cs_t, cb.x + p.px(8.0F), std::round(cb.center_y() + (0.5F * cs_t.cap_height())),
           case_sensitive_ ? pal.text_primary : pal.text_secondary);

    y += std::round(p.px(30.0F));

    // Query input box.
    const float field_h = std::round(p.px(32.0F));
    const RectF field{box.x + pad, y, box.w - (2.0F * pad), field_h};
    r.fill_rounded_rect(field, p.px(6.0F), pal.surface);
    r.stroke_rounded_rect(field, p.px(6.0F), hair, pal.focus_ring);
    TextFieldStyle st;
    st.text = &type::row;
    st.caret_height_dp = 15.0F;
    (void)draw_text_field(p, pal, query_,
                          RectF{field.x + p.px(8.0F), field.y, field.w - p.px(16.0F), field.h},
                          tr("content_search.placeholder"), query_scroll_, st);
    y += field_h + p.px(8.0F);

    // Status line.
    const std::string status_s =
        session_.active()
            ? std::string(tr("content_search.searching")) + " (" +
                  std::to_string(stats_.files_scanned) + " files, " +
                  std::to_string(matches_.size()) + " matches)"
            : std::to_string(matches_.size()) + " matches (" +
                  std::to_string(stats_.files_scanned) + " files)";
    const ShapedText stat_t = p.shape(type::caption, status_s);
    p.draw(stat_t, box.x + pad, y + stat_t.cap_height(), pal.text_muted);
    y += std::round(p.px(20.0F));

    // Results list.
    const RectF list_rect{box.x + pad, y, box.w - (2.0F * pad),
                          std::max(0.0F, box.bottom() - pad - y)};
    const float row_h = std::round(p.px(36.0F));
    const auto visible =
        static_cast<std::size_t>(std::max(1.0F, std::floor(list_rect.h / row_h)));
    if (cursor_ < scroll_row_) {
        scroll_row_ = cursor_;
    } else if (cursor_ >= scroll_row_ + visible) {
        scroll_row_ = cursor_ - visible + 1;
    }
    if (contains(list_rect, ptr.x, ptr.y) && ptr.wheel_y != 0.0F && matches_.size() > visible) {
        const int delta = ptr.wheel_y > 0.0F ? -3 : 3;
        const auto max_s = matches_.size() - visible;
        const int next = std::clamp(static_cast<int>(scroll_row_) + delta, 0,
                                    static_cast<int>(max_s));
        scroll_row_ = static_cast<std::size_t>(next);
    }

    if (matches_.empty() && !query_.text().empty() && stats_.done) {
        const ShapedText empty_t = p.shape(type::row, tr("content_search.no_matches"));
        p.draw(empty_t, list_rect.x + p.px(8.0F), list_rect.y + p.px(18.0F), pal.text_muted);
        return std::nullopt;
    }

    r.set_clip(list_rect);
    std::optional<Path> activated;
    for (std::size_t i = scroll_row_; i < matches_.size() && i < scroll_row_ + visible + 1; ++i) {
        const RectF rr{list_rect.x, list_rect.y + (static_cast<float>(i - scroll_row_) * row_h),
                       list_rect.w, row_h - p.px(2.0F)};
        const bool is_cur = (i == cursor_);
        const bool inside = contains(rr, ptr.x, ptr.y);
        if (inside && ptr.pressed) {
            cursor_ = i;
            activated = matches_[i].file_path;
        }
        if (is_cur) {
            r.fill_rounded_rect(rr, p.px(5.0F), pal.selection);
        } else if (inside) {
            r.fill_rounded_rect(rr, p.px(5.0F), pal.row_hover);
        }
        const auto& m = matches_[i];
        const std::string loc = m.rel_path + ":" + std::to_string(m.line_number);
        const ShapedText lt = fitter_.fit(p, type::caption, loc, rr.w - p.px(16.0F), Elide::Middle);
        p.draw(lt, rr.x + p.px(8.0F), rr.y + p.px(13.0F), pal.accent);
        const ShapedText stxt =
            fitter_.fit(p, type::row, m.line_snippet, rr.w - p.px(16.0F), Elide::End);
        p.draw(stxt, rr.x + p.px(8.0F), rr.y + p.px(29.0F), pal.text_primary);
    }
    r.set_clip(std::nullopt);
    if (activated) {
        close();
    }
    return activated;
}

} // namespace navigator::ui
